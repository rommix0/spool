#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "app/format_text.h"
#include "app/settings.h"
#include "test.h"

using namespace sl;

namespace {

// A unique directory under %TEMP%, removed (with its files and subdirectories) on scope exit.
struct SettingsTempDir {
  SettingsTempDir() {
    static int counter = 0;
    wchar_t tmp[MAX_PATH + 1];
    const DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
    path = std::wstring(tmp, n) + L"sl_settings_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
           std::to_wstring(++counter);
    CreateDirectoryW(path.c_str(), nullptr);
  }
  ~SettingsTempDir() { remove_tree(path); }
  static void remove_tree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    const HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
      do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(dir + L"\\" + name);
        else DeleteFileW((dir + L"\\" + name).c_str());
      } while (FindNextFileW(h, &fd));
      FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
  }
  std::wstring path;
};

bool has(const std::vector<std::wstring>& v, const std::wstring& s) {
  return std::find(v.begin(), v.end(), s) != v.end();
}

DestinationConfig dest(const std::wstring& name) {
  DestinationConfig d;
  d.name = name;
  d.folders = {L"D:\\Gigs"};
  return d;
}

AppSettings with(const DestinationConfig& d) {
  AppSettings s;
  s.dests = {d};
  return s;
}

}  // namespace

TEST(settings_roundtrip_unicode) {
  SettingsTempDir td;
  const std::wstring ini = td.path + L"\\SoundLogger\\settings.ini";  // parent does not exist yet

  AppSettings s;
  s.input.device_id = L"{0.0.1.00000000}.{abc-\u00FC}";
  s.input.mode = CaptureMode::Exclusive;
  s.input.sample_rate = 96000;
  s.ring_seconds = 45;
  s.session_name = L"  G\u00E4ste \u65E5\u672C ";
  s.flush_seconds = 3;
  s.low_water_minutes = 15;
  s.low_water_min_gb = 4;
  s.stall_percent = 60;
  s.start_on_launch = true;
  s.global_hotkey = true;
  DestinationConfig a;
  a.name = L"Main \u2013 B\u00E4ckup";
  a.format = OutFormat::Wav;
  a.wav_depth = WavDepth::Float32;
  a.channels = {0, 2, 3};
  a.folders = {L"D:\\Gigs", L"E:\\B\u00E4ckup"};
  a.rotation_minutes = 0;
  a.name_template = L"{session}_{n}";
  DestinationConfig b;
  b.name = L"opus";
  b.format = OutFormat::Opus;
  b.opus_bitrate = 96000;
  b.channels = {1};
  b.folders = {L"F:\\x"};
  s.dests = {a, b};

  CHECK(save_settings(ini, s));
  const AppSettings r = load_settings(ini);

  CHECK(r.input.device_id == s.input.device_id);
  CHECK(r.input.mode == CaptureMode::Exclusive);
  CHECK_EQ(r.input.sample_rate, 96000u);
  CHECK_EQ(r.ring_seconds, 45u);
  CHECK(r.session_name == s.session_name);
  CHECK_EQ(r.flush_seconds, 3u);
  CHECK_EQ(r.low_water_minutes, 15u);
  CHECK_EQ(r.low_water_min_gb, 4u);
  CHECK_EQ(r.stall_percent, 60u);
  CHECK(r.start_on_launch);
  CHECK(r.global_hotkey);
  CHECK_EQ(r.dests.size(), size_t(2));
  if (r.dests.size() != 2) return;
  CHECK(r.dests[0].name == a.name);
  CHECK(r.dests[0].format == OutFormat::Wav);
  CHECK(r.dests[0].wav_depth == WavDepth::Float32);
  CHECK(r.dests[0].channels == a.channels);
  CHECK(r.dests[0].folders == a.folders);
  CHECK_EQ(r.dests[0].rotation_minutes, 0u);
  CHECK(r.dests[0].name_template == a.name_template);
  CHECK(r.dests[1].format == OutFormat::Opus);
  CHECK_EQ(r.dests[1].opus_bitrate, 96000u);
  CHECK(r.dests[1].channels == b.channels);
  CHECK(r.dests[1].folders == b.folders);
  CHECK_EQ(r.dests[1].rotation_minutes, 30u);

  // Channels are 1-based in the file.
  wchar_t buf[64];
  GetPrivateProfileStringW(L"Destination0", L"channels", L"", buf, 64, ini.c_str());
  CHECK(std::wstring(buf) == L"1,3,4");
  GetPrivateProfileStringW(L"Destination0", L"folders", L"", buf, 64, ini.c_str());
  CHECK(std::wstring(buf) == L"D:\\Gigs|E:\\B\u00E4ckup");
}

