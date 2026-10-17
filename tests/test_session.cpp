#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "audio/synthetic_source.h"
#include "core/clock.h"
#include "dest_helpers.h"
#include "io/fault_file_system.h"
#include "session/event_log.h"
#include "session/manifest.h"
#include "session/session_manager.h"
#include "test.h"

using namespace sl;
using namespace dest_test;
using Kind = FaultFileSystem::Kind;

namespace {

// 2026-10-16 18:30:00 UTC, local = UTC-4 -> 14:30:00.
WallTime s_wall() { return WallTime{1792175400000LL, -240}; }

const std::wstring kP = L"D:\\p";
const std::wstring kS = L"E:\\s";
constexpr SampleIndex kRate = 8000;

// Manual ramp source at 8 kHz, 32-bit float WAV into D:\p, event log on C:.
struct SessRig {
  explicit SessRig(StreamFormat fmt = {uint32_t(kRate), 1})
      : clock(s_wall(), 1000), log(fs, L"C:\\logs\\s.log"), src(fmt, SyntheticSource::Pattern::Ramp, true),
        mgr(src, fs, clock, log) {
    std::wstring err;
    fs.ensure_dir(L"C:\\logs", &err);
    cfg.params.session_name = L"gig";
    DestinationConfig d;
    d.format = OutFormat::Wav;
    d.wav_depth = WavDepth::Float32;
    d.folders = {kP};
    cfg.dests = {d};
  }
  ~SessRig() {
    mgr.stop();
    fs.release_hangs();
  }

  bool start() {
    std::wstring err;
    return mgr.start(cfg, nullptr, &err);
  }
  // Every destination that is writing has reached the capture position.
  bool caught_up() {
    return poll([&] {
      const SampleIndex pos = mgr.stats().position;
      for (const auto& d : mgr.dest_status())
        if (d.state == DestState::Writing && d.cursor < pos) return false;
      return true;
    });
  }
  // Pumps `seconds` in 100 ms blocks with the clock advancing alongside; optionally waits
  // for the writers after every block so the ring never overruns.
  bool pump(double seconds, bool wait = true) {
    const SampleIndex rate = mgr.stats().fmt.sample_rate;
    const int blocks = int(seconds * 10 + 0.5);
    for (int i = 0; i < blocks; ++i) {
      src.pump(size_t(rate / 10));
      clock.advance_ms(100);
      if (wait && !caught_up()) return false;
      if (!wait) std::this_thread::sleep_for(std::chrono::milliseconds(20));  // let the watchdog tick
    }
    return true;
  }
  // Input lost: advances the clock in step_ms steps, waiting for the silence fill (up to
  // `fill_end`) and the writers after each step.
  bool advance_lost(double seconds, SampleIndex fill_from, SampleIndex fill_end, int step_ms = 100) {
    const int steps = int(seconds * 1000 / step_ms + 0.5);
    for (int i = 1; i <= steps; ++i) {
      clock.advance_ms(step_ms);
      const SampleIndex want = std::min(fill_end, fill_from + SampleIndex(i) * step_ms * kRate / 1000);
      if (!poll([&] { return mgr.stats().position >= want; })) return false;
      if (!caught_up()) return false;
    }
    return true;
  }
  bool wait_state(SessionState s) {
    return poll([&] { return mgr.stats().state == s; });
  }
  std::vector<ManifestRecord> only_manifest(const std::wstring& dir) {
    const auto m = fs.list(dir, L".jsonl");
    return m.size() == 1 ? read_manifest(fs, m[0]) : std::vector<ManifestRecord>{};
  }

  FaultFileSystem fs;
  FakeClock clock;
  EventLog log;
  SyntheticSource src;
  SessionManager mgr;
  SessionConfig cfg;
};

std::string str(const ManifestRecord& r, const char* key) {
  const auto it = r.find(key);
  return it == r.end() ? std::string() : it->second;
}

int64_t num_of(const ManifestRecord& r, const char* key) {
  ManifestRecord c = r;
  return num(c, key);
}

}  // namespace

