#pragma once
// Display strings for the dashboard.
#include <cstdint>
#include <optional>
#include <string>

#include "audio/capture_source.h"
#include "core/types.h"
#include "session/config.h"

namespace sl {

std::wstring format_elapsed(double seconds);    // "02:14:37"
std::wstring format_time_left(double hours);    // "31 h left", "45 min left" (< 1 h)
// "WAV 16-bit · ch 1–2", "Opus 128k · ch 1, 3"; empty channels = all input_channels.
std::wstring format_summary(const DestinationConfig& d, uint16_t input_channels);
// "USB mic · 48 kHz · 2 ch · Raw"; 44100 Hz -> "44.1 kHz".
std::wstring format_input_line(const std::wstring& device, StreamFormat fmt, CaptureMode mode);
// "Buffer 3% · Gaps 0 · Discontinuities 0"; fill is a fraction (clamped to 0..1).
std::wstring format_buffer_line(double fill, uint64_t gaps, uint64_t discontinuities);
// format_time_left for free_bytes at bytes_per_s, or "—" when the free space is unknown
// or the data rate is 0 (never passes inf or NaN to format_time_left).
std::wstring format_folder_time_left(std::optional<uint64_t> free_bytes, uint64_t bytes_per_s);
// Local time of day, "14:30:05".
std::wstring format_clock(WallTime t);

}  // namespace sl
