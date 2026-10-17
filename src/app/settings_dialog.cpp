#include "app/settings_dialog.h"

#include <commctrl.h>

#include <algorithm>
#include <map>
#include <optional>

#include "app/folder_picker.h"
#include "app/format_text.h"
#include "app/resource.h"
#include "app/settings_model.h"
#include "core/clock.h"

namespace sl {

namespace {

constexpr wchar_t kCheckTitle[] = L"Check these settings";
constexpr wchar_t kAppTitle[] = L"Sound logger";
constexpr uint16_t kUnknownDeviceChannels = 8;  // spec limit; Record checks the real device

// ---- small control helpers ----

HWND item(HWND dlg, int id) { return GetDlgItem(dlg, id); }

std::wstring text_of(HWND dlg, int id) {
  HWND h = item(dlg, id);
  std::wstring s(size_t(GetWindowTextLengthW(h)) + 1, L'\0');
  s.resize(size_t(GetWindowTextW(h, s.data(), int(s.size()))));
  return s;
}

void set_text(HWND dlg, int id, const std::wstring& s) { SetDlgItemTextW(dlg, id, s.c_str()); }

void show(HWND dlg, int id, bool on) { ShowWindow(item(dlg, id), on ? SW_SHOWNA : SW_HIDE); }

void enable(HWND dlg, int id, bool on) {
  HWND h = item(dlg, id);
  if (!on && GetFocus() == h) SendMessageW(dlg, WM_NEXTDLGCTL, 0, FALSE);  // don't strand the focus
  EnableWindow(h, on);
}

void combo_fill(HWND dlg, int id, const std::vector<std::wstring>& labels, int sel) {
  HWND h = item(dlg, id);
  SendMessageW(h, CB_RESETCONTENT, 0, 0);
  for (const auto& l : labels) SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(l.c_str()));
  SendMessageW(h, CB_SETCURSEL, WPARAM(sel), 0);
}

int combo_sel(HWND dlg, int id) { return int(SendMessageW(item(dlg, id), CB_GETCURSEL, 0, 0)); }

template <class T>
int index_of(const std::vector<T>& v, const T& x) {
  const auto it = std::find(v.begin(), v.end(), x);
  return it == v.end() ? -1 : int(it - v.begin());
}

std::wstring trim(const std::wstring& s) {
  const size_t a = s.find_first_not_of(L" \t");
  if (a == std::wstring::npos) return {};
  return s.substr(a, s.find_last_not_of(L" \t") - a + 1);
}

bool eq_nocase(const std::wstring& a, const std::wstring& b) {
  return CompareStringOrdinal(a.c_str(), int(a.size()), b.c_str(), int(b.size()), TRUE) == CSTR_EQUAL;
}

std::wstring join(const std::vector<std::wstring>& lines) {
  std::wstring out;
  for (const auto& l : lines) out += (out.empty() ? L"" : L"\n") + l;
  return out;
}

void show_problems(HWND dlg, const std::vector<std::wstring>& p) {
  MessageBoxW(dlg, join(p).c_str(), kCheckTitle, MB_OK | MB_ICONWARNING);
}

void add_column(HWND lv, int i, const wchar_t* title, int width) {
  LVCOLUMNW c{};
  c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
  c.pszText = const_cast<wchar_t*>(title);
  c.cx = width;
  c.iSubItem = i;
  ListView_InsertColumn(lv, i, &c);
}

int client_width(HWND h) {
  RECT r;
  GetClientRect(h, &r);
  return r.right;
}

int vscroll_width(HWND h) { return GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(h)); }

int list_selection(HWND lv) { return ListView_GetNextItem(lv, -1, LVNI_SELECTED); }

template <class State>
State* state_of(HWND dlg) {
  return reinterpret_cast<State*>(GetWindowLongPtrW(dlg, DWLP_USER));
}

// ---- destination editor ----

