// Sound logger: the dashboard app. `--synthetic` records a 48 kHz stereo sine from
// SyntheticSource instead of a WASAPI device, so the UI runs without audio hardware.
#include <windows.h>

#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>

#include <memory>
#include <string>

#include "app/main_window.h"
#include "app/settings.h"
#include "audio/synthetic_source.h"
#include "audio/wasapi_source.h"
#include "core/clock.h"
#include "io/file_system.h"
#include "session/event_log.h"
#include "session/session_manager.h"

namespace {

bool has_flag(const wchar_t* flag) {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  bool found = false;
  for (int i = 1; argv && i < argc; ++i)
    if (lstrcmpiW(argv[i], flag) == 0) found = true;
  LocalFree(argv);
  return found;
}

std::wstring env(const wchar_t* name) {
  const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
  if (n == 0) return {};
  std::wstring v(n, L'\0');
  v.resize(GetEnvironmentVariableW(name, v.data(), n));
  return v;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  using namespace sl;
  const bool synthetic = has_flag(L"--synthetic");
  // STA for the folder picker; WasapiSource does its COM work on its own MTA threads.
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES};
  InitCommonControlsEx(&icc);

  // The one file read on the UI thread: settings at startup (missing file -> defaults).
  AppSettings settings = load_settings(settings_path());

  IFileSystem& fs = real_fs();
  std::wstring log_dir = env(L"LOCALAPPDATA");
  if (!log_dir.empty()) log_dir += L"\\SoundLogger\\logs";
  std::wstring err;
  const bool log_dir_ok = !log_dir.empty() && fs.ensure_dir(log_dir, &err);
  EventLog log(fs, log_dir_ok ? log_dir + L"\\app.log" : std::wstring());

  std::unique_ptr<ICaptureSource> source;
  WasapiSource* wasapi = nullptr;
  if (synthetic) {
    source = std::make_unique<SyntheticSource>(StreamFormat{48000, 2}, SyntheticSource::Pattern::Sine, false);
  } else {
    auto w = std::make_unique<WasapiSource>();
    if (!w->open(settings.input, &err))
      log.add(Severity::Error, system_clock().now(), L"", L"Can't open the input device: " + err);
    wasapi = w.get();
    source = std::move(w);
  }

  int code = 0;
  {
    SessionManager mgr(*source, fs, system_clock(), log);
    MainWindow win(AppContext{settings, mgr, log, *source, wasapi, synthetic ? L"Synthetic sine" : L""});
    if (!win.create(inst, show)) {
      code = 1;
    } else {
      MSG msg{};
      BOOL r;
      while ((r = GetMessageW(&msg, nullptr, 0, 0)) != 0 && r != -1) {
        if (win.translate(msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      if (msg.message == WM_QUIT) code = int(msg.wParam);
    }
    mgr.stop();  // already stopped unless Windows is ending the session
  }
  if (SUCCEEDED(com)) CoUninitialize();
  return code;
}
