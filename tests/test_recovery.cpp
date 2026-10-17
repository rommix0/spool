#include <windows.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "audio/synthetic_source.h"
#include "core/clock.h"
#include "format/wav_writer.h"
#include "dest_helpers.h"
#include "io/fault_file_system.h"
#include "session/event_log.h"
#include "session/manifest.h"
#include "session/recovery.h"
#include "session/session_manager.h"
#include "test.h"

using namespace sl;
using namespace dest_test;
using Kind = FaultFileSystem::Kind;

namespace {

// 2026-10-16 18:30:00 UTC, local = UTC-4 -> 14:30:00.
WallTime r_wall() { return WallTime{1792175400000LL, -240}; }

const std::wstring kP = L"D:\\p";
const std::wstring kS = L"E:\\s";
constexpr SampleIndex kRate = 8000;

// A unique directory under %TEMP% (with a non-ASCII name), removed with its files.
struct TempDir {
  TempDir() {
    static int counter = 0;
    wchar_t tmp[MAX_PATH + 1];
    const DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
    path = std::wstring(tmp, n) + L"sl_test_\u00FC\u65E5_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
           std::to_wstring(++counter);
    CreateDirectoryW(path.c_str(), nullptr);
  }
  ~TempDir() {
    WIN32_FIND_DATAW fd;
    const HANDLE h = FindFirstFileW((path + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((path + L"\\" + fd.cFileName).c_str());
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
    RemoveDirectoryW(path.c_str());
  }
  std::wstring path;
};

// Manual ramp source at 8 kHz mono, 32-bit float WAV into D:\p, crash state in a temp INI.
struct RecRig {
  RecRig()
      : clock(r_wall(), 1000),
        log(fs, L"C:\\logs\\s.log"),
        src({uint32_t(kRate), 1}, SyntheticSource::Pattern::Ramp, true),
        mgr(src, fs, clock, log),
        store(dir.path + L"\\state.ini") {
    std::wstring err;
    fs.ensure_dir(L"C:\\logs", &err);
    cfg.params.session_name = L"gig";
    DestinationConfig d;
    d.format = OutFormat::Wav;
    d.wav_depth = WavDepth::Float32;
    d.folders = {kP};
    cfg.dests = {d};
    mgr.set_state_store(&store);
  }
  ~RecRig() {
    mgr.stop();
    fs.release_hangs();
  }

  // Starts and waits for every writer to open its first file (pump() only waits for
  // writers that are writing).
  bool start(const ResumeInfo* resume = nullptr) {
    std::wstring err;
    if (!mgr.start(cfg, resume, &err)) return false;
    return poll([&] {
      for (const auto& d : mgr.dest_status())
        if (d.state != DestState::Writing) return false;
      return true;
    });
  }
  bool caught_up() {
    return poll([&] {
      const SampleIndex pos = mgr.stats().position;
      for (const auto& d : mgr.dest_status())
        if (d.state == DestState::Writing && d.cursor < pos) return false;
      return true;
    });
  }
  // Pumps `seconds` in 100 ms blocks with the clock advancing alongside.
  bool pump(double seconds, bool wait = true) {
    const int blocks = int(seconds * 10 + 0.5);
    for (int i = 0; i < blocks; ++i) {
      src.pump(size_t(kRate / 10));
      clock.advance_ms(100);
      if (wait && !caught_up()) return false;
      if (!wait) std::this_thread::sleep_for(std::chrono::milliseconds(20));  // let the watchdog tick
    }
    return true;
  }
  // The crash state names a manifest (saved every 2 s and on every new file or manifest).
  bool state_has_manifest() {
    return poll([&] {
      const auto st = store.load();
      return st && st->recording && !st->dests.empty() && !st->dests[0].manifest_path.empty();
    });
  }
  std::vector<ManifestRecord> only_manifest(const std::wstring& folder) {
    const auto m = fs.list(folder, L".jsonl");
    return m.size() == 1 ? read_manifest(fs, m[0]) : std::vector<ManifestRecord>{};
  }

  TempDir dir;
  FaultFileSystem fs;
  FakeClock clock;
  EventLog log;
  SyntheticSource src;
  SessionManager mgr;
  StateStore store;
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

// Frames physically in a 32-bit float mono WAV, whatever its header claims.
uint64_t physical_frames(FaultFileSystem& fs, const std::wstring& path) {
  const auto info = wav_info(fs, path);
  const auto bytes = fs.contents(path);
  return info && bytes.size() >= info->data_offset ? (bytes.size() - info->data_offset) / 4 : 0;
}

void put_line(FaultFileSystem& fs, const std::wstring& manifest, const JsonLine& l) {
  std::wstring err;
  auto m = fs.exists(manifest) ? Manifest::open_append(fs, manifest, &err) : Manifest::create(fs, manifest, &err);
  CHECK(m != nullptr);
  if (!m) return;
  m->append(l);
  CHECK(m->flush());
}

}  // namespace

TEST(state_store_roundtrip) {
  TempDir dir;
  StateStore store(dir.path + L"\\state.ini");
  const auto none = store.load();
  CHECK(!none || !none->recording);

  RecordingState s;
  s.recording = true;
  s.session_id = L"20261016-143000";
  s.session_name = L" Konzert \"Gro\u00DF\" \u65E5\u672C ";
  s.start_sample = 123;
  s.last_sample = 9876543210LL;
  s.start_utc_ms = 1792175400000LL;
  s.last_utc_ms = 1792175460000LL;
  s.local_offset_min = -240;
  s.rate = 48000;
  s.channels = 8;
  s.dests = {{L"main", L"D:\\p\\gig_main.jsonl", 3},
             {L"zweite \u00FC", L"E:\\M\u00FCsik\\\u65E5\u672C\\gig_zweite.jsonl", 7}};
  store.save(s);
  const auto l = store.load();
  CHECK(l.has_value());
  if (!l) return;
  CHECK(l->recording);
  CHECK(l->session_id == s.session_id);
  CHECK(l->session_name == s.session_name);
  CHECK_EQ(l->start_sample, s.start_sample);
  CHECK_EQ(l->last_sample, s.last_sample);
  CHECK_EQ(l->start_utc_ms, s.start_utc_ms);
  CHECK_EQ(l->last_utc_ms, s.last_utc_ms);
  CHECK_EQ(l->local_offset_min, s.local_offset_min);
  CHECK_EQ(l->rate, s.rate);
  CHECK_EQ(l->channels, s.channels);
  CHECK_EQ(l->dests.size(), size_t(2));
  for (size_t i = 0; i < 2 && i < l->dests.size(); ++i) {
    CHECK(l->dests[i].name == s.dests[i].name);
    CHECK(l->dests[i].manifest_path == s.dests[i].manifest_path);
    CHECK_EQ(l->dests[i].next_n, s.dests[i].next_n);
  }

  // Fewer destinations than before: the stale one is gone.
  s.dests.pop_back();
  store.save(s);
  const auto one = store.load();
  CHECK(one.has_value());
  if (one) CHECK_EQ(one->dests.size(), size_t(1));

  store.clear();
  const auto c = store.load();
  CHECK(!c || !c->recording);
}

TEST(recovery_repairs_crashed_wav) {
  RecRig rig;
  CHECK(rig.start());
  CHECK(rig.pump(10.5));  // flushed every 2 s; the last 0.5 s is past the last header update
  CHECK(rig.state_has_manifest());
  rig.mgr.abandon_for_test();
  CHECK(rig.mgr.stats().state == SessionState::Idle);

  const auto st = rig.store.load();
  CHECK(st.has_value() && st->recording);
  if (!st) return;
  CHECK_EQ(st->dests.size(), size_t(1));
  CHECK(!st->dests.empty() && st->dests[0].next_n == 2u);
  const auto wavs = rig.fs.list(kP, L".wav");
  CHECK_EQ(wavs.size(), size_t(1));
  if (wavs.size() != 1) return;
  const auto before = wav_info(rig.fs, wavs[0]);
  CHECK(before && before->frames < uint64_t(10.5 * kRate));  // the crash left the tail unclaimed

  const RepairReport r = repair_open_files(rig.fs, *st);
  CHECK_EQ(r.repaired.size(), size_t(1));
  CHECK_EQ(r.failed.size(), size_t(0));
  const auto info = wav_info(rig.fs, wavs[0]);
  CHECK(info.has_value());
  if (!info) return;
  CHECK_EQ(info->frames, physical_frames(rig.fs, wavs[0]));
  CHECK_EQ(info->frames, uint64_t(10.5 * kRate));
  CHECK_EQ(ramp_errors(wav_samples(rig.fs, wavs[0]), 0), size_t(0));

  const auto recs = rig.only_manifest(kP);
  const auto closes = events(recs, "file_close");
  CHECK_EQ(closes.size(), size_t(1));
  if (closes.size() == 1) {
    CHECK_EQ(str(closes[0], "path"), narrow(wavs[0]));
    CHECK_EQ(num_of(closes[0], "start"), int64_t(0));
    CHECK_EQ(num_of(closes[0], "end"), int64_t(10.5 * kRate));
    CHECK_EQ(num_of(closes[0], "bytes"), int64_t(rig.fs.contents(wavs[0]).size()));
  }
  const auto gaps = events(recs, "gap_start");
  CHECK_EQ(gaps.size(), size_t(1));
  if (gaps.size() == 1) {
    CHECK_EQ(str(gaps[0], "reason"), std::string("crash"));
    // Where the recorded audio ends: the state was saved up to 2 s before the crash.
    CHECK(st->last_sample <= SampleIndex(10.5 * kRate));
    CHECK_EQ(num_of(gaps[0], "sample"), int64_t(10.5 * kRate));
  }
  CHECK(!recs.empty() && str(recs.back(), "ev") == "gap_start");

  // A second scan finds nothing left open.
  const RepairReport again = repair_open_files(rig.fs, *st);
  CHECK_EQ(again.repaired.size(), size_t(0));
  CHECK_EQ(again.failed.size(), size_t(0));
  CHECK_EQ(events(rig.only_manifest(kP), "gap_start").size(), size_t(1));
}

TEST(recovery_resume_continues_session) {
  RecRig rig;
  CHECK(rig.start());
  const std::wstring id = rig.mgr.stats().session_id;
  CHECK(rig.pump(10.5));
  CHECK(rig.state_has_manifest());
  rig.mgr.abandon_for_test();

  const auto st = rig.store.load();
  CHECK(st.has_value());
  if (!st || st->dests.empty()) return;
  // Back up a minute after the last state save.
  rig.clock.set(WallTime{st->last_utc_ms + 60000, r_wall().local_offset_min});
  const RepairReport r = repair_open_files(rig.fs, *st);
  CHECK_EQ(r.repaired.size(), size_t(1));
  const ResumeInfo ri = make_resume(*st, rig.clock.now());
  CHECK_EQ(ri.resume_sample, st->last_sample + 60 * kRate);

  CHECK(rig.start(&ri));
  const SessionStats s = rig.mgr.stats();
  CHECK(s.session_id == id);
  CHECK(s.recovered_from_crash);
  CHECK_EQ(s.position, ri.resume_sample);
  CHECK(poll([&] {
    const auto d = rig.mgr.dest_status();
    return d.size() == 1 && d[0].state == DestState::Writing && d[0].n == st->dests[0].next_n;
  }));
  const uint32_t n = rig.mgr.dest_status()[0].n;
  CHECK_EQ(n, 2u);  // the previous file was n = 1
  CHECK(rig.pump(3));
  CHECK(poll([&] { return rig.store.load()->dests[0].next_n == 3u; }));
  rig.mgr.stop();
  const auto after = rig.store.load();
  CHECK(!after || !after->recording);  // a clean stop clears the crash state

  // The same manifest continues: same session, gap over the downtime, n continues.
  const auto recs = rig.only_manifest(kP);
  for (const auto& ss : events(recs, "session_start")) CHECK_EQ(str(ss, "session_id"), narrow(id));
  CHECK_EQ(events(recs, "session_start").size(), size_t(2));
  const auto opens = events(recs, "file_open");
  CHECK_EQ(opens.size(), size_t(2));
  if (opens.size() == 2) {
    CHECK_EQ(num_of(opens[0], "n"), int64_t(1));
    CHECK_EQ(num_of(opens[1], "n"), int64_t(2));
    CHECK_EQ(num_of(opens[1], "sample"), int64_t(ri.resume_sample));
  }
  const auto gs = events(recs, "gap_start"), ge = events(recs, "gap_end");
  CHECK_EQ(gs.size(), size_t(1));
  CHECK_EQ(ge.size(), size_t(1));
  if (gs.size() == 1 && ge.size() == 1) {
    CHECK_EQ(str(gs[0], "reason"), std::string("crash"));
    CHECK_EQ(str(ge[0], "reason"), std::string("crash"));
    CHECK_EQ(num_of(gs[0], "sample"), int64_t(10.5 * kRate));
    CHECK_EQ(num_of(ge[0], "sample"), int64_t(ri.resume_sample));
    // Wall times stay on the session's clock.
    CHECK_EQ(str(gs[0], "utc"), iso_utc(WallTime{r_wall().utc_ms + 10500, 0}));
    CHECK_EQ(str(ge[0], "utc"), iso_utc(WallTime{r_wall().utc_ms + ri.resume_sample * 1000 / kRate, 0}));
  }
  const auto closes = events(recs, "file_close");
  CHECK_EQ(closes.size(), size_t(2));
  if (closes.size() == 2) {
    CHECK_EQ(num_of(closes[1], "start"), int64_t(ri.resume_sample));
    CHECK_EQ(num_of(closes[1], "end"), int64_t(ri.resume_sample + 3 * kRate));
  }
  CHECK(!recs.empty() && str(recs.back(), "ev") == "session_end");
  const auto wavs = rig.fs.list(kP, L".wav");
  CHECK_EQ(wavs.size(), size_t(2));
  if (wavs.size() == 2) CHECK_EQ(wav_samples(rig.fs, wavs[1]).size(), size_t(3 * kRate));
}

// Review Focus 2: a writer stalls, the takeover continues in E: from the last confirmed
// sample, then the process dies before the old file is finalized. Repair caps the old file
// at the failover's confirmed sample (its later samples are in the _b file), never at its
// physical length.
TEST(recovery_failed_over_wav_capped_at_confirmed) {
  RecRig rig;
  rig.cfg.ring_seconds = 4;  // the takeover starts up to 2 s (one flush) behind: keep that in the ring
  rig.cfg.dests[0].folders = {kP, kS};
  CHECK(rig.start());
  CHECK(rig.pump(2.5));  // confirmed at the 2 s flush
  rig.fs.add_rule({kP + L"\\", Kind::HangWrite, 16000});  // ~0.5 s more lands, then the WAV hangs
  CHECK(rig.pump(3.5, false));  // stalled once 2 s (half the ring) behind
  CHECK(poll([&] {
    const auto d = rig.mgr.dest_status();
    return d[0].state == DestState::Writing && d[0].folders[1].health == FolderHealth::Active;
  }));
  CHECK(rig.caught_up());
  CHECK(rig.pump(3));  // the E: file and manifest are flushed
  CHECK(poll([&] {
    const auto st = rig.store.load();
    return st && !st->dests.empty() && st->dests[0].manifest_path.rfind(kS, 0) == 0;
  }));
  const SampleIndex end = rig.mgr.stats().position;
  rig.mgr.abandon_for_test();
  rig.fs.clear_rules();  // the drive works again after the restart
  rig.fs.release_hangs();

  const auto st = rig.store.load();
  CHECK(st.has_value());
  if (!st) return;
  const auto fo = events(rig.only_manifest(kS), "failover");
  CHECK_EQ(fo.size(), size_t(1));
  if (fo.size() != 1) return;
  const SampleIndex confirmed = num_of(fo[0], "confirmed");
  const auto a = rig.fs.list(kP, L".wav"), b = rig.fs.list(kS, L".wav");
  CHECK_EQ(a.size(), size_t(1));
  CHECK_EQ(b.size(), size_t(1));
  if (a.size() != 1 || b.size() != 1) return;
  CHECK(ends_with(b[0], L"_001_b.wav"));
  CHECK(physical_frames(rig.fs, a[0]) > uint64_t(confirmed));  // samples past confirmed reached D:

  const RepairReport r = repair_open_files(rig.fs, *st);
  CHECK_EQ(r.repaired.size(), size_t(2));
  CHECK_EQ(r.failed.size(), size_t(0));
  const auto va = wav_samples(rig.fs, a[0]), vb = wav_samples(rig.fs, b[0]);
  CHECK_EQ(va.size(), size_t(confirmed));
  CHECK_EQ(va.size() + vb.size(), size_t(end));
  CHECK_EQ(ramp_errors(va, 0) + ramp_errors(vb, confirmed), size_t(0));  // continuous, no duplicates
  // The old file's bytes past `confirmed` are wrapped in a chunk: RIFF size = file length.
  const auto bytes = rig.fs.contents(a[0]);
  uint32_t riff = 0;
  if (bytes.size() >= 8) std::memcpy(&riff, bytes.data() + 4, 4);
  CHECK_EQ(uint64_t(riff), uint64_t(bytes.size() - 8));

  // Each file is closed in the manifest that opened it; the gap goes to the current one.
  const auto pc = events(rig.only_manifest(kP), "file_close");
  CHECK_EQ(pc.size(), size_t(1));
  if (pc.size() == 1) CHECK_EQ(num_of(pc[0], "end"), int64_t(confirmed));
  const auto srecs = rig.only_manifest(kS);
  const auto sc = events(srecs, "file_close");
  CHECK_EQ(sc.size(), size_t(1));
  if (sc.size() == 1) {
    CHECK_EQ(num_of(sc[0], "start"), int64_t(confirmed));
    CHECK_EQ(num_of(sc[0], "end"), int64_t(end));
  }
  CHECK(!srecs.empty() && str(srecs.back(), "ev") == "gap_start" && str(srecs.back(), "reason") == "crash");
  CHECK_EQ(events(rig.only_manifest(kP), "gap_start").size(), size_t(0));
}

// A vanished drive or a missing file is reported, never thrown, and never stops the rest.
TEST(recovery_tolerates_missing_files_and_manifests) {
  FaultFileSystem fs;
  std::wstring err;
  fs.ensure_dir(kP, &err);
  const std::wstring man = kP + L"\\gig_main.jsonl";
  const std::wstring wav = kP + L"\\gig_001.wav", gone = kP + L"\\gig_002.wav", opus = kP + L"\\gig_003.opus";
  const WallTime w = r_wall();
  put_line(fs, man, JsonLine("session_start", 0, w).str("session_id", L"x").str("dest", L"main"));
  put_line(fs, man, JsonLine("file_open", 0, w).str("path", wav).num("n", 1).num("seg", 0));
  put_line(fs, man, JsonLine("file_open", 0, w).str("path", gone).num("n", 2).num("seg", 0));
  put_line(fs, man, JsonLine("file_open", 100, w).str("path", opus).num("n", 3).num("seg", 0));
  {
    auto enc = open_wav(fs, wav, WavParams{uint32_t(kRate), 1, WavDepth::Float32}, &err);
    CHECK(enc != nullptr);
    if (!enc) return;
    std::vector<float> v(500);
    for (size_t i = 0; i < v.size(); ++i) v[i] = ramp_value(SampleIndex(i), 0);
    CHECK(enc->write(v.data(), v.size()));
    enc->abandon();
    auto f = fs.create_new(opus, &err);
    CHECK(f != nullptr);
    if (f) {
      const char page[] = "OggS-not-really";
      f->write(page, sizeof page);
      f->close();
    }
  }
  fs.add_rule({L"Z:\\", Kind::FailOpen});

  RecordingState st;
  st.recording = true;
  st.session_id = L"x";
  st.rate = uint32_t(kRate);
  st.channels = 1;
  st.start_utc_ms = w.utc_ms;
  st.last_utc_ms = w.utc_ms + 1000;
  st.last_sample = kRate;
  st.dests = {{L"lost", L"Z:\\gone\\gig_lost.jsonl", 4}, {L"main", man, 4}};
  RepairReport r;
  r = repair_open_files(fs, st);
  CHECK_EQ(r.repaired.size(), size_t(2));  // the WAV and the Opus entry
  CHECK_EQ(r.failed.size(), size_t(2));    // the unreachable manifest and the vanished WAV
  CHECK(wav_samples(fs, wav).size() == 500);
  const auto recs = read_manifest(fs, man);
  const auto closes = events(recs, "file_close");
  CHECK_EQ(closes.size(), size_t(2));
  for (const auto& c : closes)
    if (str(c, "path") == narrow(opus)) {
      CHECK_EQ(num_of(c, "start"), int64_t(100));
      CHECK_EQ(num_of(c, "end"), int64_t(100));  // nothing known confirmed past the open
      CHECK_EQ(num_of(c, "bytes"), int64_t(fs.contents(opus).size()));
    }
  CHECK_EQ(events(recs, "gap_start").size(), size_t(1));
}

TEST(recovery_make_resume_advances_by_downtime) {
  RecordingState st;
  st.rate = 48000;
  st.last_sample = 1000;
  st.last_utc_ms = 5000;
  CHECK_EQ(make_resume(st, WallTime{5000 + 90000, 0}).resume_sample, SampleIndex(1000 + 90 * 48000));
  CHECK_EQ(make_resume(st, WallTime{4000, 0}).resume_sample, SampleIndex(1000));  // clock went back
}
