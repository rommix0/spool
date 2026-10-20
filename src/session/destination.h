#pragma once
// One output stream (§4.2): a writer thread that reads the capture ring through its
// own cursor, extracts its channels, writes WAV or Opus files with wall-clock aligned
// rotation, confirms data to disk every flush_ms and keeps a JSON Lines manifest.
// Failures (§4.3, §8.2) fail over along the folder chain from the last confirmed
// sample; with no folder left the destination waits in LastResort and retries.
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "core/clock.h"
#include "core/sample_ring.h"
#include "core/types.h"
#include "io/file_system.h"
#include "session/config.h"
#include "session/event_log.h"

namespace spool {

struct CmdMarker { SampleIndex at; std::wstring name; };
struct CmdSplit { SampleIndex at; };
struct CmdAppendFolder { std::wstring path; };  // in LastResort it is tried at once
struct CmdGapStart { SampleIndex at; std::string reason; };
struct CmdGapEnd { SampleIndex at; std::string reason; };
struct CmdCloseForGap { SampleIndex at; };  // write through `at`, close, no file until CmdResumeAt
struct CmdResumeAt { SampleIndex at; };     // open the next file at `at` and resume writing
struct CmdClockPair { SampleIndex at; WallTime wall; };
struct CmdAcknowledge {};                   // clears needs_ack
using DestCommand = std::variant<CmdMarker, CmdSplit, CmdAppendFolder, CmdGapStart, CmdGapEnd,
                                 CmdCloseForGap, CmdResumeAt, CmdClockPair, CmdAcknowledge>;

enum class DestState { Idle, Writing, FailingOver, LastResort, GapClosed, Stopped, Disabled };
enum class FolderHealth { Active, Standby, Failed };

struct FolderStatus {
  std::wstring path;
  FolderHealth health;
  std::optional<uint64_t> free_bytes;
  double hours_left;
};

struct DestStatus {
  std::wstring name, current_file;
  DestinationConfig config;
  DestState state;
  bool needs_ack;  // set by a failover or LastResort, cleared by CmdAcknowledge
  std::vector<FolderStatus> folders;
  uint64_t gaps;
  SampleIndex cursor, confirmed;
  uint32_t n;  // current file index + manifest, for crash state (Task 13)
  std::wstring manifest_path;
};

struct DestResumeSeed {
  uint32_t first_n = 1;
  std::wstring prev_manifest;  // appended to instead of creating a new manifest
};

// start, stop and declare_stalled are called from one controlling thread; post, status,
// progress and disable from any thread. The ring, file system, clock and event log must
// outlive the Destination; a writer abandoned on a hung drive never touches the ring,
// clock or log again, and touches the file system only from inside the call that hung.
class Destination {
 public:
  Destination(DestinationConfig, const SessionParams&, SampleRing&, IFileSystem&, IClock&, EventLog&,
              DestResumeSeed = {});
  ~Destination();  // stops at the current progress if still running
  Destination(const Destination&) = delete;
  Destination& operator=(const Destination&) = delete;

  void start(SampleIndex at);
  void post(DestCommand);      // mutex-protected deque drained by the worker
  // Writes through end and finalizes, with session_end reason `reason`; returns within 3 s
  // even if a drive hangs (the stuck worker is then abandoned).
  void stop(SampleIndex end, std::string reason = "stop");
  SampleIndex progress() const;
  // The stall watchdog fired (see is_stalled): the worker is abandoned where it hangs, its
  // folder is quarantined, and a new one continues from the last confirmed sample in the
  // next healthy folder. The watchdog may call this whenever is_stalled() holds:
  // - It is a no-op when Stopped or Disabled.
  // - In Writing and FailingOver it is honored. In any other state (Idle, GapClosed,
  //   LastResort) it is honored only while the writer has been inside one file I/O call
  //   (probe, open, manifest flush, ...) for at least 2 s of steady time: a hang in the
  //   first open, a LastResort retry or a resume reopen. A writer idle on purpose, or in a
  //   routine flush, is not stalled and keeps its folders.
  // - A hang reopening the seed manifest (crash resume) is never retried: the next
  //   generation starts a new manifest that names it as prev_manifest.
  // - After a stall takeover it is ignored until progress() reaches the capture position
  //   recorded at the takeover (the new writer starts that far behind) or a grace deadline
  //   passes, whichever comes first. Deadline = takeover steady time + clamp(headroom / 2,
  //   0.5 s, 5 s), headroom = capacity * (100 - stall_percent) / 100 / rate seconds, so a
  //   takeover writer that hangs as well is rescued while its data is still in the ring.
  void declare_stalled();
  void disable(std::wstring reason);  // state Disabled, nothing (more) written
  // Test hook (crash simulation): the writer stops where it is without finalizing or
  // flushing anything; its encoder and manifest are leaked unclosed. State Stopped.
  void abandon_for_test();
  DestStatus status() const;
  uint64_t data_rate() const;  // bytes/s: WAV rate*ch*bytes; Opus bitrate/8

 private:
  struct Core;   // state shared with the worker generations; outlives the Destination
  class Writer;  // one generation of the writer thread; see destination.cpp

  void log(Severity, std::wstring text);
  void set_state(DestState);  // Disabled is sticky

  std::shared_ptr<Core> core_;
  // A replaced or abandoned Writer is leaked, never destroyed: its thread may still be
  // inside a stalled write. Written under core_->m (disable() reads it from any thread).
  Writer* writer_ = nullptr;
  std::thread thread_;  // controlling thread only
};

// Bytes per second written by a destination with `channels` channels at `rate`.
uint64_t data_rate(const DestinationConfig&, uint32_t rate, size_t channels);

// The stall watchdog condition: the cursor lags the capture position by more than
// stall_percent of the ring.
bool is_stalled(SampleIndex write_index, SampleIndex progress, size_t capacity, uint32_t stall_percent);

}  // namespace spool
