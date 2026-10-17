#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "audio/ramp.h"
#include "core/clock.h"
#include "core/sample_ring.h"
#include "format/wav_reader.h"
#include "io/fault_file_system.h"
#include "session/config.h"
#include "session/destination.h"
#include "session/event_log.h"
#include "session/manifest.h"
#include "dest_helpers.h"
#include "test.h"

using namespace sl;
using namespace dest_test;

namespace {

// 2026-10-16 18:29:58 UTC, local = UTC-4 -> 14:29:58 local; the first file is 2 s long.
WallTime start_wall() { return WallTime{1792175398000LL, -240}; }

const std::wstring kDir = L"D:\\rec";
const std::wstring kManifest = L"D:\\rec\\gig_main.jsonl";

struct Rig {
  explicit Rig(uint32_t rate = 8000, OutFormat fmt = OutFormat::Wav)
      : clock(start_wall(), 1000), log(fs, L"D:\\logs\\gig.log"), ring(1, size_t(1) << 20, 0) {
    std::wstring err;
    fs.ensure_dir(L"D:\\logs", &err);
    sp.session_name = L"gig";
    sp.session_id = L"abc123";
    sp.rate = rate;
    sp.input_channels = 1;
    sp.start_sample = 0;
    sp.start_wall = start_wall();
    cfg.format = fmt;
    cfg.wav_depth = WavDepth::Float32;
    cfg.folders = {kDir};
    cfg.rotation_minutes = 1;
  }
  // Feeds [from, to) in chunks, waiting for the writer so the ring never overruns.
  bool pump(Destination& d, SampleIndex from, SampleIndex to) {
    const SampleIndex chunk = SampleIndex(sp.rate) * 5;
    for (SampleIndex s = from; s < to; s += chunk) {
      const SampleIndex n = std::min(chunk, to - s);
      feed(ring, s, size_t(n));
      if (!wait_progress(d, s + n)) return false;
    }
    return true;
  }
  std::vector<std::wstring> files(const wchar_t* ext = L".wav") { return fs.list(kDir, ext); }
  std::vector<ManifestRecord> manifest() { return read_manifest(fs, kManifest); }
  std::vector<ManifestRecord> events(const std::string& ev) {
    std::vector<ManifestRecord> out;
    for (auto& r : manifest())
      if (r["ev"] == ev) out.push_back(r);
    return out;
  }

  FaultFileSystem fs;
  FakeClock clock;
  EventLog log;
  SampleRing ring;
  SessionParams sp;
  DestinationConfig cfg;
};

}  // namespace

TEST(dest_continuous_across_rotations) {
  Rig rig;
  Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  d.start(0);
  CHECK(rig.pump(d, 0, 150 * 8000));
  d.stop(150 * 8000);
  CHECK_EQ(d.status().state, DestState::Stopped);

  const auto files = rig.files();
  CHECK_EQ(files.size(), size_t(4));
  if (files.size() != 4) return;
  const uint64_t expect[4] = {2 * 8000, 60 * 8000, 60 * 8000, 28 * 8000};
  SampleIndex s = 0;
  size_t bad = 0;
  for (size_t i = 0; i < 4; ++i) {
    const auto v = wav_samples(rig.fs, files[i]);
    CHECK_EQ(uint64_t(v.size()), expect[i]);
    for (float x : v) bad += (x != ramp_value(s++, 0));
  }
  CHECK_EQ(s, SampleIndex(150 * 8000));
  CHECK_EQ(bad, size_t(0));
  CHECK(files[0].find(L"gig_2026-10-16_142958_001.wav") != std::wstring::npos);
  CHECK(files[1].find(L"gig_2026-10-16_143000_002.wav") != std::wstring::npos);
  const auto bext = wav_info(rig.fs, files[1]);
  CHECK(bext.has_value());
  if (bext) {
    CHECK_EQ(bext->bext.originator_reference, std::string("abc123"));
    CHECK_EQ(bext->bext.date, std::string("2026-10-16"));
    CHECK_EQ(bext->bext.time, std::string("14:30:00"));
    CHECK_EQ(bext->bext.time_reference, uint64_t(14 * 3600 + 30 * 60) * 8000);
  }
}

TEST(dest_manifest_open_close_ranges) {
  Rig rig;
  Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  d.start(0);
  CHECK(rig.pump(d, 0, 70 * 8000));
  d.stop(70 * 8000);

  auto recs = rig.manifest();
  CHECK(recs.size() >= 2);
  if (recs.size() < 2) return;
  CHECK_EQ(recs.front()["ev"], std::string("session_start"));
  CHECK_EQ(recs.front()["session_id"], std::string("abc123"));
  CHECK_EQ(recs.back()["ev"], std::string("session_end"));
  CHECK_EQ(recs.back()["reason"], std::string("stop"));
  CHECK_EQ(num(recs.back(), "sample"), int64_t(70 * 8000));

  auto opens = rig.events("file_open");
  auto closes = rig.events("file_close");
  CHECK_EQ(opens.size(), size_t(3));
  CHECK_EQ(closes.size(), size_t(3));
  CHECK_EQ(rig.events("rotation").size(), size_t(2));
  if (closes.size() != 3 || opens.size() != 3) return;
  const int64_t ends[3] = {2 * 8000, 62 * 8000, 70 * 8000};
  int64_t prev = 0;
  for (size_t i = 0; i < 3; ++i) {
    CHECK_EQ(num(closes[i], "start"), prev);
    CHECK_EQ(num(closes[i], "end"), ends[i]);
    CHECK_EQ(num(opens[i], "sample"), prev);
    CHECK_EQ(closes[i]["path"], opens[i]["path"]);
    std::wstring wpath(closes[i]["path"].begin(), closes[i]["path"].end());  // ASCII paths
    CHECK_EQ(num(closes[i], "bytes"), int64_t(rig.fs.contents(wpath).size()));
    prev = ends[i];
  }
}

