#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/clock.h"
#include "core/sample_ring.h"
#include "dest_helpers.h"
#include "io/fault_file_system.h"
#include "session/config.h"
#include "session/destination.h"
#include "session/event_log.h"
#include "session/manifest.h"
#include "test.h"

using namespace sl;
using namespace dest_test;
using Kind = FaultFileSystem::Kind;

namespace {

// 2026-10-16 18:30:00 UTC, local = UTC-4 -> 14:30:00: file 1 may run a full minute.
WallTime fo_wall() { return WallTime{1792175400000LL, -240}; }

const std::wstring kP = L"D:\\p";
const std::wstring kS = L"E:\\s";
const std::wstring kPManifest = L"D:\\p\\gig_main.jsonl";
constexpr SampleIndex kRate = 8000;

// 8 kHz mono float WAV into D:\p then E:\s, rotation 1 min. The event log lives on C:
// so that faults injected on D: and E: never reach it.
struct FoRig {
  explicit FoRig(size_t ring_frames = size_t(1) << 20)
      : clock(fo_wall(), 1000), log(fs, L"C:\\logs\\gig.log"), ring(1, ring_frames, 0) {
    std::wstring err;
    fs.ensure_dir(L"C:\\logs", &err);
    sp.session_name = L"gig";
    sp.session_id = L"abc123";
    sp.rate = uint32_t(kRate);
    sp.input_channels = 1;
    sp.start_wall = fo_wall();
    cfg.format = OutFormat::Wav;
    cfg.wav_depth = WavDepth::Float32;
    cfg.folders = {kP, kS};
    cfg.rotation_minutes = 1;
  }
  ~FoRig() { fs.release_hangs(); }

  // Feeds [from, to) in 5 s chunks, waiting for the writer so the ring never overruns.
  bool pump(Destination& d, SampleIndex from, SampleIndex to) {
    for (SampleIndex s = from; s < to; s += 5 * kRate) {
      const SampleIndex n = std::min(5 * kRate, to - s);
      feed(ring, s, size_t(n));
      if (!wait_progress(d, s + n)) return false;
    }
    return true;
  }
  // Writes [0, 2 s) and confirms it with one flush.
  bool confirm_first_2s(Destination& d) {
    feed(ring, 0, size_t(2 * kRate));
    if (!wait_progress(d, 2 * kRate)) return false;
    clock.advance_ms(2100);
    return poll([&] { return d.status().confirmed >= 2 * kRate; });
  }
  std::vector<std::wstring> wavs(const std::wstring& dir) { return fs.list(dir, L".wav"); }
  std::vector<std::wstring> manifests(const std::wstring& dir) { return fs.list(dir, L".jsonl"); }
  std::vector<ManifestRecord> records(const std::wstring& path) { return read_manifest(fs, path); }
  std::vector<ManifestRecord> only_manifest(const std::wstring& dir) {
    const auto m = manifests(dir);
    return m.size() == 1 ? records(m[0]) : std::vector<ManifestRecord>{};
  }

  FaultFileSystem fs;
  FakeClock clock;
  EventLog log;
  SampleRing ring;
  SessionParams sp;
  DestinationConfig cfg;
};

bool on_folder(const DestStatus& s, size_t i) {
  return s.state == DestState::Writing && i < s.folders.size() && s.folders[i].health == FolderHealth::Active;
}

// The failover record that the new manifest carries as its second line.
void check_failover_record(std::vector<ManifestRecord> recs, const std::string& prev, const std::string& trigger,
                           SampleIndex confirmed, const std::wstring& old_path) {
  CHECK(recs.size() >= 2);
  if (recs.size() < 2) return;
  CHECK_EQ(recs[0]["ev"], std::string("session_start"));
  CHECK_EQ(recs[0]["prev_manifest"], prev);
  CHECK_EQ(recs[1]["ev"], std::string("failover"));
  CHECK_EQ(recs[1]["from"], narrow(kP));
  CHECK_EQ(recs[1]["to"], narrow(kS));
  CHECK_EQ(recs[1]["trigger"], trigger);
  CHECK_EQ(num(recs[1], "confirmed"), int64_t(confirmed));
  CHECK_EQ(recs[1]["old_path"], narrow(old_path));  // crash repair caps that file at confirmed
}

}  // namespace

TEST(failover_on_write_error_no_loss_no_dup) {
  FoRig rig;
  Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  d.start(0);
  CHECK(rig.confirm_first_2s(d));
  rig.fs.add_rule({L"D:\\", Kind::FailWrite, 100 * 1024});
  CHECK(rig.pump(d, 2 * kRate, 10 * kRate));
  const DestStatus st = d.status();
  CHECK(on_folder(st, 1));
  CHECK_EQ(st.folders[0].health, FolderHealth::Failed);
  CHECK(st.needs_ack);
  d.stop(10 * kRate);
  CHECK(d.status().state == DestState::Stopped);

  const auto dp = rig.wavs(kP), es = rig.wavs(kS);
  CHECK_EQ(dp.size(), size_t(1));
  CHECK_EQ(es.size(), size_t(1));
  if (dp.size() != 1 || es.size() != 1) return;
  CHECK(ends_with(dp[0], L"_001.wav"));
  CHECK(ends_with(es[0], L"_001_b.wav"));
  const auto a = wav_samples(rig.fs, dp[0]);
  const auto b = wav_samples(rig.fs, es[0]);
  CHECK_EQ(a.size(), size_t(2 * kRate));  // the D file claims exactly what was confirmed
  CHECK_EQ(a.size() + b.size(), size_t(10 * kRate));
  CHECK_EQ(ramp_errors(a, 0) + ramp_errors(b, SampleIndex(a.size())), size_t(0));

  check_failover_record(rig.only_manifest(kS), narrow(kPManifest), "write_error", 2 * kRate, dp[0]);
  // Best effort, but this folder still accepts the small manifest write.
  auto old = events(rig.records(kPManifest), "failover");
  CHECK_EQ(old.size(), size_t(1));
  if (old.size() == 1) {
    CHECK_EQ(old[0]["to"], narrow(kS));
    CHECK_EQ(old[0]["old_path"], narrow(dp[0]));
  }
}

