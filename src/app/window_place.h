#pragma once
// The main window's size and position: kept in %APPDATA%\Spool\window.ini between runs,
// and always fitted to the screen it opens on.
#include <windows.h>

#include <optional>
#include <string>

namespace spool {

struct WindowPlace {
  RECT rect;  // the restored (not maximized) window, in workspace coordinates (WINDOWPLACEMENT)
  bool maximized = false;
};

// %APPDATA%\Spool\window.ini.
std::wstring window_place_path();
// nullopt if the file or a value is missing, or the size is not positive.
std::optional<WindowPlace> load_window_place(const std::wstring& ini);
bool save_window_place(const std::wstring& ini, const WindowPlace&);

// r shrunk to fit inside work (but never below min_w x min_h), then moved inside it
// as far as its size allows.
RECT fit_rect(const RECT& r, const RECT& work, int min_w, int min_h);

}  // namespace spool