TEST(dest_marker_into_correct_file) {
  Rig rig;
  Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  d.start(0);
  d.post(CmdMarker{70 * 8000, L"cue"});
  CHECK(rig.pump(d, 0, 130 * 8000));
  // File 1 [0, 2 s) is long finalized: this marker can only go to the manifest.
  d.post(CmdMarker{1000, L"late"});
  d.stop(130 * 8000);  // commands posted before stop are handled first

  const auto files = rig.files();
  CHECK_EQ(files.size(), size_t(4));
  if (files.size() != 4) return;
  const auto f1 = wav_info(rig.fs, files[0]);
  const auto f3 = wav_info(rig.fs, files[2]);
  CHECK(f1 && f3);
  if (!f1 || !f3) return;
  CHECK(f1->markers.empty());
  CHECK_EQ(f3->markers.size(), size_t(1));
  if (f3->markers.size() == 1) {
    CHECK_EQ(f3->markers[0].frame_in_file, uint64_t(8 * 8000));
    CHECK(f3->markers[0].name == L"cue");
  }
  auto marks = rig.events("marker");
  CHECK_EQ(marks.size(), size_t(2));
  if (marks.size() == 2) {
    CHECK_EQ(num(marks[0], "sample"), int64_t(70 * 8000));
    CHECK_EQ(marks[0]["name"], std::string("cue"));
    CHECK_EQ(num(marks[1], "sample"), int64_t(1000));
    CHECK_EQ(marks[1]["name"], std::string("late"));
  }
  size_t logged = 0;
  for (const auto& e : rig.log.recent(1000))
    logged += (e.sev == Severity::Info && e.text.find(L"Marker") != std::wstring::npos);
  CHECK_EQ(logged, size_t(2));
}

TEST(dest_split_command) {
  Rig rig;
  Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  d.start(0);
  d.post(CmdSplit{10 * 8000});
  CHECK(rig.pump(d, 0, 20 * 8000));
  d.stop(20 * 8000);

  auto closes = rig.events("file_close");
  CHECK_EQ(closes.size(), size_t(3));
  if (closes.size() == 3) {
    CHECK_EQ(num(closes[0], "end"), int64_t(2 * 8000));
    CHECK_EQ(num(closes[1], "end"), int64_t(10 * 8000));
    CHECK_EQ(num(closes[2], "start"), int64_t(10 * 8000));
    CHECK_EQ(num(closes[2], "end"), int64_t(20 * 8000));
  }
  auto rots = rig.events("rotation");
  CHECK_EQ(rots.size(), size_t(2));
  if (rots.size() == 2) {
    CHECK_EQ(rots[1]["reason"], std::string("split"));
    CHECK_EQ(num(rots[1], "sample"), int64_t(10 * 8000));
  }
  const auto files = rig.files();
  CHECK_EQ(files.size(), size_t(3));
  if (files.size() == 3) CHECK_EQ(wav_samples(rig.fs, files[2]).size(), size_t(10 * 8000));
}

TEST(dest_rotation_ignores_clock_jump) {
  Rig rig;
  Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  d.start(0);
  CHECK(rig.pump(d, 0, 20 * 8000));  // past the first rotation at 2 s
  WallTime back = rig.clock.now();
  back.utc_ms -= 3600 * 1000;
  rig.clock.set(back);
  CHECK(rig.pump(d, 20 * 8000, 80 * 8000));
  d.stop(80 * 8000);

  auto closes = rig.events("file_close");
  CHECK_EQ(closes.size(), size_t(3));
  if (closes.size() == 3) {
    CHECK_EQ(num(closes[0], "end"), int64_t(2 * 8000));
    CHECK_EQ(num(closes[1], "end"), int64_t(2 * 8000 + 60 * 8000));
    CHECK_EQ(num(closes[2], "end"), int64_t(80 * 8000));
  }
  const auto files = rig.files();
  CHECK_EQ(files.size(), size_t(3));
  if (files.size() == 3)  // named from the sample clock, not the PC clock
    CHECK(files[2].find(L"gig_2026-10-16_143100_003.wav") != std::wstring::npos);
}