TEST(failover_on_low_space) {
  FoRig rig;
  Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  d.start(0);
  CHECK(rig.confirm_first_2s(d));
  rig.fs.set_free_bytes(L"D:\\", 100ull << 20);  // below the 1 GB low-water mark
  feed(rig.ring, 2 * kRate, size_t(2 * kRate));
  CHECK(wait_progress(d, 4 * kRate));
  CHECK(on_folder(d.status(), 0));  // only checked every 10 s
  rig.clock.advance_ms(10000);
  CHECK(poll([&] { return on_folder(d.status(), 1); }));
  CHECK(rig.pump(d, 4 * kRate, 8 * kRate));
  d.stop(8 * kRate);

  const auto dp = rig.wavs(kP), es = rig.wavs(kS);
  CHECK_EQ(dp.size(), size_t(1));
  CHECK_EQ(es.size(), size_t(1));
  if (dp.size() != 1 || es.size() != 1) return;
  const auto a = wav_samples(rig.fs, dp[0]);
  const auto b = wav_samples(rig.fs, es[0]);
  CHECK_EQ(a.size(), size_t(4 * kRate));  // flushed right before the check
  CHECK_EQ(a.size() + b.size(), size_t(8 * kRate));
  CHECK_EQ(ramp_errors(a, 0) + ramp_errors(b, SampleIndex(a.size())), size_t(0));
  check_failover_record(rig.only_manifest(kS), narrow(kPManifest), "low_space", 4 * kRate, dp[0]);
  auto old = rig.records(kPManifest);
  auto closes = events(old, "file_close");
  CHECK_EQ(closes.size(), size_t(1));
  if (closes.size() == 1) CHECK_EQ(num(closes[0], "end"), int64_t(4 * kRate));
  CHECK_EQ(events(old, "failover").size(), size_t(1));
}

