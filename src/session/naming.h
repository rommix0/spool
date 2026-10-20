#pragma once
// File naming (templates, sanitizing, collision handling) and wall-clock
// aligned rotation boundaries.
#include <cstdint>
#include <string>

#include "core/types.h"
#include "io/file_system.h"

namespace spool {

// <>:"/\|?* and chars < 0x20 -> '_'; trailing '.' and ' ' trimmed; empty -> "_".
// A reserved device name before the first '.' (CON, PRN, AUX, NUL, COM1-9, LPT1-9, any
// case) gets a leading '_'.
std::wstring sanitize_component(std::wstring s);

struct NameInputs {
  std::wstring session, dest;
  WallTime first_sample;
  uint32_t n = 0;    // file index, zero-padded to 3 digits
  uint32_t seg = 0;  // failover segment: 0 -> "", 1 -> "_b", 2 -> "_c", ... 25 -> "_z", then "_<seg>"
};

// Tokens: {session} {dest} {date} (local YYYY-MM-DD) {time} (local HHMMSS) {n} {seg}.
// Unknown {tokens} stay literal. Token values and the final stem are sanitized.
std::wstring expand_name(const std::wstring& tmpl, const NameInputs& in);

// dir\stem.ext, else dir\stem_1.ext, stem_2.ext, ... (first that does not exist).
std::wstring unique_path(IFileSystem& fs, const std::wstring& dir, const std::wstring& stem,
                         const std::wstring& ext);

constexpr SampleIndex kNoRotation = INT64_MAX;

// Next local-time multiple of period_min since local midnight, strictly after
// `now`, as a sample index. period_min == 0 -> kNoRotation.
SampleIndex first_rotation_boundary(SampleIndex now, WallTime now_wall, uint32_t rate,
                                    uint32_t period_min);
// prev_boundary + period_min * 60 * rate.
SampleIndex next_rotation_boundary(SampleIndex prev_boundary, uint32_t rate, uint32_t period_min);

}  // namespace spool
