// Sound logger: the dashboard app.
// Command line:
//   --synthetic        record a 48 kHz stereo sine from SyntheticSource instead of a WASAPI
//                      device, so the UI runs without audio hardware.
//   --recover          passed by Windows when it restarts the app after a crash or hang
//                      (RegisterApplicationRestart). Recovery runs on every launch anyway;
//                      the flag only makes the start wait for the crashed process to exit.
//   --data-root <dir>  for automated tests only: APPDATA and LOCALAPPDATA become
//                      <dir>\Roaming and <dir>\Local for this process, so settings, the
//                      crash state and the logs stay out of the user's real folders.
//   --crash-after <s>  Debug builds only: writes through a null pointer after <s> seconds,
//                      to test the restart after a crash.
//   --synthetic-unavailable-for <s>
//                      Debug builds only, with --synthetic: the synthetic input can't be
//                      opened for the first <s> seconds, to test a crashed session waiting
//                      for its input device at launch.
// --synthetic, --data-root and --synthetic-unavailable-for are passed on to the restarted
// process.
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
#include "session/recovery.h"
#include "session/session_manager.h"

namespace {

constexpr wchar_t kMutexName[] = L"Local\\SoundLogger.SingleInstance";

constexpr DWORD kRecoverWaitMs = 60000;  // for the crashed instance to let go of the mutex

struct Args {
  bool synthetic = false;
  bool recover = false;
  std::wstring data_root;      // without trailing backslashes
  unsigned crash_after_s = 0;  // Debug only
  unsigned unavailable_s = 0;  // Debug only
};

Args parse_args() {
  Args a;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  for (int i = 1; argv && i < argc; ++i) {
    const bool has_value = i + 1 < argc;
    if (lstrcmpiW(argv[i], L"--synthetic") == 0) {
      a.synthetic = true;
    } else if (lstrcmpiW(argv[i], L"--recover") == 0) {
      a.recover = true;
    } else if (lstrcmpiW(argv[i], L"--data-root") == 0 && has_value) {
      a.data_root = argv[++i];
      // "C:\x\" quoted into the restart command line would escape the closing quote.
      while (a.data_root.size() > 1 && (a.data_root.back() == L'\\' || a.data_root.back() == L'/'))
        a.data_root.pop_back();
#ifdef _DEBUG
    } else if (lstrcmpiW(argv[i], L"--crash-after") == 0 && has_value) {
      a.crash_after_s = unsigned(_wtoi(argv[++i]));
    } else if (lstrcmpiW(argv[i], L"--synthetic-unavailable-for") == 0 && has_value) {
      a.unavailable_s = unsigned(_wtoi(argv[++i]));
#endif
    }
  }
  LocalFree(argv);
  return a;
}

// A second launch: show the running instance's window and leave.
void activate_running_instance() {
  HWND h = FindWindowW(sl::MainWindow::kClassName, nullptr);
  if (!h) return;
  if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
  SetForegroundWindow(h);
}

#ifdef _DEBUG
// --synthetic-unavailable-for: a source that can't be opened (and has no format) until the
// deadline, like an interface that is not back yet.
class LateSource : public sl::ICaptureSource {
 public:
  LateSource(std::unique_ptr<sl::ICaptureSource> inner, unsigned seconds)
      : inner_(std::move(inner)), until_(GetTickCount64() + uint64_t(seconds) * 1000) {}
  bool open(const sl::CaptureConfig& c, std::wstring* err) override {
    if (GetTickCount64() < until_) {
      if (err) *err = L"The synthetic input is unavailable (test).";
      return false;
    }
    open_ = true;
    return inner_->open(c, err);
  }
  sl::StreamFormat format() const override { return open_ ? inner_->format() : sl::StreamFormat{}; }
  bool start(sl::CaptureCallbacks* cb) override { return open_ && inner_->start(cb); }
  void stop() override { inner_->stop(); }

 private:
  std::unique_ptr<sl::ICaptureSource> inner_;
  uint64_t until_;
  bool open_ = false;
};

[[noreturn]] void crash_now() {
  volatile int* p = nullptr;
  *p = 1;  // an access violation, not a CRT error: Windows Error Reporting handles it
  for (;;) {
  }
}
#endif

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
  using namespace sl;
  const Args args = parse_args();
  IFileSystem& fs = real_fs();
  std::wstring err;
  if (!args.data_root.empty()) {
    for (const auto& [var, sub] : {std::pair{L"APPDATA", L"\\Roaming"}, std::pair{L"LOCALAPPDATA", L"\\Local"}}) {
      const std::wstring dir = args.data_root + sub;
      fs.ensure_dir(dir, &err);
      SetEnvironmentVariableW(var, dir.c_str());
    }
  }