TEST(failover_on_stall_then_stop_returns_fast) {
  FoRig rig;
  Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  // A second destination on another drive must not notice D: hanging.
  DestinationConfig other = rig.cfg;
  other.name = L"copy";
  other.folders = {L"F:\\c"};
  Destination d2(other, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  d.start(0);
  d2.start(0);
  CHECK(rig.confirm_first_2s(d));
  rig.fs.add_rule({L"D:\\", Kind::HangWrite});
  feed(rig.ring, 2 * kRate, size_t(kRate));
  CHECK(wait_progress(d2, 3 * kRate));
  // Let the D: writer reach the write that hangs (the outcome is the same if it has not).
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK_EQ(d.progress(), SampleIndex(2 * kRate));
  d.declare_stalled();
  CHECK(poll([&] { return on_folder(d.status(), 1); }));
  CHECK(d.status().folders[0].health == FolderHealth::Failed);
  CHECK(d.status().needs_ack);
  CHECK(rig.pump(d, 3 * kRate, 6 * kRate));
  CHECK(wait_progress(d2, 6 * kRate));

  const auto t0 = std::chrono::steady_clock::now();
  d.stop(6 * kRate);
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  CHECK(ms < 3000);
  CHECK(d.status().state == DestState::Stopped);
  d2.stop(6 * kRate);
  rig.fs.release_hangs();

  const auto dp = rig.wavs(kP), es = rig.wavs(kS);
  CHECK_EQ(dp.size(), size_t(1));
  CHECK_EQ(es.size(), size_t(1));
  if (dp.size() != 1 || es.size() != 1) return;
  CHECK(ends_with(es[0], L"_001_b.wav"));
  const auto a = wav_samples(rig.fs, dp[0]);  // never finalized: the last flushed header
  const auto b = wav_samples(rig.fs, es[0]);
  CHECK_EQ(a.size(), size_t(2 * kRate));
  CHECK_EQ(a.size() + b.size(), size_t(6 * kRate));
  CHECK_EQ(ramp_errors(a, 0) + ramp_errors(b, SampleIndex(a.size())), size_t(0));
  check_failover_record(rig.only_manifest(kS), narrow(kPManifest), "stall", 2 * kRate, dp[0]);
  const auto c = rig.wavs(L"F:\\c");
  CHECK_EQ(c.size(), size_t(1));
  if (c.size() == 1) CHECK_EQ(wav_samples(rig.fs, c[0]).size(), size_t(6 * kRate));
}

TEST(failover_drive_vanishes) {
  FoRig rig;
  Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  d.start(0);
  CHECK(rig.confirm_first_2s(d));
  rig.fs.add_rule({L"D:\\", Kind::FailOpen});
  rig.fs.add_rule({L"D:\\", Kind::FailWrite});
  CHECK(rig.pump(d, 2 * kRate, 6 * kRate));
  CHECK(on_folder(d.status(), 1));
  d.stop(6 * kRate);
  rig.fs.clear_rules();  // the drive comes back so the test can look at it

  const auto dp = rig.wavs(kP), es = rig.wavs(kS);
  CHECK_EQ(dp.size(), size_t(1));
  CHECK_EQ(es.size(), size_t(1));
  if (dp.size() != 1 || es.size() != 1) return;
  const auto a = wav_samples(rig.fs, dp[0]);
  const auto b = wav_samples(rig.fs, es[0]);
  CHECK_EQ(a.size(), size_t(2 * kRate));
  CHECK_EQ(a.size() + b.size(), size_t(6 * kRate));
  CHECK_EQ(ramp_errors(a, 0) + ramp_errors(b, SampleIndex(a.size())), size_t(0));
  check_failover_record(rig.only_manifest(kS), narrow(kPManifest), "write_error", 2 * kRate, dp[0]);
}

TEST(last_resort_then_append_folder) {
  FoRig rig;
  Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  d.start(0);
  CHECK(rig.confirm_first_2s(d));
  rig.fs.add_rule({L"D:\\", Kind::FailOpen});
  rig.fs.add_rule({L"E:\\", Kind::FailOpen});
  rig.fs.add_rule({L"D:\\", Kind::FailWrite});
  feed(rig.ring, 2 * kRate, size_t(kRate));
  CHECK(poll([&] { return d.status().state == DestState::LastResort; }));
  CHECK(d.status().needs_ack);
  CHECK(d.status().gaps == 1);
  CHECK(d.status().current_file.empty());
  // The capture goes on; the cursor follows it so the watchdog stays quiet.
  feed(rig.ring, 3 * kRate, size_t(kRate));
  CHECK(wait_progress(d, 4 * kRate));

  d.post(CmdAppendFolder{L"F:\\new"});
  CHECK(poll([&] { return d.status().state == DestState::Writing; }));
  CHECK(rig.pump(d, 4 * kRate, 6 * kRate));
  d.post(CmdAcknowledge{});
  CHECK(poll([&] { return !d.status().needs_ack; }));
  d.stop(6 * kRate);
  CHECK(d.status().gaps == 1);

  const auto f = rig.wavs(L"F:\\new");
  CHECK_EQ(f.size(), size_t(1));
  if (f.size() == 1) {
    const auto v = wav_samples(rig.fs, f[0]);
    CHECK_EQ(v.size(), size_t(2 * kRate));  // [4 s, 6 s): from the live position on
    CHECK_EQ(ramp_errors(v, 4 * kRate), size_t(0));
  }
  auto recs = rig.only_manifest(L"F:\\new");
  CHECK(!recs.empty());
  if (recs.empty()) return;
  CHECK_EQ(recs[0]["ev"], std::string("session_start"));
  CHECK_EQ(recs[0]["prev_manifest"], narrow(kPManifest));
  auto starts = events(recs, "gap_start");
  auto ends = events(recs, "gap_end");
  CHECK_EQ(starts.size(), size_t(1));
  CHECK_EQ(ends.size(), size_t(1));
  if (starts.size() == 1 && ends.size() == 1) {
    CHECK_EQ(starts[0]["reason"], std::string("last_resort"));
    CHECK_EQ(num(starts[0], "sample"), int64_t(2 * kRate));  // nothing after confirmed is on disk
    CHECK_EQ(ends[0]["reason"], std::string("last_resort"));
    CHECK_EQ(num(ends[0], "sample"), int64_t(4 * kRate));
  }
  auto fo = events(recs, "failover");
  CHECK_EQ(fo.size(), size_t(1));
  if (fo.size() == 1) {
    CHECK_EQ(fo[0]["to"], std::string());
    CHECK(fo[0]["old_path"].rfind("D:\\p\\", 0) == 0);
  }
}

TEST(return_to_primary_at_rotation) {
  FoRig rig;
  Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  d.start(0);
  CHECK(rig.confirm_first_2s(d));
  rig.fs.add_rule({L"D:\\", Kind::FailWrite});
  feed(rig.ring, 2 * kRate, size_t(kRate));
  CHECK(poll([&] { return on_folder(d.status(), 1); }));
  rig.fs.clear_rules();  // D: recovers, but no flapping before the boundary at 14:31:00
  CHECK(rig.pump(d, 3 * kRate, 50 * kRate));
  CHECK(on_folder(d.status(), 1));
  CHECK(rig.pump(d, 50 * kRate, 70 * kRate));
  const DestStatus st = d.status();
  CHECK(on_folder(st, 0));
  CHECK(st.current_file.rfind(L"D:\\p\\", 0) == 0);
  CHECK(ends_with(st.current_file, L"_002.wav"));
  d.stop(70 * kRate);

  const auto dp = rig.wavs(kP), es = rig.wavs(kS);
  CHECK_EQ(dp.size(), size_t(2));
  CHECK_EQ(es.size(), size_t(1));
  if (dp.size() != 2 || es.size() != 1) return;
  const auto a = wav_samples(rig.fs, dp[0]);
  const auto b = wav_samples(rig.fs, es[0]);
  const auto c = wav_samples(rig.fs, dp[1]);
  CHECK_EQ(a.size(), size_t(2 * kRate));
  CHECK_EQ(b.size(), size_t(58 * kRate));
  CHECK_EQ(c.size(), size_t(10 * kRate));
  CHECK_EQ(ramp_errors(a, 0) + ramp_errors(b, 2 * kRate) + ramp_errors(c, 60 * kRate), size_t(0));

  // The manifest follows the destination back to D:\p.
  const auto ms = rig.manifests(kP);
  CHECK_EQ(ms.size(), size_t(2));
  const auto es_man = rig.manifests(kS);
  if (ms.size() != 2 || es_man.size() != 1) return;
  auto recs = rig.records(ms[1]);
  CHECK(!recs.empty());
  if (recs.empty()) return;
  CHECK_EQ(recs[0]["ev"], std::string("session_start"));
  CHECK_EQ(recs[0]["prev_manifest"], narrow(es_man[0]));
  CHECK_EQ(num(recs[0], "sample"), int64_t(60 * kRate));
  auto opens = events(recs, "file_open");
  CHECK_EQ(opens.size(), size_t(1));
  if (opens.size() == 1) CHECK_EQ(num(opens[0], "seg"), int64_t(0));
}

TEST(overrun_records_gap) {
  FoRig rig{size_t(kRate)};  // 1 s ring (8192 frames)
  rig.fs.add_rule({L"D:\\", Kind::SlowWrite, 0, 500});
  Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  d.start(0);
  feed(rig.ring, 0, 4000);
  CHECK(wait_progress(d, 4000));
  feed(rig.ring, 4000, size_t(3 * kRate));  // far more than the ring holds
  CHECK(poll([&] { return d.status().gaps >= 1; }));
  rig.fs.clear_rules();
  CHECK(wait_progress(d, 4000 + 3 * kRate));
  d.stop(4000 + 3 * kRate);
  // Normally one gap; a read torn by the feed itself can add a second one.
  const size_t gaps = size_t(d.status().gaps);
  CHECK(gaps >= 1);

  auto recs = rig.records(kPManifest);
  auto starts = events(recs, "gap_start");
  auto ends = events(recs, "gap_end");
  CHECK_EQ(starts.size(), gaps);
  CHECK_EQ(ends.size(), gaps);
  if (gaps >= 1 && starts.size() == gaps && ends.size() == gaps) {
    for (size_t i = 0; i < gaps; ++i) {
      CHECK_EQ(starts[i]["reason"], std::string("overrun"));
      CHECK_EQ(ends[i]["reason"], std::string("overrun"));
      CHECK(num(ends[i], "sample") > num(starts[i], "sample"));
    }
    CHECK(num(ends.back(), "sample") >= 4000 + 3 * kRate - int64_t(rig.ring.capacity()));
  }
  // Every file holds exactly the ramp of the range its manifest record names.
  auto closes = events(recs, "file_close");
  CHECK_EQ(closes.size(), gaps + 1);
  for (auto& c : closes) {
    std::wstring path(c["path"].begin(), c["path"].end());
    const auto v = wav_samples(rig.fs, path);
    CHECK_EQ(int64_t(v.size()), num(c, "end") - num(c, "start"));
    CHECK_EQ(ramp_errors(v, num(c, "start")), size_t(0));
  }
}

TEST(is_stalled_threshold) {
  CHECK(!is_stalled(1000, 500, 1024, 50));
  CHECK(is_stalled(1000, 400, 1024, 50));
}

TEST(last_resort_retries_every_30s) {
  FoRig rig;
  Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  d.start(0);
  CHECK(rig.confirm_first_2s(d));
  rig.fs.add_rule({L"D:\\", Kind::FailOpen});
  rig.fs.add_rule({L"E:\\", Kind::FailOpen});
  rig.fs.add_rule({L"D:\\", Kind::FailWrite});
  feed(rig.ring, 2 * kRate, size_t(kRate));
  CHECK(poll([&] { return d.status().state == DestState::LastResort; }));
  CHECK(wait_progress(d, 3 * kRate));
  rig.fs.clear_rules();  // D: is back, but nobody looks before the next retry
  rig.clock.advance_ms(29000);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(d.status().state == DestState::LastResort);
  rig.clock.advance_ms(1100);
  CHECK(poll([&] { return on_folder(d.status(), 0); }));
  CHECK(rig.pump(d, 3 * kRate, 5 * kRate));
  d.stop(5 * kRate);

  const auto dp = rig.wavs(kP);
  CHECK_EQ(dp.size(), size_t(2));
  if (dp.size() != 2) return;
  CHECK(ends_with(dp[1], L"_002.wav"));  // a new file (n + 1) at the live position
  const auto v = wav_samples(rig.fs, dp[1]);
  CHECK_EQ(v.size(), size_t(2 * kRate));
  CHECK_EQ(ramp_errors(v, 3 * kRate), size_t(0));
  CHECK(d.status().gaps == 1);
}

TEST(failover_opus_smoke) {
  FoRig rig;
  rig.cfg.format = OutFormat::Opus;
  Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  d.start(0);
  feed(rig.ring, 0, size_t(2 * kRate));
  CHECK(wait_progress(d, 2 * kRate));
  rig.clock.advance_ms(2100);  // one flush; Opus confirms only the pages written so far
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  rig.fs.add_rule({L"D:\\", Kind::FailWrite});
  CHECK(rig.pump(d, 2 * kRate, 8 * kRate));
  CHECK(on_folder(d.status(), 1));
  d.stop(8 * kRate);

  const auto dp = rig.fs.list(kP, L".opus"), es = rig.fs.list(kS, L".opus");
  CHECK_EQ(dp.size(), size_t(1));
  CHECK_EQ(es.size(), size_t(1));
  if (dp.size() != 1 || es.size() != 1) return;
  CHECK(ends_with(es[0], L"_001_b.opus"));
  for (const auto& f : {dp[0], es[0]}) {
    const auto c = rig.fs.contents(f);
    CHECK(c.size() > 4 && std::memcmp(c.data(), "OggS", 4) == 0);
  }
  auto recs = rig.only_manifest(kS);
  CHECK(recs.size() >= 3);
  if (recs.size() < 3) return;
  CHECK_EQ(recs[1]["ev"], std::string("failover"));
  CHECK_EQ(recs[1]["trigger"], std::string("write_error"));
  const int64_t confirmed = num(recs[1], "confirmed");
  CHECK(confirmed >= 0 && confirmed <= 2 * kRate);  // Opus confirms whole pages only
  CHECK_EQ(recs[2]["ev"], std::string("file_open"));
  CHECK_EQ(num(recs[2], "sample"), confirmed);  // never a hole at the seam
}

// stop() with a writer hung in a write returns after its 3 s budget. The abandoned writer
// wakes up only after its Destination is gone and must touch nothing (Debug CRT heap
// checks would catch a use after free).
TEST(failover_stop_abandons_hung_writer) {
  FoRig rig;
  rig.cfg.folders = {kP};
  {
    Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
    d.start(0);
    CHECK(rig.confirm_first_2s(d));
    rig.fs.add_rule({L"D:\\", Kind::HangWrite});
    feed(rig.ring, 2 * kRate, size_t(kRate));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));  // let it reach the write
    const auto t0 = std::chrono::steady_clock::now();
    d.stop(3 * kRate);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms >= 2900 && ms < 3500);
    CHECK(d.status().state == DestState::Stopped);
  }
  rig.fs.release_hangs();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto dp = rig.wavs(kP);
  CHECK_EQ(dp.size(), size_t(1));
  if (dp.size() == 1) CHECK_EQ(wav_samples(rig.fs, dp[0]).size(), size_t(2 * kRate));
}

