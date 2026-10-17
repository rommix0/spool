#include "app/main_window.h"

#include <commctrl.h>

#include <algorithm>
#include <optional>

#include "app/format_text.h"
#include "app/lifecycle.h"
#include "app/resource.h"
#include "app/settings_dialog.h"
#include "app/text_util.h"
#include "audio/wasapi_source.h"
#include "core/clock.h"

namespace sl {

namespace {

constexpr wchar_t kTitle[] = L"Sound logger";
constexpr wchar_t kCantStart[] = L"Can't start recording";
constexpr wchar_t kAppLog[] = L"app";  // app.log: events outside a session
constexpr UINT kLaunchMsg = WM_APP + 1;
constexpr COLORREF kBannerBack = RGB(255, 233, 168);  // the destination panel's amber
constexpr COLORREF kBannerLost = RGB(255, 204, 204);  // ... and its red
constexpr COLORREF kBannerText = RGB(28, 28, 28);
constexpr UINT_PTR kTimerId = 1;
constexpr UINT kTimerMs = 33;
constexpr uint64_t kSlowMs = 250;  // stats, destinations and events at 4 Hz
constexpr uint64_t kResumeRetryMs = 1000;  // a crashed session waiting for its input (R18)
constexpr int kHotkeyId = 1;
constexpr size_t kEventRows = 500;

std::wstring one_line(std::wstring s) {
  std::replace_if(s.begin(), s.end(), [](wchar_t c) { return c == L'\r' || c == L'\n'; }, L' ');
  return s;
}

HWND child(HWND parent, HINSTANCE inst, const wchar_t* cls, const wchar_t* text, DWORD style, int id) {
  return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent,
                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
}

void place(HWND h, int x, int y, int w, int hgt) {
  SetWindowPos(h, nullptr, x, y, std::max(0, w), std::max(0, hgt), SWP_NOZORDER | SWP_NOACTIVATE);
}

int text_height(HWND h, HFONT f) {
  int out = 16;
  if (HDC dc = GetDC(h)) {
    HGDIOBJ old = SelectObject(dc, f);
    TEXTMETRICW tm{};
    if (GetTextMetricsW(dc, &tm)) out = tm.tmHeight;
    SelectObject(dc, old);
    ReleaseDC(h, dc);
  }
  return out;
}

int text_width(HWND h, HFONT f, const wchar_t* s) {
  int out = 0;
  if (HDC dc = GetDC(h)) {
    HGDIOBJ old = SelectObject(dc, f);
    SIZE ext{};
    if (GetTextExtentPoint32W(dc, s, int(wcslen(s)), &ext)) out = ext.cx;
    SelectObject(dc, old);
    ReleaseDC(h, dc);
  }
  return out;
}

}  // namespace

MainWindow::MainWindow(AppContext ctx) : ctx_(std::move(ctx)) {}

MainWindow::~MainWindow() {
  if (hwnd_) DestroyWindow(hwnd_);
  for (HFONT f : {font_, bold_, clock_font_})
    if (f) DeleteObject(f);
  if (amber_) DeleteObject(amber_);
  if (red_) DeleteObject(red_);
}

bool MainWindow::create(HINSTANCE inst, int show) {
  inst_ = inst;
  if (!MeterControl::register_class(inst) || !DestPanel::register_class(inst)) return false;
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = proc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
  wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  wc.lpszClassName = kClassName;
  if (!RegisterClassExW(&wc)) return false;
  accel_ = LoadAcceleratorsW(inst, MAKEINTRESOURCEW(IDR_ACCEL));
  if (!CreateWindowExW(0, kClassName, kTitle, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                       CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, inst, this))
    return false;
  SetWindowPos(hwnd_, nullptr, 0, 0, px(940), px(760), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  ShowWindow(hwnd_, show);
  UpdateWindow(hwnd_);
  PostMessageW(hwnd_, kLaunchMsg, 0, 0);  // recovery runs once the window is up
  return true;
}

bool MainWindow::translate(MSG& m) {
  if (!hwnd_) return false;
  // Auto-repeat of a held M or S would drop a marker or split per repeat: ignore it.
  if (m.message == WM_KEYDOWN && (m.wParam == 'M' || m.wParam == 'S') && (m.lParam & (1 << 30))) return true;
  if (accel_ && TranslateAcceleratorW(hwnd_, accel_, &m)) return true;
  return IsDialogMessageW(hwnd_, &m) != 0;
}

LRESULT CALLBACK MainWindow::proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    auto* self = static_cast<MainWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    self->hwnd_ = h;
    SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  auto* self = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  return self ? self->handle(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

LRESULT MainWindow::handle(UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      on_create();
      return 0;
    case WM_SIZE:
      layout();
      return 0;
    case WM_GETMINMAXINFO: {
      auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
      mm->ptMinTrackSize = POINT{px(860), px(560)};
      return 0;
    }
    case WM_DPICHANGED: {
      make_fonts();
      apply_fonts();
      const RECT* r = reinterpret_cast<const RECT*>(lp);
      SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      layout();
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hwnd_, &ps);
      if (shown_ != Banner::None) FillRect(dc, &banner_rect_, shown_ == Banner::InputLost ? red_ : amber_);
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
      HDC dc = reinterpret_cast<HDC>(wp);
      if (reinterpret_cast<HWND>(lp) == banner_ || reinterpret_cast<HWND>(lp) == banner_ack_) {
        const bool lost = shown_ == Banner::InputLost;
        SetBkColor(dc, lost ? kBannerLost : kBannerBack);
        SetTextColor(dc, kBannerText);
        return reinterpret_cast<LRESULT>(lost ? red_ : amber_);
      }
      SetBkColor(dc, GetSysColor(COLOR_WINDOW));
      SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
      return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_ACTIVATE:
      // Keep the keyboard focus across message boxes and app switches (not a dialog class,
      // so nothing else remembers it); refresh_slow() then moves it off a button it disables.
      if (LOWORD(wp) == WA_INACTIVE) {
        focus_ = GetFocus();
      } else if (!HIWORD(wp)) {
        HWND to = focus_ && IsChild(hwnd_, focus_) && IsWindowEnabled(focus_) ? focus_ : nullptr;
        if (!to) to = IsWindowEnabled(record_) ? record_ : IsWindowEnabled(stop_) ? stop_ : hwnd_;
        SetFocus(to);
      }
      return 0;
    case WM_TIMER:
      if (wp == kTimerId) tick();
      return 0;
    case WM_COMMAND:
      // Controls send BN_CLICKED; accelerators send 1, menus 0.
      if (lp && HIWORD(wp) != BN_CLICKED) break;
      on_command(LOWORD(wp));
      return 0;
    case WM_NOTIFY:
      return on_notify(reinterpret_cast<NMHDR*>(lp));
    case WM_HOTKEY:
      if (wp == kHotkeyId) marker(0);
      return 0;
    case WM_CLOSE:
      on_close();
      return 0;
    case WM_ENDSESSION:
      if (wp) ctx_.mgr.stop();  // Windows is ending the session: finalize the files
      return 0;
    case kLaunchMsg:
      on_launch();
      return 0;
    case WM_DESTROY:
      KillTimer(hwnd_, kTimerId);
      if (hotkey_) UnregisterHotKey(hwnd_, kHotkeyId);
      hotkey_ = false;
      PostQuitMessage(0);
      return 0;
    case WM_NCDESTROY: {
      HWND h = hwnd_;
      SetWindowLongPtrW(h, GWLP_USERDATA, 0);
      hwnd_ = nullptr;
      return DefWindowProcW(h, msg, wp, lp);
    }
  }
  return DefWindowProcW(hwnd_, msg, wp, lp);
}

void MainWindow::on_create() {
  constexpr DWORD kButton = WS_TABSTOP | BS_PUSHBUTTON;
  constexpr DWORD kLabel = SS_LEFT | SS_NOPREFIX | SS_CENTERIMAGE;
  record_ = child(hwnd_, inst_, WC_BUTTONW, L"Record", kButton, IDC_RECORD);
  stop_ = child(hwnd_, inst_, WC_BUTTONW, L"Stop", kButton, IDC_STOP);
  clock_ = child(hwnd_, inst_, WC_STATICW, L"00:00:00", kLabel, IDC_CLOCK);
  device_ = child(hwnd_, inst_, WC_STATICW, L"", kLabel | SS_ENDELLIPSIS, IDC_DEVICE);
  marker_ = child(hwnd_, inst_, WC_BUTTONW, L"Marker", WS_TABSTOP | BS_SPLITBUTTON, IDC_MARKER);
  split_ = child(hwnd_, inst_, WC_BUTTONW, L"Split file", kButton, IDC_SPLIT);
  settings_ = child(hwnd_, inst_, WC_BUTTONW, L"Settings…", kButton, IDC_SETTINGS);
  levels_label_ = child(hwnd_, inst_, WC_STATICW, L"Levels", kLabel, IDC_LEVELS_LABEL);
  meters_.create(hwnd_, IDC_METERS, inst_);
  buffer_ = child(hwnd_, inst_, WC_STATICW, L"", kLabel, IDC_BUFFER);
  dests_label_ = child(hwnd_, inst_, WC_STATICW, L"Destinations", kLabel, IDC_DESTS_LABEL);
  ack_ = child(hwnd_, inst_, WC_BUTTONW, L"Acknowledge", kButton, IDC_ACK);
  ShowWindow(ack_, SW_HIDE);
  amber_ = CreateSolidBrush(kBannerBack);
  red_ = CreateSolidBrush(kBannerLost);
  banner_ = child(hwnd_, inst_, WC_STATICW, L"", kLabel, IDC_BANNER);
  banner_ack_ = child(hwnd_, inst_, WC_BUTTONW, L"Acknowledge", kButton, IDC_BANNER_ACK);
  ShowWindow(banner_, SW_HIDE);
  ShowWindow(banner_ack_, SW_HIDE);
  dests_.create(hwnd_, IDC_DESTS, inst_);
  events_label_ = child(hwnd_, inst_, WC_STATICW, L"Events", kLabel, IDC_EVENTS_LABEL);
  events_ = CreateWindowExW(0, WC_LISTVIEWW, L"",
                            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA |
                                LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
                            0, 0, 0, 0, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EVENTS)), inst_,
                            nullptr);
  ListView_SetExtendedListViewStyle(events_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  const wchar_t* cols[] = {L"Time", L"Destination", L"Event"};
  for (int i = 0; i < 3; ++i) {
    LVCOLUMNW c{};
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    c.pszText = const_cast<wchar_t*>(cols[i]);
    c.cx = 100;
    c.iSubItem = i;
    ListView_InsertColumn(events_, i, &c);
  }

  meters_.on_reset_clips([this] {
    if (MeterReader* m = ctx_.mgr.meter()) m->reset_clips();
  });
  dests_.on_add_folder([this](size_t dest, std::wstring path) { add_folder(dest, std::move(path)); });

  make_fonts();
  apply_fonts();
  update_device_name();

  update_hotkey();

  tick();
  refresh_slow();
  layout();
  SetTimer(hwnd_, kTimerId, kTimerMs, nullptr);
}

void MainWindow::on_command(int id) {
  switch (id) {
    case IDC_RECORD: record(); break;
    case IDC_STOP: confirm_stop(); break;
    case IDC_MARKER:
    case IDM_MARKER_NOW: marker(0); break;
    case IDM_MARKER_5: marker(5); break;
    case IDM_MARKER_10: marker(10); break;
    case IDM_MARKER_30: marker(30); break;
    case IDC_SPLIT:
    case IDM_SPLIT: split(); break;
    case IDC_SETTINGS: settings(); break;
    case IDC_ACK:
      if (recording()) ctx_.mgr.acknowledge();
      break;
    case IDC_BANNER_ACK:  // a notice, not a status (Waiting, InputLost)
      if (shown_ == banner_mode_ && banner_mode_ != Banner::Waiting) show_banner(Banner::None);
      break;
  }
}

LRESULT MainWindow::on_notify(NMHDR* n) {
  if (n->hwndFrom == marker_ && n->code == BCN_DROPDOWN) {
    RECT r = reinterpret_cast<NMBCDROPDOWN*>(n)->rcButton;
    MapWindowPoints(marker_, nullptr, reinterpret_cast<POINT*>(&r), 2);
    marker_menu(r);
    return 0;
  }
  if (n->hwndFrom != events_) return 0;
  if (n->code == LVN_ODFINDITEMW) return -1;  // no type-ahead: it would select a row
  if (n->code == LVN_GETDISPINFOW) {
    LVITEMW& it = reinterpret_cast<NMLVDISPINFOW*>(n)->item;
    if ((it.mask & LVIF_TEXT) && it.iItem >= 0 && size_t(it.iItem) < rows_.size() && it.cchTextMax > 0) {
      const EventRow& r = rows_[size_t(it.iItem)];
      const std::wstring& s = it.iSubItem == 0 ? r.time : it.iSubItem == 1 ? r.dest : r.text;
      lstrcpynW(it.pszText, s.c_str(), it.cchTextMax);
    }
    return 0;
  }
  if (n->code == NM_CUSTOMDRAW) {
    auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(n);
    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
      const size_t i = size_t(cd->nmcd.dwItemSpec);
      if (i < rows_.size() && rows_[i].sev != Severity::Info) {
        cd->clrText = rows_[i].sev == Severity::Error ? RGB(192, 0, 0) : RGB(150, 84, 0);
        return CDRF_NEWFONT;
      }
    }
    return CDRF_DODEFAULT;
  }
  return 0;
}

