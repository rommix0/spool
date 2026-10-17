#include "app/settings.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>

#include "app/text_util.h"
#include "io/ini_file.h"

namespace sl {

namespace {

constexpr size_t kMaxDests = 64;

std::wstring env(const wchar_t* name) {
  const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
  if (n == 0) return {};
  std::wstring v(n, L'\0');
  v.resize(GetEnvironmentVariableW(name, v.data(), n));
  return v;
}

std::wstring dest_section(size_t i) { return L"Destination" + std::to_wstring(i); }

std::wstring lower(std::wstring s) {
  for (wchar_t& c : s) c = wchar_t(std::towlower(c));
  return s;
}

// Plain decimal digits only; anything else, or a value outside [lo, hi], is garbage.
uint32_t parse_u32(const std::optional<std::wstring>& v, uint32_t def, uint32_t lo, uint32_t hi) {
  if (!v || v->empty()) return def;
  uint64_t n = 0;
  for (const wchar_t c : *v) {
    if (c < L'0' || c > L'9') return def;
    n = n * 10 + uint64_t(c - L'0');
    if (n > hi) return def;
  }
  return n < lo ? def : uint32_t(n);
}

uint32_t get_u32(const std::wstring& ini, const std::wstring& sec, const wchar_t* key, uint32_t def, uint32_t lo,
                 uint32_t hi) {
  return parse_u32(ini::get(ini, sec, key), def, lo, hi);
}

bool get_bool(const std::wstring& ini, const std::wstring& sec, const wchar_t* key, bool def) {
  const auto v = ini::get(ini, sec, key);
  if (v && *v == L"1") return true;
  if (v && *v == L"0") return false;
  return def;
}

std::wstring get_str(const std::wstring& ini, const std::wstring& sec, const wchar_t* key, const std::wstring& def) {
  const auto v = ini::get(ini, sec, key);
  return v && !v->empty() ? *v : def;
}

std::vector<std::wstring> split(const std::wstring& s, wchar_t sep) {
  std::vector<std::wstring> out;
  size_t from = 0;
  for (;;) {
    const size_t at = s.find(sep, from);
    out.push_back(s.substr(from, at == std::wstring::npos ? at : at - from));
    if (at == std::wstring::npos) return out;
    from = at + 1;
  }
}

std::wstring join(const std::vector<std::wstring>& v, wchar_t sep) {
  std::wstring out;
  for (size_t i = 0; i < v.size(); ++i) out += (i ? std::wstring(1, sep) : std::wstring()) + v[i];
  return out;
}

// 1-based in the file; entries that are not a number in 1..65535 are dropped.
std::vector<uint16_t> parse_channels(const std::wstring& s) {
  std::vector<uint16_t> out;
  for (const auto& t : split(s, L','))
    if (const uint32_t n = parse_u32(t, 0, 1, 65535)) out.push_back(uint16_t(n - 1));
  return out;
}

std::wstring channels_text(const std::vector<uint16_t>& chs) {
  std::vector<std::wstring> t;
  for (const uint16_t c : chs) t.push_back(std::to_wstring(c + 1));
  return join(t, L',');
}

bool dest_exists(const std::wstring& ini, size_t i) { return ini::get(ini, dest_section(i), L"name").has_value(); }

DestinationConfig load_dest(const std::wstring& ini, size_t i) {
  const std::wstring sec = dest_section(i);
  const DestinationConfig def;
  DestinationConfig d;
  d.name = get_str(ini, sec, L"name", L"Destination " + std::to_wstring(i + 1));
  d.format = lower(ini::get(ini, sec, L"format").value_or(L"")) == L"opus" ? OutFormat::Opus : OutFormat::Wav;
  const std::wstring depth = lower(ini::get(ini, sec, L"wav_depth").value_or(L""));
  d.wav_depth = depth == L"24" ? WavDepth::Int24 : depth == L"32f" ? WavDepth::Float32 : WavDepth::Int16;
  d.opus_bitrate = get_u32(ini, sec, L"opus_bitrate", def.opus_bitrate, 0, 1000000);
  d.channels = parse_channels(ini::get(ini, sec, L"channels").value_or(L""));
  for (auto& f : split(ini::get(ini, sec, L"folders").value_or(L""), L'|'))
    if (!f.empty()) d.folders.push_back(std::move(f));
  d.rotation_minutes = get_u32(ini, sec, L"rotation_minutes", def.rotation_minutes, 0, 10080);
  d.name_template = get_str(ini, sec, L"name_template", def.name_template);
  return d;
}

bool ensure_dir(const std::wstring& dir) {
  return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

}  // namespace

std::wstring settings_path() { return env(L"APPDATA") + L"\\SoundLogger\\settings.ini"; }

bool ensure_settings_dir() {
  const std::wstring appdata = env(L"APPDATA");
  return !appdata.empty() && ensure_dir(appdata + L"\\SoundLogger");
}

AppSettings default_settings() {
  AppSettings s;
  DestinationConfig d;
  d.folders = {env(L"USERPROFILE") + L"\\Music\\SoundLogger"};
  s.dests = {d};
  return s;
}

AppSettings load_settings(const std::wstring& ini) {
  AppSettings s = default_settings();
  if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) return s;