// The flush before a close fails: the unconfirmed tail is written again in the next
// folder, and the close happens there.
TEST(failover_at_gap_close_keeps_samples) {
  FoRig rig;
  rig.cfg.rotation_minutes = 0;
  Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  d.start(0);
  CHECK(rig.confirm_first_2s(d));
  feed(rig.ring, 2 * kRate, size_t(3 * kRate));
  CHECK(wait_progress(d, 5 * kRate));
  rig.fs.add_rule({L"D:\\", Kind::FailFlush});
  d.post(CmdCloseForGap{5 * kRate});
  CHECK(poll([&] { return d.status().state == DestState::GapClosed; }));
  CHECK_EQ(d.progress(), SampleIndex(5 * kRate));
  d.stop(5 * kRate);

  const auto dp = rig.wavs(kP), es = rig.wavs(kS);
  CHECK_EQ(dp.size(), size_t(1));
  CHECK_EQ(es.size(), size_t(1));
  if (dp.size() != 1 || es.size() != 1) return;
  const auto a = wav_samples(rig.fs, dp[0]);
  const auto b = wav_samples(rig.fs, es[0]);
  CHECK_EQ(a.size(), size_t(2 * kRate));
  CHECK_EQ(b.size(), size_t(3 * kRate));
  CHECK_EQ(ramp_errors(a, 0) + ramp_errors(b, 2 * kRate), size_t(0));
  check_failover_record(rig.only_manifest(kS), narrow(kPManifest), "flush_error", 2 * kRate, dp[0]);
}

