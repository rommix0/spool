#pragma once
// Record-start checks (§8.3) and the folder health check shared with failover (§4.3).
#include <cstdint>
#include <string>
#include <vector>

#include "io/file_system.h"
#include "session/config.h"

namespace spool {

struct PreflightResult {
  bool can_start;  // false only if no destination has a usable folder (see preflight)
  std::vector<std::wstring> warnings, errors;
};

// Probes every folder of every destination. Warns on a folder that is on the same volume
// as an earlier folder in its chain, an unwritable folder, a folder below the low-water
// mark and a primary with under 1 h of space. A destination with no folder that is writable
// and above its low-water mark (what the writer requires) is an error: it starts in
// LastResort. can_start is false only if every destination is in that state.
PreflightResult preflight(const std::vector<DestinationConfig>&, const SessionParams&, IFileSystem&);

// ensure_dir + create, write, flush and remove a probe file + free space >= low_water_bytes.
// The probe is ".spool_probe_<sanitized prober>_<pid>_<counter>.tmp", unique per call, so
// concurrent probers of one folder never collide. A probe that cannot be removed does not
// make the folder unhealthy.
bool folder_healthy(IFileSystem&, const std::wstring& dir, uint64_t low_water_bytes,
                    const std::wstring& prober = L"preflight");

// max(low_water_minutes of audio at data_rate, low_water_min_bytes).
uint64_t low_water_bytes(const SessionParams&, uint64_t data_rate);

}  // namespace spool
