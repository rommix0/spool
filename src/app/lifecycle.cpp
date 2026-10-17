#include "app/lifecycle.h"

#include <windows.h>

#include <algorithm>

#include "core/sample_ring.h"

namespace sl {

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

LaunchRecoveryResult recover_crashed_session(const RecordingState& st, const SessionConfig& cfg, SessionManager& mgr,
                                             StateStore& store, IFileSystem& fs, IClock& clock, EventLog& log) {
  const auto add = [&](Severity sev, std::wstring text) { log.add(sev, clock.now(), L"", std::move(text)); };
  const auto clear = [&] {
    if (!store.clear())
      add(Severity::Error, L"Cannot clear the recording state: the next launch checks the manifests again");
  };
  const RepairReport rep = repair_open_files(fs, st);
  for (const std::wstring& p : rep.repaired) add(Severity::Info, L"Repaired after the crash: " + p);
  for (const std::wstring& p : rep.failed) add(Severity::Error, L"Couldn't repair after the crash: " + p);
  if (rep.session_ended) {
    clear();
    add(Severity::Info, L"The last session (" + st.session_id + L") had ended cleanly: nothing to resume");
    return {LaunchRecovery::EndedCleanly, {}};
  }
  const ResumeInfo resume = make_resume(st, clock.now(), rep.audio_end);
  std::wstring err;
  if (!mgr.start(cfg, &resume, &err)) {
    clear();
    add(Severity::Error, L"Couldn't resume the crashed session: " + err);
    return {LaunchRecovery::Failed, err};
  }
  return {LaunchRecovery::Resumed, {}};
}

}  // namespace sl