  // One instance only: two loggers would fight over the device, the crash state and the
  // files. The running instance owns the mutex. Windows may restart a crashed instance while
  // the old process still exists (WER is writing its report), so --recover waits for it.
  const HANDLE instance = CreateMutexW(nullptr, FALSE, kMutexName);
  if (instance) {
    const DWORD w = WaitForSingleObject(instance, args.recover ? kRecoverWaitMs : 0);
    if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) {
      activate_running_instance();
      CloseHandle(instance);
      return 0;
    }
  }

  // After a crash or hang Windows restarts the app (if it ran for at least 60 s).
  std::wstring restart = L"--recover";
  if (args.synthetic) restart += L" --synthetic";
  if (!args.data_root.empty()) restart += L" --data-root \"" + args.data_root + L"\"";
#ifdef _DEBUG
  if (args.unavailable_s) restart += L" --synthetic-unavailable-for " + std::to_wstring(args.unavailable_s);
#endif
  const HRESULT restart_hr = RegisterApplicationRestart(restart.c_str(), 0);

  // STA for the folder picker; WasapiSource does its COM work on its own MTA threads.
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES};
  InitCommonControlsEx(&icc);

  // The file reads on the UI thread: settings and the crash state at startup (missing
  // settings file -> defaults), and the repair scan if the state says a session crashed.
  AppSettings settings = load_settings(settings_path());

  // Outside a session the log is app.log; each session switches it to <session_id>.log.
  const std::wstring log_dir = logs_dir();
  if (!log_dir.empty()) fs.ensure_dir(log_dir, &err);
  EventLog log(fs, log_file(L"app"));
  // StateStore does not create its folder.
  if (!ensure_settings_dir())
    log.add(Severity::Warning, system_clock().now(), L"",
            L"Can't create the settings folder: a crashed session can't be resumed");
  StateStore store(state_path());
  if (FAILED(restart_hr))
    log.add(Severity::Warning, system_clock().now(), L"",
            L"Couldn't register for a restart after a crash: Windows won't restart the app by itself");

  std::unique_ptr<ICaptureSource> source;
  WasapiSource* wasapi = nullptr;
  if (args.synthetic) {
    source = std::make_unique<SyntheticSource>(StreamFormat{48000, 2}, SyntheticSource::Pattern::Sine, false);
#ifdef _DEBUG
    if (args.unavailable_s) {
      source = std::make_unique<LateSource>(std::move(source), args.unavailable_s);
      if (!source->open(settings.input, &err))
        log.add(Severity::Error, system_clock().now(), L"", L"Can't open the input device: " + err);
    }
#endif
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
    mgr.set_state_store(&store);
    // May run on the manager thread (a linked session); log outlives the manager.
    mgr.on_new_session([&log](const std::wstring& id) { log.set_file(log_file(id)); });
    MainWindow win(AppContext{settings, mgr, log, *source, wasapi, args.synthetic ? L"Synthetic sine" : L"", fs, store});
    if (!win.create(inst, show)) {
      code = 1;
    } else {
#ifdef _DEBUG
      UINT_PTR crash_timer = 0;
      if (args.crash_after_s) {
        crash_timer = SetTimer(nullptr, 0, args.crash_after_s * 1000, nullptr);
        log.add(Severity::Info, system_clock().now(), L"",
                L"Crash test: the app crashes in " + std::to_wstring(args.crash_after_s) +
                    L" s. Windows restarts it only if it ran for at least 60 s.");
      }
#endif
      MSG msg{};
      BOOL r;
      while ((r = GetMessageW(&msg, nullptr, 0, 0)) != 0 && r != -1) {
#ifdef _DEBUG
        // Here rather than in a TIMERPROC: Windows swallows exceptions raised in those.
        if (crash_timer && msg.message == WM_TIMER && !msg.hwnd && msg.wParam == crash_timer) crash_now();
#endif
        if (win.translate(msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      if (msg.message == WM_QUIT) code = int(msg.wParam);
    }
    mgr.stop();  // already stopped unless Windows is ending the session
  }
  if (SUCCEEDED(com)) CoUninitialize();
  if (instance) {
    ReleaseMutex(instance);
    CloseHandle(instance);
  }
  return code;
}
