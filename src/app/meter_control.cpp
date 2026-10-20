#include "app/meter_control.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace spool {

namespace {

constexpr wchar_t kClass[] = L"SpoolMeter";
constexpr float kFloorDb = -60.0f;
constexpr float kClipDb = -0.1f;  // spec 10.2

// Layout in 96-dpi pixels.
constexpr int kRowH = 16, kRowGap = 6, kScaleH = 18, kLabelW = 24, kLampW = 12, kGap = 8, kCountsW = 64;

constexpr COLORREF kTrough = RGB(38, 38, 38);
constexpr COLORREF kTick = RGB(66, 66, 66);
constexpr COLORREF kGreen = RGB(46, 168, 72);
constexpr COLORREF kYellow = RGB(226, 186, 40);
constexpr COLORREF kRed = RGB(222, 58, 48);
constexpr COLORREF kPeak = RGB(205, 205, 205);
constexpr COLORREF kHold = RGB(255, 255, 255);
constexpr COLORREF kLampOff = RGB(96, 44, 44);
constexpr COLORREF kLampOn = RGB(240, 36, 36);
constexpr COLORREF kClipText = RGB(196, 0, 0);

constexpr float kTicks[] = {-60, -48, -36, -24, -18, -12, -6, -3, 0};
constexpr int kLabels[] = {-60, -48, -36, -24, -12, -6, 0};

// Fraction of the scale for a dBFS value; NaN and anything at or below the floor -> 0.
double frac(float db) {
  if (!(db > kFloorDb)) return 0.0;
  return std::min(0.0, double(db)) / -double(kFloorDb) + 1.0;
}

void fill(HDC dc, int l, int t, int r, int b, COLORREF c) {
  if (r <= l || b <= t) return;
  SetDCBrushColor(dc, c);
  const RECT rc{l, t, r, b};
  FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

bool same(const ChannelMeter& a, const ChannelMeter& b) {
  return a.peak_db == b.peak_db && a.hold_db == b.hold_db && a.rms_db == b.rms_db && a.clips == b.clips;
}

}  // namespace

bool MeterControl::register_class(HINSTANCE inst) {
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = proc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kClass;
  return RegisterClassExW(&wc) != 0;
}

MeterControl::~MeterControl() { free_buffer(); }

HWND MeterControl::create(HWND parent, int id, HINSTANCE inst) {
  return CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent,
                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, this);
}

