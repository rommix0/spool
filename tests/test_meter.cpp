#include <atomic>
#include <cmath>
#include <limits>
#include <thread>
#include <vector>

#include "core/clock.h"
#include "core/sample_ring.h"
#include "session/meter.h"
#include "test.h"

using spool::ChannelMeter;
using spool::FakeClock;
using spool::MeterReader;
using spool::SampleRing;

namespace {
constexpr uint32_t kRate = 48000;

ChannelMeter g0(const MeterReader& m) { return m.get()[0]; }  // copy: get() returns by value

// Writes `frames` of a sine on channel 0 (other channels silent).
void write_sine(SampleRing& r, size_t frames, float amp, double hz = 1000.0, size_t phase0 = 0) {
  const uint32_t ch = r.channels();
  std::vector<float> buf(frames * ch, 0.0f);
  for (size_t i = 0; i < frames; ++i)
    buf[i * ch] = amp * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * hz *
                                                     double(phase0 + i) / kRate));
  r.write(buf.data(), frames);
}

void write_const(SampleRing& r, size_t frames, float v) {
  std::vector<float> buf(frames * r.channels(), v);
  r.write(buf.data(), frames);
}
}  // namespace

TEST(meter_full_scale_sine) {
  SampleRing ring(1, kRate * 2);
  FakeClock clock;
  MeterReader m(ring, kRate, clock);
  write_sine(ring, kRate / 2, 1.0f);
  m.update();
  const auto g = m.get();
  CHECK_EQ(g.size(), size_t(1));
  CHECK_NEAR(g[0].peak_db, 0.0, 0.1);
  CHECK_NEAR(g[0].rms_db, -3.01, 0.1);
}

TEST(meter_silence_floor) {
  SampleRing ring(2, kRate);
  FakeClock clock;
  MeterReader m(ring, kRate, clock);
  write_const(ring, kRate / 10, 0.0f);
  m.update();
  const auto g = m.get();
  CHECK_EQ(g.size(), size_t(2));
  for (const auto& c : g) {
    CHECK_EQ(c.peak_db, -60.0f);
    CHECK_EQ(c.hold_db, -60.0f);
    CHECK_EQ(c.rms_db, -60.0f);
    CHECK_EQ(c.clips, uint64_t(0));
  }
}

TEST(meter_clip_runs) {
  SampleRing ring(1, kRate);
  FakeClock clock;
  MeterReader m(ring, kRate, clock);
  write_const(ring, 10, 1.0f);
  write_const(ring, 5, 0.5f);
  write_const(ring, 10, 1.0f);
  write_const(ring, 5, 0.0f);
  m.update();
  CHECK_EQ(g0(m).clips, uint64_t(2));
  m.reset_clips();
  CHECK_EQ(g0(m).clips, uint64_t(0));
}

TEST(meter_clip_run_spans_updates) {
  SampleRing ring(1, kRate);
  FakeClock clock;
  MeterReader m(ring, kRate, clock);
  write_const(ring, 5, -1.0f);
  m.update();
  write_const(ring, 5, -1.0f);
  m.update();
  write_const(ring, 5, 0.0f);
  m.update();
  CHECK_EQ(g0(m).clips, uint64_t(1));
  write_const(ring, 1, 0.99f);  // just above the -0.1 dBFS threshold
  write_const(ring, 1, 0.98f);  // below it
  m.update();
  CHECK_EQ(g0(m).clips, uint64_t(2));
}

TEST(meter_hold_2s) {
  SampleRing ring(1, kRate * 4);
  FakeClock clock;
  MeterReader m(ring, kRate, clock);
  write_const(ring, 480, 1.0f);
  m.update();
  CHECK_NEAR(g0(m).hold_db, 0.0, 0.01);
  write_const(ring, kRate / 2, 0.0f);
  clock.advance_ms(1900);
  m.update();
  CHECK_NEAR(g0(m).hold_db, 0.0, 0.01);
  CHECK_EQ(g0(m).peak_db, -60.0f);
  clock.advance_ms(200);
  m.update();
  CHECK_EQ(g0(m).hold_db, -60.0f);
}

