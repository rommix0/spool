#pragma once
// The session manager (§4.1): wires a capture source into the capture ring, N
// destinations and a meter reader. Its manager thread drains the UI command queue every
// 10 ms, runs the stall watchdog every 250 ms and posts a clock pair every 5 min (§8.6).
// It handles input-device loss with a silence fill, its cap and the jump on return, and a
// format change with a linked new session (§5.3). With a StateStore it keeps the crash
// state up to date and resumes a crashed session (§8.5).
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "audio/capture_source.h"
#include "core/clock.h"
#include "core/spsc_queue.h"
#include "io/file_system.h"
#include "session/config.h"
#include "session/destination.h"
#include "session/event_log.h"
#include "session/meter.h"
#include "session/preflight.h"
#include "session/recovery.h"

namespace sl {

struct SessionConfig {
  SessionParams params;  // start() sets session_id, start_wall, rate and input_channels (from the source)
  std::vector<DestinationConfig> dests;
  uint32_t ring_seconds = 30;
};

enum class SessionState { Idle, Recording, InputLost };

struct SessionStats {
  SessionState state;
  std::wstring session_id;
  StreamFormat fmt;
  SampleIndex position;  // the ring's write index
  double elapsed_s;      // (position - start_sample) / rate: keeps counting through input gaps
  uint64_t gaps;         // input gaps (device loss) in this session; per-destination gaps are in DestStatus
  uint64_t discontinuities;
  double ring_fill;      // lag of the slowest writing destination, as a fraction of the ring
  bool recovered_from_crash;
};

class SessionManager : CaptureCallbacks {
 public:
  // The source must be open; its format() is the session's format. All references must
  // outlive the manager.
  SessionManager(ICaptureSource&, IFileSystem&, IClock&, EventLog&);
  ~SessionManager();  // stop()
  SessionManager(const SessionManager&) = delete;
  SessionManager& operator=(const SessionManager&) = delete;

  // The UI thread calls everything below.
  PreflightResult preflight(const SessionConfig&);
  // Crash state (§8.5): saved by the manager thread every flush_ms of steady time and
  // whenever a destination's file or manifest changes; cleared by stop(). Set before start().
  void set_state_store(StateStore*);
  // A new session with id yyyymmdd-hhmmss (local start time). Does not run preflight:
  // destinations without a usable folder start in LastResort.
  // With a resume (after repair_open_files): the crashed session continues with the same
  // id, name, start_sample and start_wall; the ring starts at resume_sample; each
  // destination (matched by name) continues its {n} and its manifest, which gets
  // gap_end(crash); stats().recovered_from_crash is true. Fails if the input format is not
  // the crashed session's.
  bool start(const SessionConfig&, const ResumeInfo* resume, std::wstring* err);
  // Stops capture, then every destination in parallel (one shared ~3 s budget even with
  // several hung drives).
  void stop();
  // Test hook (crash simulation): stops every thread without finalizing, flushing or
  // clearing anything; encoders and manifests are leaked unclosed.
  void abandon_for_test();
  // At max(first sample, write_index - s*rate), named "Marker N". The first sample is
  // start_sample, or resume_sample after a resume (never inside the crash gap).
  void marker(uint32_t seconds_ago);
  void split();                       // at write_index
  void append_folder(size_t dest, std::wstring path);  // also kept for a linked session
  void acknowledge();
  // Never wait on file I/O (callable at UI rate).
  SessionStats stats() const;
  std::vector<DestStatus> dest_status() const;
  // The current session's meter, or null when idle. A format change replaces it: fetch it
  // again on every UI tick and never keep the pointer. A replaced meter stays valid until
  // the next meter() or stop() call (both on the UI thread).
  MeterReader* meter();