TEST(session_basic_record_stop) {
  SessRig rig;
  CHECK(rig.start());
  const SessionStats st = rig.mgr.stats();
  CHECK(st.state == SessionState::Recording);
  CHECK(st.session_id == L"20261016-143000");
  CHECK(rig.pump(10));
  CHECK_EQ(rig.mgr.stats().position, 10 * kRate);
  CHECK_NEAR(rig.mgr.stats().elapsed_s, 10.0, 1e-9);
  rig.mgr.stop();
  CHECK(rig.mgr.stats().state == SessionState::Idle);

  const auto wavs = rig.fs.list(kP, L".wav");
  CHECK_EQ(wavs.size(), size_t(1));
  if (wavs.size() != 1) return;
  const auto v = wav_samples(rig.fs, wavs[0]);
  CHECK_EQ(v.size(), size_t(10 * kRate));
  CHECK_EQ(ramp_errors(v, 0), size_t(0));

  const auto recs = rig.only_manifest(kP);
  CHECK(recs.size() >= 2);
  if (recs.size() < 2) return;
  CHECK_EQ(str(recs.front(), "ev"), std::string("session_start"));
  CHECK_EQ(str(recs.front(), "session_id"), std::string("20261016-143000"));
  CHECK_EQ(str(recs.back(), "ev"), std::string("session_end"));
  CHECK_EQ(num_of(recs.back(), "sample"), int64_t(10 * kRate));
}

TEST(session_marker_clamped_to_start) {
  SessRig rig;
  CHECK(rig.start());
  CHECK(rig.pump(5));
  rig.mgr.marker(30);  // 30 s ago is before the session: clamped to its first sample
  rig.mgr.marker(1);
  rig.mgr.stop();
  const auto ms = events(rig.only_manifest(kP), "marker");
  CHECK_EQ(ms.size(), size_t(2));
  if (ms.size() != 2) return;
  CHECK_EQ(num_of(ms[0], "sample"), int64_t(0));
  CHECK_EQ(str(ms[0], "name"), std::string("Marker 1"));
  CHECK_EQ(num_of(ms[1], "sample"), int64_t(4 * kRate));
  CHECK_EQ(str(ms[1], "name"), std::string("Marker 2"));
}

TEST(session_split_rotates_at_write_index) {
  SessRig rig;
  CHECK(rig.start());
  CHECK(rig.pump(2));
  rig.mgr.split();
  // A split is never retroactive: wait until it took effect before feeding more.
  CHECK(poll([&] { return rig.mgr.dest_status()[0].n == 2; }));
  CHECK(rig.pump(1));
  rig.mgr.stop();
  const auto wavs = rig.fs.list(kP, L".wav");
  CHECK_EQ(wavs.size(), size_t(2));
  if (wavs.size() != 2) return;
  const auto a = wav_samples(rig.fs, wavs[0]);
  const auto b = wav_samples(rig.fs, wavs[1]);
  CHECK_EQ(a.size(), size_t(2 * kRate));
  CHECK_EQ(b.size(), size_t(kRate));
  CHECK_EQ(ramp_errors(a, 0) + ramp_errors(b, 2 * kRate), size_t(0));
}

TEST(session_device_loss_fills_silence) {
  SessRig rig;
  CHECK(rig.start());
  CHECK(rig.pump(2));
  rig.src.simulate_loss();
  CHECK(rig.wait_state(SessionState::InputLost));
  CHECK(rig.advance_lost(3, 2 * kRate, 5 * kRate));
  rig.src.simulate_return({uint32_t(kRate), 1});
  CHECK(rig.wait_state(SessionState::Recording));
  CHECK(rig.pump(2));
  CHECK_EQ(rig.mgr.stats().gaps, uint64_t(1));
  rig.mgr.stop();

  const auto wavs = rig.fs.list(kP, L".wav");
  CHECK_EQ(wavs.size(), size_t(1));
  if (wavs.size() != 1) return;
  const auto v = wav_samples(rig.fs, wavs[0]);
  CHECK(v.size() >= size_t(7 * kRate - 80) && v.size() <= size_t(7 * kRate + 80));
  if (v.size() < size_t(7 * kRate - 80)) return;
  const std::vector<float> before(v.begin(), v.begin() + 2 * kRate);
  CHECK_EQ(ramp_errors(before, 0), size_t(0));
  size_t z = size_t(2 * kRate);
  while (z < v.size() && v[z] == 0.0f) ++z;
  CHECK_NEAR(double(z), double(5 * kRate), 80);  // 3 s of silence (±10 ms)
  const std::vector<float> after(v.begin() + z, v.end());
  CHECK_EQ(after.size(), size_t(2 * kRate));
  CHECK_EQ(ramp_errors(after, 2 * kRate), size_t(0));  // the source's own frames continue

  const auto recs = rig.only_manifest(kP);
  const auto gs = events(recs, "gap_start");
  const auto ge = events(recs, "gap_end");
  CHECK_EQ(gs.size(), size_t(1));
  CHECK_EQ(ge.size(), size_t(1));
  if (gs.size() != 1 || ge.size() != 1) return;
  CHECK_EQ(str(gs[0], "reason"), std::string("device_lost"));
  CHECK_EQ(num_of(gs[0], "sample"), int64_t(2 * kRate));
  CHECK_EQ(str(ge[0], "reason"), std::string("device_lost"));
  CHECK_NEAR(double(num_of(ge[0], "sample")), double(5 * kRate), 80);
}