  const AppSettings def = s;
  s.input.device_id = ini::get(ini, L"Input", L"device_id").value_or(L"");
  s.input.mode = lower(ini::get(ini, L"Input", L"mode").value_or(L"")) == L"exclusive" ? CaptureMode::Exclusive
                                                                                          : CaptureMode::Raw;
  s.input.sample_rate = get_u32(ini, L"Input", L"sample_rate", 0, 0, 768000);

  s.ring_seconds = get_u32(ini, L"General", L"ring_seconds", def.ring_seconds, kRingSecondsRange.lo,
                           kRingSecondsRange.hi);
  s.session_name = get_str(ini, L"General", L"session_name", def.session_name);
  s.flush_seconds = get_u32(ini, L"General", L"flush_seconds", def.flush_seconds, kFlushSecondsRange.lo,
                            kFlushSecondsRange.hi);
  s.low_water_minutes = get_u32(ini, L"General", L"low_water_minutes", def.low_water_minutes,
                                kLowWaterMinutesRange.lo, kLowWaterMinutesRange.hi);
  s.low_water_min_gb = get_u32(ini, L"General", L"low_water_min_gb", def.low_water_min_gb, 0, 100000);
  s.stall_percent = get_u32(ini, L"General", L"stall_percent", def.stall_percent, kStallPercentRange.lo,
                            kStallPercentRange.hi);
  s.start_on_launch = get_bool(ini, L"General", L"start_on_launch", def.start_on_launch);
  s.global_hotkey = get_bool(ini, L"General", L"global_hotkey", def.global_hotkey);

