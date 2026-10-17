#pragma once
// The dashboard (spec 10, layout A): top bar (Record, Stop, elapsed clock, input line,
// Marker, Split file, Settings), levels, destinations and the event list. A 33 ms timer
// updates the meters; stats, destinations and events refresh at 4 Hz. Nothing a single
// stray click or key does stops a recording: Stop has no shortcut and always asks.
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "app/dest_panel.h"
#include "app/meter_control.h"
#include "app/settings.h"
#include "session/event_log.h"
#include "session/session_manager.h"

namespace sl {

class WasapiSource;

struct AppContext {
  AppSettings& settings;
  SessionManager& mgr;
  EventLog& log;
  ICaptureSource& source;
  WasapiSource* wasapi;      // the same object as source, or null with --synthetic
  std::wstring source_name;  // shown in the input line for a non-WASAPI source
};

class MainWindow {
 public:
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

  void record();
  void confirm_stop();
  void marker(uint32_t seconds_ago);
  void split();
  void settings();
  void marker_menu(const RECT& button_screen);
  void add_folder(size_t dest, std::wstring path);

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

  HWND record_ = nullptr, stop_ = nullptr, clock_ = nullptr, device_ = nullptr;
  HWND marker_ = nullptr, split_ = nullptr, settings_ = nullptr;
  HWND levels_label_ = nullptr, buffer_ = nullptr, dests_label_ = nullptr, ack_ = nullptr;
  HWND events_label_ = nullptr, events_ = nullptr;
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

}  // namespace sl
