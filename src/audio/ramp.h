#pragma once
// Test-pattern sample values: exact in float32, period 2^17 frames.
#include <cstdint>

#include "core/types.h"

namespace sl {
inline float ramp_value(SampleIndex frame, uint32_t ch) {
  return float(((uint64_t(frame) * 8u) + ch) & 0xFFFFFu) / 1048576.0f;
}
}  // namespace sl
