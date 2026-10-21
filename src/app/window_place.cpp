#include "app/window_place.h"

#include <algorithm>

#include "app/settings.h"
#include "io/ini_file.h"

namespace spool {

namespace {
constexpr wchar_t kSection[] = L"Window";
}

std::wstring window_place_path() {
  const std::wstring s = settings_path();  // ...\Spool\settings.ini
  return s.substr(0, s.find_last_of(L'\\') + 1) + L"window.ini";
}

std::optional<WindowPlace> load_window_place(const std::wstring& ini) {
  for (const wchar_t* k : {L"left", L"top", L"right", L"bottom"})
    if (!ini::get(ini, kSection, k)) return std::nullopt;
  WindowPlace p;
  p.rect = RECT{LONG(ini::get_i64(ini, kSection, L"left")), LONG(ini::get_i64(ini, kSection, L"top")),
                LONG(ini::get_i64(ini, kSection, L"right")), LONG(ini::get_i64(ini, kSection, L"bottom"))};
  p.maximized = ini::get_i64(ini, kSection, L"maximized") != 0;
  if (p.rect.right <= p.rect.left || p.rect.bottom <= p.rect.top) return std::nullopt;
  return p;
}

bool save_window_place(const std::wstring& ini, const WindowPlace& p) {
  return ini::write_section(ini, kSection,
                            {{L"left", std::to_wstring(p.rect.left)},
                             {L"top", std::to_wstring(p.rect.top)},
                             {L"right", std::to_wstring(p.rect.right)},
                             {L"bottom", std::to_wstring(p.rect.bottom)},
                             {L"maximized", p.maximized ? L"1" : L"0"}});
}

RECT fit_rect(const RECT& r, const RECT& work, int min_w, int min_h) {
  const int w = std::max<int>(min_w, std::min<int>(r.right - r.left, work.right - work.left));
  const int h = std::max<int>(min_h, std::min<int>(r.bottom - r.top, work.bottom - work.top));
  // Inside the work area where it fits; a window larger than it starts at its top left.
  const int x = std::max<int>(work.left, std::min<int>(r.left, work.right - w));
  const int y = std::max<int>(work.top, std::min<int>(r.top, work.bottom - h));
  return RECT{x, y, x + w, y + h};
}

}  // namespace spool