TEST(session_long_loss_closes_and_jumps) {
  SessRig rig;
  rig.cfg.dests[0].rotation_minutes = 1;  // the silence cap: 1 min
  CHECK(rig.start());
  CHECK(rig.pump(2));
  rig.src.simulate_loss();
  CHECK(rig.wait_state(SessionState::InputLost));
  const SampleIndex cap_at = 2 * kRate + 60 * kRate;
  CHECK(rig.advance_lost(90, 2 * kRate, cap_at, 1000));
  CHECK_EQ(rig.mgr.stats().position, cap_at);  // the filler stopped at the cap
  CHECK(poll([&] { return rig.mgr.dest_status()[0].state == DestState::GapClosed; }));
  rig.src.simulate_return({uint32_t(kRate), 1});
  CHECK(rig.wait_state(SessionState::Recording));
  const SampleIndex resume_at = 92 * kRate;  // start + elapsed wall time × rate
  CHECK_NEAR(double(rig.mgr.stats().position), double(resume_at), 80);
  CHECK(rig.pump(2));
  rig.mgr.stop();

  const auto recs = rig.only_manifest(kP);
  bool closed_at_cap = false;
  for (const auto& r : events(recs, "file_close")) closed_at_cap |= num_of(r, "end") == int64_t(cap_at);
  CHECK(closed_at_cap);
  const auto opens = events(recs, "file_open");
  CHECK(!opens.empty());
  if (opens.empty()) return;
  CHECK_NEAR(double(num_of(opens.back(), "sample")), double(resume_at), 80);
  const auto wavs = rig.fs.list(kP, L".wav");
  CHECK(!wavs.empty());
  if (wavs.empty()) return;
  const auto last = wav_samples(rig.fs, wavs.back());
  CHECK_EQ(last.size(), size_t(2 * kRate));
  CHECK_EQ(ramp_errors(last, 2 * kRate), size_t(0));
  const auto ge = events(recs, "gap_end");
  CHECK_EQ(ge.size(), size_t(1));
  if (ge.size() == 1) CHECK_EQ(str(ge[0], "reason"), std::string("device_lost"));
}

TEST(session_format_change_links_new_session) {
  SessRig rig;
  CHECK(rig.start());
  const std::wstring old_id = rig.mgr.stats().session_id;
  CHECK(rig.pump(2));
  rig.src.simulate_loss();
  CHECK(rig.wait_state(SessionState::InputLost));
  CHECK(rig.advance_lost(1, 2 * kRate, 3 * kRate));
  rig.src.simulate_return({16000, 1});
  CHECK(poll([&] {
    const SessionStats s = rig.mgr.stats();
    return s.state == SessionState::Recording && s.fmt.sample_rate == 16000;
  }));
  const SessionStats st = rig.mgr.stats();
  CHECK(st.session_id == L"20261016-143003");
  CHECK_EQ(st.position, SampleIndex(0));  // a new clock
  CHECK(rig.mgr.meter() != nullptr);
  CHECK(rig.pump(1));
  rig.mgr.stop();

  const auto ms = rig.fs.list(kP, L".jsonl");
  CHECK_EQ(ms.size(), size_t(2));
  bool ended = false, linked = false;
  for (const auto& m : ms) {
    const auto recs = read_manifest(rig.fs, m);
    for (const auto& r : events(recs, "session_end")) ended |= str(r, "reason") == "format_change";
    for (const auto& r : events(recs, "session_start"))
      if (str(r, "prev_session") == narrow(old_id)) {
        linked = true;
        CHECK_EQ(num_of(r, "rate"), int64_t(16000));
        CHECK_EQ(str(r, "session_id"), std::string("20261016-143003"));
      }
  }
  CHECK(ended);
  CHECK(linked);
  CHECK_EQ(rig.fs.list(kP, L".wav").size(), size_t(2));
}

TEST(session_watchdog_fails_over_stalled) {
  SessRig rig;
  rig.cfg.ring_seconds = 1;
  rig.cfg.dests[0].folders = {kP, kS};
  rig.fs.add_rule({kP + L"\\", Kind::HangWrite, 16000});  // the WAV hangs after ~0.5 s
  CHECK(rig.start());
  CHECK(rig.pump(2, false));
  CHECK(poll([&] {
    const auto d = rig.mgr.dest_status();
    return d[0].state == DestState::Writing && d[0].folders[1].health == FolderHealth::Active;
  }));
  rig.mgr.stop();
  const auto recs = rig.only_manifest(kS);
  const auto fo = events(recs, "failover");
  CHECK_EQ(fo.size(), size_t(1));
  if (fo.size() == 1) CHECK_EQ(str(fo[0], "trigger"), std::string("stall"));
}

