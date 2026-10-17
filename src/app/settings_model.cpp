#include "app/settings_model.h"

#include <algorithm>

#include "app/settings.h"
#include "app/text_util.h"
#include "session/naming.h"

namespace sl {

namespace {

// `standard` plus `extra` in sorted position when it is not already there.
std::vector<uint32_t> with_extra(std::vector<uint32_t> standard, uint32_t extra) {
  if (std::find(standard.begin(), standard.end(), extra) == standard.end())
    standard.insert(std::upper_bound(standard.begin(), standard.end(), extra), extra);
  return standard;
}

}  // namespace

std::vector<uint32_t> rotation_choices(uint32_t current) { return with_extra({0, 5, 10, 15, 30, 60}, current); }

std::wstring rotation_label(uint32_t minutes) {
  return minutes == 0 ? L"Off" : std::to_wstring(minutes) + L" min";
}

std::vector<uint32_t> bitrate_choices(uint32_t current) {
  const std::vector<uint32_t> standard{32000, 48000, 64000, 96000, 128000, 160000, 192000, 256000};
  return current == 0 ? standard : with_extra(standard, current);
}

std::wstring bitrate_label(uint32_t bps) { return std::to_wstring(bps / 1000) + L"k"; }

uint32_t effective_bitrate(uint32_t opus_bitrate, size_t channels) {
  if (opus_bitrate) return opus_bitrate;
  return channels == 1 ? 64000 : 128000;
}

int depth_index(WavDepth d) {
  switch (d) {
    case WavDepth::Int24: return 1;
    case WavDepth::Float32: return 2;
    case WavDepth::Int16: break;
  }
  return 0;
}

WavDepth depth_from_index(int i) {
  return i == 1 ? WavDepth::Int24 : i == 2 ? WavDepth::Float32 : WavDepth::Int16;
}

std::vector<bool> channel_checks(const std::vector<uint16_t>& channels, uint16_t device_channels) {
  size_t rows = device_channels;
  for (const uint16_t c : channels) rows = std::max(rows, size_t(c) + 1);
  std::vector<bool> out(rows, channels.empty());
  for (const uint16_t c : channels) out[c] = true;
  return out;
}

std::optional<std::vector<uint16_t>> channels_from_checks(const std::vector<bool>& checks,
                                                          uint16_t device_channels, bool was_all,
                                                          OutFormat format) {
  std::vector<uint16_t> out;
  for (size_t i = 0; i < checks.size(); ++i)
    if (checks[i]) out.push_back(uint16_t(i));
  if (out.empty()) return std::nullopt;
  if (was_all && format != OutFormat::Opus && out.size() == checks.size() && checks.size() == device_channels)
    return std::vector<uint16_t>{};
  return out;
}

std::wstring name_preview(const std::wstring& tmpl, const std::wstring& session, const std::wstring& dest,
                          OutFormat format, WallTime now) {
  NameInputs in;
  in.session = session;
  in.dest = dest;
  in.first_sample = now;
  in.n = 1;
  in.seg = 0;
  return expand_name(tmpl, in) + (format == OutFormat::Opus ? L".opus" : L".wav");
}

std::wstring new_destination_name(const std::vector<DestinationConfig>& dests) {
  for (size_t n = dests.size() + 1;; ++n) {
    const std::wstring name = L"Destination " + std::to_wstring(n);
    if (std::none_of(dests.begin(), dests.end(), [&](const DestinationConfig& d) { return eq_nocase(d.name, name); }))
      return name;
  }
}

std::optional<uint32_t> parse_number(const std::wstring& text, uint32_t lo, uint32_t hi) {
  const std::wstring t = trim(text);
  if (t.empty()) return std::nullopt;
  uint64_t n = 0;
  for (const wchar_t c : t) {
    if (c < L'0' || c > L'9') return std::nullopt;
    n = n * 10 + uint64_t(c - L'0');
    if (n > hi) return std::nullopt;
  }
  if (n < lo) return std::nullopt;
  return uint32_t(n);
}

std::wstring range_message(const std::wstring& what, uint32_t lo, uint32_t hi, const std::wstring& unit) {
  return what + L" must be " + std::to_wstring(lo) + L"–" + std::to_wstring(hi) + unit + L".";
}

std::vector<std::wstring> destination_problems(const DestinationConfig& d, uint16_t device_channels) {
  std::vector<std::wstring> out;
  if (trim(d.name).empty()) out.push_back(L"Enter a destination name.");
  AppSettings one;
  one.dests = {d};
  for (auto& p : validate(one, device_channels)) out.push_back(std::move(p));
  return out;
}

std::wstring raw_rate_note(uint32_t rate) {
  if (rate == 0) return L"Set in Windows sound settings";
  return std::to_wstring(rate) + L" Hz, set in Windows sound settings";
}

}  // namespace sl