TEST(failover_at_stop_keeps_samples) {
  FoRig rig;
  Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  d.start(0);
  CHECK(rig.confirm_first_2s(d));
  feed(rig.ring, 2 * kRate, size_t(3 * kRate));
  CHECK(wait_progress(d, 5 * kRate));
  rig.fs.add_rule({L"D:\\", Kind::FailFlush});
  d.stop(5 * kRate);
  CHECK(d.status().state == DestState::Stopped);

  const auto dp = rig.wavs(kP), es = rig.wavs(kS);
  CHECK_EQ(dp.size(), size_t(1));
  CHECK_EQ(es.size(), size_t(1));
  if (dp.size() != 1 || es.size() != 1) return;
  const auto a = wav_samples(rig.fs, dp[0]);
  const auto b = wav_samples(rig.fs, es[0]);
  CHECK_EQ(a.size() + b.size(), size_t(5 * kRate));
  CHECK_EQ(ramp_errors(a, 0) + ramp_errors(b, SampleIndex(a.size())), size_t(0));
  auto recs = rig.only_manifest(kS);
  CHECK(!recs.empty());
  if (!recs.empty()) {
    CHECK_EQ(recs.back()["ev"], std::string("session_end"));
    CHECK_EQ(num(recs.back(), "sample"), int64_t(5 * kRate));
  }
}

namespace {
size_t stall_log_entries(EventLog& log) {
  size_t n = 0;
  for (const auto& e : log.recent(1000)) n += e.text.find(L"stalled") != std::wstring::npos;
  return n;
}
}  // namespace

// The watchdog keeps firing while the writer that took over catches up from half a ring
// behind: only the first call fails over; the healthy folder is not quarantined.
TEST(failover_stall_declared_twice_fails_over_once) {
  FoRig rig;
  Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  d.start(0);
  CHECK(rig.confirm_first_2s(d));
  rig.fs.add_rule({L"D:\\", Kind::HangWrite});
  rig.fs.add_rule({L"E:\\", Kind::SlowWrite, 0, 300});  // catching up takes a while
  feed(rig.ring, 2 * kRate, size_t(kRate));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK_EQ(d.progress(), SampleIndex(2 * kRate));
  d.declare_stalled();
  d.declare_stalled();  // at once: the new writer is still at the confirmed sample
  CHECK(poll([&] { return on_folder(d.status(), 1); }));
  d.declare_stalled();  // writing on E:, but not yet at the takeover's capture position
  CHECK(d.progress() < 3 * kRate);
  CHECK(wait_progress(d, 3 * kRate));
  rig.fs.clear_rules();
  CHECK(rig.pump(d, 3 * kRate, 5 * kRate));
  CHECK(on_folder(d.status(), 1));
  d.stop(5 * kRate);
  rig.fs.release_hangs();

  CHECK_EQ(stall_log_entries(rig.log), size_t(1));
  const auto es = rig.wavs(kS);
  CHECK_EQ(es.size(), size_t(1));
  if (es.size() == 1) {
    const auto b = wav_samples(rig.fs, es[0]);
    CHECK_EQ(b.size(), size_t(3 * kRate));
    CHECK_EQ(ramp_errors(b, 2 * kRate), size_t(0));
  }
  CHECK_EQ(events(rig.only_manifest(kS), "failover").size(), size_t(1));
}

// LastResort and GapClosed idle on purpose: a watchdog call there must not cost a folder.
TEST(failover_stall_ignored_when_idle) {
  {
    FoRig rig;
    Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
    d.start(0);
    CHECK(rig.confirm_first_2s(d));
    rig.fs.add_rule({L"D:\\", Kind::FailOpen});
    rig.fs.add_rule({L"E:\\", Kind::FailOpen});
    rig.fs.add_rule({L"D:\\", Kind::FailWrite});
    feed(rig.ring, 2 * kRate, size_t(kRate));
    CHECK(poll([&] { return d.status().state == DestState::LastResort; }));
    d.declare_stalled();  // the last folder probed was E:
    CHECK(d.status().state == DestState::LastResort);
    rig.fs.clear_rules();
    rig.fs.add_rule({L"D:\\", Kind::FailOpen});  // only E: comes back
    rig.clock.advance_ms(30100);
    CHECK(poll([&] { return on_folder(d.status(), 1); }));  // E: was not quarantined
    d.stop(d.progress());
    CHECK_EQ(stall_log_entries(rig.log), size_t(0));
  }
  {
    FoRig rig;
    rig.cfg.rotation_minutes = 0;
    Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
    d.start(0);
    d.post(CmdCloseForGap{2 * kRate});
    feed(rig.ring, 0, size_t(2 * kRate));
    CHECK(poll([&] { return d.status().state == DestState::GapClosed; }));
    d.declare_stalled();
    CHECK(d.status().state == DestState::GapClosed);
    rig.ring.jump_to(4 * kRate);
    d.post(CmdResumeAt{4 * kRate});
    CHECK(poll([&] { return on_folder(d.status(), 0); }));  // D:\p, not quarantined
    CHECK(rig.pump(d, 4 * kRate, 5 * kRate));
    d.stop(5 * kRate);
    CHECK_EQ(stall_log_entries(rig.log), size_t(0));
    CHECK_EQ(rig.wavs(kP).size(), size_t(2));
  }
}