TEST(settings_save_drops_removed_destinations) {
  SettingsTempDir td;
  const std::wstring ini = td.path + L"\\settings.ini";
  AppSettings s;
  s.dests = {dest(L"a"), dest(L"b"), dest(L"c")};
  CHECK(save_settings(ini, s));
  s.dests.resize(1);
  CHECK(save_settings(ini, s));
  const AppSettings r = load_settings(ini);
  CHECK_EQ(r.dests.size(), size_t(1));
}

TEST(settings_defaults_when_missing) {
  SettingsTempDir td;
  const AppSettings s = load_settings(td.path + L"\\nope.ini");
  CHECK_EQ(s.dests.size(), size_t(1));
  if (s.dests.size() != 1) return;
  CHECK(s.dests[0].name == L"main");
  CHECK(s.dests[0].format == OutFormat::Wav);
  CHECK(s.dests[0].wav_depth == WavDepth::Int16);
  CHECK(s.dests[0].channels.empty());
  CHECK_EQ(s.dests[0].rotation_minutes, 30u);
  CHECK_EQ(s.dests[0].folders.size(), size_t(1));
  if (s.dests[0].folders.size() == 1) {
    const std::wstring& f = s.dests[0].folders[0];
    const std::wstring tail = L"\\Music\\SoundLogger";
    CHECK(f.size() > tail.size() && f.compare(f.size() - tail.size(), tail.size(), tail) == 0);
  }
  CHECK_EQ(s.ring_seconds, 30u);
  CHECK_EQ(s.flush_seconds, 2u);
  CHECK_EQ(s.low_water_minutes, 10u);
  CHECK_EQ(s.low_water_min_gb, 1u);
  CHECK_EQ(s.stall_percent, 50u);
  CHECK(!s.start_on_launch);
  CHECK(!s.global_hotkey);
  CHECK(s.session_name == L"session");
}

TEST(settings_garbage_falls_back_per_field) {
  SettingsTempDir td;
  const std::wstring ini = td.path + L"\\settings.ini";
  CHECK(save_settings(ini, default_settings()));
  WritePrivateProfileStringW(L"General", L"ring_seconds", L"banana", ini.c_str());
  WritePrivateProfileStringW(L"General", L"stall_percent", L"-5", ini.c_str());
  WritePrivateProfileStringW(L"General", L"flush_seconds", L"99999999999999", ini.c_str());
  WritePrivateProfileStringW(L"General", L"start_on_launch", L"maybe", ini.c_str());
  WritePrivateProfileStringW(L"Input", L"mode", L"\"turbo\"", ini.c_str());
  WritePrivateProfileStringW(L"Destination0", L"format", L"\"mp3\"", ini.c_str());
  WritePrivateProfileStringW(L"Destination0", L"wav_depth", L"\"8\"", ini.c_str());
  WritePrivateProfileStringW(L"Destination0", L"channels", L"\"1,x,0,3\"", ini.c_str());
  WritePrivateProfileStringW(L"Destination0", L"rotation_minutes", L"soon", ini.c_str());
  const AppSettings s = load_settings(ini);
  CHECK_EQ(s.ring_seconds, 30u);
  CHECK_EQ(s.stall_percent, 50u);
  CHECK_EQ(s.flush_seconds, 2u);
  CHECK(!s.start_on_launch);
  CHECK(s.input.mode == CaptureMode::Raw);
  CHECK_EQ(s.dests.size(), size_t(1));
  if (s.dests.size() != 1) return;
  CHECK(s.dests[0].format == OutFormat::Wav);
  CHECK(s.dests[0].wav_depth == WavDepth::Int16);
  CHECK((s.dests[0].channels == std::vector<uint16_t>{0, 2}));
  CHECK_EQ(s.dests[0].rotation_minutes, 30u);
}

