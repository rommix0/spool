#pragma once
// Wall/steady clock abstraction so naming and rotation are testable.
#include <cstdint>
#include <mutex>
#include <string>

#include "core/types.h"

namespace sl {

class IClock {
 public:
  virtual ~IClock() = default;
  virtual WallTime now() const = 0;
  virtual uint64_t steady_ms() const = 0;  // monotonic, unaffected by wall-clock changes
};

// The process-wide clock backed by the OS. local_offset_min is the current
// DST-aware bias (local = utc + offset; US Eastern daylight = -240).
IClock& system_clock();

// Thread-safe test clock. set() changes wall time only; advance_ms() advances both.
class FakeClock : public IClock {
 public:
  explicit FakeClock(WallTime start = {}, uint64_t steady_start_ms = 0)
      : wall_(start), steady_(steady_start_ms) {}
  WallTime now() const override;
  uint64_t steady_ms() const override;
  void set(WallTime t);
  void advance_ms(int64_t ms);

 private:
  mutable std::mutex m_;
  WallTime wall_;
  uint64_t steady_;
};

// "2026-10-16T14:00:00.000Z" (UTC; ignores local_offset_min).
std::string iso_utc(WallTime t);

struct LocalParts {
  int year, month, day, hour, minute, second, ms;
};
// Local civil time from utc_ms + local_offset_min, pure arithmetic (no OS calls).
LocalParts local_parts(WallTime t);

}  // namespace sl