struct DestEditor {
  DestinationConfig d;  // in: the destination to edit; out (on OK): the edited one
  uint16_t device_channels = 2;
  std::wstring session;  // for the name preview
  std::vector<uint32_t> bitrates, rotations;
  std::vector<std::wstring> folders;
};

void dest_update_format_rows(HWND dlg) {
  const bool opus = combo_sel(dlg, IDC_D_FORMAT) == 1;
  show(dlg, IDC_D_DEPTH_LABEL, !opus);
  show(dlg, IDC_D_DEPTH, !opus);
  show(dlg, IDC_D_BITRATE_LABEL, opus);
  show(dlg, IDC_D_BITRATE, opus);
}

void dest_update_preview(HWND dlg) {
  const DestEditor* st = state_of<DestEditor>(dlg);
  const OutFormat f = combo_sel(dlg, IDC_D_FORMAT) == 1 ? OutFormat::Opus : OutFormat::Wav;
  set_text(dlg, IDC_D_PREVIEW,
           L"Example: " + name_preview(text_of(dlg, IDC_D_TEMPLATE), st->session, trim(text_of(dlg, IDC_D_NAME)), f,
                                       system_clock().now()));
}

void dest_update_folder_buttons(HWND dlg) {
  const DestEditor* st = state_of<DestEditor>(dlg);
  const int sel = int(SendMessageW(item(dlg, IDC_D_FOLDERS), LB_GETCURSEL, 0, 0));
  const int n = int(st->folders.size());
  enable(dlg, IDC_D_FOLDER_REMOVE, sel >= 0);
  enable(dlg, IDC_D_FOLDER_UP, sel > 0);
  enable(dlg, IDC_D_FOLDER_DOWN, sel >= 0 && sel + 1 < n);
}

void dest_fill_folders(HWND dlg, int sel) {
  const DestEditor* st = state_of<DestEditor>(dlg);
  HWND lb = item(dlg, IDC_D_FOLDERS);
  SendMessageW(lb, LB_RESETCONTENT, 0, 0);
  int widest = 0;
  HDC dc = GetDC(lb);
  HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(lb, WM_GETFONT, 0, 0)));
  for (const auto& f : st->folders) {
    SendMessageW(lb, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(f.c_str()));
    SIZE ext{};
    if (GetTextExtentPoint32W(dc, f.c_str(), int(f.size()), &ext)) widest = std::max(widest, int(ext.cx));
  }
  SelectObject(dc, old);
  ReleaseDC(lb, dc);
  SendMessageW(lb, LB_SETHORIZONTALEXTENT, WPARAM(widest + 8), 0);
  SendMessageW(lb, LB_SETCURSEL, WPARAM(sel), 0);
  dest_update_folder_buttons(dlg);
}