TEST(settings_validate_ok_and_empty) {
  CHECK(validate(default_settings(), 2).empty());
  AppSettings none;
  none.dests.clear();
  CHECK(has(validate(none, 2), L"Add at least one destination."));
}

TEST(settings_validate_needs_folder) {
  DestinationConfig d = dest(L"x");
  d.folders.clear();
  CHECK(has(validate(with(d), 2), L"\"x\" needs at least one folder."));
}

TEST(settings_validate_opus_channels) {
  DestinationConfig d = dest(L"x");
  d.format = OutFormat::Opus;
  d.channels = {0, 1, 2};
  CHECK(has(validate(with(d), 8), L"\"x\": Opus records 1 or 2 channels."));
  d.channels = {0, 1};
  CHECK(validate(with(d), 8).empty());
  // Empty channels means all inputs: invalid on an 8-channel device, fine on stereo.
  d.channels.clear();
  CHECK(has(validate(with(d), 8), L"\"x\": Opus records 1 or 2 channels."));
  CHECK(validate(with(d), 2).empty());
}

TEST(settings_validate_channel_range) {
  DestinationConfig d = dest(L"x");
  d.channels = {0, 5};
  CHECK(has(validate(with(d), 2), L"\"x\": channel 6 doesn't exist on this device."));
  d.channels = {0, 1};
  CHECK(validate(with(d), 2).empty());
}

TEST(settings_validate_opus_bitrate) {
  const std::wstring msg =
      L"\"x\": Opus bitrate must be 32\u2013256 kbps for stereo or 16\u2013128 kbps for mono.";
  DestinationConfig d = dest(L"x");
  d.format = OutFormat::Opus;
  d.channels = {0, 1};
  d.opus_bitrate = 0;
  CHECK(validate(with(d), 2).empty());
  d.opus_bitrate = 32000;
  CHECK(validate(with(d), 2).empty());
  d.opus_bitrate = 256000;
  CHECK(validate(with(d), 2).empty());
  d.opus_bitrate = 31999;
  CHECK(has(validate(with(d), 2), msg));
  d.opus_bitrate = 257000;
  CHECK(has(validate(with(d), 2), msg));
  d.channels = {0};
  d.opus_bitrate = 16000;
  CHECK(validate(with(d), 2).empty());
  d.opus_bitrate = 128000;
  CHECK(validate(with(d), 2).empty());
  d.opus_bitrate = 129000;
  CHECK(has(validate(with(d), 2), msg));
  d.opus_bitrate = 15000;
  CHECK(has(validate(with(d), 2), msg));
  // A WAV destination ignores the bitrate.
  d.format = OutFormat::Wav;
  CHECK(validate(with(d), 2).empty());
}

TEST(settings_validate_unique_names) {
  AppSettings s;
  s.dests = {dest(L"Main"), dest(L"mAIN")};
  CHECK(has(validate(s, 2), L"Destination names must be unique."));
  s.dests = {dest(L"Main"), dest(L"Other")};
  CHECK(validate(s, 2).empty());
}

TEST(text_elapsed_and_time_left) {
  CHECK(format_elapsed(8077) == L"02:14:37");
  CHECK(format_elapsed(0) == L"00:00:00");
  CHECK(format_elapsed(59.9) == L"00:00:59");
  CHECK(format_elapsed(100 * 3600 + 5) == L"100:00:05");
  CHECK(format_elapsed(-3) == L"00:00:00");
  CHECK(format_time_left(31.4) == L"31 h left");
  CHECK(format_time_left(0.75) == L"45 min left");
  CHECK(format_time_left(1.0) == L"1 h left");
  CHECK(format_time_left(0.0) == L"0 min left");
  CHECK(format_time_left(-1.0) == L"0 min left");
}

