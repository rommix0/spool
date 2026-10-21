#pragma once
// App lifecycle helpers, kept out of the window code so they can be tested: the process
// working set the capture ring needs to be locked in memory, and resuming a crashed
// session at launch (§8.5).
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "audio/capture_source.h"
#include "core/clock.h"
#include "io/file_system.h"
#include "session/event_log.h"
#include "session/recovery.h"
#include "session/session_manager.h"

namespace spool {

struct WorkingSetSize {
  size_t min_bytes, max_bytes;
};

// min = the capture ring's bytes (ring_seconds * rate frames rounded up to a power of two
// as SampleRing does; at least 1 s and 1 channel, like the session) + 64 MiB for the rest
// of the process; max = min + 256 MiB.
WorkingSetSize working_set_for_ring(uint32_t ring_seconds, uint32_t rate, uint16_t channels);

// SetProcessWorkingSetSizeEx with both limits soft (QUOTA_LIMITS_HARDWS_MIN_DISABLE |
// QUOTA_LIMITS_HARDWS_MAX_DISABLE), so that VirtualLock of the ring can succeed. Call it
// before the session creates its ring. False if Windows refused; recording still works
// (the session logs that the ring is not locked).
bool raise_working_set(const WorkingSetSize&);

enum class LaunchRecovery {
  Resumed,
  EndedCleanly,  // the manifests show the session had ended: nothing to resume
  Failed,        // the resume did not start
  Waiting,       // the input device is not available yet: nothing was touched
  GaveUp,        // the input did not return within kResumeWaitMs
};

struct LaunchRecoveryResult {
  LaunchRecovery outcome;
  std::wstring error;  // Failed: why the resume did not start
};

// How long a crashed session waits at launch for its input device (R18).
inline constexpr uint64_t kResumeWaitMs = 30ull * 60 * 1000;

// For a crash state with recording=1: repair_open_files, logging each repaired file (Info)
// and each file or manifest that could not be repaired (Error). If the manifests show the
// session had already ended, nothing is resumed. Otherwise the session resumes with
// make_resume(state, clock.now(), audio_end). If that fails to start, the crashed manifests
// get session_end(resume_failed). When nothing is resumed the state is cleared, so the next
// launch does not try again. The manager must be idle and use `store` as its state store.
LaunchRecoveryResult recover_crashed_session(const RecordingState&, const SessionConfig&, SessionManager&,
                                             StateStore& store, IFileSystem&, IClock&, EventLog&);

// R18, called at launch and then about once a second while it returns Waiting. With the
// input ready: recover_crashed_session. Without it: Waiting (nothing repaired, the state
// kept) until waited_ms reaches kResumeWaitMs; then the session is repaired, closed with
// session_end(resume_failed), the state cleared and an error logged (GaveUp).
LaunchRecoveryResult resume_when_input_ready(const RecordingState&, bool input_ready, uint64_t waited_ms,
                                             const SessionConfig&, SessionManager&, StateStore& store, IFileSystem&,
                                             IClock&, EventLog&);

// The two halves of the above, so the app can repair on a worker thread (a hung drive
// must not freeze the window) and resume on the UI thread.
// The file work only (no manager, store or log): repair_open_files and, with give_up,
// end_crashed_session(resume_failed) unless the session had ended.
struct RecoveryWork {
  RepairReport rep;
  std::vector<std::wstring> not_closed;  // give_up: manifests the session_end couldn't be written to
};
RecoveryWork repair_crashed_session(IFileSystem&, const RecordingState&, bool give_up);
// Logs what the repair did, then ends as recover_crashed_session (give_up false) or the
// timed-out resume_when_input_ready (give_up true) does.
LaunchRecoveryResult finish_launch_recovery(const RecordingState&, const RecoveryWork&, bool give_up,
                                            const SessionConfig&, SessionManager&, StateStore& store, IFileSystem&,
                                            IClock&, EventLog&);

// Whether start_on_launch applies after the crash recovery ended this way (nothing resumed).
bool auto_start_after(LaunchRecovery, bool start_on_launch);

// R19: a crashed session resumes only on the device it recorded from. The input settings
// with the state's device id, if it has one (not for a test source, or a pre-R19 state).
CaptureConfig resume_input(const CaptureConfig& settings_input, const RecordingState&);
// Whether the source must be (re)opened before the resume: it is not open, or it is open
// on another device than the pinned one.
bool resume_needs_open(const std::wstring& open_device_id, bool open, const std::wstring& pinned_id);

}  // namespace spool
