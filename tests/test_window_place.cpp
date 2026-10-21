#include <windows.h>

#include <string>

#include "app/window_place.h"
#include "test.h"

using namespace spool;

namespace {

bool same(const RECT& a, const RECT& b) {
  return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

}  // namespace

TEST(window_fit_rect) {
  const RECT work{0, 0, 1920, 1032};  // 1080p minus the taskbar
  // The default window at 150 %: 1410 x 1140 is too tall and gets the work area's height.
  CHECK(same(fit_rect(RECT{100, 50, 1510, 1190}, work, 1290, 840), RECT{100, 0, 1510, 1032}));
  // Fits as it is: unchanged.
  CHECK(same(fit_rect(RECT{10, 20, 950, 780}, work, 860, 560), RECT{10, 20, 950, 780}));
  // Off the right and bottom edges (a monitor that went away): moved back in.
  CHECK(same(fit_rect(RECT{1800, 900, 2740, 1660}, work, 860, 560), RECT{980, 272, 1920, 1032}));
  // Off the top left (a second monitor at negative coordinates was unplugged).
  CHECK(same(fit_rect(RECT{-2000, -300, -1060, 460}, work, 860, 560), RECT{0, 0, 940, 760}));
  // A screen smaller than the minimum size: the minimum wins, from the top left.
  CHECK(same(fit_rect(RECT{0, 0, 940, 760}, RECT{0, 0, 800, 500}, 860, 560), RECT{0, 0, 860, 560}));
  // A work area that does not start at 0 (taskbar on the left or top).
  CHECK(same(fit_rect(RECT{0, 0, 940, 760}, RECT{62, 40, 1920, 1080}, 860, 560), RECT{62, 40, 1002, 800}));
}

TEST(window_place_round_trip) {
  wchar_t tmp[MAX_PATH + 1];
  const DWORD n = GetTempPathW(MAX_PATH + 1, tmp);
  const std::wstring ini = std::wstring(tmp, n) + L"spool_window_" + std::to_wstring(GetCurrentProcessId()) + L".ini";
  DeleteFileW(ini.c_str());
  CHECK(!load_window_place(ini).has_value());  // no file yet
  CHECK(save_window_place(ini, WindowPlace{RECT{-1500, 20, -400, 900}, true}));
  const auto p = load_window_place(ini);
  CHECK(p.has_value());
  if (p) {
    CHECK(same(p->rect, RECT{-1500, 20, -400, 900}));
    CHECK(p->maximized);
  }
  CHECK(save_window_place(ini, WindowPlace{RECT{10, 10, 5, 5}, false}));  // not a real size
  CHECK(!load_window_place(ini).has_value());
  DeleteFileW(ini.c_str());
}