void dest_init(HWND dlg) {
  DestEditor* st = state_of<DestEditor>(dlg);
  const DestinationConfig& d = st->d;
  set_text(dlg, IDC_D_NAME, d.name);
  combo_fill(dlg, IDC_D_FORMAT, {L"WAV", L"Opus"}, d.format == OutFormat::Opus ? 1 : 0);
  combo_fill(dlg, IDC_D_DEPTH, {L"16-bit", L"24-bit", L"32-bit float"}, depth_index(d.wav_depth));

  const size_t chs = d.channels.empty() ? st->device_channels : d.channels.size();
  st->bitrates = bitrate_choices(d.opus_bitrate);
  std::vector<std::wstring> labels;
  for (const uint32_t b : st->bitrates) labels.push_back(bitrate_label(b));
  combo_fill(dlg, IDC_D_BITRATE, labels, std::max(0, index_of(st->bitrates, effective_bitrate(d.opus_bitrate, chs))));

  HWND lv = item(dlg, IDC_D_CHANNELS);
  ListView_SetExtendedListViewStyle(lv, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
  add_column(lv, 0, L"", client_width(lv) - vscroll_width(lv));
  const std::vector<bool> checks = channel_checks(d.channels, st->device_channels);
  for (size_t i = 0; i < checks.size(); ++i) {
    std::wstring label = L"Ch " + std::to_wstring(i + 1);
    LVITEMW it{};
    it.mask = LVIF_TEXT;
    it.iItem = int(i);
    it.pszText = label.data();
    ListView_InsertItem(lv, &it);
    ListView_SetCheckState(lv, int(i), checks[i] ? TRUE : FALSE);
  }
  if (!checks.empty()) ListView_SetItemState(lv, 0, LVIS_FOCUSED, LVIS_FOCUSED);

  st->folders = d.folders;
  dest_fill_folders(dlg, st->folders.empty() ? -1 : 0);

  st->rotations = rotation_choices(d.rotation_minutes);
  labels.clear();
  for (const uint32_t m : st->rotations) labels.push_back(rotation_label(m));
  combo_fill(dlg, IDC_D_ROTATION, labels, std::max(0, index_of(st->rotations, d.rotation_minutes)));

  set_text(dlg, IDC_D_TEMPLATE, d.name_template);
  dest_update_format_rows(dlg);
  dest_update_preview(dlg);
}

void dest_add_folder(HWND dlg) {
  DestEditor* st = state_of<DestEditor>(dlg);
  const std::wstring path = pick_folder(dlg);
  if (path.empty()) return;
  for (size_t i = 0; i < st->folders.size(); ++i)
    if (eq_nocase(st->folders[i], path)) {  // already in the chain: just select it
      dest_fill_folders(dlg, int(i));
      return;
    }
  st->folders.push_back(path);
  dest_fill_folders(dlg, int(st->folders.size()) - 1);
}

void dest_move_folder(HWND dlg, int by) {  // by = -1 (up), +1 (down), 0 (remove)
  DestEditor* st = state_of<DestEditor>(dlg);
  const int sel = int(SendMessageW(item(dlg, IDC_D_FOLDERS), LB_GETCURSEL, 0, 0));
  const int n = int(st->folders.size());
  if (sel < 0 || sel >= n) return;
  if (by == 0) {
    st->folders.erase(st->folders.begin() + sel);
    dest_fill_folders(dlg, st->folders.empty() ? -1 : std::min(sel, n - 2));
    return;
  }
  const int to = sel + by;
  if (to < 0 || to >= n) return;
  std::swap(st->folders[size_t(sel)], st->folders[size_t(to)]);
  dest_fill_folders(dlg, to);
}

void dest_ok(HWND dlg) {
  DestEditor* st = state_of<DestEditor>(dlg);
  DestinationConfig out = st->d;
  out.name = trim(text_of(dlg, IDC_D_NAME));
  out.format = combo_sel(dlg, IDC_D_FORMAT) == 1 ? OutFormat::Opus : OutFormat::Wav;
  out.wav_depth = depth_from_index(combo_sel(dlg, IDC_D_DEPTH));
  if (out.format == OutFormat::Opus) {
    const int b = combo_sel(dlg, IDC_D_BITRATE);
    if (b >= 0 && size_t(b) < st->bitrates.size()) out.opus_bitrate = st->bitrates[size_t(b)];
  }
  HWND lv = item(dlg, IDC_D_CHANNELS);
  std::vector<bool> checks(size_t(ListView_GetItemCount(lv)));
  for (size_t i = 0; i < checks.size(); ++i) checks[i] = ListView_GetCheckState(lv, int(i)) != 0;
  const auto chs = channels_from_checks(checks, st->device_channels);
  if (!chs) {
    show_problems(dlg, {L"Choose at least one channel."});
    return;
  }
  out.channels = *chs;
  out.folders = st->folders;
  const int r = combo_sel(dlg, IDC_D_ROTATION);
  if (r >= 0 && size_t(r) < st->rotations.size()) out.rotation_minutes = st->rotations[size_t(r)];
  out.name_template = text_of(dlg, IDC_D_TEMPLATE);

  const std::vector<std::wstring> problems = destination_problems(out, st->device_channels);
  if (!problems.empty()) {
    show_problems(dlg, problems);
    return;
  }
  st->d = std::move(out);
  EndDialog(dlg, IDOK);
}

INT_PTR CALLBACK dest_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_INITDIALOG:
      SetWindowLongPtrW(dlg, DWLP_USER, lp);
      dest_init(dlg);
      return TRUE;
    case WM_COMMAND: {
      const int id = LOWORD(wp), code = HIWORD(wp);
      switch (id) {
        case IDC_D_FORMAT:
          if (code == CBN_SELCHANGE) {
            dest_update_format_rows(dlg);
            dest_update_preview(dlg);
          }
          return TRUE;
        case IDC_D_NAME:
        case IDC_D_TEMPLATE:
          if (code == EN_CHANGE) dest_update_preview(dlg);
          return TRUE;
        case IDC_D_FOLDERS:
          if (code == LBN_SELCHANGE) dest_update_folder_buttons(dlg);
          return TRUE;
        case IDC_D_FOLDER_ADD: dest_add_folder(dlg); return TRUE;
        case IDC_D_FOLDER_REMOVE: dest_move_folder(dlg, 0); return TRUE;
        case IDC_D_FOLDER_UP: dest_move_folder(dlg, -1); return TRUE;
        case IDC_D_FOLDER_DOWN: dest_move_folder(dlg, 1); return TRUE;
        case IDOK: dest_ok(dlg); return TRUE;
        case IDCANCEL: EndDialog(dlg, IDCANCEL); return TRUE;
      }
      return FALSE;
    }
  }
  return FALSE;
}

