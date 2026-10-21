#include "app/markers_window.h"

#include <commctrl.h>

#include <algorithm>

#include "app/format_text.h"

namespace spool {

namespace {

constexpr wchar_t kClass[] = L"SpoolMarkers";
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kRefreshMs = 250;
constexpr int kListId = 1;

const wchar_t kHint[] = L"F2, Enter or a double-click renames the selected marker. The new name goes into the "
                        L"WAV files and the manifests.";
const wchar_t kEmpty[] = L"No markers yet. Press Marker (or M) while recording.";

}  // namespace

MarkersWindow::MarkersWindow(Source s, Rename r) : source_(std::move(s)), rename_(std::move(r)) {}

MarkersWindow::~MarkersWindow() {
  if (hwnd_) DestroyWindow(hwnd_);
  if (font_) DeleteObject(font_);
}

bool MarkersWindow::register_class(HINSTANCE inst) {
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = proc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kClass;
  return RegisterClassExW(&wc) != 0;
}

void MarkersWindow::show(HWND owner, HINSTANCE inst, bool edit_last) {
  if (!hwnd_) {
    // Beside the dashboard's right edge, on the same monitor.
    RECT o{};
    GetWindowRect(owner, &o);
    const int dpi = int(GetDpiForWindow(owner));
    const int w = MulDiv(440, dpi, 96), h = MulDiv(380, dpi, 96);
    CreateWindowExW(0, kClass, L"Markers", WS_OVERLAPPEDWINDOW & ~WS_MINIMIZEBOX & ~WS_MAXIMIZEBOX,
                    o.right - w - MulDiv(24, dpi, 96), o.top + MulDiv(90, dpi, 96), w, h, owner, nullptr, inst, this);
    if (!hwnd_) return;
  }
  ShowWindow(hwnd_, SW_SHOW);
  SetForegroundWindow(hwnd_);
  refresh();
  SetFocus(list_);
  const int n = ListView_GetItemCount(list_);
  if (edit_last && n > 0) edit(n - 1);
}

LRESULT CALLBACK MarkersWindow::proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    auto* self = static_cast<MarkersWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    self->hwnd_ = h;
    SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  auto* self = reinterpret_cast<MarkersWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  return self ? self->handle(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

LRESULT MarkersWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: {
      list_ = CreateWindowExW(0, WC_LISTVIEWW, L"",
                              WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | LVS_REPORT | LVS_EDITLABELS |
                                  LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                              0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListId)),
                              GetModuleHandleW(nullptr), nullptr);
      ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
      const wchar_t* cols[] = {L"Name", L"Time", L"Position"};
      for (int i = 0; i < 3; ++i) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        c.pszText = const_cast<wchar_t*>(cols[i]);
        c.cx = 100;
        c.iSubItem = i;
        ListView_InsertColumn(list_, i, &c);
      }
      hint_ = CreateWindowExW(0, WC_STATICW, kHint, WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, hwnd_,
                              nullptr, GetModuleHandleW(nullptr), nullptr);
      make_font();
      SetTimer(hwnd_, kTimerId, kRefreshMs, nullptr);
      return 0;
    }
    case WM_SIZE:
      layout();
      return 0;
    case WM_TIMER:
      if (wp == kTimerId && IsWindowVisible(hwnd_)) refresh();
      return 0;
    case WM_DPICHANGED: {
      make_font();
      const RECT* r = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_GETMINMAXINFO:
      reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = POINT{px(300), px(200)};
      return 0;
    case WM_CTLCOLORSTATIC: {
      HDC dc = reinterpret_cast<HDC>(wp);
      SetBkColor(dc, GetSysColor(COLOR_WINDOW));
      SetTextColor(dc, GetSysColor(COLOR_GRAYTEXT));
      return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_ACTIVATE:
      if (LOWORD(wp) != WA_INACTIVE && list_) SetFocus(list_);
      return 0;
    case WM_NOTIFY:
      return on_notify(reinterpret_cast<NMHDR*>(lp));
    case WM_CLOSE:
      ShowWindow(hwnd_, SW_HIDE);  // kept for the next show()
      return 0;
    case WM_NCDESTROY: {
      HWND h = hwnd_;
      KillTimer(h, kTimerId);
      SetWindowLongPtrW(h, GWLP_USERDATA, 0);
      hwnd_ = list_ = hint_ = nullptr;
      return DefWindowProcW(h, msg, wp, lp);
    }
  }
  return DefWindowProcW(hwnd_, msg, wp, lp);
}

