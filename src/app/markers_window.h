#pragma once
// The Markers window: the markers of the current recording (or of the last one, until the
// next Record) with their time of day and position, renamed in place (F2, Enter or a
// double-click). It stays open beside the dashboard while recording; new markers appear
// in it within a quarter of a second. Escape or the close box hides it.
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "session/session_manager.h"

namespace spool {

class MarkersWindow {
 public:
  using Source = std::function<std::vector<MarkerInfo>()>;
  using Rename = std::function<bool(uint32_t id, const std::wstring& name)>;

  MarkersWindow(Source, Rename);
  ~MarkersWindow();
  MarkersWindow(const MarkersWindow&) = delete;
  MarkersWindow& operator=(const MarkersWindow&) = delete;

  static bool register_class(HINSTANCE);
  // Creates the window (owned by `owner`) on first use, shows and activates it. edit_last:
  // starts renaming the newest marker.
  void show(HWND owner, HINSTANCE, bool edit_last);
  HWND hwnd() const { return hwnd_; }

 private:
  static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
  LRESULT handle(UINT, WPARAM, LPARAM);
  LRESULT on_notify(NMHDR*);
  void refresh();  // from source_, unless a name is being edited
  void edit(int item);
  void make_font();
  void layout();
  int px(int v) const;

  Source source_;
  Rename rename_;
  HWND hwnd_ = nullptr, list_ = nullptr, hint_ = nullptr;
  HFONT font_ = nullptr;  // owned
  std::vector<std::pair<uint32_t, std::wstring>> shown_;  // id and name of each row
};

}  // namespace spool
