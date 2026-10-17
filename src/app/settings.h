#pragma once
// Persistent app settings (INI in %APPDATA%\SoundLogger) and their validation.
#include <cstdint>
#include <string>
#include <vector>

#include "audio/capture_source.h"
#include "session/config.h"

namespace sl {

struct AppSettings {
  CaptureConfig input;
  uint32_t ring_seconds = 30;
  std::wstring session_name = L"session";
  uint32_t flush_seconds = 2, low_water_minutes = 10, low_water_min_gb = 1, stall_percent = 50;
  bool start_on_launch = false, global_hotkey = false;
  std::vector<DestinationConfig> dests;
};

// %APPDATA%\SoundLogger\settings.ini. Creates nothing.
std::wstring settings_path();
// Creates %APPDATA%\SoundLogger if missing.
bool ensure_settings_dir();

// One WAV destination "main", 16-bit, all channels, %USERPROFILE%\Music\SoundLogger, 30 min.
AppSettings default_settings();

// Missing file -> default_settings(); unreadable values fall back per field. Never throws.
AppSettings load_settings(const std::wstring& ini);
// Creates the parent directory if missing. Channels are 1-based in the file, 0-based in memory.
bool save_settings(const std::wstring& ini, const AppSettings&);

// User-facing problems that must be fixed before recording; empty if the settings are valid.
std::vector<std::wstring> validate(const AppSettings&, uint16_t device_channels);

}  // namespace sl