// R13 (revised): the grace after a takeover also ends at a deadline, early enough that a
// rescue after a second hang starts on data still in the ring. 30 s ring (262144 frames,
// 32.8 s) at 50 %: headroom 16.4 s, deadline 5 s. A deadline of the full threshold
// (16.4 s) would have let the capture overwrite the confirmed sample.
TEST(failover_stall_grace_deadline_rescues_hung_takeover) {
  const std::wstring kF = L"F:\\f";
  FoRig rig{size_t(30 * kRate)};
  rig.cfg.folders = {kP, kS, kF};
  Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  d.start(0);
  CHECK(rig.confirm_first_2s(d));
  rig.fs.add_rule({L"D:\\", Kind::HangWrite});
  rig.fs.add_rule({L"E:\\", Kind::HangWrite});
  // The watchdog fires once the lag passes half the ring: confirmed is 16.5 s behind.
  SampleIndex w = 2 * kRate;
  feed(rig.ring, w, size_t(33 * kRate / 2));
  w += 33 * kRate / 2;
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK_EQ(d.progress(), SampleIndex(2 * kRate));
  CHECK(is_stalled(rig.ring.write_index(), d.progress(), rig.ring.capacity(), rig.sp.stall_percent));
  d.declare_stalled();  // takes over towards E:, which hangs too (in its probe)
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  d.declare_stalled();  // within the grace: ignored
  // The capture goes on during the grace; the watchdog keeps firing.
  for (int s = 0; s < 4; ++s) {
    feed(rig.ring, w, size_t(kRate));
    w += kRate;
    rig.clock.advance_ms(1000);
    d.declare_stalled();  // at most 4 s after the takeover: ignored
  }
  CHECK_EQ(stall_log_entries(rig.log), size_t(1));
  feed(rig.ring, w, size_t(kRate));
  w += kRate;
  rig.clock.advance_ms(1000);
  d.declare_stalled();  // 5 s: the deadline has passed, honored
  CHECK(poll([&] { return on_folder(d.status(), 2); }));
  CHECK_EQ(stall_log_entries(rig.log), size_t(2));
  const DestStatus st = d.status();
  CHECK(st.folders[0].health == FolderHealth::Failed);
  CHECK(st.folders[1].health == FolderHealth::Failed);
  CHECK(rig.pump(d, w, w + 5 * kRate));
  w += 5 * kRate;
  d.stop(w);
  CHECK(d.status().gaps == 0);  // the rescue started on data still in the ring
  rig.fs.release_hangs();

  const auto dp = rig.wavs(kP), ff = rig.wavs(kF);
  CHECK_EQ(rig.wavs(kS).size(), size_t(0));  // E: never got further than its probe
  CHECK_EQ(dp.size(), size_t(1));
  CHECK_EQ(ff.size(), size_t(1));
  if (dp.size() != 1 || ff.size() != 1) return;
  const auto a = wav_samples(rig.fs, dp[0]);
  const auto c = wav_samples(rig.fs, ff[0]);
  CHECK_EQ(a.size(), size_t(2 * kRate));
  CHECK_EQ(SampleIndex(a.size() + c.size()), w);
  CHECK_EQ(ramp_errors(a, 0) + ramp_errors(c, 2 * kRate), size_t(0));
  auto recs = rig.only_manifest(kF);
  CHECK(recs.size() >= 2);
  if (recs.size() >= 2) {
    CHECK_EQ(recs[1]["ev"], std::string("failover"));
    CHECK_EQ(recs[1]["from"], narrow(kS));
    CHECK_EQ(recs[1]["trigger"], std::string("stall"));
    CHECK_EQ(num(recs[1], "confirmed"), int64_t(2 * kRate));
    CHECK_EQ(recs[1]["old_path"], narrow(dp[0]));  // still the file that holds the data
  }
  CHECK(events(recs, "gap_start").empty());
}

// A drive that hangs between preflight and start: the writer is stuck in its first open
// (state Idle). That is a stall, and recording starts in the next folder.
TEST(failover_stall_in_first_open) {
  FoRig rig;
  rig.fs.add_rule({L"D:\\", Kind::HangWrite});
  Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  d.start(0);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));  // stuck in D:'s probe
  CHECK(d.status().state == DestState::Idle);
  d.declare_stalled();  // stuck, but not for 2 s yet (R14): ignored
  CHECK_EQ(stall_log_entries(rig.log), size_t(0));
  rig.clock.advance_ms(2100);
  d.declare_stalled();
  CHECK(poll([&] { return on_folder(d.status(), 1); }));
  CHECK(rig.pump(d, 0, 3 * kRate));
  d.stop(3 * kRate);
  rig.fs.release_hangs();

  CHECK_EQ(stall_log_entries(rig.log), size_t(1));
  CHECK(d.status().folders[0].health == FolderHealth::Failed);
  const auto es = rig.wavs(kS);
  CHECK_EQ(es.size(), size_t(1));
  if (es.size() != 1) return;
  CHECK(ends_with(es[0], L"_001.wav"));  // the session's first file
  const auto v = wav_samples(rig.fs, es[0]);
  CHECK_EQ(v.size(), size_t(3 * kRate));
  CHECK_EQ(ramp_errors(v, 0), size_t(0));
}

