#pragma once
// Level meters for the dashboard (window class SLMeter, spec 10.2): one horizontal bar
// per channel on a -60..0 dBFS scale with an RMS bar, a peak line, a hold tick and a clip
// lamp, and the session clip count at the right. Drawn with GDI into a memory DC.
#include <windows.h>

#include <functional>
#include <vector>

#include "session/meter.h"

namespace spool {

class MeterControl {
 public:
  static bool register_class(HINSTANCE);

  MeterControl() = default;
  ~MeterControl();
  MeterControl(const MeterControl&) = delete;
  MeterControl& operator=(const MeterControl&) = delete;

  // A child of parent; takes its font from WM_SETFONT.
  HWND create(HWND parent, int id, HINSTANCE);
  HWND hwnd() const { return hwnd_; }

  // Repaints only this control, and only when something visible changed.
  void set_levels(const std::vector<ChannelMeter>&);
  // Called when the user clicks the clip counts.
  void on_reset_clips(std::function<void()> f) { on_reset_ = std::move(f); }
  // Height that fits `channels` rows and the scale, at the control's DPI.
  int preferred_height(size_t channels) const;

 private:
  static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
  LRESULT handle(UINT, WPARAM, LPARAM);
  int px(int v) const;  // 96-dpi pixels to device pixels
  RECT counts_rect() const;
  void paint(HDC, const RECT& client) const;
  void free_buffer();

  HWND hwnd_ = nullptr;
  HFONT font_ = nullptr;  // not owned
  std::vector<ChannelMeter> levels_;
  std::function<void()> on_reset_;
  // Back buffer, recreated when the client size changes.
  HDC mem_dc_ = nullptr;
  HBITMAP mem_bmp_ = nullptr, old_bmp_ = nullptr;
  SIZE mem_size_{};
};

}  // namespace spool
