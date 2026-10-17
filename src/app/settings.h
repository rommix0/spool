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

// Accepted ranges for the numeric fields (load falls back to the default outside them; the
// settings dialog rejects them).
struct U32Range {
  uint32_t lo, hi;
};
inline constexpr U32Range kRingSecondsRange{1, 3600};
inline constexpr U32Range kFlushSecondsRange{1, 3600};
inline constexpr U32Range kLowWaterMinutesRange{0, 100000};
inline constexpr U32Range kStallPercentRange{1, 100};

// %APPDATA%\SoundLogger\settings.ini. Creates nothing.
std::wstring settings_path();
// Creates %APPDATA%\SoundLogger if missing.
bool ensure_settings_dir();
// %APPDATA%\SoundLogger\state.ini: the crash state (StateStore). A file of its own, since
// StateStore writes it as UTF-16 with a BOM. Creates nothing.
std::wstring state_path();
// %LOCALAPPDATA%\SoundLogger\logs ("" if LOCALAPPDATA is not set). Creates nothing.
std::wstring logs_dir();
// <logs_dir>\<name>.log, "" without a logs dir. The app's event log is app.log outside a
// session and <session_id>.log while one runs (a resumed session appends to its own).
std::wstring log_file(const std::wstring& name);

// One WAV destination "main", 16-bit, all channels, %USERPROFILE%\Music\SoundLogger, 30 min.
AppSettings default_settings();

// Missing file -> default_settings(); unreadable values fall back per field. Never throws.
AppSettings load_settings(const std::wstring& ini);
// Creates the parent directory if missing. Channels are 1-based in the file, 0-based in memory.
bool save_settings(const std::wstring& ini, const AppSettings&);

// User-facing problems that must be fixed before recording; empty if the settings are valid.
std::vector<std::wstring> validate(const AppSettings&, uint16_t device_channels);

}  // namespace sl
