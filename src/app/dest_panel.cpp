#include "app/dest_panel.h"

#include <windowsx.h>

#include <algorithm>

#include "app/folder_picker.h"
#include "app/format_text.h"
#include "app/resource.h"

namespace spool {

namespace {

constexpr wchar_t kClass[] = L"SpoolDestPanel";

// Layout in 96-dpi pixels.
constexpr int kPad = 8, kBlockGap = 8, kHealthW = 72, kLeftW = 110, kLineExtra = 4;

constexpr COLORREF kNeutral = RGB(242, 242, 242);
constexpr COLORREF kNeutralEdge = RGB(208, 208, 208);
constexpr COLORREF kAmber = RGB(255, 233, 168);
constexpr COLORREF kAmberEdge = RGB(222, 170, 40);
constexpr COLORREF kRed = RGB(255, 204, 204);
constexpr COLORREF kRedEdge = RGB(208, 64, 64);
constexpr COLORREF kText = RGB(28, 28, 28);
constexpr COLORREF kDim = RGB(96, 96, 96);
constexpr COLORREF kFailed = RGB(176, 0, 0);

const wchar_t* health_text(FolderHealth h) {
  switch (h) {
    case FolderHealth::Active: return L"active";
    case FolderHealth::Failed: return L"failed";
    case FolderHealth::Standby: break;
  }
  return L"standby";
}

std::wstring file_name(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

void fill(HDC dc, const RECT& r, COLORREF c) {
  SetDCBrushColor(dc, c);
  FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

}  // namespace

bool DestPanel::register_class(HINSTANCE inst) {
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = proc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = kClass;
  return RegisterClassExW(&wc) != 0;
}

DestPanel::~DestPanel() {
  free_buffer();
  if (bold_) DeleteObject(bold_);
}

HWND DestPanel::create(HWND parent, int id, HINSTANCE inst) {
  return CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL, 0, 0, 0, 0, parent,
                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, this);
}

void DestPanel::set_status(const std::vector<DestStatus>& v, StreamFormat input, bool live) {
  std::vector<Block> blocks;
  blocks.reserve(v.size());
  for (size_t i = 0; i < v.size(); ++i) {
    const DestStatus& d = v[i];
    Block b;
    b.index = i;
    b.name = d.name;
    b.summary = format_summary(d.config, input.channels);
    b.file = d.current_file.empty() ? L"—" : file_name(d.current_file);
    const size_t chs = d.config.channels.empty() ? input.channels : d.config.channels.size();
    const uint64_t rate = input.sample_rate ? data_rate(d.config, input.sample_rate, chs) : 0;
    for (const FolderStatus& f : d.folders)
      b.folders.push_back(FolderRow{f.path, health_text(f.health), format_folder_time_left(f.free_bytes, rate)});
    if (d.state == DestState::LastResort || d.state == DestState::Disabled) b.tone = Tone::Red;
    else if (d.needs_ack) b.tone = Tone::Amber;
    blocks.push_back(std::move(b));
  }
  // Red, then amber, then the rest, each in list order, so trouble is never scrolled away.
  const auto rank = [](const Block& b) { return b.tone == Tone::Red ? 0 : b.tone == Tone::Amber ? 1 : 2; };
  std::stable_sort(blocks.begin(), blocks.end(), [&](const Block& a, const Block& b) { return rank(a) < rank(b); });
  // Which destinations need attention (sorted indices): when that set changes, show the top.
  const auto attention = [](const std::vector<Block>& bs) {
    std::vector<size_t> out;
    for (const Block& b : bs)
      if (b.tone != Tone::Neutral) out.push_back(b.index);
    std::sort(out.begin(), out.end());
    return out;
  };
  live_ = live;
  if (blocks == blocks_) return;
  if (attention(blocks) != attention(blocks_)) top_ = 0;
  blocks_ = std::move(blocks);
  update_scroll();
  if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

int DestPanel::px(int v) const { return MulDiv(v, hwnd_ ? int(GetDpiForWindow(hwnd_)) : 96, 96); }

int DestPanel::line_height() const { return line_h_ ? line_h_ : px(16 + kLineExtra); }

int DestPanel::block_height(const Block& b) const {
  return 2 * px(kPad) + int(2 + b.folders.size()) * line_height();
}

size_t DestPanel::max_top() const {
  if (blocks_.empty() || !hwnd_) return 0;
  RECT c;
  GetClientRect(hwnd_, &c);
  int h = 0;
  size_t top = blocks_.size();
  while (top > 0) {
    const int add = block_height(blocks_[top - 1]) + (top < blocks_.size() ? px(kBlockGap) : 0);
    if (h + add > c.bottom) break;
    h += add;
    --top;
  }
  return std::min(top, blocks_.size() - 1);
}

void DestPanel::update_scroll() {
  if (!hwnd_) return;
  const size_t last = max_top();
  top_ = std::min(top_, last);
  SCROLLINFO si{sizeof(si)};
  si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
  si.nMin = 0;
  si.nMax = blocks_.empty() ? 0 : int(blocks_.size()) - 1;
  si.nPage = UINT(std::max<size_t>(1, blocks_.size() - last));  // all blocks fit: no scrollbar
  si.nPos = int(top_);
  SetScrollInfo(hwnd_, SB_VERT, &si, TRUE);
}

void DestPanel::scroll_to(size_t top) {
  top = std::min(top, max_top());
  if (top == top_) return;
  top_ = top;
  SetScrollPos(hwnd_, SB_VERT, int(top_), TRUE);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void DestPanel::on_vscroll(int code) {
  const size_t page = std::max<size_t>(1, blocks_.size() - max_top());
  switch (code) {
    case SB_LINEUP: scroll_to(top_ ? top_ - 1 : 0); break;
    case SB_LINEDOWN: scroll_to(top_ + 1); break;
    case SB_PAGEUP: scroll_to(top_ > page ? top_ - page : 0); break;
    case SB_PAGEDOWN: scroll_to(top_ + page); break;
    case SB_TOP: scroll_to(0); break;
    case SB_BOTTOM: scroll_to(blocks_.size()); break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: {
      SCROLLINFO si{sizeof(si)};
      si.fMask = SIF_TRACKPOS;
      if (GetScrollInfo(hwnd_, SB_VERT, &si)) scroll_to(size_t(std::max(0, si.nTrackPos)));
      break;
    }
  }
}

int DestPanel::preferred_height() const {
  int h = 0;
  for (size_t i = 0; i < blocks_.size(); ++i) h += block_height(blocks_[i]) + (i ? px(kBlockGap) : 0);
  return h;
}

int DestPanel::hit_test(POINT p) const {
  int y = 0;
  for (size_t i = top_; i < blocks_.size(); ++i) {
    const int h = block_height(blocks_[i]);
    if (p.y >= y && p.y < y + h) return int(i);
    y += h + px(kBlockGap);
  }
  return -1;
}

void DestPanel::context_menu(POINT screen) {
  int i = int(top_);
  if (screen.x == -1 && screen.y == -1) {  // from the keyboard: the first block
    RECT r;
    GetWindowRect(hwnd_, &r);
    screen = POINT{r.left + px(kPad), r.top + px(kPad)};
  } else {
    POINT c = screen;
    ScreenToClient(hwnd_, &c);
    i = hit_test(c);
  }
  if (i < 0 || size_t(i) >= blocks_.size()) return;
  // Blocks can re-sort while the menu or the folder picker is up: keep the destination's index.
  const size_t dest = blocks_[size_t(i)].index;
  HMENU m = CreatePopupMenu();
  if (!m) return;
  const std::wstring text = L"Add folder to \"" + blocks_[size_t(i)].name + L"\"…";
  AppendMenuW(m, MF_STRING | (live_ ? 0 : MF_GRAYED), IDM_ADD_FOLDER, text.c_str());
  const UINT cmd = UINT(TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screen.x, screen.y, 0,
                                       hwnd_, nullptr));
  DestroyMenu(m);
  if (cmd != IDM_ADD_FOLDER) return;
  const std::wstring path = pick_folder(GetAncestor(hwnd_, GA_ROOT));
  if (!path.empty() && on_add_) on_add_(dest, path);
}

void DestPanel::set_font(HFONT f) {
  font_ = f;
  if (bold_) DeleteObject(bold_);
  bold_ = nullptr;
  LOGFONTW lf{};
  if (f && GetObjectW(f, sizeof(lf), &lf)) {
    lf.lfWeight = FW_SEMIBOLD;
    bold_ = CreateFontIndirectW(&lf);
  }
  line_h_ = 0;
  if (HDC dc = hwnd_ ? GetDC(hwnd_) : nullptr) {
    HGDIOBJ old = SelectObject(dc, bold_ ? static_cast<HGDIOBJ>(bold_) : GetStockObject(DEFAULT_GUI_FONT));
    TEXTMETRICW tm{};
    if (GetTextMetricsW(dc, &tm)) line_h_ = tm.tmHeight + px(kLineExtra);
    SelectObject(dc, old);
    ReleaseDC(hwnd_, dc);
  }
}

void DestPanel::free_buffer() {
  if (!mem_dc_) return;
  SelectObject(mem_dc_, old_bmp_);
  DeleteObject(mem_bmp_);
  DeleteDC(mem_dc_);
  mem_dc_ = nullptr;
  mem_bmp_ = old_bmp_ = nullptr;
  mem_size_ = {};
}

LRESULT CALLBACK DestPanel::proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    auto* self = static_cast<DestPanel*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    self->hwnd_ = h;
    SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  auto* self = reinterpret_cast<DestPanel*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  return self ? self->handle(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

LRESULT DestPanel::handle(UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_SETFONT:
      set_font(reinterpret_cast<HFONT>(wp));
      update_scroll();  // block heights follow the font
      if (LOWORD(lp)) InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    case WM_GETFONT:
      return reinterpret_cast<LRESULT>(font_);
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      update_scroll();
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    case WM_VSCROLL:
      on_vscroll(LOWORD(wp));
      return 0;
    case WM_MOUSEWHEEL:
      wheel_ += GET_WHEEL_DELTA_WPARAM(wp);
      for (; wheel_ >= WHEEL_DELTA; wheel_ -= WHEEL_DELTA) on_vscroll(SB_LINEUP);
      for (; wheel_ <= -WHEEL_DELTA; wheel_ += WHEEL_DELTA) on_vscroll(SB_LINEDOWN);
      return 0;
    case WM_CONTEXTMENU:
      context_menu(POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
      return 0;
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

void DestPanel::paint(HDC dc, const RECT& c) const {
  fill(dc, c, GetSysColor(COLOR_WINDOW));
  HGDIOBJ normal = font_ ? static_cast<HGDIOBJ>(font_) : GetStockObject(DEFAULT_GUI_FONT);
  HGDIOBJ bold = bold_ ? static_cast<HGDIOBJ>(bold_) : normal;
  HGDIOBJ old_font = SelectObject(dc, normal);
  SetBkMode(dc, TRANSPARENT);
  const int line = line_height(), pad = px(kPad);
  constexpr UINT kOne = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX;

  int y = 0;
  for (size_t i = top_; i < blocks_.size() && y < c.bottom; ++i) {
    const Block& b = blocks_[i];
    const int h = block_height(b);
    const RECT box{0, y, c.right, y + h};
    const COLORREF bg = b.tone == Tone::Red ? kRed : b.tone == Tone::Amber ? kAmber : kNeutral;
    const COLORREF edge = b.tone == Tone::Red ? kRedEdge : b.tone == Tone::Amber ? kAmberEdge : kNeutralEdge;
    fill(dc, box, edge);
    const RECT inner{box.left + px(1), box.top + px(1), box.right - px(1), box.bottom - px(1)};
    fill(dc, inner, bg);

    const int l = pad, r = c.right - pad;
    int ty = y + pad;
    // Line 1: name and format summary.
    SelectObject(dc, bold);
    SetTextColor(dc, kText);
    RECT name{l, ty, r, ty + line};
    DrawTextW(dc, b.name.c_str(), -1, &name, kOne | DT_END_ELLIPSIS | DT_CALCRECT);
    name.top = ty;
    name.bottom = ty + line;
    name.right = std::min(name.right, LONG(r));
    DrawTextW(dc, b.name.c_str(), -1, &name, kOne | DT_END_ELLIPSIS);
    SelectObject(dc, normal);
    RECT sum{name.right + px(12), ty, r, ty + line};
    DrawTextW(dc, b.summary.c_str(), -1, &sum, kOne | DT_END_ELLIPSIS);
    ty += line;
    // Line 2: the current file.
    SetTextColor(dc, kDim);
    RECT file{l, ty, r, ty + line};
    DrawTextW(dc, b.file.c_str(), -1, &file, kOne | DT_PATH_ELLIPSIS);
    ty += line;
    // One row per folder: path, state, time left.
    const int left_l = r - px(kLeftW), health_l = left_l - px(kHealthW);
    for (const FolderRow& f : b.folders) {
      SetTextColor(dc, kText);
      RECT path{l + px(12), ty, health_l - px(8), ty + line};
      DrawTextW(dc, f.path.c_str(), -1, &path, kOne | DT_PATH_ELLIPSIS);
      SetTextColor(dc, f.health == L"failed" ? kFailed : kText);
      RECT health{health_l, ty, left_l, ty + line};
      DrawTextW(dc, f.health.c_str(), -1, &health, kOne);
      SetTextColor(dc, kText);
      RECT left{left_l, ty, r, ty + line};
      DrawTextW(dc, f.left.c_str(), -1, &left, kOne | DT_RIGHT);
      ty += line;
    }
    y += h + px(kBlockGap);
  }
  SelectObject(dc, old_font);
}

}  // namespace spool
