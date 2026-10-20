#pragma once
// The modal settings dialog (spec 10.5): input (device, mode, sample rate, ring length),
// the destination list with its editor, and general options. OK validates; problems show
// in a "Check these settings" box and the dialog stays open.
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "app/settings.h"
#include "audio/wasapi_source.h"

namespace spool {

struct SettingsChoices {
  std::vector<DeviceInfo> devices;  // the input devices offered
  std::wstring default_device_id;   // selected when the settings name no device
  // The device's exclusive-mode rates; may open the device, so it's called only when needed.
  std::function<std::vector<uint32_t>(const std::wstring& device_id)> exclusive_rates;
};

// Shows the dialog over owner. On OK the edited settings are validated against the selected
// device, written to ini and stored in s, and the result is true. On Cancel s is unchanged.
bool run_settings_dialog(HWND owner, HINSTANCE, const SettingsChoices&, const std::wstring& ini, AppSettings& s);

}  // namespace spool