 private:
  struct Session;
  struct UiCommand {
    enum class Kind { Marker, Split, AppendFolder, Acknowledge } kind = Kind::Marker;
    uint64_t session = 0;  // Session::seq it was stamped in
    SampleIndex at = 0;
    size_t dest = 0;
    std::wstring text;  // marker name or folder path
  };

  // CaptureCallbacks. on_frames runs on the capture thread; the device events only record
  // what happened (steady and wall time) for the manager thread.
  void on_frames(const float* interleaved, size_t frames, bool discontinuity) override;
  void on_device_lost() override;
  void on_device_returned(StreamFormat fmt) override;

  std::shared_ptr<Session> current() const;
  std::shared_ptr<Session> make_session(const SessionParams&, StreamFormat, SampleIndex ring_start,
                                        std::vector<DestResumeSeed> seeds, std::wstring* err);
  std::wstring next_session_id(WallTime);
  void push(UiCommand);
  void log(Severity, std::wstring text);

  // Manager thread.
  void run();
  void drain_ui(Session&);
  void handle_device_events();
  void on_lost(Session&, uint64_t lost_steady_ms);
  void on_returned(StreamFormat fmt, uint64_t steady_ms, WallTime wall);
  void link_new_session(StreamFormat fmt, uint64_t steady_ms, WallTime wall);
  void restart_capture(Session&, uint64_t steady_ms);
  void watchdog(Session&);
  void save_state(Session&, uint64_t steady_ms);  // if due or changed; warns once per session on failure
  void clear_state();                             // logs an error on failure
  void post_all(Session&, const DestCommand&);
  // Silence filler (§5.3): the ring's only producer while the input is lost.
  void start_filler(Session&);
  void stop_filler();
  void fill_to(Session&, uint64_t steady_ms);  // filler thread, or the manager with the filler stopped

  ICaptureSource& src_;
  IFileSystem& fs_;
  IClock& clock_;
  EventLog& log_;

  mutable std::mutex sess_m_;            // guards cur_ and retired_ only; never held across I/O
  std::shared_ptr<Session> cur_;
  std::vector<std::shared_ptr<Session>> retired_;  // replaced by a linked session; freed on the UI thread
  SessionConfig cfg_;                    // manager thread while running (append_folder updates it)
  std::set<std::wstring> used_ids_;
  uint64_t next_seq_ = 1;
  uint32_t marker_n_ = 0;                // UI thread

  std::atomic<SampleRing*> cap_ring_{nullptr};  // the ring on_frames writes into
  std::atomic<SessionState> state_{SessionState::Idle};
  std::atomic<uint64_t> discontinuities_{0}, input_gaps_{0}, ui_dropped_{0};

  // Device events, recorded by the callbacks (any thread), handled by the manager thread.
  std::atomic<uint64_t> lost_seq_{0}, lost_steady_{0};
  std::atomic<uint64_t> ret_seq_{0}, ret_steady_{0}, ret_fmt_{0};
  std::atomic<int64_t> ret_utc_{0};
  std::atomic<int32_t> ret_offset_{0};
  uint64_t handled_lost_ = 0, handled_ret_ = 0;

  // The open input gap (manager thread; the filler thread while it runs).
  SampleIndex gap_at_ = 0, cap_frames_ = 0;
  uint64_t gap_steady_ = 0;
  bool capped_ = false;
  std::vector<float> zeros_;
  std::atomic<bool> filler_quit_{false};
  std::thread filler_;

  // Crash state (manager thread while running).
  StateStore* store_ = nullptr;
  uint64_t saved_seq_ = 0, saved_steady_ = 0;
  std::vector<std::pair<uint32_t, std::wstring>> saved_dests_;  // next n and manifest at the last save
  bool save_warned_ = false;
  std::atomic<bool> recovered_{false};

  SpscQueue<UiCommand, 256> ui_q_;
  std::atomic<bool> quit_{false};
  uint64_t start_steady_ = 0;  // watchdog and clock-pair timers start here
  std::thread thread_;
};

}  // namespace sl
