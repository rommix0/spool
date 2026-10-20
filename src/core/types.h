#pragma once
#include <cstdint>

namespace spool {
using SampleIndex = int64_t;
struct StreamFormat {
  uint32_t sample_rate = 0;
  uint16_t channels = 0;
  bool operator==(const StreamFormat&) const = default;
};
struct WallTime {
  int64_t utc_ms = 0;
  int32_t local_offset_min = 0;  // local = utc + offset
};
}  // namespace spool
