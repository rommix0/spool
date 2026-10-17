#pragma once
// One output stream (§4.2): a writer thread that reads the capture ring through its
// own cursor, extracts its channels, writes WAV or Opus files with wall-clock aligned
// rotation, confirms data to disk every flush_ms and keeps a JSON Lines manifest.
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
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

namespace sl {

struct CmdMarker { SampleIndex at; std::wstring name; };
struct CmdSplit { SampleIndex at; };
struct CmdAppendFolder { std::wstring path; };
struct CmdGapStart { SampleIndex at; std::string reason; };
struct CmdGapEnd { SampleIndex at; std::string reason; };
struct CmdCloseForGap { SampleIndex at; };  // write through `at`, close, no file until CmdResumeAt
struct CmdResumeAt { SampleIndex at; };     // open the next file at `at` and resume writing
struct CmdClockPair { SampleIndex at; WallTime wall; };
struct CmdAcknowledge {};
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
  bool needs_ack;
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

class Destination {
 public:
  Destination(DestinationConfig, const SessionParams&, SampleRing&, IFileSystem&, IClock&, EventLog&,
              DestResumeSeed = {});
  ~Destination();  // stops at the current progress if still running
  Destination(const Destination&) = delete;
  Destination& operator=(const Destination&) = delete;

  void start(SampleIndex at);
  void post(DestCommand);      // any thread; mutex-protected deque drained by the worker
  void stop(SampleIndex end);  // writes through end, finalizes, waits <= 3 s, else abandons the worker
  SampleIndex progress() const { return cursor_.load(std::memory_order_acquire); }
  void declare_stalled();             // Task 10
  void disable(std::wstring reason);  // state Disabled, nothing (more) written
  DestStatus status() const;
  uint64_t data_rate() const;  // bytes/s: WAV rate*ch*bytes; Opus bitrate/8

 private:
  class Writer;  // one generation of the writer thread; see destination.cpp

  void log(Severity, std::wstring text);
  void set_state(DestState);  // Disabled is sticky

  // Immutable after construction.
  const SessionParams sp_;
  SampleRing& ring_;
  IFileSystem& fs_;
  IClock& clock_;
  EventLog& log_;
  const DestResumeSeed seed_;
  const std::wstring name_;
  std::vector<uint16_t> map_;  // ring channel per destination channel

  // Shared with the writer. m_ is never held across file I/O.
  mutable std::mutex m_;
  std::deque<DestCommand> cmds_;
  DestStatus st_;
  bool stop_requested_ = false;
  SampleIndex stop_end_ = 0;
  std::string stop_reason_;
  std::atomic<SampleIndex> cursor_{0};

  // Owned by the controlling thread. An abandoned writer (and its encoder) is leaked,
  // never destroyed: its thread may still be inside a stalled write.
  Writer* writer_ = nullptr;
  std::thread thread_;
};

// The stall watchdog condition: the cursor lags the capture position by more than
// stall_percent of the ring.
bool is_stalled(SampleIndex write_index, SampleIndex progress, size_t capacity, uint32_t stall_percent);

}  // namespace sl