LRESULT MarkersWindow::on_notify(NMHDR* n) {
  if (n->hwndFrom != list_) return 0;
  switch (n->code) {
    case LVN_BEGINLABELEDITW:
      if (HWND e = ListView_GetEditControl(list_)) SendMessageW(e, EM_LIMITTEXT, SessionManager::kMaxMarkerName, 0);
      return FALSE;  // allow
    case LVN_ENDLABELEDITW: {
      const LVITEMW& it = reinterpret_cast<NMLVDISPINFOW*>(n)->item;
      if (it.pszText && it.iItem >= 0 && size_t(it.iItem) < shown_.size())
        rename_(shown_[size_t(it.iItem)].first, it.pszText);
      PostMessageW(hwnd_, WM_TIMER, kTimerId, 0);  // shows the name as stored (trimmed)
      return FALSE;
    }
    case NM_DBLCLK:
      edit(reinterpret_cast<NMITEMACTIVATE*>(n)->iItem);
      return 0;
    case NM_RETURN:
      edit(ListView_GetNextItem(list_, -1, LVNI_SELECTED));
      return 0;
    case LVN_KEYDOWN: {
      const WORD key = reinterpret_cast<NMLVKEYDOWN*>(n)->wVKey;
      if (key == VK_F2) edit(ListView_GetNextItem(list_, -1, LVNI_SELECTED));
      if (key == VK_ESCAPE) ShowWindow(hwnd_, SW_HIDE);
      return 0;
    }
  }
  return 0;
}

void MarkersWindow::edit(int item) {
  if (item < 0 || item >= ListView_GetItemCount(list_)) return;
  SetFocus(list_);
  ListView_SetItemState(list_, item, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
  ListView_EnsureVisible(list_, item, FALSE);
  ListView_EditLabel(list_, item);
}

void MarkersWindow::refresh() {
  if (!list_ || ListView_GetEditControl(list_)) return;  // never under the user's typing
  const std::vector<MarkerInfo> ms = source_();
  std::vector<std::pair<uint32_t, std::wstring>> rows;
  for (const MarkerInfo& m : ms) rows.emplace_back(m.id, m.name);
  SetWindowTextW(hint_, ms.empty() ? kEmpty : kHint);
  if (rows == shown_) return;

  const int sel = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
  const uint32_t sel_id = sel >= 0 && size_t(sel) < shown_.size() ? shown_[size_t(sel)].first : 0;
  const bool grew = rows.size() > shown_.size();
  SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
  ListView_DeleteAllItems(list_);
  int select = -1;
  for (size_t i = 0; i < ms.size(); ++i) {
    LVITEMW it{};
    it.mask = LVIF_TEXT;
    it.iItem = int(i);
    it.pszText = const_cast<wchar_t*>(ms[i].name.c_str());
    ListView_InsertItem(list_, &it);
    std::wstring t = format_clock(ms[i].wall), p = format_elapsed(ms[i].elapsed_s);
    ListView_SetItemText(list_, int(i), 1, t.data());
    ListView_SetItemText(list_, int(i), 2, p.data());
    if (ms[i].id == sel_id) select = int(i);
  }
  if (grew && !ms.empty()) select = int(ms.size()) - 1;  // a new marker: follow it
  if (select >= 0) {
    ListView_SetItemState(list_, select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(list_, select, FALSE);
  }
  SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(list_, nullptr, TRUE);
  shown_ = std::move(rows);
}

int MarkersWindow::px(int v) const { return MulDiv(v, hwnd_ ? int(GetDpiForWindow(hwnd_)) : 96, 96); }

void MarkersWindow::make_font() {
  if (font_) DeleteObject(font_);
  font_ = nullptr;
  NONCLIENTMETRICSW ncm{};
  ncm.cbSize = sizeof(ncm);
  LOGFONTW lf{};
  if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, GetDpiForWindow(hwnd_)))
    lf = ncm.lfMessageFont;
  else
    GetObjectW(GetStockObject(DEFAULT_GUI_FONT), sizeof(lf), &lf);
  font_ = CreateFontIndirectW(&lf);
  for (HWND h : {list_, hint_}) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
  layout();
}

void MarkersWindow::layout() {
  if (!list_) return;
  RECT c;
  GetClientRect(hwnd_, &c);
  const int m = px(10), hint_h = px(36);
  const int w = c.right - 2 * m;
  SetWindowPos(list_, nullptr, m, m, w, std::max(0, int(c.bottom) - 3 * m - hint_h), SWP_NOZORDER);
  SetWindowPos(hint_, nullptr, m, c.bottom - m - hint_h, w, hint_h, SWP_NOZORDER);
  const int time_w = px(80), pos_w = px(80);
  ListView_SetColumnWidth(list_, 1, time_w);
  ListView_SetColumnWidth(list_, 2, pos_w);
  const int name_w = w - time_w - pos_w - GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(hwnd_)) - px(6);
  ListView_SetColumnWidth(list_, 0, std::max(px(100), name_w));
}

}  // namespace spool