TEST(session_channel_map_disabled_after_format_change) {
  SessRig rig({uint32_t(kRate), 2});
  DestinationConfig b = rig.cfg.dests[0];
  rig.cfg.dests[0].name = L"second";
  rig.cfg.dests[0].channels = {1};
  b.name = L"all";
  rig.cfg.dests.push_back(b);
  CHECK(rig.start());
  rig.src.simulate_loss();
  CHECK(rig.wait_state(SessionState::InputLost));
  rig.src.simulate_return({uint32_t(kRate), 1});  // same second: the time-based id collides
  CHECK(poll([&] {
    const SessionStats s = rig.mgr.stats();
    return s.state == SessionState::Recording && s.fmt.channels == 1;
  }));
  CHECK(rig.mgr.stats().session_id == L"20261016-143000-2");
  CHECK(poll([&] {
    const auto d = rig.mgr.dest_status();
    return d.size() == 2 && d[0].state == DestState::Disabled && d[1].state == DestState::Writing;
  }));
  bool logged = false;
  for (const auto& e : rig.log.recent(100))
    logged |= e.sev == Severity::Error && e.dest == L"second" && e.text.find(L"disabled") != std::wstring::npos;
  CHECK(logged);
}

TEST(session_stop_with_hung_destinations_is_parallel) {
  SessRig rig;
  const std::wstring hung[] = {L"F:\\a", L"G:\\b", L"H:\\c"};
  rig.cfg.dests[0].folders = {L"C:\\ok"};
  for (const auto& f : hung) {
    DestinationConfig d = rig.cfg.dests[0];
    d.name = f.substr(0, 1);
    d.folders = {f};
    rig.cfg.dests.push_back(d);
    rig.fs.add_rule({f + L"\\", Kind::HangWrite, 4000});
  }
  CHECK(rig.start());
  for (int i = 0; i < 10; ++i) {
    rig.src.pump(size_t(kRate / 10));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  CHECK(poll([&] { return rig.mgr.dest_status()[0].cursor >= kRate; }));
  const auto t0 = std::chrono::steady_clock::now();
  rig.mgr.stop();
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  CHECK(secs < 5.0);  // one shared budget, not 3 s per hung destination
  const auto wavs = rig.fs.list(L"C:\\ok", L".wav");
  CHECK_EQ(wavs.size(), size_t(1));
  if (wavs.size() == 1) CHECK_EQ(wav_samples(rig.fs, wavs[0]).size(), size_t(kRate));  // healthy one finalized
}

TEST(session_clock_pair_every_5_min) {
  SessRig rig;
  CHECK(rig.start());
  CHECK(rig.pump(0.1));
  const WallTime at = rig.clock.now();
  rig.clock.advance_ms(5 * 60 * 1000);
  // Further 100 ms steps until the writer's next flush puts the pair in the manifest.
  CHECK(poll([&] {
    rig.clock.advance_ms(100);
    return !events(rig.only_manifest(kP), "clock_pair").empty();
  }));
  rig.mgr.stop();
  const auto cp = events(rig.only_manifest(kP), "clock_pair");
  CHECK_EQ(cp.size(), size_t(1));
  if (cp.size() != 1) return;
  CHECK_EQ(num_of(cp[0], "sample"), int64_t(kRate / 10));
  CHECK(str(cp[0], "utc") >= iso_utc(WallTime{at.utc_ms + 5 * 60 * 1000, 0}));
}

TEST(session_discontinuity_counted_and_logged) {
  SessRig rig;
  CHECK(rig.start());
  rig.src.simulate_discontinuity();
  CHECK(rig.pump(0.2));
  CHECK(poll([&] { return rig.mgr.stats().discontinuities == 1; }));
  CHECK(poll([&] {
    for (const auto& e : rig.log.recent(100))
      if (e.sev == Severity::Warning && e.text.find(L"discontinuity") != std::wstring::npos) return true;
    return false;
  }));
}

TEST(session_synthetic_source_realtime_blocks) {
  struct Count : CaptureCallbacks {
    void on_frames(const float*, size_t frames, bool) override {
      total += frames;
      last = frames;
    }
    void on_device_lost() override {}
    void on_device_returned(StreamFormat) override {}
    std::atomic<size_t> total{0}, last{0};
  } cb;
  SyntheticSource src({48000, 2}, SyntheticSource::Pattern::Sine, false);
  CHECK(src.start(&cb));
  CHECK(poll([&] { return cb.total.load() >= 4800; }));  // 100 ms arrives in real time
  src.stop();
  const size_t after_stop = cb.total.load();
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK_EQ(cb.total.load(), after_stop);
  CHECK_EQ(cb.last.load(), size_t(480));  // 10 ms blocks
}
