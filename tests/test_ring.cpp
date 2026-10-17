#include <atomic>
#include <thread>
#include <vector>

#include "audio/ramp.h"
#include "core/sample_ring.h"
#include "core/spsc_queue.h"
#include "test.h"

using sl::SampleIndex;
using sl::SampleRing;
using sl::SpscQueue;

namespace {
// Writes ramp frames [from, from + n) into the ring.
void write_ramp(SampleRing& r, SampleIndex from, size_t n) {
  const uint32_t ch = r.channels();
  std::vector<float> buf(n * ch);
  for (size_t i = 0; i < n; ++i)
    for (uint32_t c = 0; c < ch; ++c) buf[i * ch + c] = sl::ramp_value(from + SampleIndex(i), c);
  r.write(buf.data(), n);
}

// Counts frames in buf (starting at frame `from`) that differ from the ramp.
size_t count_bad(const std::vector<float>& buf, SampleIndex from, size_t frames, uint32_t ch) {
  size_t bad = 0;
  for (size_t i = 0; i < frames; ++i)
    for (uint32_t c = 0; c < ch; ++c)
      if (buf[i * ch + c] != sl::ramp_value(from + SampleIndex(i), c)) { ++bad; break; }
  return bad;
}
}  // namespace

TEST(ring_capacity_rounds_up) {
  SampleRing r(2, 1000);
  CHECK_EQ(r.capacity(), size_t(1024));
  CHECK_EQ(r.channels(), 2u);
}

TEST(ring_roundtrip_across_wrap) {
  SampleRing r(2, 1024);
  write_ramp(r, 0, 700);
  write_ramp(r, 700, 700);
  CHECK_EQ(r.write_index(), SampleIndex(1400));
  std::vector<float> buf(700 * 2);
  auto res = r.read(700, buf.data(), 700);
  CHECK_EQ(res.frames, size_t(700));
  CHECK(!res.overrun);
  CHECK_EQ(count_bad(buf, 700, 700, 2), size_t(0));
}

TEST(ring_overrun_when_reader_too_far_behind) {
  SampleRing r(2, 1024);
  for (SampleIndex f = 0; f < 3000; f += 500) write_ramp(r, f, 500);
  std::vector<float> buf(4096 * 2);
  auto a = r.read(0, buf.data(), 4096);
  CHECK_EQ(a.frames, size_t(0));
  CHECK(a.overrun);
  auto b = r.read(2000, buf.data(), 4096);
  CHECK_EQ(b.frames, size_t(1000));
  CHECK(!b.overrun);
  CHECK_EQ(count_bad(buf, 2000, 1000, 2), size_t(0));
}

TEST(ring_oversized_write_keeps_newest) {
  SampleRing r(1, 16);
  write_ramp(r, 0, 40);
  CHECK_EQ(r.write_index(), SampleIndex(40));
  CHECK_EQ(r.oldest_valid(), SampleIndex(24));
  std::vector<float> buf(16);
  auto res = r.read(24, buf.data(), 16);
  CHECK_EQ(res.frames, size_t(16));
  CHECK_EQ(count_bad(buf, 24, 16, 1), size_t(0));
}

TEST(ring_jump_invalidates_skipped) {
  SampleRing r(1, 1024);
  write_ramp(r, 0, 100);
  r.jump_to(5000);
  CHECK_EQ(r.write_index(), SampleIndex(5000));
  CHECK_EQ(r.oldest_valid(), SampleIndex(5000));
  std::vector<float> buf(64);
  auto old = r.read(50, buf.data(), 64);
  CHECK_EQ(old.frames, size_t(0));
  CHECK(old.overrun);
  write_ramp(r, 5000, 10);
  auto res = r.read(5000, buf.data(), 64);
  CHECK_EQ(res.frames, size_t(10));
  CHECK(!res.overrun);
  CHECK_EQ(count_bad(buf, 5000, 10, 1), size_t(0));
}

TEST(ring_read_future_is_empty) {
  SampleRing r(1, 64);
  write_ramp(r, 0, 10);
  std::vector<float> buf(8);
  auto res = r.read(r.write_index() + 5, buf.data(), 8);
  CHECK_EQ(res.frames, size_t(0));
  CHECK(!res.overrun);
  res = r.read(r.write_index(), buf.data(), 8);
  CHECK_EQ(res.frames, size_t(0));
  CHECK(!res.overrun);
}

TEST(ring_start_index) {
  SampleRing r(1, 16, 1'000'000);
  CHECK_EQ(r.write_index(), SampleIndex(1'000'000));
  CHECK_EQ(r.oldest_valid(), SampleIndex(1'000'000));
  write_ramp(r, 1'000'000, 4);
  std::vector<float> buf(4);
  auto res = r.read(1'000'000, buf.data(), 4);
  CHECK_EQ(res.frames, size_t(4));
  CHECK_EQ(count_bad(buf, 1'000'000, 4, 1), size_t(0));
}

// VirtualLock is limited by the working-set minimum (default ~200 KB): a page fits, 64 MB
// does not. The ring works either way; locked() lets the session warn.
TEST(ring_reports_lock) {
  SampleRing small(1, 1024);
  CHECK(small.locked());
  // 64 MiB: locked only if the process working set was raised (the app does; a test may
  // have). The ring works either way.
  SampleRing big(1, size_t(1) << 24);
  write_ramp(big, 0, 4);
  std::vector<float> buf(4);
  CHECK_EQ(big.read(0, buf.data(), 4).frames, size_t(4));
}

TEST(ring_threaded_no_tearing) {
  constexpr SampleIndex kTotal = 5'000'000;
  constexpr uint32_t kCh = 2;
  SampleRing r(kCh, 16384);
  std::atomic<bool> done{false};
  std::thread producer([&] {
    std::vector<float> chunk(480 * kCh);
    for (SampleIndex f = 0; f < kTotal; f += 480) {
      for (size_t i = 0; i < 480; ++i)
        for (uint32_t c = 0; c < kCh; ++c) chunk[i * kCh + c] = sl::ramp_value(f + SampleIndex(i), c);
      r.write(chunk.data(), 480);
    }
    done.store(true, std::memory_order_release);
  });

  std::vector<float> buf(4096 * kCh);
  SampleIndex cursor = 0;
  size_t torn = 0, overruns = 0, got = 0;
  while (true) {
    const bool was_done = done.load(std::memory_order_acquire);
    auto res = r.read(cursor, buf.data(), 4096);
    if (res.overrun) {
      ++overruns;
      cursor = r.oldest_valid();
    } else if (res.frames) {
      torn += count_bad(buf, cursor, res.frames, kCh);
      got += res.frames;
      cursor += SampleIndex(res.frames);
    } else if (was_done) {
      break;
    }
  }
  producer.join();
  CHECK_EQ(torn, size_t(0));
  CHECK(got > 0);
  CHECK_EQ(cursor, kTotal + (kTotal % 480 ? 480 - kTotal % 480 : 0));
  std::printf("  (read %zu frames, %zu overruns)\n", got, overruns);
}

TEST(spsc_fifo_and_full) {
  SpscQueue<int, 4> q;
  CHECK(!q.pop().has_value());
  CHECK(q.push(1));
  CHECK(q.push(2));
  CHECK(q.push(3));
  CHECK(q.push(4));
  CHECK(!q.push(5));
  CHECK_EQ(*q.pop(), 1);
  CHECK(q.push(5));
  CHECK_EQ(*q.pop(), 2);
  CHECK_EQ(*q.pop(), 3);
  CHECK_EQ(*q.pop(), 4);
  CHECK_EQ(*q.pop(), 5);
  CHECK(!q.pop().has_value());
}