void MainWindow::on_close() {
  if (recording()) {
    if (box(L"Recording is in progress. Stop recording and exit?", kTitle,
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
      return;
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    ctx_.mgr.stop();
    SetCursor(old);
    use_log(kAppLog);
  }
  DestroyWindow(hwnd_);
}

// ---- commands ----

bool MainWindow::recording() const { return ctx_.mgr.stats().state != SessionState::Idle; }

SessionConfig MainWindow::session_config() const {
  const AppSettings& s = ctx_.settings;
  const StreamFormat f = ctx_.source.format();
  SessionConfig cfg;
  cfg.params.session_name = s.session_name;
  cfg.params.rate = f.sample_rate;
  cfg.params.input_channels = f.channels;
  cfg.params.flush_ms = s.flush_seconds * 1000;
  cfg.params.low_water_minutes = s.low_water_minutes;
  cfg.params.low_water_min_bytes = uint64_t(s.low_water_min_gb) << 30;
  cfg.params.stall_percent = s.stall_percent;
  cfg.dests = s.dests;
  cfg.ring_seconds = s.ring_seconds;
  if (ctx_.wasapi) cfg.input_device_id = ctx_.wasapi->device_id();  // R19: resume on the same device
  return cfg;
}

int MainWindow::box(const std::wstring& text, const wchar_t* title, UINT flags) {
  ++modal_;  // tick() starts nothing while a modal loop runs
  const int r = MessageBoxW(hwnd_, text.c_str(), title, flags);
  --modal_;
  return r;
}

void MainWindow::record() {
  if (!pending_) {
    start_recording(true);
    return;
  }
  // A crashed session is waiting for its input: Record retries that now.
  try_resume();
  if (pending_)
    box(L"Waiting for the input device to resume the crashed session.\n\n" + pending_err_, kCantStart,
        MB_OK | MB_ICONINFORMATION);
}

bool MainWindow::input_ready(std::wstring* err) {
  if (reopen_input_ && !recording()) {  // an input change saved while a session was running
    reopen_input_ = false;
    const bool ok = ctx_.source.open(ctx_.settings.input, err);
    update_device_name();
    return ok;
  }
  if (ctx_.source.format().sample_rate) return true;
  if (!ctx_.source.open(ctx_.settings.input, err)) return false;  // failed at launch: try again
  update_device_name();
  return true;
}

void MainWindow::before_start() {
  // Lets the session VirtualLock its ring; if Windows refuses, the session logs the warning.
  const StreamFormat f = ctx_.source.format();
  raise_working_set(working_set_for_ring(ctx_.settings.ring_seconds, f.sample_rate, f.channels));
}

bool MainWindow::start_recording(bool ask) {
  if (recording()) return false;
  const auto block = [this](const std::wstring& text) {
    box(text, kCantStart, MB_OK | MB_ICONERROR);
    return false;
  };
  std::wstring err;
  if (!input_ready(&err)) return block(err);
  const std::vector<std::wstring> problems = validate(ctx_.settings, ctx_.source.format().channels);
  if (!problems.empty()) return block(join_lines(problems));
  const SessionConfig cfg = session_config();
  HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
  const PreflightResult pf = ctx_.mgr.preflight(cfg);
  SetCursor(old);
  if (!pf.can_start) return block(join_lines(pf.errors));
  // Destinations that will start in LastResort are not a block while another one works.
  std::vector<std::wstring> lines = pf.errors;
  lines.insert(lines.end(), pf.warnings.begin(), pf.warnings.end());
  if (ask && !lines.empty() &&
      box(join_lines(lines) + L"\n\nStart anyway?", kTitle, MB_YESNO | MB_ICONWARNING) != IDYES)
    return false;
  before_start();
  if (!ctx_.mgr.start(cfg, nullptr, &err)) {
    use_log(kAppLog);
    return block(err);
  }
  if (!ask)  // auto-start: into the session's log
    for (const std::wstring& l : lines) warn(l);
  after_start();
  return true;
}

void MainWindow::after_start() {
  if (ctx_.wasapi && !ctx_.wasapi->raw_active())
    warn(L"Windows audio processing couldn't be turned off for this device.");
  show_config_ = false;
  refresh_slow();
}

void MainWindow::on_launch() {
  // Recovery runs on every launch, whether or not Windows passed --recover.
  const std::optional<RecordingState> st = ctx_.store.load();
  if (st && st->recording) {
    pending_ = *st;
    pending_since_ = GetTickCount64();
    use_log(st->session_id);  // the repair lines belong with the crashed session's events
    try_resume();
  } else if (ctx_.settings.start_on_launch) {
    start_recording(false);
  }
}

// R18: at launch, then once a second from tick() while the input device is missing.
void MainWindow::try_resume() {
  const uint64_t now = GetTickCount64();
  pending_try_ = now;
  pending_err_.clear();
  bool ready;
  const bool open = ctx_.source.format().sample_rate != 0;
  if (ctx_.wasapi && resume_needs_open(ctx_.wasapi->device_id(), open, pending_->device_id)) {
    // R19: only the device the session recorded from, not whatever is the default now.
    ready = ctx_.source.open(resume_input(ctx_.settings.input, *pending_), &pending_err_);
    update_device_name();
  } else {
    ready = input_ready(&pending_err_);
  }
  if (ready) before_start();
  HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
  const LaunchRecoveryResult r = resume_when_input_ready(*pending_, ready, now - pending_since_, session_config(),
                                                         ctx_.mgr, ctx_.store, ctx_.fs, system_clock(), ctx_.log);
  SetCursor(old);
  if (r.outcome == LaunchRecovery::Waiting) {
    if (banner_mode_ != Banner::Waiting) {
      warn(L"Waiting for the input device to resume the crashed session. Can't open it: " + pending_err_);
      show_banner(Banner::Waiting);
    }
    return;
  }
  pending_.reset();
  if (r.outcome == LaunchRecovery::Resumed) {
    show_banner(Banner::Recovered);
    after_start();
    return;
  }
  use_log(kAppLog);
  const bool lost = r.outcome == LaunchRecovery::Failed || r.outcome == LaunchRecovery::GaveUp;
  // Unattended first: start_on_launch records before anything waits for a click. The
  // failure is already in the event log; the banner reports it.
  if (auto_start_after(r.outcome, ctx_.settings.start_on_launch) && start_recording(false)) {
    show_banner(lost ? Banner::NotResumedNewRecording : Banner::None);
    return;
  }
  show_banner(r.outcome == LaunchRecovery::GaveUp ? Banner::NotResumed : Banner::None);
  if (r.outcome == LaunchRecovery::Failed)
    box(L"The crashed session couldn't be resumed.\n\n" + r.error, kCantStart, MB_OK | MB_ICONERROR);
}

void MainWindow::use_log(const std::wstring& name) { ctx_.log.set_file(log_file(name)); }

void MainWindow::show_banner(Banner mode) {
  if (mode == banner_mode_) return;
  banner_mode_ = mode;
  update_banner();
}

void MainWindow::update_banner() {
  const Banner mode = input_lost_ ? Banner::InputLost : banner_mode_;
  if (mode == shown_) return;
  shown_ = mode;
  // Waiting and InputLost are statuses; the others are notices to acknowledge.
  const bool ack = mode != Banner::None && mode != Banner::Waiting && mode != Banner::InputLost;
  if (!ack && GetFocus() == banner_ack_) SetFocus(IsWindowEnabled(stop_) ? stop_ : record_);
  const wchar_t* text = L"";
  switch (mode) {
    case Banner::None: break;
    case Banner::Waiting: text = L"Waiting for the input device to resume the crashed session."; break;
    case Banner::Recovered: text = L"Recovered from a crash. Recording resumed."; break;
    case Banner::NotResumed: text = L"The crashed session couldn't be resumed."; break;
    case Banner::NotResumedNewRecording:
      text = L"The crashed session couldn't be resumed. A new recording started.";
      break;
    case Banner::InputLost: text = L"Input lost. Waiting for the device to come back."; break;
  }
  SetWindowTextW(banner_, text);
  ShowWindow(banner_, mode != Banner::None ? SW_SHOWNA : SW_HIDE);
  ShowWindow(banner_ack_, ack ? SW_SHOWNA : SW_HIDE);
  layout();
  // Everything below the banner moved: repaint all of it (copied bits can be stale).
  RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void MainWindow::confirm_stop() {
  if (!recording()) return;
  // No is the default button, so a stray Enter or Space keeps recording.
  if (box(L"Stop recording?", kTitle, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
  HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
  ctx_.mgr.stop();
  SetCursor(old);
  use_log(kAppLog);
  refresh_slow();
}

void MainWindow::marker(uint32_t seconds_ago) {
  if (recording()) ctx_.mgr.marker(seconds_ago);
}

void MainWindow::split() {
  if (recording()) ctx_.mgr.split();
}

void MainWindow::settings() {
  if (recording()) {
    box(L"Stop recording to change settings.", kTitle, MB_OK | MB_ICONINFORMATION);
    return;
  }
  SettingsChoices choices;
  if (ctx_.wasapi) {
    choices.devices = list_capture_devices();
    choices.default_device_id = default_capture_device_id();
    choices.exclusive_rates = [](const std::wstring& id) { return exclusive_rates(id); };
  } else {  // --synthetic: the test source is the only input
    choices.devices = {DeviceInfo{ctx_.settings.input.device_id, ctx_.source_name, ctx_.source.format()}};
    choices.default_device_id = ctx_.settings.input.device_id;
    const uint32_t rate = ctx_.source.format().sample_rate;
    choices.exclusive_rates = [rate](const std::wstring&) { return std::vector<uint32_t>{rate}; };
  }
  const std::wstring ini = settings_path();
  AppSettings edited = ctx_.settings;
  ++modal_;
  const bool saved = run_settings_dialog(hwnd_, inst_, choices, ini, edited);
  --modal_;
  if (!saved) return;

  // Saved: take the settings as stored, and reopen the input if it changed.
  const CaptureConfig old_input = ctx_.settings.input;
  ctx_.settings = load_settings(ini);
  const CaptureConfig& in = ctx_.settings.input;
  const bool input_changed = in.device_id != old_input.device_id || in.mode != old_input.mode ||
                             (in.mode == CaptureMode::Exclusive && in.sample_rate != old_input.sample_rate);
  if (recording()) {
    // A session started while the dialog was open: never reopen its input under it. The
    // saved settings apply from the next start.
    reopen_input_ = reopen_input_ || (input_changed && ctx_.wasapi);
    update_hotkey();
    box(L"Stop recording to change settings.", kTitle, MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (input_changed && ctx_.wasapi) {
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    std::wstring err;
    if (!ctx_.source.open(in, &err))
      ctx_.log.add(Severity::Error, system_clock().now(), L"", L"Can't open the input device: " + err);
    SetCursor(old);
    update_device_name();
  }
  update_hotkey();
  show_config_ = true;
  refresh_slow();
}

void MainWindow::update_hotkey() {
  const bool want = ctx_.settings.global_hotkey;
  if (want == hotkey_) return;
  if (!want) {
    UnregisterHotKey(hwnd_, kHotkeyId);
    hotkey_ = false;
    return;
  }
  hotkey_ = RegisterHotKey(hwnd_, kHotkeyId, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'M') != 0;
  if (!hotkey_) warn(L"Couldn't register Ctrl+Alt+M. Another program may be using it.");
}

void MainWindow::marker_menu(const RECT& r) {
  HMENU m = CreatePopupMenu();
  if (!m) return;
  AppendMenuW(m, MF_STRING, IDM_MARKER_NOW, L"Now");
  AppendMenuW(m, MF_STRING, IDM_MARKER_5, L"5 s ago");
  AppendMenuW(m, MF_STRING, IDM_MARKER_10, L"10 s ago");
  AppendMenuW(m, MF_STRING, IDM_MARKER_30, L"30 s ago");
  const int cmd = int(TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN | TPM_NONOTIFY, r.left,
                                     r.bottom, 0, hwnd_, nullptr));
  DestroyMenu(m);
  if (cmd) on_command(cmd);
}

void MainWindow::add_folder(size_t dest, std::wstring path) {
  if (recording()) ctx_.mgr.append_folder(dest, std::move(path));
}

void MainWindow::warn(std::wstring text) {
  ctx_.log.add(Severity::Warning, system_clock().now(), L"", std::move(text));
}

// ---- refresh ----

void MainWindow::tick() {
  // meter() may change after a linked session: fetch it on every tick, never keep it.
  std::vector<ChannelMeter> levels;
  if (MeterReader* m = ctx_.mgr.meter()) {
    m->update();
    levels = m->get();
  } else {
    levels.assign(ctx_.source.format().channels, ChannelMeter{-60.0f, -60.0f, -60.0f, 0});
  }
  if (levels.size() != meter_rows_) {
    meter_rows_ = levels.size();
    layout();
  }
  meters_.set_levels(levels);

  const uint64_t now = GetTickCount64();
  // Not inside a modal loop (a message box or Settings): the resume would start under it.
  if (pending_ && !modal_ && now - pending_try_ >= kResumeRetryMs) try_resume();
  if (now - last_slow_ms_ >= kSlowMs) refresh_slow();
}

void MainWindow::refresh_slow() {
  last_slow_ms_ = GetTickCount64();
  const SessionStats st = ctx_.mgr.stats();
  const bool rec = st.state != SessionState::Idle;
  if ((st.state == SessionState::InputLost) != input_lost_) {  // §5.3: red until the input returns
    input_lost_ = !input_lost_;
    update_banner();
  }
  if (rec != awake_) {  // §5.2: no system sleep while recording (display sleep is fine)
    awake_ = rec;
    SetThreadExecutionState(rec ? ES_CONTINUOUS | ES_SYSTEM_REQUIRED : ES_CONTINUOUS);
    if (!rec) use_log(kAppLog);  // also when the manager went idle by itself (failed linked session)
  }
  // stats().fmt stays the last session's format after it stops; idle, the source's is current.
  const StreamFormat fmt = rec && st.fmt.sample_rate ? st.fmt : ctx_.source.format();

  set_text(clock_, clock_text_, format_elapsed(st.elapsed_s));
  // The mode the input is open in (a test source has no Windows processing).
  const CaptureMode mode = !ctx_.wasapi              ? ctx_.settings.input.mode
                           : ctx_.wasapi->exclusive() ? CaptureMode::Exclusive
                                                      : CaptureMode::Raw;
  const bool raw = !ctx_.wasapi || ctx_.wasapi->raw_active();
  set_text(device_, device_text_,
           fmt.sample_rate ? format_input_line(device_name_, fmt, mode, raw) : L"No input device");
  set_text(buffer_, buffer_text_, format_buffer_line(st.ring_fill, st.gaps, st.discontinuities));
  // Enable the partner first and hand it the focus, so disabling never strands the keyboard.
  HWND on = rec ? stop_ : record_, off = rec ? record_ : stop_;
  enable(on, true);
  if (GetFocus() == off) SetFocus(on);
  enable(off, false);
  enable(marker_, rec);
  enable(split_, rec);

  std::vector<DestStatus> ds = ctx_.mgr.dest_status();
  const bool configured = ds.empty() || (!rec && show_config_);  // no session yet, or settings changed since
  if (configured) {
    ds.clear();
    for (const DestinationConfig& c : ctx_.settings.dests) {
      DestStatus d{};
      d.name = c.name;
      d.config = c;
      d.state = DestState::Idle;
      for (const std::wstring& f : c.folders) d.folders.push_back(FolderStatus{f, FolderHealth::Standby, {}, 0.0});
      ds.push_back(std::move(d));
    }
  }
  // A finished session's blocks keep that session's format; the configured ones use the source's.
  dests_.set_status(ds, configured || !st.fmt.sample_rate ? fmt : st.fmt, rec);
  const bool ack = rec && std::any_of(ds.begin(), ds.end(), [](const DestStatus& d) { return d.needs_ack; });
  if (ack != ((GetWindowLongW(ack_, GWL_STYLE) & WS_VISIBLE) != 0)) ShowWindow(ack_, ack ? SW_SHOWNA : SW_HIDE);
  if (dests_.preferred_height() != dests_height_) layout();

  refresh_events();
}

void MainWindow::refresh_events() {
  std::vector<EventRow> rows;
  for (const LogEntry& e : ctx_.log.recent(kEventRows))
    rows.push_back(EventRow{format_clock(e.wall), one_line(e.dest), one_line(e.text), e.sev});
  if (rows == rows_) return;
  rows_ = std::move(rows);
  ListView_SetItemCountEx(events_, int(rows_.size()), LVSICF_NOSCROLL);
  InvalidateRect(events_, nullptr, FALSE);
}

void MainWindow::update_device_name() {
  device_name_ = ctx_.source_name;
  if (!ctx_.wasapi) return;
  const std::wstring id = ctx_.wasapi->device_id();
  device_name_ = L"Input device";
  for (const DeviceInfo& d : list_capture_devices())
    if (d.id == id) device_name_ = d.name;
}

void MainWindow::set_text(HWND h, std::wstring& cache, const std::wstring& text) {
  if (cache == text) return;
  cache = text;
  SetWindowTextW(h, text.c_str());
}

void MainWindow::enable(HWND h, bool on) {
  if ((IsWindowEnabled(h) != 0) != on) EnableWindow(h, on);
}

// ---- layout ----

int MainWindow::px(int v) const { return MulDiv(v, hwnd_ ? int(GetDpiForWindow(hwnd_)) : 96, 96); }

void MainWindow::make_fonts() {
  for (HFONT* f : {&font_, &bold_, &clock_font_})
    if (*f) {
      DeleteObject(*f);
      *f = nullptr;
    }
  NONCLIENTMETRICSW ncm{};
  ncm.cbSize = sizeof(ncm);
  LOGFONTW lf{};
  if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, GetDpiForWindow(hwnd_)))
    lf = ncm.lfMessageFont;
  else
    GetObjectW(GetStockObject(DEFAULT_GUI_FONT), sizeof(lf), &lf);
  font_ = CreateFontIndirectW(&lf);
  lf.lfWeight = FW_SEMIBOLD;
  bold_ = CreateFontIndirectW(&lf);
  lf.lfHeight = -MulDiv(24, int(GetDpiForWindow(hwnd_)), 72);
  clock_font_ = CreateFontIndirectW(&lf);
}

void MainWindow::apply_fonts() {
  for (HWND h : {record_, stop_, device_, marker_, split_, settings_, meters_.hwnd(), buffer_, ack_,
                 dests_.hwnd(), events_, banner_, banner_ack_})
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
  for (HWND h : {levels_label_, dests_label_, events_label_})
    SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(bold_), TRUE);
  SendMessageW(clock_, WM_SETFONT, reinterpret_cast<WPARAM>(clock_font_), TRUE);
}

void MainWindow::layout() {
  if (!hwnd_ || !events_) return;
  RECT c;
  GetClientRect(hwnd_, &c);
  const int m = px(12), gap = px(8), bh = px(30), line = text_height(hwnd_, font_) + px(4);
  const int width = c.right - 2 * m;

  // Top bar: Record, Stop, clock, input line ... Marker, Split file, Settings.
  const int bar = std::max(bh, text_height(hwnd_, clock_font_) + px(4));
  const int by = m + (bar - bh) / 2;
  int x = m;
  place(record_, x, by, px(84), bh);
  x += px(84) + gap;
  place(stop_, x, by, px(84), bh);
  x += px(84) + px(16);
  const int clock_w = text_width(hwnd_, clock_font_, L"000:00:00") + px(4);
  place(clock_, x, m, clock_w, bar);
  x += clock_w + px(12);
  int r = c.right - m;
  r -= px(96);
  place(settings_, r, by, px(96), bh);
  r -= gap + px(88);
  place(split_, r, by, px(88), bh);
  r -= gap + px(104);
  place(marker_, r, by, px(104), bh);
  place(device_, x, m + (bar - line) / 2, r - px(12) - x, line);
  int y = m + bar + px(12);

  // The crash-recovery banner (until acknowledged) or the input-lost banner.
  if (shown_ != Banner::None) {
    const int bnh = bh + px(12);
    const RECT old = banner_rect_;
    banner_rect_ = RECT{m, y, c.right - m, y + bnh};
    if (!EqualRect(&old, &banner_rect_)) {
      InvalidateRect(hwnd_, &old, TRUE);
      InvalidateRect(hwnd_, &banner_rect_, TRUE);
    }
    place(banner_ack_, c.right - m - px(6) - px(116), y + px(6), px(116), bh);
    place(banner_, m + px(12), y + (bnh - line) / 2, width - px(12) - px(116) - px(18), line);
    y += bnh + px(12);
  }

  // Levels.
  place(levels_label_, m, y, width, line);
  y += line + px(4);
  const int mh = meters_.preferred_height(meter_rows_);
  place(meters_.hwnd(), m, y, width, mh);
  y += mh + px(4);
  place(buffer_, m, y, width, line);
  y += line + px(12);

  // Destinations, with Acknowledge at the right of the heading.
  const int head = std::max(line, px(26));
  place(dests_label_, m, y, width - px(120), head);
  place(ack_, c.right - m - px(116), y, px(116), head);
  y += head + px(4);
  dests_height_ = dests_.preferred_height();
  const int room = c.bottom - m - y - px(12) - line - px(4) - px(96);  // keep the event list usable
  const int dh = std::clamp(dests_height_, 0, std::max(0, room));
  place(dests_.hwnd(), m, y, width, dh);
  y += dh + px(12);

  // Events fill the rest.
  place(events_label_, m, y, width, line);
  y += line + px(4);
  place(events_, m, y, width, c.bottom - m - y);
  const int time_w = px(80), dest_w = px(110);
  ListView_SetColumnWidth(events_, 0, time_w);
  ListView_SetColumnWidth(events_, 1, dest_w);
  const int event_w = width - time_w - dest_w - GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(hwnd_)) - px(6);
  ListView_SetColumnWidth(events_, 2, std::max(px(120), event_w));
}

}  // namespace sl
