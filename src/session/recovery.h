#pragma once
// Crash recovery (§8.5): the "recording in progress" state the session manager keeps in an
// INI file, the repair scan that closes what a crash left open, and the resume point.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/types.h"
#include "io/file_system.h"

namespace sl {

struct DestResume {
  std::wstring name, manifest_path;  // the destination's current manifest
  uint32_t next_n;                   // {n} of its next file
};

struct RecordingState {
  bool recording = false;
  std::wstring session_id, session_name;
  SampleIndex start_sample = 0, last_sample = 0;  // last_sample: the write index at the save
  int64_t start_utc_ms = 0, last_utc_ms = 0;      // last_utc_ms: the wall clock at the save
  int32_t local_offset_min = 0;                   // of the session's start_wall
  uint32_t rate = 0;
  uint16_t channels = 0;
  std::wstring device_id;  // the capture endpoint recorded from ("" for a test source)
  std::vector<DestResume> dests;
};

// Get/WritePrivateProfileStringW on a UTF-16 INI file: section [State] and one section
// [Dest0], [Dest1], ... per destination. Each save rewrites every section it owns.
// save() and clear() return false if the file could not be written (its folder must exist).
class StateStore {
 public:
  explicit StateStore(std::wstring ini_path);
  bool save(const RecordingState&);
  // nullopt if nothing was ever saved; recording false after clear().
  std::optional<RecordingState> load() const;
  bool clear();

 private:
  std::wstring path_;
};

struct RepairReport {
  std::vector<std::wstring> repaired;  // files whose manifest entry was closed (WAV headers fixed)
  std::vector<std::wstring> failed;    // manifests or files that could not be read or repaired
  SampleIndex audio_end = 0;           // where the recorded audio ends (>= last_sample): for make_resume
  // Every readable current manifest already ends the session with session_end (a clean stop
  // whose crash state could not be cleared): nothing to resume, and no gap was written.
  bool session_ended = false;
  // The current manifests in which the session is still live (readable, not ended).
  std::vector<std::wstring> live_heads;
};

// For each destination, follows its manifest's prev_manifest chain. Every file_open
// without a file_close is repaired (WAV: repair_wav, capped at the failover's confirmed
// sample if the file failed over; Opus: nothing to repair) and closed with a file_close in
// the manifest that opened it (or the current one if that cannot be written). The current
// manifest then gets gap_start(crash) at last_sample, or where the repaired audio ends if
// that is later (the state is saved every flush_ms, so up to that much may follow it).
// Never throws; a missing drive or file is reported in `failed` and the scan goes on.
// A manifest whose last session_start for this session is followed by session_end gets no
// gap_start (see RepairReport::session_ended).
RepairReport repair_open_files(IFileSystem&, const RecordingState&);

// When a crashed session will not be resumed: appends session_end(reason) at
// RepairReport::audio_end to each manifest in RepairReport::live_heads, so the session is
// closed and a later scan sees it as ended. Call with the report of repair_open_files. Best
// effort: returns the manifests that could not be written.
std::vector<std::wstring> end_crashed_session(IFileSystem&, const RecordingState&, const RepairReport&,
                                              const std::wstring& reason);

struct ResumeInfo {
  RecordingState prior;
  SampleIndex resume_sample;
};

// resume_sample = last_sample + (now.utc_ms - last_utc_ms) * rate / 1000, but never before
// audio_end (RepairReport::audio_end), so the resumed samples never overlap the repaired
// files even if the wall clock went back.
ResumeInfo make_resume(const RecordingState&, WallTime now, SampleIndex audio_end = 0);

}  // namespace sl