  std::vector<DestinationConfig> dests;
  for (size_t i = 0; i < kMaxDests && dest_exists(ini, i); ++i) dests.push_back(load_dest(ini, i));
  // A saved file with no destinations at all means the user removed them; a hand-made or damaged
  // file without the marker gets the default one.
  if (!dests.empty() || ini::get(ini, L"General", L"dests").has_value()) s.dests = std::move(dests);
  return s;
}

bool save_settings(const std::wstring& ini, const AppSettings& s) {
  if (const size_t cut = ini.find_last_of(L"\\/"); cut != std::wstring::npos && cut > 0)
    if (!ensure_dir(ini.substr(0, cut))) return false;
  if (!ini::ensure_unicode_file(ini)) return false;

  using ini::quoted;
  bool ok = ini::write_section(ini, L"Input",
                               {{L"device_id", quoted(s.input.device_id)},
                                {L"mode", s.input.mode == CaptureMode::Exclusive ? L"exclusive" : L"raw"},
                                {L"sample_rate", std::to_wstring(s.input.sample_rate)}});
  ok = ini::write_section(ini, L"General",
                          {{L"ring_seconds", std::to_wstring(s.ring_seconds)},
                           {L"session_name", quoted(s.session_name)},
                           {L"flush_seconds", std::to_wstring(s.flush_seconds)},
                           {L"low_water_minutes", std::to_wstring(s.low_water_minutes)},
                           {L"low_water_min_gb", std::to_wstring(s.low_water_min_gb)},
                           {L"stall_percent", std::to_wstring(s.stall_percent)},
                           {L"start_on_launch", s.start_on_launch ? L"1" : L"0"},
                           {L"global_hotkey", s.global_hotkey ? L"1" : L"0"},
                           {L"dests", std::to_wstring(s.dests.size())}}) &&
       ok;

  for (size_t i = 0; i < s.dests.size(); ++i) {
    const DestinationConfig& d = s.dests[i];
    const wchar_t* depth = d.wav_depth == WavDepth::Int24 ? L"24" : d.wav_depth == WavDepth::Float32 ? L"32f" : L"16";
    ok = ini::write_section(ini, dest_section(i),
                            {{L"name", quoted(d.name)},
                             {L"format", d.format == OutFormat::Opus ? L"opus" : L"wav"},
                             {L"wav_depth", depth},
                             {L"opus_bitrate", std::to_wstring(d.opus_bitrate)},
                             {L"channels", quoted(channels_text(d.channels))},
                             {L"folders", quoted(join(d.folders, L'|'))},
                             {L"rotation_minutes", std::to_wstring(d.rotation_minutes)},
                             {L"name_template", quoted(d.name_template)}}) &&
         ok;
  }
  // Sections left over from a longer destination list.
  for (size_t i = s.dests.size(); i < kMaxDests && dest_exists(ini, i); ++i)
    ok = WritePrivateProfileStringW(dest_section(i).c_str(), nullptr, nullptr, ini.c_str()) && ok;
  return ok;
}

std::vector<std::wstring> validate(const AppSettings& s, uint16_t device_channels) {
  std::vector<std::wstring> out;
  if (s.dests.empty()) out.push_back(L"Add at least one destination.");

  for (const DestinationConfig& d : s.dests) {
    const std::wstring q = L"\"" + d.name + L"\"";
    if (std::none_of(d.folders.begin(), d.folders.end(), [](const std::wstring& f) { return !f.empty(); }))
      out.push_back(q + L" needs at least one folder.");

    std::vector<uint16_t> bad;
    for (const uint16_t c : d.channels)
      if (c >= device_channels && std::find(bad.begin(), bad.end(), c) == bad.end()) bad.push_back(c);
    for (const uint16_t c : bad)
      out.push_back(q + L": channel " + std::to_wstring(c + 1) + L" doesn't exist on this device.");

    if (d.format != OutFormat::Opus) continue;
    const size_t n = d.channels.empty() ? device_channels : d.channels.size();
    if (n != 1 && n != 2) {
      out.push_back(q + L": Opus records 1 or 2 channels.");
      continue;
    }
    const bool in_range = n == 2 ? d.opus_bitrate >= 32000 && d.opus_bitrate <= 256000
                                 : d.opus_bitrate >= 16000 && d.opus_bitrate <= 128000;
    if (d.opus_bitrate != 0 && !in_range)
      out.push_back(q + L": Opus bitrate must be 32–256 kbps for stereo or 16–128 kbps for mono.");
  }

  for (size_t i = 0; i < s.dests.size(); ++i)
    for (size_t j = i + 1; j < s.dests.size(); ++j)
      if (eq_nocase(s.dests[i].name, s.dests[j].name)) {
        out.push_back(L"Destination names must be unique.");
        return out;
      }
  return out;
}

}  // namespace sl