// ---- settings dialog ----

struct SettingsDialog {
  const SettingsChoices* choices = nullptr;
  std::wstring ini;
  HINSTANCE inst = nullptr;
  AppSettings original;
  AppSettings s;                      // working copy; s.input follows the input controls
  std::vector<DeviceInfo> devices;    // choices plus the saved device when it isn't connected
  std::map<std::wstring, std::vector<uint32_t>> rate_cache;
  std::vector<uint32_t> rates;        // the rate combo's values
};

const DeviceInfo* selected_device(HWND dlg) {
  const SettingsDialog* st = state_of<SettingsDialog>(dlg);
  const int i = combo_sel(dlg, IDC_S_DEVICE);
  return i >= 0 && size_t(i) < st->devices.size() ? &st->devices[size_t(i)] : nullptr;
}

uint16_t device_channels(HWND dlg) {
  const DeviceInfo* d = selected_device(dlg);
  return d && d->mix_format.channels ? d->mix_format.channels : kUnknownDeviceChannels;
}

void settings_update_rate_row(HWND dlg) {
  SettingsDialog* st = state_of<SettingsDialog>(dlg);
  const DeviceInfo* dev = selected_device(dlg);
  const bool exclusive = st->s.input.mode == CaptureMode::Exclusive;
  st->rates.clear();
  if (exclusive && dev && dev->mix_format.sample_rate && st->choices->exclusive_rates) {
    auto it = st->rate_cache.find(dev->id);
    if (it == st->rate_cache.end()) {
      HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
      it = st->rate_cache.emplace(dev->id, st->choices->exclusive_rates(dev->id)).first;
      SetCursor(old);
    }
    st->rates = it->second;
  }
  if (exclusive) {
    int sel = index_of(st->rates, st->s.input.sample_rate);
    if (sel < 0 && dev) sel = index_of(st->rates, dev->mix_format.sample_rate);
    if (sel < 0 && !st->rates.empty()) sel = 0;
    std::vector<std::wstring> labels;
    for (const uint32_t r : st->rates) labels.push_back(std::to_wstring(r) + L" Hz");
    combo_fill(dlg, IDC_S_RATE, labels, sel);
    if (sel >= 0) st->s.input.sample_rate = st->rates[size_t(sel)];
  }
  show(dlg, IDC_S_RATE, exclusive);
  enable(dlg, IDC_S_RATE, exclusive && !st->rates.empty());
  show(dlg, IDC_S_RATE_NOTE, !exclusive);
  set_text(dlg, IDC_S_RATE_NOTE, raw_rate_note(dev ? dev->mix_format.sample_rate : 0));
}

