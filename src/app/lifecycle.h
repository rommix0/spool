#pragma once
// App lifecycle helpers, kept out of the window code so they can be tested: the process
// working set the capture ring needs to be locked in memory, and resuming a crashed
// session at launch (§8.5).
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/clock.h"
#include "io/file_system.h"
#include "session/event_log.h"
#include "session/recovery.h"
#include "session/session_manager.h"

namespace sl {

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

enum class LaunchRecovery { Resumed, EndedCleanly, Failed };

struct LaunchRecoveryResult {
  LaunchRecovery outcome;
  std::wstring error;  // Failed: why the resume did not start
};

// For a crash state with recording=1: repair_open_files, logging each repaired file (Info)
// and each file or manifest that could not be repaired (Error). If the manifests show the
// session had already ended, nothing is resumed. Otherwise the session resumes with
// make_resume(state, clock.now(), audio_end). When nothing is resumed (ended, or the resume
// failed to start) the state is cleared, so the next launch does not try again. The
// manager must be idle and use `store` as its state store.
LaunchRecoveryResult recover_crashed_session(const RecordingState&, const SessionConfig&, SessionManager&,
                                             StateStore& store, IFileSystem&, IClock&, EventLog&);

}  // namespace sl
