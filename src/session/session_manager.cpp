#include "session/session_manager.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <new>
#include <system_error>

namespace spool {

namespace {

constexpr int kTickMs = 10;                       // UI queue drain interval
constexpr uint64_t kWatchdogMs = 250;             // stall watchdog interval (steady)
constexpr uint64_t kClockPairMs = 5 * 60 * 1000;  // §8.6
constexpr int kFillPollMs = 5;                    // silence filler poll (real sleep)
constexpr uint32_t kDefaultCapMinutes = 30;       // silence cap with rotation off
constexpr size_t kFillChunk = 4096;

uint64_t pack(StreamFormat f) { return uint64_t(f.sample_rate) << 16 | f.channels; }
StreamFormat unpack(uint64_t v) { return StreamFormat{uint32_t(v >> 16), uint16_t(v & 0xFFFF)}; }

std::wstring fmt_text(StreamFormat f) {
  return std::to_wstring(f.sample_rate) + L" Hz, " + std::to_wstring(f.channels) + L" ch";
}

// The silence cap (§5.3): the shortest rotation period of any destination (one ring is
// shared), 30 min if none rotates.
uint32_t cap_minutes(const SessionConfig& c) {
  uint32_t m = 0;
  for (const auto& d : c.dests)
    if (d.rotation_minutes && (!m || d.rotation_minutes < m)) m = d.rotation_minutes;
  return m ? m : kDefaultCapMinutes;
}

// Review Focus 1: every destination stops at once, so hung drives share one ~3 s budget
// instead of adding up. Each Destination is still stopped by exactly one thread while
// its controlling thread waits here.
void stop_all(std::vector<std::unique_ptr<Destination>>& ds, SampleIndex end, const std::string& reason) {
  std::vector<std::thread> ts;
  for (auto& d : ds) {
    Destination* p = d.get();
    try {
      ts.emplace_back([p, end, reason] { p->stop(end, reason); });
    } catch (const std::system_error&) {
      p->stop(end, reason);
    }
  }
  for (auto& t : ts) t.join();
}

}  // namespace

struct SessionManager::Session {
  SessionParams params;
  StreamFormat fmt;
  uint64_t seq = 0;
  std::vector<DestResumeSeed> seeds;  // per destination (crash state before its first file)
  SampleIndex first = 0;              // the ring's first sample (> start_sample after a resume)
  // Destroyed in reverse order: destinations and meter before the ring they read.
  std::unique_ptr<SampleRing> ring;
  std::unique_ptr<MeterReader> meter;
  std::vector<std::unique_ptr<Destination>> dests;
};

SessionManager::SessionManager(ICaptureSource& src, IFileSystem& fs, IClock& clock, EventLog& log)
    : src_(src), fs_(fs), clock_(clock), log_(log) {}

SessionManager::~SessionManager() { stop(); }

PreflightResult SessionManager::preflight(const SessionConfig& cfg) {
  SessionParams p = cfg.params;
  const StreamFormat f = src_.format();
  if (f.sample_rate) p.rate = f.sample_rate;
  if (f.channels) p.input_channels = f.channels;
  return spool::preflight(cfg.dests, p, fs_);
}

void SessionManager::set_state_store(StateStore* store) { store_ = store; }

void SessionManager::on_new_session(std::function<void(const std::wstring&)> f) { on_new_session_ = std::move(f); }

bool SessionManager::start(const SessionConfig& cfg, const ResumeInfo* resume, std::wstring* err) {
  std::wstring e;
  if (!err) err = &e;
  if (thread_.joinable()) {
    if (state_.load() != SessionState::Idle) {
      *err = L"Already recording";
      return false;
    }
    thread_.join();  // the manager thread ended itself: a linked session could not start
  }
  const StreamFormat fmt = src_.format();
  if (!fmt.sample_rate || !fmt.channels) {
    *err = L"The input has no format";
    return false;
  }
  if (resume && !(StreamFormat{resume->prior.rate, resume->prior.channels} == fmt)) {
    *err = L"The input format (" + fmt_text(fmt) + L") is not the crashed session's (" +
           fmt_text(StreamFormat{resume->prior.rate, resume->prior.channels}) + L")";
    return false;
  }
  cfg_ = cfg;
  SessionParams p = cfg.params;
  p.rate = fmt.sample_rate;
  p.input_channels = fmt.channels;
  SampleIndex ring_start = p.start_sample;
  std::vector<DestResumeSeed> seeds(cfg.dests.size());
  if (resume) {
    const RecordingState& prior = resume->prior;
    p.session_id = prior.session_id;
    used_ids_.insert(p.session_id);
    if (!prior.session_name.empty()) p.session_name = prior.session_name;
    p.start_sample = prior.start_sample;
    p.start_wall = WallTime{prior.start_utc_ms, prior.local_offset_min};
    ring_start = std::max(resume->resume_sample, prior.start_sample);
    std::vector<bool> taken(prior.dests.size());
    for (size_t i = 0; i < cfg.dests.size(); ++i)
      for (size_t j = 0; j < prior.dests.size(); ++j)
        if (!taken[j] && prior.dests[j].name == cfg.dests[i].name) {
          taken[j] = true;
          seeds[i] = DestResumeSeed{std::max<uint32_t>(prior.dests[j].next_n, 1), prior.dests[j].manifest_path};
          break;
        }
  } else {
    const WallTime now = clock_.now();
    p.session_id = next_session_id(now);
    p.start_wall = now;
  }
  if (on_new_session_) on_new_session_(p.session_id);
  {  // the previous session (and any retired one) goes before the new ring is allocated
    std::shared_ptr<Session> prev;
    std::vector<std::shared_ptr<Session>> old;
    std::lock_guard<std::mutex> lk(sess_m_);
    prev.swap(cur_);
    old.swap(retired_);
  }
  auto s = make_session(p, fmt, ring_start, std::move(seeds), err);
  if (!s) return false;

  handled_lost_ = lost_seq_.load(std::memory_order_acquire);  // events before this session do not count
  handled_ret_ = ret_seq_.load(std::memory_order_acquire);
  discontinuities_.store(0);
  input_gaps_.store(0);
  ui_dropped_.store(0);
  marker_n_ = 0;
  capped_ = false;
  {
    std::lock_guard<std::mutex> lk(sess_m_);
    cur_ = s;
    last_stats_ = SessionStats{};
    last_dests_.clear();
  }
  cap_ring_.store(s->ring.get(), std::memory_order_release);
  if (!src_.start(this)) {
    cap_ring_.store(nullptr);
    {
      std::lock_guard<std::mutex> lk(sess_m_);
      cur_.reset();
    }
    *err = L"Cannot start the input";
    return false;
  }
  // Queued ahead of everything else, and only where the crashed session wrote a gap_start.
  if (resume)
    for (size_t i = 0; i < s->dests.size(); ++i)
      if (!s->seeds[i].prev_manifest.empty()) s->dests[i]->post(CmdGapEnd{ring_start, "crash"});
  for (auto& d : s->dests) d->start(ring_start);
  state_.store(SessionState::Recording);
  recovered_.store(resume != nullptr);
  log(Severity::Info, L"Session " + p.session_id + (resume ? L" resumed after a crash (" : L" started (") +
                          fmt_text(fmt) + L")");
  quit_.store(false);
  start_steady_ = clock_.steady_ms();
  saved_seq_ = 0;  // the manager thread saves the crash state at once
  saved_dests_.clear();
  thread_ = std::thread([this] { run(); });
  return true;
}

void SessionManager::stop() {
  if (!thread_.joinable()) return;
  quit_.store(true);
  thread_.join();  // the manager thread drains the UI queue one last time
  stop_filler();
  src_.stop();
  cap_ring_.store(nullptr);
  const auto s = current();
  if (s) {
    stop_all(s->dests, s->ring->write_index(), "stop");
    log(Severity::Info, L"Session " + s->params.session_id + L" stopped");
  }
  clear_state();
  state_.store(SessionState::Idle);
  release_session();
}

void SessionManager::abandon_for_test() {
  if (!thread_.joinable()) return;
  quit_.store(true);
  thread_.join();
  stop_filler();
  src_.stop();
  cap_ring_.store(nullptr);
  std::shared_ptr<Session> s;
  std::vector<std::shared_ptr<Session>> old;
  {
    std::lock_guard<std::mutex> lk(sess_m_);
    s.swap(cur_);
    old.swap(retired_);
  }
  // Every writer exits without finalizing; then the sessions (rings, meters) can go.
  if (s)
    for (auto& d : s->dests) d->abandon_for_test();
  state_.store(SessionState::Idle);
}

void SessionManager::marker(uint32_t seconds_ago) {
  const auto s = current();
  if (!s || !thread_.joinable()) return;
  const SampleIndex back = SampleIndex(seconds_ago) * SampleIndex(s->fmt.sample_rate);
  // Review Focus 5; after a resume never inside the crash gap.
  const SampleIndex at = std::max(s->first, s->ring->write_index() - back);
  push(UiCommand{UiCommand::Kind::Marker, s->seq, at, 0, L"Marker " + std::to_wstring(++marker_n_)});
}

void SessionManager::split() {
  const auto s = current();
  if (!s || !thread_.joinable()) return;
  push(UiCommand{UiCommand::Kind::Split, s->seq, s->ring->write_index(), 0, {}});
}

void SessionManager::append_folder(size_t dest, std::wstring path) {
  const auto s = current();
  if (!s || !thread_.joinable()) return;
  push(UiCommand{UiCommand::Kind::AppendFolder, s->seq, 0, dest, std::move(path)});
}

void SessionManager::acknowledge() {
  const auto s = current();
  if (!s || !thread_.joinable()) return;
  push(UiCommand{UiCommand::Kind::Acknowledge, s->seq, 0, 0, {}});
}

SessionStats SessionManager::stats() const {
  SessionStats st{};
  std::shared_ptr<Session> s;
  {
    std::lock_guard<std::mutex> lk(sess_m_);
    s = cur_;
    if (!s) st = last_stats_;
  }
  st.state = state_.load();
  if (!s) return st;
  st.session_id = s->params.session_id;
  st.fmt = s->fmt;
  st.position = s->ring->write_index();
  st.elapsed_s = double(st.position - s->params.start_sample) / double(s->fmt.sample_rate);
  st.gaps = input_gaps_.load();
  st.discontinuities = discontinuities_.load();
  SampleIndex slowest = st.position;
  for (const auto& d : s->dests) {
    const DestStatus ds = d->status();
    if (ds.state == DestState::Writing || ds.state == DestState::FailingOver) slowest = std::min(slowest, ds.cursor);
  }
  st.ring_fill = std::clamp(double(st.position - slowest) / double(s->ring->capacity()), 0.0, 1.0);
  st.recovered_from_crash = recovered_.load();
  return st;
}

std::vector<DestStatus> SessionManager::dest_status() const {
  std::vector<DestStatus> out;
  std::shared_ptr<Session> s;
  {
    std::lock_guard<std::mutex> lk(sess_m_);
    s = cur_;
    if (!s) out = last_dests_;
  }
  if (s)
    for (const auto& d : s->dests) out.push_back(d->status());
  return out;
}

MeterReader* SessionManager::meter() {
  std::vector<std::shared_ptr<Session>> old;  // freed here, outside the lock
  std::shared_ptr<Session> s;
  {
    std::lock_guard<std::mutex> lk(sess_m_);
    old.swap(retired_);  // the UI has re-fetched: an earlier meter is no longer in use
    s = cur_;
  }
  return s ? s->meter.get() : nullptr;
}

// ---- capture callbacks ----

void SessionManager::on_frames(const float* interleaved, size_t frames, bool discontinuity) {
  if (SampleRing* r = cap_ring_.load(std::memory_order_acquire)) r->write(interleaved, frames);
  if (discontinuity) discontinuities_.fetch_add(1, std::memory_order_relaxed);
}

void SessionManager::on_device_lost() {
  lost_steady_.store(clock_.steady_ms(), std::memory_order_relaxed);
  lost_seq_.fetch_add(1, std::memory_order_release);
}

void SessionManager::on_device_returned(StreamFormat fmt) {
  const WallTime w = clock_.now();
  ret_fmt_.store(pack(fmt), std::memory_order_relaxed);
  ret_steady_.store(clock_.steady_ms(), std::memory_order_relaxed);
  ret_utc_.store(w.utc_ms, std::memory_order_relaxed);
  ret_offset_.store(w.local_offset_min, std::memory_order_relaxed);
  ret_seq_.fetch_add(1, std::memory_order_release);
}

// ---- helpers ----

std::shared_ptr<SessionManager::Session> SessionManager::current() const {
  std::lock_guard<std::mutex> lk(sess_m_);
  return cur_;
}

void SessionManager::release_session(bool retire) {
  SessionStats st = stats();
  std::vector<DestStatus> ds = dest_status();
  std::shared_ptr<Session> s;  // freed here, outside the lock
  std::vector<std::shared_ptr<Session>> old;
  std::lock_guard<std::mutex> lk(sess_m_);
  last_stats_ = std::move(st);
  last_dests_ = std::move(ds);
  s.swap(cur_);
  if (retire) {
    retired_.push_back(std::move(s));
  } else {
    old.swap(retired_);
  }
}

std::shared_ptr<SessionManager::Session> SessionManager::make_session(const SessionParams& p, StreamFormat fmt,
                                                                      SampleIndex ring_start,
                                                                      std::vector<DestResumeSeed> seeds,
                                                                      std::wstring* err) {
  auto s = std::make_shared<Session>();
  s->params = p;
  s->fmt = fmt;
  s->seq = next_seq_++;
  s->first = ring_start;
  seeds.resize(cfg_.dests.size());
  s->seeds = std::move(seeds);
  const size_t frames = size_t(std::max<uint32_t>(cfg_.ring_seconds, 1)) * fmt.sample_rate;
  try {
    s->ring = std::make_unique<SampleRing>(fmt.channels, frames, ring_start);
  } catch (const std::bad_alloc&) {
    *err = L"Not enough memory for the capture ring";
    return nullptr;
  }
  if (!s->ring->locked())
    log(Severity::Warning, L"The capture ring could not be locked in memory; capture may glitch under memory pressure");
  s->meter = std::make_unique<MeterReader>(*s->ring, fmt.sample_rate, clock_);
  // A destination whose channel map does not fit the input disables itself (logged as an error).
  for (size_t i = 0; i < cfg_.dests.size(); ++i)
    s->dests.push_back(
        std::make_unique<Destination>(cfg_.dests[i], s->params, *s->ring, fs_, clock_, log_, s->seeds[i]));
  return s;
}

// yyyymmdd-hhmmss (local); "-2", "-3", ... if this manager already used it.
std::wstring SessionManager::next_session_id(WallTime t) {
  const LocalParts p = local_parts(t);
  wchar_t buf[32];
  std::swprintf(buf, 32, L"%04d%02d%02d-%02d%02d%02d", p.year, p.month, p.day, p.hour, p.minute, p.second);
  std::wstring id = buf;
  for (int n = 2; used_ids_.count(id); ++n) id = std::wstring(buf) + L"-" + std::to_wstring(n);
  used_ids_.insert(id);
  return id;
}

void SessionManager::push(UiCommand c) {
  if (!ui_q_.push(c)) ui_dropped_.fetch_add(1);  // logged by the manager thread
}

void SessionManager::log(Severity s, std::wstring text) { log_.add(s, clock_.now(), L"", std::move(text)); }

void SessionManager::post_all(Session& s, const DestCommand& c) {
  for (auto& d : s.dests) d->post(c);
}

// ---- manager thread ----

void SessionManager::run() {
  uint64_t last_watchdog = start_steady_, last_pair = start_steady_;  // the thread may start late
  uint64_t seen_disc = 0, seen_dropped = 0;
  for (;;) {
    const bool quitting = quit_.load();
    drain_ui(*current());
    if (quitting) return;
    handle_device_events();
    const auto cur = current();  // only this thread replaces it
    if (!cur) return;            // a linked session could not start: nothing left to run
    Session& s = *cur;
    const uint64_t now = clock_.steady_ms();
    if (now - last_watchdog >= kWatchdogMs) {
      last_watchdog = now;
      watchdog(s);
    }
    // Every tick, so a new file or manifest (rotation, failover) reaches the crash state
    // within one tick even while steady time stands still.
    save_state(s, now);
    // Not on silence fill: the pair that falls due while the input is lost follows its return.
    if (now - last_pair >= kClockPairMs && state_.load() != SessionState::InputLost) {
      last_pair = now;
      post_all(s, CmdClockPair{s.ring->write_index(), clock_.now()});
    }
    const uint64_t disc = discontinuities_.load();
    if (disc > seen_disc) log(Severity::Warning, L"The input reported a discontinuity: input samples may have been lost");
    seen_disc = disc;
    const uint64_t dropped = ui_dropped_.load();
    if (dropped > seen_dropped) log(Severity::Warning, L"The command queue was full: a marker or command was dropped");
    seen_dropped = dropped;
    std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
  }
}

void SessionManager::drain_ui(Session& s) {
  while (auto c = ui_q_.pop()) {
    if (c->session != s.seq) {  // stamped in the session a format change replaced
      log(Severity::Warning, L"A command from the previous session was dropped");
      continue;
    }
    switch (c->kind) {
      case UiCommand::Kind::Marker: post_all(s, CmdMarker{c->at, c->text}); break;
      case UiCommand::Kind::Split: post_all(s, CmdSplit{c->at}); break;
      case UiCommand::Kind::AppendFolder:
        if (c->dest < s.dests.size()) {
          s.dests[c->dest]->post(CmdAppendFolder{c->text});
          auto& folders = cfg_.dests[c->dest].folders;  // a linked session keeps it
          if (std::find(folders.begin(), folders.end(), c->text) == folders.end()) folders.push_back(c->text);
        }
        break;
      case UiCommand::Kind::Acknowledge: post_all(s, CmdAcknowledge{}); break;
    }
  }
}

void SessionManager::handle_device_events() {
  const uint64_t lost = lost_seq_.load(std::memory_order_acquire);
  if (lost != handled_lost_) {
    handled_lost_ = lost;
    if (state_.load() == SessionState::Recording) on_lost(*current(), lost_steady_.load(std::memory_order_relaxed));
  }
  const uint64_t ret = ret_seq_.load(std::memory_order_acquire);
  if (ret != handled_ret_) {
    handled_ret_ = ret;
    if (state_.load() == SessionState::InputLost)
      on_returned(unpack(ret_fmt_.load(std::memory_order_relaxed)), ret_steady_.load(std::memory_order_relaxed),
                  WallTime{ret_utc_.load(std::memory_order_relaxed), ret_offset_.load(std::memory_order_relaxed)});
  }
}

// §5.3: the gap starts at the last captured sample; silence follows at real-time pace.
void SessionManager::on_lost(Session& s, uint64_t lost_steady_ms) {
  src_.stop();  // the filler must be the ring's only producer
  gap_at_ = s.ring->write_index();
  gap_steady_ = lost_steady_ms;
  capped_ = false;
  input_gaps_.fetch_add(1);
  state_.store(SessionState::InputLost);
  post_all(s, CmdGapStart{gap_at_, "device_lost"});
  log(Severity::Error, L"Input lost: recording silence until it returns");
  start_filler(s);
}

void SessionManager::on_returned(StreamFormat fmt, uint64_t steady_ms, WallTime wall) {
  Session& s = *current();
  stop_filler();
  fill_to(s, steady_ms);  // silence up to the moment it returned (or the cap)
  if (!(fmt == s.fmt)) {
    link_new_session(fmt, steady_ms, wall);
    return;
  }
  if (capped_) {
    // The files were closed at the cap: the sample counter catches up with wall time.
    const SampleIndex j = s.params.start_sample +
                          (wall.utc_ms - s.params.start_wall.utc_ms) * SampleIndex(s.fmt.sample_rate) / 1000;
    s.ring->jump_to(j);  // forward only
    post_all(s, CmdResumeAt{s.ring->write_index()});
  }
  restart_capture(s, steady_ms);
}

// Ends the gap at the current position and starts capture; if that fails a new gap
// starts right there.
void SessionManager::restart_capture(Session& s, uint64_t steady_ms) {
  post_all(s, CmdGapEnd{s.ring->write_index(), "device_lost"});
  if (!src_.start(this)) {
    log(Severity::Error, L"Cannot restart the input");
    on_lost(s, steady_ms);
    return;
  }
  state_.store(SessionState::Recording);
  log(Severity::Info, L"Input returned: recording resumed");
}

// §5.3: a different format ends the session; a linked one with the same destinations
// continues on a new ring (and a new meter) with its own sample clock.
void SessionManager::link_new_session(StreamFormat fmt, uint64_t steady_ms, WallTime wall) {
  const auto old = current();
  log(Severity::Warning, L"Input returned with a different format (" + fmt_text(fmt) + L"): ending session " +
                             old->params.session_id);
  stop_all(old->dests, old->ring->write_index(), "format_change");
  SessionParams p = old->params;
  p.prev_session = old->params.session_id;
  p.session_id = next_session_id(wall);
  p.rate = fmt.sample_rate;
  p.input_channels = fmt.channels;
  p.start_sample = 0;
  p.start_wall = wall;
  if (on_new_session_) on_new_session_(p.session_id);
  std::wstring err;
  const auto s = fmt.sample_rate && fmt.channels ? make_session(p, fmt, p.start_sample, {}, &err) : nullptr;
  if (!s) {
    cap_ring_.store(nullptr);
    state_.store(SessionState::Idle);
    clear_state();  // nothing to resume: the session has ended
    log(Severity::Error, L"Cannot start the linked session: " + (err.empty() ? L"the input has no format" : err));
    release_session(true);  // the manager thread ends; Record starts a new session
    return;
  }
  cap_ring_.store(s->ring.get(), std::memory_order_release);
  {
    std::lock_guard<std::mutex> lk(sess_m_);
    retired_.push_back(cur_);  // its meter may still be in the UI's hands until meter() is called again
    cur_ = s;
  }
  input_gaps_.store(0);
  discontinuities_.store(0);
  capped_ = false;
  for (auto& d : s->dests) d->start(p.start_sample);
  log(Severity::Info, L"Session " + p.session_id + L" started (" + fmt_text(fmt) + L"), linked to " + p.prev_session);
  if (!src_.start(this)) {
    log(Severity::Error, L"Cannot restart the input");
    on_lost(*s, steady_ms);
    return;
  }
  state_.store(SessionState::Recording);
}

// Level-triggered (R14): declare_stalled() on every tick while the condition holds; the
// destination's own grace and idle rules decide whether it acts.
void SessionManager::watchdog(Session& s) {
  const SampleIndex wi = s.ring->write_index();
  for (auto& d : s.dests)
    if (is_stalled(wi, d->progress(), s.ring->capacity(), s.params.stall_percent)) d->declare_stalled();
}

// The crash state (§8.5): written every flush_ms of steady time, and as soon as a file or
// manifest changes (rotation, failover) or a linked session replaces this one.
void SessionManager::save_state(Session& s, uint64_t steady_ms) {
  if (!store_ || state_.load() == SessionState::Idle) {
    saved_seq_ = s.seq;  // nothing to keep
    return;
  }
  RecordingState r;
  r.recording = true;
  r.session_id = s.params.session_id;
  r.session_name = s.params.session_name;
  r.start_sample = s.params.start_sample;
  r.start_utc_ms = s.params.start_wall.utc_ms;
  r.local_offset_min = s.params.start_wall.local_offset_min;
  r.rate = s.fmt.sample_rate;
  r.channels = s.fmt.channels;
  r.device_id = cfg_.input_device_id;
  std::vector<std::pair<uint32_t, std::wstring>> now_dests;
  for (size_t i = 0; i < s.dests.size(); ++i) {
    const DestStatus ds = s.dests[i]->status();
    const DestResumeSeed& seed = s.seeds[i];
    // Before its first file (or manifest) a resumed destination still continues the seed's.
    const uint32_t next_n = ds.n ? ds.n + 1 : seed.first_n;
    const std::wstring man = ds.manifest_path.empty() ? seed.prev_manifest : ds.manifest_path;
    r.dests.push_back(DestResume{ds.name, man, next_n});
    now_dests.emplace_back(next_n, man);
  }
  const bool due = saved_seq_ != s.seq || steady_ms - saved_steady_ >= s.params.flush_ms || now_dests != saved_dests_;
  if (!due) return;
  r.last_sample = s.ring->write_index();
  r.last_utc_ms = clock_.now().utc_ms;
  if (saved_seq_ != s.seq) save_warned_ = false;  // once per session
  if (!store_->save(r) && !save_warned_) {
    save_warned_ = true;
    log(Severity::Warning, L"Cannot save the recording state: the session cannot be resumed after a crash");
  }
  saved_seq_ = s.seq;
  saved_steady_ = steady_ms;
  saved_dests_ = std::move(now_dests);
}

void SessionManager::clear_state() {
  if (store_ && !store_->clear())
    log(Severity::Error, L"Cannot clear the recording state: the next launch checks the manifests before resuming");
}

// ---- silence filler ----

void SessionManager::start_filler(Session& s) {
  cap_frames_ = SampleIndex(cap_minutes(cfg_)) * 60 * SampleIndex(s.fmt.sample_rate);
  zeros_.assign(kFillChunk * s.fmt.channels, 0.0f);
  filler_quit_.store(false);
  Session* sp = &s;  // outlives the filler: it is joined before the session is replaced
  try {
    filler_ = std::thread([this, sp] {
      while (!filler_quit_.load() && !capped_) {
        fill_to(*sp, clock_.steady_ms());
        std::this_thread::sleep_for(std::chrono::milliseconds(kFillPollMs));
      }
    });
  } catch (const std::system_error&) {
    log(Severity::Error, L"Cannot start the silence filler; the gap is filled when the input returns");
  }
}

void SessionManager::stop_filler() {
  filler_quit_.store(true);
  if (filler_.joinable()) filler_.join();
}

// R4: paced by the steady clock, never by real time, so tests with a FakeClock are exact.
void SessionManager::fill_to(Session& s, uint64_t steady_ms) {
  if (capped_) return;
  const uint64_t ms = steady_ms > gap_steady_ ? steady_ms - gap_steady_ : 0;
  const SampleIndex elapsed = SampleIndex(ms * s.fmt.sample_rate / 1000);
  const SampleIndex target = gap_at_ + std::min(elapsed, cap_frames_);
  for (SampleIndex wi = s.ring->write_index(); wi < target;) {
    const size_t n = size_t(std::min<SampleIndex>(target - wi, SampleIndex(kFillChunk)));
    s.ring->write(zeros_.data(), n);
    wi += SampleIndex(n);
  }
  if (elapsed < cap_frames_) return;
  capped_ = true;
  post_all(s, CmdCloseForGap{target});
  log(Severity::Warning, L"Input lost for " + std::to_wstring(cap_frames_ / 60 / SampleIndex(s.fmt.sample_rate)) +
                             L" min: files closed until it returns");
}

}  // namespace spool