void MeterControl::set_levels(const std::vector<ChannelMeter>& v) {
  if (v.size() == levels_.size() && std::equal(v.begin(), v.end(), levels_.begin(), same)) return;
  levels_ = v;
  if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

int MeterControl::px(int v) const { return MulDiv(v, hwnd_ ? int(GetDpiForWindow(hwnd_)) : 96, 96); }

int MeterControl::preferred_height(size_t channels) const {
  return int(channels) * px(kRowH + kRowGap) + px(kScaleH);
}

RECT MeterControl::counts_rect() const {
  RECT c{};
  GetClientRect(hwnd_, &c);
  c.left = std::max(c.left, c.right - px(kCountsW));
  return c;
}

void MeterControl::free_buffer() {
  if (!mem_dc_) return;
  SelectObject(mem_dc_, old_bmp_);
  DeleteObject(mem_bmp_);
  DeleteDC(mem_dc_);
  mem_dc_ = nullptr;
  mem_bmp_ = old_bmp_ = nullptr;
  mem_size_ = {};
}

LRESULT CALLBACK MeterControl::proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    auto* self = static_cast<MeterControl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    self->hwnd_ = h;
    SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  auto* self = reinterpret_cast<MeterControl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  return self ? self->handle(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

LRESULT MeterControl::handle(UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_SETFONT:
      font_ = reinterpret_cast<HFONT>(wp);
      if (LOWORD(lp)) InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    case WM_GETFONT:
      return reinterpret_cast<LRESULT>(font_);
    case WM_ERASEBKGND:
      return 1;  // everything is painted from the back buffer
    case WM_SETCURSOR:
      if (LOWORD(lp) == HTCLIENT) {
        POINT p;
        GetCursorPos(&p);
        ScreenToClient(hwnd_, &p);
        const RECT r = counts_rect();
        SetCursor(LoadCursorW(nullptr, PtInRect(&r, p) ? IDC_HAND : IDC_ARROW));
        return TRUE;
      }
      break;
    case WM_LBUTTONUP: {
      const POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      const RECT r = counts_rect();
      if (PtInRect(&r, p) && on_reset_) on_reset_();
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd_, &ps);
      RECT c;
      GetClientRect(hwnd_, &c);
      if (c.right > 0 && c.bottom > 0) {
        if (!mem_dc_ || c.right != mem_size_.cx || c.bottom != mem_size_.cy) {
          free_buffer();
          mem_dc_ = CreateCompatibleDC(dc);
          mem_bmp_ = CreateCompatibleBitmap(dc, c.right, c.bottom);
          old_bmp_ = static_cast<HBITMAP>(SelectObject(mem_dc_, mem_bmp_));
          mem_size_ = SIZE{c.right, c.bottom};
        }
        paint(mem_dc_, c);
        BitBlt(dc, 0, 0, c.right, c.bottom, mem_dc_, 0, 0, SRCCOPY);
      }
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_DESTROY:
      free_buffer();
      SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
      hwnd_ = nullptr;
      return 0;
  }
  return DefWindowProcW(hwnd_, msg, wp, lp);
}

void MeterControl::paint(HDC dc, const RECT& c) const {
  fill(dc, 0, 0, c.right, c.bottom, GetSysColor(COLOR_WINDOW));
  HGDIOBJ old_font = SelectObject(dc, font_ ? static_cast<HGDIOBJ>(font_) : GetStockObject(DEFAULT_GUI_FONT));
  SetBkMode(dc, TRANSPARENT);

  const int row = px(kRowH), step = px(kRowH + kRowGap);
  const int bar_l = px(kLabelW);
  const int counts_l = c.right - px(kCountsW);
  const int lamp_r = counts_l - px(kGap);
  const int lamp_l = lamp_r - px(kLampW);
  const int bar_r = lamp_l - px(kGap);
  const int bar_w = bar_r - bar_l;
  const auto x_of = [&](float db) { return bar_l + int(std::lround(frac(db) * bar_w)); };
  const int inset = std::max(1, row / 5);

  for (size_t i = 0; i < levels_.size(); ++i) {
    const ChannelMeter& m = levels_[i];
    const int t = int(i) * step, b = t + row;

    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    RECT label{0, t, bar_l - px(4), b};
    const std::wstring n = std::to_wstring(i + 1);
    DrawTextW(dc, n.c_str(), -1, &label, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    if (bar_w > 0) {
      fill(dc, bar_l, t, bar_r, b, kTrough);
      for (float db : kTicks) fill(dc, x_of(db), t, x_of(db) + 1, b, kTick);
      // RMS bar, colored by zone: green below -18 dBFS, yellow to -6, red above.
      const int rms_x = x_of(m.rms_db), x18 = x_of(-18), x6 = x_of(-6);
      fill(dc, bar_l, t + inset, std::min(rms_x, x18), b - inset, kGreen);
      fill(dc, x18, t + inset, std::min(rms_x, x6), b - inset, kYellow);
      fill(dc, x6, t + inset, rms_x, b - inset, kRed);
      if (m.peak_db > kFloorDb) {
        const int x = std::min(x_of(m.peak_db), bar_r - px(1));
        fill(dc, x, t + inset, x + px(1), b - inset, kPeak);
      }
      if (m.hold_db > kFloorDb) {
        const int x = std::min(x_of(m.hold_db), bar_r - px(2));
        fill(dc, x, t, x + px(2), b, kHold);
      }
    }
    // Clip lamp: lit while the held peak is at clip level (up to 2 s after a clip).
    const int lamp_t = t + (row - px(kLampW)) / 2;
    fill(dc, lamp_l, lamp_t, lamp_r, lamp_t + px(kLampW), m.hold_db >= kClipDb ? kLampOn : kLampOff);

    SetTextColor(dc, m.clips ? kClipText : GetSysColor(COLOR_GRAYTEXT));
    RECT count{counts_l, t, c.right, b};
    const std::wstring clips = std::to_wstring(m.clips);
    DrawTextW(dc, clips.c_str(), -1, &count, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
  }

  // The scale under the bars, and the clip count heading.
  const int st = int(levels_.size()) * step;
  SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
  if (bar_w > 0)
    for (int db : kLabels) {
      const std::wstring s = std::to_wstring(db);
      SIZE ext{};
      GetTextExtentPoint32W(dc, s.c_str(), int(s.size()), &ext);
      const int l = std::clamp(x_of(float(db)) - int(ext.cx) / 2, bar_l, std::max(bar_l, bar_r - int(ext.cx)));
      RECT r{l, st, l + ext.cx, c.bottom};
      DrawTextW(dc, s.c_str(), -1, &r, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOCLIP);
    }
  RECT head{counts_l, st, c.right, c.bottom};
  DrawTextW(dc, L"Clips", -1, &head, DT_RIGHT | DT_TOP | DT_SINGLELINE);
  SelectObject(dc, old_font);
}

}  // namespace spool
