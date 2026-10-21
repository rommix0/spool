// Renaming markers while recording and after (MarkerBook, SessionManager::rename_marker).
#include <string>
#include <vector>

#include "audio/synthetic_source.h"
#include "core/clock.h"
#include "dest_helpers.h"
#include "format/wav_writer.h"
#include "io/fault_file_system.h"
#include "session/event_log.h"
#include "session/manifest.h"
#include "session/marker_book.h"
#include "session/session_manager.h"
#include "test.h"

using namespace spool;
using namespace dest_test;

namespace {

const std::wstring kP = L"D:\\p";
constexpr SampleIndex kRate = 8000;

// 8 kHz mono ramp into one 32-bit float WAV destination in D:\p.
struct Rig {
  Rig() : clock(WallTime{1792175400000LL, -240}, 1000), log(fs, L"C:\\logs\\m.log"),
          src({uint32_t(kRate), 1}, SyntheticSource::Pattern::Ramp, true), mgr(src, fs, clock, log) {
    std::wstring err;
    fs.ensure_dir(L"C:\\logs", &err);
    DestinationConfig d;
    d.format = OutFormat::Wav;
    d.wav_depth = WavDepth::Float32;
    d.folders = {kP};
    cfg.dests = {d};
  }
  ~Rig() { mgr.stop(); }

