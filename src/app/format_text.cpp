#include "app/format_text.h"

#include <cmath>
#include <cwchar>

namespace sl {

namespace {

constexpr wchar_t kDot[] = L" · ";   // middle dot
constexpr wchar_t kDash[] = L"–";    // en dash

// Whole units of v, never negative; NaN counts as 0.
int64_t whole(double v) { return v > 0 ? int64_t(std::floor(v)) : 0; }

// Runs of consecutive ascending channels: "1–2", "3", "1, 3, 5–6" (1-based).
std::wstring channel_text(const std::vector<uint16_t>& chs) {
  std::wstring out;
  for (size_t i = 0; i < chs.size();) {
    size_t j = i;
    while (j + 1 < chs.size() && chs[j + 1] == chs[j] + 1) ++j;
    if (!out.empty()) out += L", ";
    out += std::to_wstring(chs[i] + 1);
    if (j > i) out += kDash + std::to_wstring(chs[j] + 1);
    i = j + 1;
  }
  return out;
}

}  // namespace

std::wstring format_elapsed(double seconds) {
  const int64_t s = whole(seconds);
  wchar_t buf[32];
  swprintf(buf, 32, L"%02lld:%02lld:%02lld", (long long)(s / 3600), (long long)(s / 60 % 60), (long long)(s % 60));
  return buf;
}

std::wstring format_time_left(double hours) {
  if (hours < 1) return std::to_wstring(whole(hours * 60)) + L" min left";
  return std::to_wstring(whole(hours)) + L" h left";
}

std::wstring format_summary(const DestinationConfig& d, uint16_t input_channels) {
  std::vector<uint16_t> chs = d.channels;
  if (chs.empty())
    for (uint16_t c = 0; c < input_channels; ++c) chs.push_back(c);

  std::wstring fmt;
  if (d.format == OutFormat::Opus) {
    uint32_t bitrate = d.opus_bitrate;
    if (bitrate == 0) bitrate = chs.size() == 1 ? 64000 : 128000;
    fmt = L"Opus " + std::to_wstring(bitrate / 1000) + L"k";
  } else {
    fmt = d.wav_depth == WavDepth::Int24     ? L"WAV 24-bit"
          : d.wav_depth == WavDepth::Float32 ? L"WAV 32-bit float"
                                             : L"WAV 16-bit";
  }
  if (chs.empty()) return fmt;
  return fmt + kDot + L"ch " + channel_text(chs);
}

}  // namespace sl