// LastResort: the 30 s retry hangs probing D:. That is a stall: the next generation follows
// the capture again (still in LastResort) and a folder appended later recovers.
TEST(failover_stall_in_last_resort_retry) {
  FoRig rig;
  Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  d.start(0);
  CHECK(rig.confirm_first_2s(d));
  rig.fs.add_rule({L"D:\\", Kind::FailOpen});
  rig.fs.add_rule({L"E:\\", Kind::FailOpen});
  rig.fs.add_rule({L"D:\\", Kind::FailWrite});
  feed(rig.ring, 2 * kRate, size_t(kRate));
  CHECK(poll([&] { return d.status().state == DestState::LastResort; }));
  CHECK(wait_progress(d, 3 * kRate));
  rig.fs.clear_rules();
  rig.fs.add_rule({L"D:\\", Kind::HangWrite});  // the retry's probe of D: hangs
  rig.fs.add_rule({L"E:\\", Kind::FailOpen});
  rig.clock.advance_ms(30100);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  feed(rig.ring, 3 * kRate, size_t(kRate));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK_EQ(d.progress(), SampleIndex(3 * kRate));  // stuck: no longer following the capture
  d.declare_stalled();  // stuck, but not for 2 s yet (R14): ignored
  CHECK_EQ(stall_log_entries(rig.log), size_t(0));
  rig.clock.advance_ms(2100);
  d.declare_stalled();
  CHECK(wait_progress(d, 4 * kRate));  // following again
  CHECK(d.status().state == DestState::LastResort);
  d.post(CmdAppendFolder{L"F:\\new"});
  CHECK(poll([&] { return d.status().state == DestState::Writing; }));
  CHECK(rig.pump(d, 4 * kRate, 6 * kRate));
  d.stop(6 * kRate);
  rig.fs.release_hangs();

  CHECK_EQ(stall_log_entries(rig.log), size_t(1));
  const auto f = rig.wavs(L"F:\\new");
  CHECK_EQ(f.size(), size_t(1));
  if (f.size() == 1) {
    const auto v = wav_samples(rig.fs, f[0]);
    CHECK_EQ(v.size(), size_t(2 * kRate));
    CHECK_EQ(ramp_errors(v, 4 * kRate), size_t(0));
  }
}

// GapClosed: the reopen for CmdResumeAt hangs probing D:. That is a stall; the command is
// handled again by the next generation, in E:.
TEST(failover_stall_in_resume_reopen) {
  FoRig rig;
  rig.cfg.rotation_minutes = 0;
  Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  d.start(0);
  d.post(CmdCloseForGap{2 * kRate});
  feed(rig.ring, 0, size_t(2 * kRate));
  CHECK(poll([&] { return d.status().state == DestState::GapClosed; }));
  rig.fs.add_rule({L"D:\\", Kind::HangWrite});
  rig.ring.jump_to(4 * kRate);
  d.post(CmdResumeAt{4 * kRate});
  std::this_thread::sleep_for(std::chrono::milliseconds(200));  // stuck in D:'s probe
  CHECK(d.status().state == DestState::GapClosed);
  d.declare_stalled();  // stuck, but not for 2 s yet (R14): ignored
  CHECK_EQ(stall_log_entries(rig.log), size_t(0));
  rig.clock.advance_ms(2100);
  d.declare_stalled();
  CHECK(poll([&] { return on_folder(d.status(), 1); }));
  CHECK(rig.pump(d, 4 * kRate, 5 * kRate));
  d.stop(5 * kRate);
  rig.fs.release_hangs();

  CHECK_EQ(stall_log_entries(rig.log), size_t(1));
  const auto dp = rig.wavs(kP), es = rig.wavs(kS);
  CHECK_EQ(dp.size(), size_t(1));
  CHECK_EQ(es.size(), size_t(1));
  if (dp.size() != 1 || es.size() != 1) return;
  CHECK_EQ(ramp_errors(wav_samples(rig.fs, dp[0]), 0), size_t(0));
  const auto v = wav_samples(rig.fs, es[0]);
  CHECK_EQ(v.size(), size_t(kRate));
  CHECK_EQ(ramp_errors(v, 4 * kRate), size_t(0));
}

namespace {
// Forwards to the FaultFileSystem, but open_existing of one path blocks until release()
// (FaultFileSystem cannot hang an open). Must outlive every writer that may be blocked in it.
class HangOpenFs : public IFileSystem {
 public:
  HangOpenFs(FaultFileSystem& fs, std::wstring path) : fs_(fs), path_(std::move(path)) {}
  ~HangOpenFs() override { release(); }
  void release() {
    {
      std::lock_guard<std::mutex> lk(m_);
      released_ = true;
    }
    cv_.notify_all();
  }
  int hung() const {
    std::lock_guard<std::mutex> lk(m_);
    return hung_;
  }

  std::unique_ptr<IFile> open_existing(const std::wstring& p, std::wstring* err) override {
    if (p == path_) {
      std::unique_lock<std::mutex> lk(m_);
      ++hung_;
      cv_.wait(lk, [&] { return released_; });
    }
    return fs_.open_existing(p, err);
  }
  std::unique_ptr<IFile> create_new(const std::wstring& p, std::wstring* err) override {
    return fs_.create_new(p, err);
  }
  bool exists(const std::wstring& p) override { return fs_.exists(p); }
  bool ensure_dir(const std::wstring& d, std::wstring* err) override { return fs_.ensure_dir(d, err); }
  bool remove(const std::wstring& p) override { return fs_.remove(p); }
  std::vector<std::wstring> list(const std::wstring& d, const std::wstring& s) override { return fs_.list(d, s); }
  std::optional<uint64_t> free_bytes(const std::wstring& d) override { return fs_.free_bytes(d); }
  std::wstring volume_id(const std::wstring& d) override { return fs_.volume_id(d); }

 private:
  FaultFileSystem& fs_;
  const std::wstring path_;
  mutable std::mutex m_;
  std::condition_variable cv_;
  bool released_ = false;
  int hung_ = 0;
};
}  // namespace

