#include "core/clock.h"

#include <windows.h>

#include <cstdio>

namespace spool {
namespace {

int64_t floor_div(int64_t a, int64_t b) {
  int64_t q = a / b;
  if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
  return q;
}

// Days since 1970-01-01 -> civil date (Howard Hinnant's days_from_civil inverse).
void civil_from_days(int64_t z, int& y, int& m, int& d) {
  z += 719468;
  const int64_t era = floor_div(z, 146097);
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
  m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
  y = static_cast<int>(yy + (m <= 2 ? 1 : 0));
}

LocalParts parts_of(int64_t ms_since_epoch) {
  const int64_t days = floor_div(ms_since_epoch, 86400000);
  const int64_t in_day = ms_since_epoch - days * 86400000;
  LocalParts p{};
  civil_from_days(days, p.year, p.month, p.day);
  p.hour = static_cast<int>(in_day / 3600000);
  p.minute = static_cast<int>(in_day / 60000 % 60);
  p.second = static_cast<int>(in_day / 1000 % 60);
  p.ms = static_cast<int>(in_day % 1000);
  return p;
}

class SystemClock : public IClock {
 public:
  SystemClock() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    freq_ = static_cast<uint64_t>(f.QuadPart);
  }
  WallTime now() const override {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const uint64_t t = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    constexpr uint64_t kEpochDiff100ns = 116444736000000000ULL;  // 1601 -> 1970
    WallTime w;
    w.utc_ms = static_cast<int64_t>((t - kEpochDiff100ns) / 10000);
    TIME_ZONE_INFORMATION tz;
    LONG bias = 0;  // minutes, utc = local + bias
    switch (GetTimeZoneInformation(&tz)) {
      case TIME_ZONE_ID_DAYLIGHT: bias = tz.Bias + tz.DaylightBias; break;
      case TIME_ZONE_ID_STANDARD: bias = tz.Bias + tz.StandardBias; break;
      case TIME_ZONE_ID_UNKNOWN: bias = tz.Bias; break;
      default: break;  // failure: treat as UTC
    }
    w.local_offset_min = -static_cast<int32_t>(bias);
    return w;
  }
  uint64_t steady_ms() const override {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    const uint64_t n = static_cast<uint64_t>(c.QuadPart);
    return n / freq_ * 1000 + n % freq_ * 1000 / freq_;  // avoids overflow of n * 1000
  }

 private:
  uint64_t freq_ = 1;
};

}  // namespace

IClock& system_clock() {
  static SystemClock c;
  return c;
}

WallTime FakeClock::now() const {
  std::lock_guard<std::mutex> g(m_);
  return wall_;
}
uint64_t FakeClock::steady_ms() const {
  std::lock_guard<std::mutex> g(m_);
  return steady_;
}
void FakeClock::set(WallTime t) {
  std::lock_guard<std::mutex> g(m_);
  wall_ = t;
}
void FakeClock::advance_ms(int64_t ms) {
  std::lock_guard<std::mutex> g(m_);
  wall_.utc_ms += ms;
  steady_ += static_cast<uint64_t>(ms);
}

LocalParts local_parts(WallTime t) {
  return parts_of(t.utc_ms + static_cast<int64_t>(t.local_offset_min) * 60000);
}

std::string iso_utc(WallTime t) {
  const LocalParts p = parts_of(t.utc_ms);
  char buf[40];
  std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", p.year, p.month, p.day,
                p.hour, p.minute, p.second, p.ms);
  return buf;
}

}  // namespace spool