TEST(meter_rms_window_300ms) {
  SampleRing ring(1, kRate * 2);
  FakeClock clock;
  MeterReader m(ring, kRate, clock);
  write_const(ring, kRate / 2, 1.0f);  // loud
  write_const(ring, kRate * 3 / 10, 0.0f);  // exactly 300 ms of silence pushes it out
  m.update();
  CHECK_EQ(g0(m).rms_db, -60.0f);
  write_const(ring, kRate * 3 / 10, 0.5f);
  m.update();
  CHECK_NEAR(g0(m).rms_db, -6.02, 0.05);
}

TEST(meter_skips_when_far_behind) {
  SampleRing ring(1, kRate / 10);  // capacity 8192 frames
  FakeClock clock;
  MeterReader m(ring, kRate, clock);
  write_const(ring, ring.capacity() * 3, 1.0f);  // reader lapped
  write_const(ring, kRate / 10, 0.25f);          // the last 100 ms
  m.update();
  const auto g = m.get();
  CHECK_NEAR(g[0].peak_db, -12.04, 0.05);  // only the resynced tail is seen
}

TEST(meter_jump_to_does_not_spin) {
  SampleRing ring(1, kRate / 10);
  FakeClock clock;
  MeterReader m(ring, kRate, clock);
  write_const(ring, 1000, 1.0f);
  ring.jump_to(ring.write_index() + 50000);
  write_const(ring, 200, 0.1f);
  m.update();  // reader is below the jump floor: must resync and return
  CHECK_NEAR(g0(m).peak_db, -20.0, 0.05);  // only post-jump data
  ring.jump_to(ring.write_index() + 50000);
  m.update();  // jump with no post-jump data: returns, nothing new to read
  CHECK_NEAR(g0(m).peak_db, -20.0, 0.05);
}

TEST(meter_concurrent_writer_terminates) {
  SampleRing ring(1, 4096);
  FakeClock clock;
  MeterReader m(ring, kRate, clock);
  std::atomic<bool> stop{false};
  std::thread w([&] {
    std::vector<float> blk(256, 0.5f);
    while (!stop.load()) ring.write(blk.data(), blk.size());
  });
  for (int i = 0; i < 2000; ++i) {
    m.update();
    const float p = g0(m).peak_db;
    CHECK(p == -60.0f || std::fabs(p + 6.02f) < 0.05f);  // never torn data
  }
  stop = true;
  w.join();
}

TEST(meter_non_finite_samples_do_not_poison_rms) {
  SampleRing ring(1, kRate);
  FakeClock clock;
  MeterReader m(ring, kRate, clock);
  const float bad[] = {std::numeric_limits<float>::quiet_NaN(),
                       std::numeric_limits<float>::infinity(),
                       -std::numeric_limits<float>::infinity()};
  ring.write(bad, 3);
  m.update();
  CHECK_EQ(g0(m).peak_db, -60.0f);
  CHECK_EQ(g0(m).clips, uint64_t(0));
  write_sine(ring, kRate / 2, 1.0f);
  m.update();
  CHECK_NEAR(g0(m).rms_db, -3.01, 0.1);
  CHECK_NEAR(g0(m).peak_db, 0.0, 0.1);
}

TEST(meter_dead_capture_drops_to_floor) {
  SampleRing ring(1, kRate);
  FakeClock clock;
  MeterReader m(ring, kRate, clock);
  write_sine(ring, kRate / 2, 1.0f);
  m.update();
  clock.advance_ms(250);
  m.update();  // brief gap: last reading kept
  CHECK_NEAR(g0(m).peak_db, 0.0, 0.1);
  CHECK_NEAR(g0(m).rms_db, -3.01, 0.1);
  clock.advance_ms(60);  // 310 ms without frames
  m.update();
  CHECK_EQ(g0(m).peak_db, -60.0f);
  CHECK_EQ(g0(m).rms_db, -60.0f);
  CHECK_NEAR(g0(m).hold_db, 0.0, 0.01);  // hold still running
  clock.advance_ms(2000);
  m.update();
  CHECK_EQ(g0(m).hold_db, -60.0f);
  write_sine(ring, kRate / 2, 1.0f);  // capture comes back with a clean window
  m.update();
  CHECK_NEAR(g0(m).rms_db, -3.01, 0.1);
}
