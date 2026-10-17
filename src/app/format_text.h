#pragma once
// Display strings for the dashboard.
#include <cstdint>
#include <string>

#include "session/config.h"

namespace sl {

std::wstring format_elapsed(double seconds);    // "02:14:37"
std::wstring format_time_left(double hours);    // "31 h left", "45 min left" (< 1 h)
// "WAV 16-bit · ch 1–2", "Opus 128k · ch 1, 3"; empty channels = all input_channels.
std::wstring format_summary(const DestinationConfig& d, uint16_t input_channels);

}  // namespace sl
