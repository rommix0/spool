#pragma once
// The dashboard (spec 10, layout A): top bar (Record, Stop, elapsed clock, input line,
// Marker, Split file, Settings), levels, destinations and the event list. A 33 ms timer
// updates the meters; stats, destinations and events refresh at 4 Hz. Nothing a single
// stray click or key does stops a recording: Stop has no shortcut and always asks.
// At launch it resumes a crashed session (amber banner until acknowledged) or, with
// start_on_launch, starts recording. While the input is lost a red banner shows over any
// other. While recording the system is kept from sleeping.
// Event log files: app.log outside sessions, <session_id>.log while one runs.
#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "app/dest_panel.h"
#include "app/meter_control.h"
#include "app/settings.h"
#include "io/file_system.h"
#include "session/event_log.h"
#include "session/recovery.h"
#include "session/session_manager.h"

namespace spool {

class WasapiSource;

struct AppContext {
  AppSettings& settings;
  SessionManager& mgr;
  EventLog& log;
  ICaptureSource& source;
  WasapiSource* wasapi;      // the same object as source, or null with --synthetic
  std::wstring source_name;  // shown in the input line for a non-WASAPI source
  IFileSystem& fs;
  StateStore& store;         // the crash state; the manager uses the same store
};

class MainWindow {
 public:
  static constexpr wchar_t kClassName[] = L"SpoolMainWindow";  // a second instance finds the window by it

  explicit MainWindow(AppContext);
  ~MainWindow();
  MainWindow(const MainWindow&) = delete;
  MainWindow& operator=(const MainWindow&) = delete;

  bool create(HINSTANCE, int show);
  HWND hwnd() const { return hwnd_; }
  // Accelerators and keyboard navigation; true if the message was handled.
  bool translate(MSG&);

 private:
  struct EventRow {
    std::wstring time, dest, text;
    Severity sev;
    bool operator==(const EventRow&) const = default;
  };

  static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
  LRESULT handle(UINT, WPARAM, LPARAM);
  void on_create();
  void on_command(int id);
  LRESULT on_notify(NMHDR*);
  void on_close();
  void on_launch();   // posted by create(): crash recovery, else start_on_launch
  void try_resume();  // the pending crashed session: resume, keep waiting for the input, or give up

  // Record (ask = true: preflight problems ask "Start anyway?") and auto-start (ask =
  // false: they are logged and recording starts). Blocking problems show a message.
  bool start_recording(bool ask);
  bool input_ready(std::wstring* err);  // reopens the input if it failed to open earlier
  void before_start();                  // raises the working set for the ring
  void after_start();                   // warnings about the started session; refresh
  void use_log(const std::wstring& name);  // log_file(name)
  int box(const std::wstring& text, const wchar_t* title, UINT flags);  // MessageBox, counted in modal_
  enum class Banner { None, Waiting, Recovered, NotResumed, NotResumedNewRecording, InputLost };
  void show_banner(Banner);  // the notice (not InputLost); shown unless the input is lost
  void update_banner();      // shows InputLost while input_lost_, else the notice

  void record();
  void confirm_stop();
  void marker(uint32_t seconds_ago);
  void split();
  void settings();
  void marker_menu(const RECT& button_screen);
  void add_folder(size_t dest, std::wstring path);
  void update_hotkey();  // registers or releases Ctrl+Alt+M to match the settings

  void tick();          // 33 ms
  void refresh_slow();  // 4 Hz
  void refresh_events();
  void update_device_name();
  bool recording() const;
  SessionConfig session_config() const;
  void warn(std::wstring text);

  int px(int v) const;
  void make_fonts();
  void apply_fonts();
  void layout();
  void set_text(HWND, std::wstring& cache, const std::wstring& text);
  void enable(HWND, bool on);

  AppContext ctx_;
  HINSTANCE inst_ = nullptr;
  HWND hwnd_ = nullptr;
  HACCEL accel_ = nullptr;
  bool hotkey_ = false;
  bool awake_ = false;   // SetThreadExecutionState keeps the system awake (while recording)
  Banner banner_mode_ = Banner::None;  // the notice; InputLost covers it while input_lost_
  Banner shown_ = Banner::None;        // what the banner row shows
  bool input_lost_ = false;            // stats().state == InputLost at the last refresh
  int modal_ = 0;              // message boxes and Settings open (their loops still run tick())
  bool reopen_input_ = false;  // an input change was saved during a session: reopen at the next start
  // A crashed session found at launch that waits for its input device (R18).
  std::optional<RecordingState> pending_;
  uint64_t pending_since_ = 0, pending_try_ = 0;  // GetTickCount64
  std::wstring pending_err_;                      // why the input didn't open at the last try
  RECT banner_rect_{};   // the banner row (painted by WM_PAINT)
  HBRUSH amber_ = nullptr, red_ = nullptr;  // owned
  bool show_config_ = false;  // idle panel shows the configured destinations (settings changed)
  HWND focus_ = nullptr;      // the focused child when the window was last deactivated

  HWND record_ = nullptr, stop_ = nullptr, clock_ = nullptr, device_ = nullptr;
  HWND marker_ = nullptr, split_ = nullptr, settings_ = nullptr;
  HWND levels_label_ = nullptr, buffer_ = nullptr, dests_label_ = nullptr, ack_ = nullptr;
  HWND events_label_ = nullptr, events_ = nullptr;
  HWND banner_ = nullptr, banner_ack_ = nullptr;
  MeterControl meters_;
  DestPanel dests_;

  HFONT font_ = nullptr, bold_ = nullptr, clock_font_ = nullptr;  // owned
  std::wstring clock_text_, device_text_, buffer_text_;
  std::wstring device_name_;
  std::vector<EventRow> rows_;
  uint64_t last_slow_ms_ = 0;
  int dests_height_ = -1;
  size_t meter_rows_ = 0;
};

}  // namespace spool
