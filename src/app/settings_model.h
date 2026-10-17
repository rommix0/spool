#pragma once
// Pure helpers behind the settings dialog (spec 10.5): combo choices and labels, the
// channel checklist, the file name preview, number fields and per-destination checks.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/types.h"
#include "session/config.h"

namespace sl {

// Rotation periods in minutes: Off, 5, 10, 15, 30, 60, plus `current` (in order) when it is
// another value, such as one from a hand-edited INI.
std::vector<uint32_t> rotation_choices(uint32_t current);
std::wstring rotation_label(uint32_t minutes);  // "Off", "30 min"

// Opus bitrates 32k…256k, plus `current` (in order) when it is another nonzero value.
std::vector<uint32_t> bitrate_choices(uint32_t current);
std::wstring bitrate_label(uint32_t bps);  // "64k"
// The bitrate the writer uses: opus_bitrate, or for 0 the default (64k mono, 128k otherwise).
uint32_t effective_bitrate(uint32_t opus_bitrate, size_t channels);

// WAV bit depth <-> combo index (16-bit, 24-bit, 32-bit float). Bad indexes give 16-bit.
int depth_index(WavDepth);
WavDepth depth_from_index(int);

// Channel checklist rows: one per device channel, plus rows up to any channel the destination
// names that the device lacks (so validation can still name it). Empty channels = all checked.
std::vector<bool> channel_checks(const std::vector<uint16_t>& channels, uint16_t device_channels);
// Back from the checklist: nothing checked -> nullopt; otherwise the checked rows, 0-based.
// It gives {} ("all input channels") only when the destination already meant all channels
// (was_all), it isn't Opus (1-2 channels, so it must not follow a bigger device), and every
// device row is checked. An explicit list stays explicit.
std::optional<std::vector<uint16_t>> channels_from_checks(const std::vector<bool>& checks,
                                                          uint16_t device_channels, bool was_all,
                                                          OutFormat format);

// expand_name(tmpl) for file 1 with no failover segment at `now`, plus ".wav" or ".opus".
std::wstring name_preview(const std::wstring& tmpl, const std::wstring& session, const std::wstring& dest,
                          OutFormat, WallTime now);

// "Destination N" for the lowest N >= size() + 1 that no destination uses (case-insensitive).
std::wstring new_destination_name(const std::vector<DestinationConfig>&);

// A whole decimal number in [lo, hi] (surrounding spaces ignored); nullopt otherwise.
std::optional<uint32_t> parse_number(const std::wstring& text, uint32_t lo, uint32_t hi);
// "Ring length must be 1–3600 seconds." (unit is appended as given: L" seconds", L"%").
std::wstring range_message(const std::wstring& what, uint32_t lo, uint32_t hi, const std::wstring& unit);

// validate()'s checks for a single destination, after "Enter a destination name." when the
// name is blank. Duplicate names are checked across the whole list by validate().
std::vector<std::wstring> destination_problems(const DestinationConfig&, uint16_t device_channels);

// Raw mode's read-only rate: "48000 Hz, set in Windows sound settings" (rate unknown: no number).
std::wstring raw_rate_note(uint32_t rate);

}  // namespace sl