TEST(dest_flush_confirms_every_interval) {
  Rig rig;
  Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  d.start(0);
  feed(rig.ring, 0, 5 * 8000);
  CHECK(wait_progress(d, 5 * 8000));
  // Opened at 14:29:58 local -> file 1 spans [0, 16000) and was confirmed by its finalize;
  // steady time has not moved, so file 2 has not been flushed yet.
  CHECK_EQ(d.status().confirmed, SampleIndex(2 * 8000));
  CHECK(rig.fs.contents(kManifest).empty());
  rig.clock.advance_ms(2100);
  CHECK(poll([&] { return d.status().confirmed > 2 * 8000; }));
  const DestStatus st = d.status();
  CHECK_EQ(st.confirmed, SampleIndex(5 * 8000));
  CHECK(st.current_file.find(L"_002.wav") != std::wstring::npos);
  const auto info = wav_info(rig.fs, st.current_file);
  CHECK(info.has_value());
  if (info) CHECK_EQ(SampleIndex(info->frames), st.confirmed - 2 * 8000);
  CHECK(!rig.fs.contents(kManifest).empty());  // the manifest was flushed too
  d.stop(5 * 8000);
}

TEST(dest_opus_smoke) {
  Rig rig(48000, OutFormat::Opus);
  Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  CHECK_EQ(d.data_rate(), uint64_t(64000 / 8));
  d.start(0);
  CHECK(rig.pump(d, 0, 130 * 48000));
  d.stop(130 * 48000);
  CHECK_EQ(d.status().state, DestState::Stopped);

  const auto files = rig.files(L".opus");
  CHECK_EQ(files.size(), size_t(4));
  for (const auto& f : files) {
    const auto c = rig.fs.contents(f);
    CHECK(c.size() > 4 && std::memcmp(c.data(), "OggS", 4) == 0);
  }
  auto closes = rig.events("file_close");
  CHECK_EQ(closes.size(), size_t(4));
  if (closes.size() == 4) {
    CHECK_EQ(num(closes[0], "end"), int64_t(2 * 48000));
    CHECK_EQ(num(closes[3], "end"), int64_t(130 * 48000));
  }
}

TEST(dest_gap_close_and_resume) {
  Rig rig;
  rig.cfg.rotation_minutes = 0;
  Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  d.start(0);
  d.post(CmdGapStart{5 * 8000, "device_lost"});
  d.post(CmdCloseForGap{5 * 8000});
  feed(rig.ring, 0, 5 * 8000);
  CHECK(poll([&] { return d.status().state == DestState::GapClosed; }));
  CHECK_EQ(d.progress(), SampleIndex(5 * 8000));
  rig.ring.jump_to(10 * 8000);
  d.post(CmdGapEnd{10 * 8000, "device_back"});
  d.post(CmdResumeAt{10 * 8000});
  feed(rig.ring, 10 * 8000, 5 * 8000);
  CHECK(wait_progress(d, 15 * 8000));
  d.stop(15 * 8000);

  auto closes = rig.events("file_close");
  CHECK_EQ(closes.size(), size_t(2));
  if (closes.size() == 2) {
    CHECK_EQ(num(closes[0], "end"), int64_t(5 * 8000));
    CHECK_EQ(num(closes[1], "start"), int64_t(10 * 8000));
    CHECK_EQ(num(closes[1], "end"), int64_t(15 * 8000));
  }
  CHECK_EQ(rig.events("gap_start").size(), size_t(1));
  CHECK_EQ(rig.events("gap_end").size(), size_t(1));
  CHECK_EQ(d.status().gaps, uint64_t(1));
  const auto files = rig.files();
  CHECK_EQ(files.size(), size_t(2));
  if (files.size() == 2) {
    const auto v = wav_samples(rig.fs, files[1]);
    CHECK_EQ(v.size(), size_t(5 * 8000));
    if (!v.empty()) CHECK(v[0] == ramp_value(10 * 8000, 0));
  }
}

// CmdResumeAt keeps the session's rotation grid. At 44100 Hz a sample does not fall on
// a whole millisecond, so a boundary recomputed from the resume point's wall time
// would land up to 44 samples late.
TEST(dest_resume_keeps_rotation_grid_44100) {
  Rig rig(44100);
  Destination d(rig.cfg, rig.sp, rig.ring, rig.fs, rig.clock, rig.log);
  d.start(0);
  d.post(CmdCloseForGap{5 * 44100});
  CHECK(rig.pump(d, 0, 5 * 44100));
  CHECK(poll([&] { return d.status().state == DestState::GapClosed; }));
  const SampleIndex resume = 10 * 44100 + 43;
  rig.ring.jump_to(resume);
  d.post(CmdResumeAt{resume});
  CHECK(rig.pump(d, resume, 63 * 44100));
  d.stop(63 * 44100);

  auto rots = rig.events("rotation");
  CHECK_EQ(rots.size(), size_t(2));
  if (rots.size() == 2) {
    CHECK_EQ(num(rots[0], "sample"), int64_t(2 * 44100));
    CHECK_EQ(num(rots[1], "sample"), int64_t(2 * 44100 + 60 * 44100));  // 14:31:00 on the grid
  }
}