// Crash resume: reopening the seed manifest on D: hangs. One declare: the next generation
// does not try that manifest again (it would hang the same way), starts a new one in E:
// that names the seed as prev_manifest, and records from the session start.
TEST(failover_stall_in_seed_manifest_reopen) {
  FoRig rig;
  std::wstring err;
  CHECK(rig.fs.ensure_dir(kP, &err));
  {
    auto f = rig.fs.create_new(kPManifest, &err);
    CHECK(f != nullptr);
    if (!f) return;
    const std::string line = "{\"ev\":\"session_end\",\"sample\":0}\n";
    CHECK(f->write(line.data(), line.size()));
    f->close();
  }
  const auto before = rig.fs.contents(kPManifest);
  HangOpenFs fs(rig.fs, kPManifest);
  {
    DestResumeSeed seed;
    seed.first_n = 5;
    seed.prev_manifest = kPManifest;
    Destination d(rig.cfg, rig.sp, rig.ring, fs, rig.clock, rig.log, seed);
    d.start(0);
    CHECK(poll([&] { return fs.hung() == 1; }));
    rig.clock.advance_ms(2100);
    d.declare_stalled();
    CHECK(poll([&] { return on_folder(d.status(), 1); }));
    CHECK(rig.pump(d, 0, 3 * kRate));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK_EQ(fs.hung(), 1);  // nobody reopened the seed
    d.stop(3 * kRate);
  }
  fs.release();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));  // the stuck writer leaves

  CHECK_EQ(stall_log_entries(rig.log), size_t(1));
  CHECK(rig.fs.contents(kPManifest) == before);
  const auto es = rig.wavs(kS);
  CHECK_EQ(es.size(), size_t(1));
  if (es.size() == 1) {
    CHECK(ends_with(es[0], L"_005.wav"));
    const auto v = wav_samples(rig.fs, es[0]);
    CHECK_EQ(v.size(), size_t(3 * kRate));
    CHECK_EQ(ramp_errors(v, 0), size_t(0));
  }
  auto recs = rig.only_manifest(kS);
  CHECK(!recs.empty());
  if (!recs.empty()) {
    CHECK_EQ(recs[0]["ev"], std::string("session_start"));
    CHECK_EQ(recs[0]["prev_manifest"], narrow(kPManifest));
  }
}

// R14: in GapClosed the cursor is parked (is_stalled holds on every watchdog tick); a tick
// landing inside a routine manifest flush is not a stall.
TEST(failover_stall_routine_flush_in_gap_is_not_a_stall) {
  FoRig rig;
  rig.cfg.rotation_minutes = 0;
  Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  d.start(0);
  d.post(CmdCloseForGap{2 * kRate});
  feed(rig.ring, 0, size_t(2 * kRate));
  CHECK(poll([&] { return d.status().state == DestState::GapClosed; }));
  rig.clock.advance_ms(2100);  // first flush of the manifest (no rule yet)
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  rig.fs.add_rule({kPManifest, Kind::SlowWrite, 0, 100});
  d.post(CmdMarker{2 * kRate, L"in the gap"});
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  rig.clock.advance_ms(2100);  // the next flush writes the marker: 100 ms on the device
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
  while (std::chrono::steady_clock::now() < until) {
    d.declare_stalled();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  CHECK_EQ(stall_log_entries(rig.log), size_t(0));
  CHECK(d.status().state == DestState::GapClosed);
  rig.fs.clear_rules();
  rig.ring.jump_to(4 * kRate);
  d.post(CmdResumeAt{4 * kRate});
  CHECK(poll([&] { return on_folder(d.status(), 0); }));  // D: was not quarantined
  CHECK(rig.pump(d, 4 * kRate, 5 * kRate));
  d.stop(5 * kRate);
  CHECK_EQ(events(rig.records(kPManifest), "marker").size(), size_t(1));
}

// R14: a manifest flush in GapClosed that hangs for 2 s is a stall.
TEST(failover_stall_hung_flush_in_gap) {
  FoRig rig;
  rig.cfg.rotation_minutes = 0;
  Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  d.start(0);
  d.post(CmdCloseForGap{2 * kRate});
  feed(rig.ring, 0, size_t(2 * kRate));
  CHECK(poll([&] { return d.status().state == DestState::GapClosed; }));
  rig.fs.add_rule({kPManifest, Kind::HangWrite});
  d.post(CmdMarker{2 * kRate, L"in the gap"});
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  rig.clock.advance_ms(2100);  // the flush starts and hangs
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  d.declare_stalled();  // not 2 s yet: ignored
  CHECK_EQ(stall_log_entries(rig.log), size_t(0));
  rig.clock.advance_ms(2100);
  d.declare_stalled();  // stuck for 2.1 s: honored
  CHECK_EQ(stall_log_entries(rig.log), size_t(1));
  CHECK(d.status().state == DestState::GapClosed);
  rig.ring.jump_to(4 * kRate);
  d.post(CmdResumeAt{4 * kRate});
  CHECK(poll([&] { return on_folder(d.status(), 1); }));  // D: is quarantined
  CHECK(rig.pump(d, 4 * kRate, 5 * kRate));
  d.stop(5 * kRate);
  rig.fs.release_hangs();

  const auto es = rig.wavs(kS);
  CHECK_EQ(es.size(), size_t(1));
  if (es.size() == 1) {
    const auto v = wav_samples(rig.fs, es[0]);
    CHECK_EQ(v.size(), size_t(kRate));
    CHECK_EQ(ramp_errors(v, 4 * kRate), size_t(0));
  }
}

// The writer published its state (carry.started) when D: failed its probe, then hung in
// E:'s probe before any file was open. The takeover opens the session's first file, seg 0.
TEST(failover_stall_before_first_file) {
  const std::wstring kF = L"F:\\f";
  FoRig rig;
  rig.cfg.folders = {kP, kS, kF};
  rig.fs.add_rule({L"D:\\", Kind::FailOpen});
  rig.fs.add_rule({L"E:\\", Kind::HangWrite});
  Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  d.start(0);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));  // stuck in E:'s probe
  CHECK(d.status().state == DestState::Idle);
  CHECK(d.status().folders[0].health == FolderHealth::Failed);
  rig.clock.advance_ms(2100);
  d.declare_stalled();
  CHECK(poll([&] { return on_folder(d.status(), 2); }));
  CHECK(rig.pump(d, 0, 3 * kRate));
  d.stop(3 * kRate);
  rig.fs.release_hangs();

  const auto ff = rig.wavs(kF);
  CHECK_EQ(ff.size(), size_t(1));
  if (ff.size() == 1) {
    CHECK(ends_with(ff[0], L"_001.wav"));  // not _001_b: no file was ever open
    const auto v = wav_samples(rig.fs, ff[0]);
    CHECK_EQ(v.size(), size_t(3 * kRate));
    CHECK_EQ(ramp_errors(v, 0), size_t(0));
  }
  auto recs = rig.only_manifest(kF);
  CHECK(recs.size() >= 2);
  if (recs.size() >= 2) {
    CHECK_EQ(recs[1]["ev"], std::string("failover"));
    CHECK_EQ(recs[1]["from"], narrow(kS));
    CHECK_EQ(recs[1]["old_path"], std::string());
  }
}