void settings_update_dest_buttons(HWND dlg) {
  const bool sel = list_selection(item(dlg, IDC_S_DESTS)) >= 0;
  enable(dlg, IDC_S_DEST_EDIT, sel);
  enable(dlg, IDC_S_DEST_REMOVE, sel);
}

void settings_fill_dests(HWND dlg, int sel) {
  const SettingsDialog* st = state_of<SettingsDialog>(dlg);
  HWND lv = item(dlg, IDC_S_DESTS);
  ListView_DeleteAllItems(lv);
  const uint16_t chs = device_channels(dlg);
  for (size_t i = 0; i < st->s.dests.size(); ++i) {
    const DestinationConfig& d = st->s.dests[i];
    std::wstring folder = d.folders.empty() ? L"—" : d.folders.front();
    if (d.folders.size() > 1) folder += L" (+" + std::to_wstring(d.folders.size() - 1) + L")";
    std::wstring cols[] = {d.name, format_summary(d, chs), folder};
    LVITEMW it{};
    it.mask = LVIF_TEXT;
    it.iItem = int(i);
    it.pszText = cols[0].data();
    ListView_InsertItem(lv, &it);
    ListView_SetItemText(lv, int(i), 1, cols[1].data());
    ListView_SetItemText(lv, int(i), 2, cols[2].data());
  }
  if (sel >= 0 && size_t(sel) < st->s.dests.size()) {
    ListView_SetItemState(lv, sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    ListView_EnsureVisible(lv, sel, FALSE);
  }
  settings_update_dest_buttons(dlg);
}

// Runs the editor on d; true (and d updated) on OK.
bool settings_edit_dest(HWND dlg, DestinationConfig& d) {
  const SettingsDialog* st = state_of<SettingsDialog>(dlg);
  DestEditor ed;
  ed.d = d;
  ed.device_channels = device_channels(dlg);
  ed.session = trim(text_of(dlg, IDC_S_SESSION));
  if (DialogBoxParamW(st->inst, MAKEINTRESOURCEW(IDD_DEST), dlg, dest_proc, reinterpret_cast<LPARAM>(&ed)) != IDOK)
    return false;
  d = std::move(ed.d);
  return true;
}

void settings_add_dest(HWND dlg) {
  SettingsDialog* st = state_of<SettingsDialog>(dlg);
  DestinationConfig d;
  d.name = new_destination_name(st->s.dests);
  if (!settings_edit_dest(dlg, d)) return;
  st->s.dests.push_back(std::move(d));
  settings_fill_dests(dlg, int(st->s.dests.size()) - 1);
}

void settings_edit_selected(HWND dlg) {
  SettingsDialog* st = state_of<SettingsDialog>(dlg);
  const int sel = list_selection(item(dlg, IDC_S_DESTS));
  if (sel < 0 || size_t(sel) >= st->s.dests.size()) return;
  DestinationConfig d = st->s.dests[size_t(sel)];
  if (!settings_edit_dest(dlg, d)) return;
  st->s.dests[size_t(sel)] = std::move(d);
  settings_fill_dests(dlg, sel);
}

void settings_remove_selected(HWND dlg) {
  SettingsDialog* st = state_of<SettingsDialog>(dlg);
  const int sel = list_selection(item(dlg, IDC_S_DESTS));
  if (sel < 0 || size_t(sel) >= st->s.dests.size()) return;
  st->s.dests.erase(st->s.dests.begin() + sel);
  settings_fill_dests(dlg, std::min(sel, int(st->s.dests.size()) - 1));
}

void settings_init(HWND dlg) {
  SettingsDialog* st = state_of<SettingsDialog>(dlg);
  const AppSettings& s = st->s;

  // Input. The saved device stays selectable (and selected) while it's unplugged.
  st->devices = st->choices->devices;
  const std::wstring want = s.input.device_id.empty() ? st->choices->default_device_id : s.input.device_id;
  int dev = -1;
  for (size_t i = 0; i < st->devices.size(); ++i)
    if (st->devices[i].id == want) dev = int(i);
  if (dev < 0 && !s.input.device_id.empty()) {
    st->devices.push_back(DeviceInfo{s.input.device_id, L"Saved device (not connected)", StreamFormat{0, 0}});
    dev = int(st->devices.size()) - 1;
  }
  if (dev < 0 && !st->devices.empty()) dev = 0;
  std::vector<std::wstring> names;
  for (const auto& d : st->devices) names.push_back(d.name);
  combo_fill(dlg, IDC_S_DEVICE, names, dev);
  combo_fill(dlg, IDC_S_MODE, {L"Raw (no Windows processing)", L"Exclusive"},
             s.input.mode == CaptureMode::Exclusive ? 1 : 0);
  settings_update_rate_row(dlg);
  set_text(dlg, IDC_S_RING, std::to_wstring(s.ring_seconds));

  // Destinations.
  HWND lv = item(dlg, IDC_S_DESTS);
  ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  const int w = client_width(lv) - vscroll_width(lv);
  add_column(lv, 0, L"Name", w * 28 / 100);
  add_column(lv, 1, L"Format", w * 36 / 100);
  add_column(lv, 2, L"Folder", w - w * 28 / 100 - w * 36 / 100);
  settings_fill_dests(dlg, s.dests.empty() ? -1 : 0);

  // General.
  set_text(dlg, IDC_S_SESSION, s.session_name);
  set_text(dlg, IDC_S_FLUSH, std::to_wstring(s.flush_seconds));
  set_text(dlg, IDC_S_LOW_WATER, std::to_wstring(s.low_water_minutes));
  set_text(dlg, IDC_S_STALL, std::to_wstring(s.stall_percent));
  CheckDlgButton(dlg, IDC_S_AUTOSTART, s.start_on_launch ? BST_CHECKED : BST_UNCHECKED);
  CheckDlgButton(dlg, IDC_S_HOTKEY, s.global_hotkey ? BST_CHECKED : BST_UNCHECKED);
}

void settings_ok(HWND dlg) {
  SettingsDialog* st = state_of<SettingsDialog>(dlg);
  AppSettings out = st->s;
  std::vector<std::wstring> problems;

  const auto number = [&](int id, uint32_t lo, uint32_t hi, const wchar_t* what, const wchar_t* unit,
                          uint32_t& field) {
    if (const auto v = parse_number(text_of(dlg, id), lo, hi)) field = *v;
    else problems.push_back(range_message(what, lo, hi, unit));
  };

  // Input. A settings file that follows the default device keeps doing so while that's selected.
  const DeviceInfo* dev = selected_device(dlg);
  if (dev) {
    out.input.device_id = dev->id;
    if (st->original.input.device_id.empty() && dev->id == st->choices->default_device_id)
      out.input.device_id.clear();
  }
  // (An unplugged saved device can't be asked; its saved rate stays.)
  if (out.input.mode == CaptureMode::Exclusive && st->rates.empty() && dev && dev->mix_format.sample_rate)
    problems.push_back(L"This device doesn't support exclusive mode.");
  number(IDC_S_RING, 1, 3600, L"Ring length", L" seconds", out.ring_seconds);

  // General.
  out.session_name = trim(text_of(dlg, IDC_S_SESSION));
  if (out.session_name.empty()) problems.push_back(L"Enter a session name.");
  number(IDC_S_FLUSH, 1, 3600, L"Flush interval", L" seconds", out.flush_seconds);
  number(IDC_S_LOW_WATER, 0, 100000, L"Low-water mark", L" minutes", out.low_water_minutes);
  number(IDC_S_STALL, 1, 100, L"Stall threshold", L"%", out.stall_percent);
  out.start_on_launch = IsDlgButtonChecked(dlg, IDC_S_AUTOSTART) == BST_CHECKED;
  out.global_hotkey = IsDlgButtonChecked(dlg, IDC_S_HOTKEY) == BST_CHECKED;

  for (auto& p : validate(out, device_channels(dlg))) problems.push_back(std::move(p));
  if (!problems.empty()) {
    show_problems(dlg, problems);
    return;
  }
  if (!save_settings(st->ini, out)) {
    MessageBoxW(dlg, L"Couldn't save settings.", kAppTitle, MB_OK | MB_ICONERROR);
    return;
  }
  st->s = std::move(out);
  EndDialog(dlg, IDOK);
}

INT_PTR CALLBACK settings_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_INITDIALOG:
      SetWindowLongPtrW(dlg, DWLP_USER, lp);
      settings_init(dlg);
      return TRUE;
    case WM_COMMAND: {
      SettingsDialog* st = state_of<SettingsDialog>(dlg);
      const int id = LOWORD(wp), code = HIWORD(wp);
      switch (id) {
        case IDC_S_DEVICE:
          if (code == CBN_SELCHANGE) {
            settings_update_rate_row(dlg);
            settings_fill_dests(dlg, list_selection(item(dlg, IDC_S_DESTS)));  // summaries name channels
          }
          return TRUE;
        case IDC_S_MODE:
          if (code == CBN_SELCHANGE) {
            st->s.input.mode = combo_sel(dlg, IDC_S_MODE) == 1 ? CaptureMode::Exclusive : CaptureMode::Raw;
            settings_update_rate_row(dlg);
          }
          return TRUE;
        case IDC_S_RATE:
          if (code == CBN_SELCHANGE) {
            const int i = combo_sel(dlg, IDC_S_RATE);
            if (i >= 0 && size_t(i) < st->rates.size()) st->s.input.sample_rate = st->rates[size_t(i)];
          }
          return TRUE;
        case IDC_S_DEST_ADD: settings_add_dest(dlg); return TRUE;
        case IDC_S_DEST_EDIT: settings_edit_selected(dlg); return TRUE;
        case IDC_S_DEST_REMOVE: settings_remove_selected(dlg); return TRUE;
        case IDOK: settings_ok(dlg); return TRUE;
        case IDCANCEL: EndDialog(dlg, IDCANCEL); return TRUE;
      }
      return FALSE;
    }
    case WM_NOTIFY: {
      const auto* n = reinterpret_cast<const NMHDR*>(lp);
      if (n->idFrom != IDC_S_DESTS) return FALSE;
      if (n->code == LVN_ITEMCHANGED) settings_update_dest_buttons(dlg);
      else if (n->code == NM_DBLCLK && reinterpret_cast<const NMITEMACTIVATE*>(n)->iItem >= 0)
        settings_edit_selected(dlg);
      return FALSE;
    }
  }
  return FALSE;
}

}  // namespace

bool run_settings_dialog(HWND owner, HINSTANCE inst, const SettingsChoices& choices, const std::wstring& ini,
                         AppSettings& s) {
  SettingsDialog st;
  st.choices = &choices;
  st.ini = ini;
  st.inst = inst;
  st.original = s;
  st.s = s;
  if (DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_SETTINGS), owner, settings_proc, reinterpret_cast<LPARAM>(&st)) !=
      IDOK)
    return false;
  s = std::move(st.s);
  return true;
}

}  // namespace sl