TEST(text_summary) {
  DestinationConfig w;
  CHECK(format_summary(w, 2) == L"WAV 16-bit \u00B7 ch 1\u20132");
  w.wav_depth = WavDepth::Int24;
  CHECK(format_summary(w, 8) == L"WAV 24-bit \u00B7 ch 1\u20138");
  w.wav_depth = WavDepth::Float32;
  w.channels = {2};
  CHECK(format_summary(w, 8) == L"WAV 32-bit float \u00B7 ch 3");
  w.channels = {0, 2, 4, 5};
  CHECK(format_summary(w, 8) == L"WAV 32-bit float \u00B7 ch 1, 3, 5\u20136");

  DestinationConfig o;
  o.format = OutFormat::Opus;
  o.opus_bitrate = 128000;
  o.channels = {0, 1};
  CHECK(format_summary(o, 2) == L"Opus 128k \u00B7 ch 1\u20132");
  o.opus_bitrate = 0;
  CHECK(format_summary(o, 2) == L"Opus 128k \u00B7 ch 1\u20132");
  o.channels = {3};
  CHECK(format_summary(o, 8) == L"Opus 64k \u00B7 ch 4");
  o.channels.clear();
  CHECK(format_summary(o, 1) == L"Opus 64k \u00B7 ch 1");
  o.channels = {0};
  o.opus_bitrate = 48000;
  CHECK(format_summary(o, 2) == L"Opus 48k \u00B7 ch 1");
}

TEST(text_input_line) {
  CHECK(format_input_line(L"USB mic", StreamFormat{48000, 2}, CaptureMode::Raw) ==
        L"USB mic · 48 kHz · 2 ch · Raw");
  CHECK(format_input_line(L"Interface", StreamFormat{44100, 8}, CaptureMode::Exclusive) ==
        L"Interface · 44.1 kHz · 8 ch · Exclusive");
  CHECK(format_input_line(L"X", StreamFormat{176400, 1}, CaptureMode::Raw) ==
        L"X · 176.4 kHz · 1 ch · Raw");
  CHECK(format_input_line(L"X", StreamFormat{192000, 2}, CaptureMode::Raw) ==
        L"X · 192 kHz · 2 ch · Raw");
}

TEST(text_buffer_line) {
  CHECK(format_buffer_line(0.0, 0, 0) == L"Buffer 0% · Gaps 0 · Discontinuities 0");
  CHECK(format_buffer_line(0.034, 2, 17) == L"Buffer 3% · Gaps 2 · Discontinuities 17");
  CHECK(format_buffer_line(0.996, 0, 0) == L"Buffer 100% · Gaps 0 · Discontinuities 0");
  CHECK(format_buffer_line(1.5, 0, 0) == L"Buffer 100% · Gaps 0 · Discontinuities 0");
  CHECK(format_buffer_line(-0.2, 0, 0) == L"Buffer 0% · Gaps 0 · Discontinuities 0");
  CHECK(format_buffer_line(std::nan(""), 0, 0) == L"Buffer 0% · Gaps 0 · Discontinuities 0");
}

TEST(text_folder_time_left) {
  const uint64_t rate = 192000;  // 48 kHz, 2 ch, 16-bit
  CHECK(format_folder_time_left(std::nullopt, rate) == L"—");
  CHECK(format_folder_time_left(uint64_t(1) << 40, 0) == L"—");
  CHECK(format_folder_time_left(std::nullopt, 0) == L"—");
  CHECK(format_folder_time_left(rate * 3600 * 31, rate) == L"31 h left");
  CHECK(format_folder_time_left(rate * 60 * 45, rate) == L"45 min left");
  CHECK(format_folder_time_left(0, rate) == L"0 min left");
  CHECK(format_folder_time_left(UINT64_MAX, 1) == std::to_wstring(UINT64_MAX / 3600) + L" h left");
}

TEST(text_clock) {
  const int64_t t = (14 * 3600 + 30 * 60 + 5) * 1000LL + 999;
  CHECK(format_clock(WallTime{t, 0}) == L"14:30:05");
  CHECK(format_clock(WallTime{t + 240 * 60000LL, -240}) == L"14:30:05");
  CHECK(format_clock(WallTime{86400000LL + 2 * 3600000LL, -240}) == L"22:00:00");
}
