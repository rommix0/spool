#include "app/lifecycle.h"

#include <windows.h>

#include <algorithm>

#include "core/sample_ring.h"

namespace spool {

namespace {
constexpr size_t kMiB = size_t(1) << 20;
constexpr size_t kProcessExtra = 64 * kMiB;   // everything besides the ring
constexpr size_t kMaxHeadroom = 256 * kMiB;  // max above min
}  // namespace

WorkingSetSize working_set_for_ring(uint32_t ring_seconds, uint32_t rate, uint16_t channels) {
  const size_t frames = size_t(std::max<uint32_t>(ring_seconds, 1)) * rate;
  const size_t ring = SampleRing::capacity_for(frames) * std::max<uint16_t>(channels, 1) * sizeof(float);
  return WorkingSetSize{ring + kProcessExtra, ring + kProcessExtra + kMaxHeadroom};
}

bool raise_working_set(const WorkingSetSize& w) {
  return SetProcessWorkingSetSizeEx(GetCurrentProcess(), w.min_bytes, w.max_bytes,
                                    QUOTA_LIMITS_HARDWS_MIN_DISABLE | QUOTA_LIMITS_HARDWS_MAX_DISABLE) != FALSE;
}

namespace {

void add(EventLog& log, IClock& clock, Severity sev, std::wstring text) {
  log.add(sev, clock.now(), L"", std::move(text));
}

void clear_state(StateStore& store, EventLog& log, IClock& clock) {
  if (!store.clear())
    add(log, clock, Severity::Error, L"Cannot clear the recording state: the next launch checks the manifests again");
}

void log_not_closed(const std::vector<std::wstring>& paths, EventLog& log, IClock& clock) {
  for (const std::wstring& p : paths) add(log, clock, Severity::Error, L"Couldn't close the crashed session in " + p);
}

}  // namespace

RecoveryWork repair_crashed_session(IFileSystem& fs, const RecordingState& st, bool give_up) {
  RecoveryWork w;
  w.rep = repair_open_files(fs, st);
  if (give_up && !w.rep.session_ended) w.not_closed = end_crashed_session(fs, st, w.rep, L"resume_failed");
  return w;
}

LaunchRecoveryResult finish_launch_recovery(const RecordingState& st, const RecoveryWork& w, bool give_up,
                                            const SessionConfig& cfg, SessionManager& mgr, StateStore& store,
                                            IFileSystem& fs, IClock& clock, EventLog& log) {
  for (const std::wstring& p : w.rep.repaired) add(log, clock, Severity::Info, L"Repaired after the crash: " + p);
  for (const std::wstring& p : w.rep.failed) add(log, clock, Severity::Error, L"Couldn't repair after the crash: " + p);
  if (give_up) {
    log_not_closed(w.not_closed, log, clock);
    clear_state(store, log, clock);
    add(log, clock, Severity::Error,
        L"The input device didn't return within " + std::to_wstring(kResumeWaitMs / 60000) +
            L" min: the crashed session " + st.session_id + L" was not resumed");
    return {LaunchRecovery::GaveUp, {}};
  }
  if (w.rep.session_ended) {
    clear_state(store, log, clock);
    add(log, clock, Severity::Info, L"The last session (" + st.session_id + L") had ended cleanly: nothing to resume");
    return {LaunchRecovery::EndedCleanly, {}};
  }
  const ResumeInfo resume = make_resume(st, clock.now(), w.rep.audio_end);
  std::wstring err;
  if (!mgr.start(cfg, &resume, &err)) {
    // Nothing will resume the session: close it in its manifests (best effort; these
    // drives were just read, so this rarely waits).
    log_not_closed(end_crashed_session(fs, st, w.rep, L"resume_failed"), log, clock);
    clear_state(store, log, clock);
    add(log, clock, Severity::Error, L"Couldn't resume the crashed session: " + err);
    return {LaunchRecovery::Failed, err};
  }
  return {LaunchRecovery::Resumed, {}};
}

LaunchRecoveryResult recover_crashed_session(const RecordingState& st, const SessionConfig& cfg, SessionManager& mgr,
                                             StateStore& store, IFileSystem& fs, IClock& clock, EventLog& log) {
  return finish_launch_recovery(st, repair_crashed_session(fs, st, false), false, cfg, mgr, store, fs, clock, log);
}

LaunchRecoveryResult resume_when_input_ready(const RecordingState& st, bool input_ready, uint64_t waited_ms,
                                             const SessionConfig& cfg, SessionManager& mgr, StateStore& store,
                                             IFileSystem& fs, IClock& clock, EventLog& log) {
  if (input_ready) return recover_crashed_session(st, cfg, mgr, store, fs, clock, log);
  if (waited_ms < kResumeWaitMs) return {LaunchRecovery::Waiting, {}};
  return finish_launch_recovery(st, repair_crashed_session(fs, st, true), true, cfg, mgr, store, fs, clock, log);
}

CaptureConfig resume_input(const CaptureConfig& settings_input, const RecordingState& st) {
  CaptureConfig c = settings_input;
  if (!st.device_id.empty()) c.device_id = st.device_id;
  return c;
}

bool resume_needs_open(const std::wstring& open_device_id, bool open, const std::wstring& pinned_id) {
  return !open || (!pinned_id.empty() && open_device_id != pinned_id);
}

bool auto_start_after(LaunchRecovery r, bool start_on_launch) {
  return start_on_launch &&
         (r == LaunchRecovery::EndedCleanly || r == LaunchRecovery::Failed || r == LaunchRecovery::GaveUp);
}

}  // namespace spool