  bool start() {
    std::wstring err;
    return mgr.start(cfg, nullptr, &err);
  }
  bool pump(int seconds) {
    for (int i = 0; i < seconds * 10; ++i) {
      src.pump(size_t(kRate / 10));
      clock.advance_ms(100);
      const SampleIndex pos = mgr.stats().position;
      if (!poll([&] { return mgr.dest_status()[0].cursor >= pos; })) return false;
    }
    return true;
  }
  // "frame:name" of every marker in the n-th WAV file (by name).
  std::vector<std::wstring> wav_markers(size_t n) {
    const auto wavs = fs.list(kP, L".wav");
    std::vector<std::wstring> v;
    if (n >= wavs.size()) return v;
    if (const auto info = wav_info(fs, wavs[n]))
      for (const auto& m : info->markers) v.push_back(std::to_wstring(m.frame_in_file) + L":" + m.name);
    return v;
  }
  std::vector<ManifestRecord> manifest() {
    const auto m = fs.list(kP, L".jsonl");
    return m.size() == 1 ? read_manifest(fs, m[0]) : std::vector<ManifestRecord>{};
  }
  bool logged(const std::wstring& part) {
    for (const LogEntry& e : log.recent(100))
      if (e.text.find(part) != std::wstring::npos) return true;
    return false;
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

}  // namespace

TEST(marker_rename_in_the_open_file) {
  Rig rig;
  CHECK(rig.start());
  CHECK(rig.pump(2));
  const uint32_t id = rig.mgr.marker(1);
  CHECK_EQ(id, 1u);
  CHECK(rig.mgr.rename_marker(id, L"  Intro\r\n"));  // trimmed, line breaks become spaces
  CHECK(rig.mgr.markers().size() == 1 && rig.mgr.markers()[0].name == L"Intro");
  CHECK(rig.pump(1));
  rig.mgr.stop();
  CHECK(rig.wav_markers(0) == (std::vector<std::wstring>{L"8000:Intro"}));
  const auto recs = rig.manifest();
  const auto ms = events(recs, "marker"), rs = events(recs, "marker_rename");
  CHECK_EQ(ms.size(), size_t(1));
  CHECK_EQ(rs.size(), size_t(1));
  if (ms.size() != 1 || rs.size() != 1) return;
  CHECK_EQ(str(ms[0], "name"), std::string("Marker 1"));
  CHECK_EQ(str(ms[0], "id"), std::string("1"));
  CHECK_EQ(str(rs[0], "id"), std::string("1"));
  CHECK_EQ(str(rs[0], "sample"), std::string("8000"));
  CHECK_EQ(str(rs[0], "name"), std::string("Intro"));
  CHECK(rig.logged(L"Marker \"Marker 1\" renamed to \"Intro\""));
}

TEST(marker_rename_rewrites_a_finished_file) {
  Rig rig;
  CHECK(rig.start());
  CHECK(rig.pump(2));
  rig.mgr.marker(2);
  rig.mgr.marker(1);
  rig.mgr.split();  // file 1 is finalized with "Marker 1" and "Marker 2"
  CHECK(poll([&] { return rig.mgr.dest_status()[0].n == 2; }));
  CHECK(rig.wav_markers(0) == (std::vector<std::wstring>{L"0:Marker 1", L"8000:Marker 2"}));
  CHECK(rig.mgr.rename_marker(2, L"Second song"));
  CHECK(poll([&] { return rig.wav_markers(0) == std::vector<std::wstring>{L"0:Marker 1", L"8000:Second song"}; }));
  CHECK(rig.pump(1));
  rig.mgr.stop();
  const auto wavs = rig.fs.list(kP, L".wav");
  CHECK_EQ(wavs.size(), size_t(2));
  if (wavs.size() == 2) CHECK_EQ(ramp_errors(wav_samples(rig.fs, wavs[0]), 0), size_t(0));  // audio untouched
  CHECK_EQ(events(rig.manifest(), "marker_rename").size(), size_t(1));
}

TEST(marker_rename_after_stop) {
  Rig rig;
  CHECK(rig.start());
  CHECK(rig.pump(2));
  rig.mgr.marker(1);  // a marker at the stop sample itself would be in no file
  rig.mgr.stop();
  CHECK_EQ(rig.mgr.markers().size(), size_t(1));  // still there until the next start
  CHECK(rig.mgr.rename_marker(1, L"Encore"));
  CHECK(poll([&] { return rig.wav_markers(0) == std::vector<std::wstring>{L"8000:Encore"}; }));
  CHECK(poll([&] {
    const auto recs = rig.manifest();
    return !recs.empty() && str(recs.back(), "ev") == "marker_rename" && str(recs.back(), "name") == "Encore";
  }));
  // A second rename of the same marker rewrites the file again.
  CHECK(rig.mgr.rename_marker(1, L"Encore (acoustic)"));
  CHECK(poll([&] { return rig.wav_markers(0) == std::vector<std::wstring>{L"8000:Encore (acoustic)"}; }));
  // The next recording starts with no markers.
  CHECK(rig.start());
  CHECK(rig.mgr.markers().empty());
  CHECK(!rig.mgr.rename_marker(1, L"x"));
}

TEST(marker_rename_rejects_bad_requests) {
  Rig rig;
  CHECK_EQ(rig.mgr.marker(0), 0u);  // not recording
  CHECK(rig.start());
  CHECK(rig.pump(1));
  const uint32_t id = rig.mgr.marker(0);
  CHECK(!rig.mgr.rename_marker(id, L"   "));        // empty
  CHECK(!rig.mgr.rename_marker(id, L"Marker 1"));   // unchanged
  CHECK(!rig.mgr.rename_marker(id + 1, L"Other"));  // unknown
  CHECK(rig.mgr.rename_marker(id, std::wstring(500, L'x')));
  CHECK_EQ(rig.mgr.markers()[0].name.size(), SessionManager::kMaxMarkerName);
}

TEST(marker_rename_failure_is_logged) {
  Rig rig;
  CHECK(rig.start());
  CHECK(rig.pump(2));
  rig.mgr.marker(1);
  rig.mgr.stop();
  rig.fs.add_rule({kP, FaultFileSystem::Kind::FailOpen});  // the drive went away
  CHECK(rig.mgr.rename_marker(1, L"Lost"));
  CHECK(poll([&] { return rig.logged(L"Couldn't rename the marker in"); }));
}

TEST(marker_book_rename_between_finalize_and_placed) {
  FaultFileSystem fs;
  FakeClock clock(WallTime{0, 0}, 1000);
  EventLog log(fs, L"");
  std::wstring err;
  fs.ensure_dir(kP, &err);
  const std::wstring path = kP + L"\\a.wav";
  auto w = open_wav(fs, path, WavParams{8000, 1}, &err);
  CHECK(w != nullptr);
  if (!w) return;
  const std::vector<float> in(100, 0.0f);
  CHECK(w->write(in.data(), 100));
  CHECK(w->finalize({{10, L"Marker 1"}, {20, L"no id"}}, std::nullopt));
  auto book = MarkerBook::create(fs, clock, log);
  book->add(1, L"Marker 1");
  CHECK(book->rename(1, L"Late name"));  // renamed after the writer took the names
  book->placed(L"main", path, {{1, 10, L"Marker 1"}, {0, 20, L"no id"}});
  CHECK(book->wait_idle(5000));
  const auto info = wav_info(fs, path);
  CHECK(info.has_value());
  if (!info || info->markers.size() != 2) return;
  CHECK(info->markers[0].name == L"Late name");
  CHECK(info->markers[1].name == L"no id");  // a marker without an id is kept as it is
  CHECK(!book->rename(1, L"Late name"));      // unchanged
  CHECK(!book->rename(7, L"x"));              // unknown
  book->shutdown();
}
