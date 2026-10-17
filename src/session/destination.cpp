#include "session/destination.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cwctype>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string_view>
#include <system_error>
#include <utility>

#include "core/sample_convert.h"
#include "format/opus_writer.h"
#include "format/wav_writer.h"
#include "session/manifest.h"
#include "session/naming.h"
#include "session/preflight.h"

namespace sl {

namespace {

constexpr size_t kChunkFrames = 4096;
constexpr int kIdleMs = 5;
constexpr int kNameAttempts = 100;
constexpr int64_t kDayMs = 86400000;
constexpr uint64_t kSpaceCheckMs = 10000;  // low-water check interval (§8.2)
constexpr uint64_t kRetryMs = 30000;       // LastResort retry interval (§4.3)
constexpr DWORD kStopBudgetMs = 3000;
constexpr size_t kNone = SIZE_MAX;
constexpr uint64_t kMinStallGraceMs = 500, kMaxStallGraceMs = 5000;
constexpr uint64_t kIdleStallMs = 2000;  // R14: I/O this long outside Writing is a hang

uint32_t bytes_per_sample(WavDepth d) {
  switch (d) {
    case WavDepth::Int16: return 2;
    case WavDepth::Int24: return 3;
    default: return 4;
  }
}

uint32_t opus_bitrate(uint32_t configured, size_t channels) {
  return configured ? configured : (channels == 1 ? 64000u : 128000u);
}

// R13 (revised): how long a stall takeover may take to catch up before the watchdog may
// fire again. A takeover starts about stall_percent of the ring behind; half of the
// remaining headroom, clamped to [0.5 s, 5 s], leaves a rescue after a second hang
// starting on data that is still in the ring.
uint64_t stall_grace_ms(const SessionParams& sp, size_t ring_capacity) {
  const uint32_t pct = std::min<uint32_t>(sp.stall_percent, 100);
  const uint64_t headroom = uint64_t(ring_capacity) * (100 - pct) / 100;
  const uint64_t ms = sp.rate ? headroom * 1000 / sp.rate / 2 : 0;
  return std::clamp(ms, kMinStallGraceMs, kMaxStallGraceMs);
}

// Creates dir\stem.ext (or stem_1.ext, ...) through `open`, which must fail rather than
// overwrite (IFileSystem::create_new semantics). unique_path is only advisory: if the
// name was taken between the check and the create, the next free name is tried.
template <class Open>
auto create_unique(IFileSystem& fs, const std::wstring& dir, const std::wstring& stem,
                   const std::wstring& ext, std::wstring& path, Open open) -> decltype(open(path)) {
  for (int i = 0; i < kNameAttempts; ++i) {
    path = unique_path(fs, dir, stem, ext);
    auto p = open(path);
    if (p || !fs.exists(path)) return p;  // success, or a failure other than a taken name
  }
  return nullptr;
}

std::wstring hms(WallTime w) {
  const LocalParts p = local_parts(w);
  wchar_t buf[16];
  std::swprintf(buf, 16, L"%02d:%02d:%02d", p.hour, p.minute, p.second);
  return buf;
}

std::wstring widen(std::string_view s) { return std::wstring(s.begin(), s.end()); }  // ASCII

// Case-insensitive folder identity, ignoring trailing separators.
std::wstring folder_key(std::wstring p) {
  while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
  for (wchar_t& c : p) c = c == L'/' ? L'\\' : wchar_t(std::towlower(c));
  return p;
}

// The size of the file an encoder writes, kept after the encoder closed it, so file_close
// can report the byte length without reopening the file on a failing drive.
using SizeTap = std::shared_ptr<std::atomic<uint64_t>>;

class TappedFile final : public IFile {
 public:
  TappedFile(std::unique_ptr<IFile> f, SizeTap tap) : f_(std::move(f)), tap_(std::move(tap)) {}
  bool write(const void* p, size_t n) override {
    const bool ok = f_->write(p, n);
    note();
    return ok;
  }
  bool write_at(uint64_t off, const void* p, size_t n) override {
    const bool ok = f_->write_at(off, p, n);
    note();
    return ok;
  }
  bool read_at(uint64_t off, void* p, size_t n) override { return f_->read_at(off, p, n); }
  bool flush() override { return f_->flush(); }
  uint64_t size() const override { return f_->size(); }
  void close() override {
    note();
    f_->close();
  }

 private:
  void note() { tap_->store(f_->size(), std::memory_order_relaxed); }
  std::unique_ptr<IFile> f_;
  SizeTap tap_;
};

// Hands out TappedFiles; everything else goes straight to the real file system.
class TappingFs final : public IFileSystem {
 public:
  TappingFs(IFileSystem& fs, SizeTap tap) : fs_(fs), tap_(std::move(tap)) {}
  std::unique_ptr<IFile> create_new(const std::wstring& p, std::wstring* err) override {
    auto f = fs_.create_new(p, err);
    if (!f) return f;
    tap_->store(0, std::memory_order_relaxed);
    return std::make_unique<TappedFile>(std::move(f), tap_);
  }
  std::unique_ptr<IFile> open_existing(const std::wstring& p, std::wstring* err) override {
    return fs_.open_existing(p, err);
  }
  bool exists(const std::wstring& p) override { return fs_.exists(p); }
  bool ensure_dir(const std::wstring& d, std::wstring* err) override { return fs_.ensure_dir(d, err); }
  bool remove(const std::wstring& p) override { return fs_.remove(p); }
  std::vector<std::wstring> list(const std::wstring& d, const std::wstring& s) override { return fs_.list(d, s); }
  std::optional<uint64_t> free_bytes(const std::wstring& d) override { return fs_.free_bytes(d); }
  std::wstring volume_id(const std::wstring& d) override { return fs_.volume_id(d); }

 private:
  IFileSystem& fs_;
  SizeTap tap_;
};

enum class Mode { Writing, GapClosed, LastResort };

// Writer state that a new generation takes over after a stall. The writer publishes it
// with every status change, under the status mutex.
struct Carry {
  bool started = false;  // begin() ran
  Mode mode = Mode::Writing;
  uint32_t n = 1, seg = 0;
  size_t folder = kNone;  // folder of the open (or last) file
  std::wstring path;       // the open (or last) file
  SampleIndex session_start = 0, file_start = 0;
  SampleIndex next_boundary = kNoRotation;
  std::set<SampleIndex> splits;
  std::vector<std::pair<SampleIndex, std::wstring>> markers;  // not yet in a finalized file
  std::wstring manifest_path;     // the current (or last) manifest: the next one's prev_manifest
  std::vector<JsonLine> backlog;  // events waiting for a manifest
  SampleIndex gap_from = 0;       // LastResort: where the gap started
  bool gap_in_manifest = false;   // ... and its gap_start went to a manifest, not the backlog
};

// A file and/or manifest handed off by a failover or a manifest move. It is finished on a
// detached thread, so a failing drive never blocks the writer.
struct Retired {
  IEncoderFile* enc = nullptr;
  std::unique_ptr<Manifest> man;
  std::wstring path;
  SampleIndex start = 0;
  std::optional<uint64_t> truncate_to;
  std::vector<Marker> markers;
  SizeTap bytes;
  std::vector<JsonLine> after;  // appended to `man` after the file_close
};

}  // namespace

// Everything a writer generation shares with the Destination and with later generations.
// Writers hold it by shared_ptr, so an abandoned one never touches freed memory.
struct Destination::Core {
  Core(const SessionParams& p, SampleRing& r, IFileSystem& f, IClock& c, EventLog& l, DestResumeSeed s,
       std::wstring n)
      : sp(p),
        ring(r),
        fs(f),
        clock(c),
        log(l),
        seed(std::move(s)),
        name(std::move(n)),
        ring_channels(r.channels()),
        ring_capacity(r.capacity()) {}

  void set_state_locked(DestState s) {
    if (st.state != DestState::Disabled) st.state = s;  // Disabled is sticky
  }
  void finish(Retired r);  // runs on a detached thread

  const SessionParams sp;
  SampleRing& ring;
  IFileSystem& fs;
  IClock& clock;
  EventLog& log;
  const DestResumeSeed seed;
  const std::wstring name;
  const uint32_t ring_channels;
  const size_t ring_capacity;
  std::vector<uint16_t> map;               // ring channel per destination channel; fixed before start
  uint64_t rate_bytes = 0, low_water = 0;  // fixed before start

  // The current writer generation. Changed only with `live` held exclusively and `m` held,
  // so a writer that checked it under a shared `live` (ring, clock, log, cursor) or under
  // `m` (status, commands, carry) cannot be replaced halfway through that step.
  std::atomic<uint64_t> generation{0};
  std::shared_mutex live;

  mutable std::mutex m;  // guards everything below; never held across file I/O
  std::deque<DestCommand> cmds;
  DestStatus st;
  bool stop_requested = false;
  SampleIndex stop_end = 0;
  std::string stop_reason;
  Carry carry;
  size_t busy_folder = kNone;     // folder of the writer's current file I/O
  bool io_busy = false;           // the current writer is inside file I/O (Writer::IoScope)
  uint64_t io_since_ms = 0;       // ... since this steady time
  bool seed_opening = false;      // the current writer is reopening the seed manifest
  bool skip_seed = false;         // that reopen hung: never try it again
  std::set<size_t> quarantined;   // a writer hung there: never probed again this session
  SampleIndex stall_grace_until = -1;  // declare_stalled() ignored until the cursor gets here
  uint64_t stall_grace_deadline_ms = 0;  // ... or until this steady time (R13)
  int finalizers = 0;             // Retired items still being finished
  std::condition_variable fin_cv;

  std::atomic<SampleIndex> cursor{0};
};

// Best effort: finalize the old file (its header never claims more than was confirmed),
// then record the close and the failover in the old manifest. Touches only `r` and `sp`.
void Destination::Core::finish(Retired r) {
  if (r.enc) {
    const bool ok = r.enc->finalize(r.markers, r.truncate_to);
    const SampleIndex end = r.start + SampleIndex(r.enc->confirmed_frames());
    delete r.enc;
    if (r.man) {
      JsonLine l("file_close", end, wall_for(sp, end));
      l.str("path", r.path).num("start", r.start).num("end", end).num("bytes", int64_t(r.bytes ? r.bytes->load() : 0));
      if (!ok) l.num("ok", 0);  // a crash scan repairs it
      r.man->append(l);
    }
  }
  if (r.man) {
    for (const auto& l : r.after) r.man->append(l);
    r.man->flush();
    r.man.reset();
  }
  std::lock_guard<std::mutex> lk(m);
  --finalizers;
  fin_cv.notify_all();
}

// One generation of the writer thread. Everything it touches while it may be stuck in
// file I/O lives here or in Core, so an abandoned generation can be leaked whole: the
// encoder is a raw pointer that is deleted only after a normal finalize. Once a newer
// generation exists (stale()), this one writes nothing shared and exits at its next check.
class Destination::Writer {
 public:
  // A fresh start (carry.started false) or a generation taking over after a stall on
  // stalled_folder. Constructed with k.m held.
  Writer(std::shared_ptr<Core> core, uint64_t gen, SampleIndex cursor, Carry carry, size_t stalled_folder)
      : core_(std::move(core)),
        k_(*core_),
        gen_(gen),
        cfg_(k_.st.config),
        out_ch_(uint32_t(k_.map.size())),
        stalled_(stalled_folder),
        c_(std::move(carry)),
        cursor_(cursor),
        in_(kChunkFrames * k_.ring_channels),
        out_(kChunkFrames * k_.map.size()) {}
  ~Writer() { delete enc_; }  // null after a normal stop; never runs for an abandoned writer

  void run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    last_flush_ms_ = last_space_ms_ = now_ms();
    next_retry_ms_ = last_flush_ms_ + kRetryMs;
    if (!c_.started) {
      begin();
    } else {
      take_over();
    }
    while (!gone()) {
      std::optional<DestCommand> front;
      bool stop = false;
      SampleIndex stop_end = 0;
      std::string stop_reason;
      {
        std::lock_guard<std::mutex> lk(k_.m);
        if (stale()) continue;
        if (!k_.cmds.empty()) front = k_.cmds.front();
        stop = k_.stop_requested;
        stop_end = k_.stop_end;
        stop_reason = k_.stop_reason;
      }
      // A command leaves the queue only once handled, so a generation that takes over
      // after a stall handles one this generation did not finish.
      if (front && ready(*front)) {
        redo_front_ = false;
        std::visit([this](const auto& c) { on(c); }, *front);
        if (gone()) return;
        std::lock_guard<std::mutex> lk(k_.m);
        if (!stale() && !redo_front_) {
          k_.cmds.pop_front();
          k_.carry = c_;
        }
        continue;
      }
      if (stop && (!enc_ || cursor_ >= stop_end)) {
        if (end_session(stop_end, stop_reason)) return;
        continue;  // failed over: write through stop_end in the next folder
      }
      housekeeping();
      if (gone()) return;
      if (!enc_) {
        if (c_.mode == Mode::LastResort) follow_capture();
        idle();
        continue;
      }

      // Write up to the next cut: rotation boundary, split point, a pending close-for-gap
      // (the only command that waits for the cursor) or the stop sample.
      const SampleIndex cut = std::min(c_.next_boundary, c_.splits.empty() ? kNoRotation : *c_.splits.begin());
      if (cut <= cursor_) {
        rotate(cut);
        continue;
      }
      SampleIndex limit = stop ? stop_end : kNoRotation;
      if (front)
        if (const auto* g = std::get_if<CmdCloseForGap>(&*front)) limit = std::min(limit, g->at);
      const SampleIndex want = std::min({SampleIndex(kChunkFrames), cut - cursor_, limit - cursor_});
      if (want <= 0) {
        idle();
        continue;
      }
      SampleRing::ReadResult r{0, false};
      if (!live([&] { r = k_.ring.read(cursor_, in_.data(), size_t(want)); })) continue;
      if (r.frames == 0) {
        if (r.overrun) {
          on_overrun();
        } else {
          idle();
        }
        continue;
      }
      extract_channels(in_.data(), k_.ring_channels, k_.map.data(), out_ch_, out_.data(), r.frames);
      const bool ok = enc_->write(out_.data(), r.frames);
      if (gone()) return;
      if (!ok) {
        failover("write_error", L"Write failed: " + c_.path);
        continue;
      }
      cursor_ += SampleIndex(r.frames);
      publish_cursor();
    }
  }

 private:
  using LeadFn = std::function<std::vector<JsonLine>(const std::wstring& to)>;

  // ---- generation guard ----

  bool stale() const { return k_.generation.load(std::memory_order_acquire) != gen_; }
  // True once a newer generation exists. The encoder is then abandoned (never finalized
  // or closed: a newer generation owns the destination, and the handle may be stuck).
  bool gone() {
    if (!dead_ && stale()) {
      dead_ = true;
      if (enc_) enc_->abandon();
    }
    return dead_;
  }
  // Runs f (ring, clock, log, cursor: non-blocking uses) unless this generation is stale.
  template <class F>
  bool live(F f) {
    std::shared_lock<std::shared_mutex> lk(k_.live);
    if (stale()) return false;
    f();
    return true;
  }
  // Applies a status change and publishes the carry, unless this generation is stale.
  template <class F>
  bool commit(F f) {
    std::lock_guard<std::mutex> lk(k_.m);
    if (stale()) return false;
    f(k_.st);
    k_.carry = c_;
    return true;
  }

  // Marks the writer as inside file I/O (probe, open, manifest, encoder flush or close) for
  // its lifetime; nestable. declare_stalled() is honored outside Writing/FailingOver only
  // while this is set: a writer idle on purpose (GapClosed, LastResort) is not stalled,
  // one stuck in an open or a probe is.
  class IoScope {
   public:
    IoScope(Writer& w, size_t folder) : w_(w) { w_.set_io(+1, folder); }
    ~IoScope() { w_.set_io(-1, kNone); }
    IoScope(const IoScope&) = delete;
    IoScope& operator=(const IoScope&) = delete;

   private:
    Writer& w_;
  };
  void set_io(int delta, size_t folder) {
    io_depth_ += delta;
    std::lock_guard<std::mutex> lk(k_.m);
    if (stale()) return;
    // Not stale with m held: the Destination (and so the clock) is alive.
    if (delta > 0 && io_depth_ == 1) k_.io_since_ms = k_.clock.steady_ms();
    k_.io_busy = io_depth_ > 0;
    if (delta > 0 && folder != kNone) k_.busy_folder = folder;
  }

  void log(Severity s, std::wstring text) {
    live([&] { k_.log.add(s, k_.clock.now(), k_.name, std::move(text)); });
  }
  uint64_t now_ms() {
    uint64_t t = 0;
    live([&] { t = k_.clock.steady_ms(); });
    return t;
  }
  void publish_cursor() {
    live([&] { k_.cursor.store(cursor_, std::memory_order_release); });
  }
  static void idle() { std::this_thread::sleep_for(std::chrono::milliseconds(kIdleMs)); }

  // ---- manifest lines ----

  WallTime wall(SampleIndex s) const { return wall_for(k_.sp, s); }
  JsonLine line(std::string_view ev, SampleIndex s) const { return JsonLine(ev, s, wall(s)); }
  void event(const JsonLine& l) {
    if (man_) {
      man_->append(l);
    } else {
      c_.backlog.push_back(l);
    }
  }
  JsonLine gap_line(std::string_view ev, SampleIndex at, std::string_view reason) const {
    JsonLine l = line(ev, at);
    l.str("reason", widen(reason));
    return l;
  }
  // old_path: the file that failed over (empty if none was open). Crash repair caps its
  // header at `confirmed`: everything after that is in the next file.
  JsonLine failover_line(SampleIndex at, const std::wstring& from, const std::wstring& to,
                         std::string_view trigger, const std::wstring& old_path) const {
    JsonLine l = line("failover", at);
    l.str("from", from).str("to", to).str("trigger", widen(trigger)).num("confirmed", at);
    l.str("old_path", old_path);
    return l;
  }
  JsonLine session_start_line(SampleIndex at, const std::wstring& prev_manifest) const {
    std::wstring map;
    for (uint16_t c : k_.map) map += (map.empty() ? L"" : L",") + std::to_wstring(c);
    JsonLine l = line("session_start", at);
    l.str("session", k_.sp.session_name)
        .str("session_id", k_.sp.session_id)
        .str("dest", cfg_.name)
        .str("format", cfg_.format == OutFormat::Wav ? L"wav" : L"opus")
        .num("rate", k_.sp.rate)
        .num("channels", out_ch_)
        .str("channel_map", map);
    if (!k_.sp.prev_session.empty()) l.str("prev_session", k_.sp.prev_session);
    if (!prev_manifest.empty()) l.str("prev_manifest", prev_manifest);
    return l;
  }

  // ---- folders ----

  size_t folder_count() const {
    std::lock_guard<std::mutex> lk(k_.m);
    return k_.st.config.folders.size();
  }
  std::wstring folder_path(size_t i) const {
    std::lock_guard<std::mutex> lk(k_.m);
    return i < k_.st.config.folders.size() ? k_.st.config.folders[i] : std::wstring();
  }
  size_t folder_of(const std::wstring& file) const {
    const size_t cut = file.find_last_of(L"\\/");
    if (cut == std::wstring::npos) return kNone;
    const std::wstring dir = folder_key(file.substr(0, cut));
    std::lock_guard<std::mutex> lk(k_.m);
    for (size_t i = 0; i < k_.st.config.folders.size(); ++i)
      if (folder_key(k_.st.config.folders[i]) == dir) return i;
    return kNone;
  }
  // True if folder i's health changed.
  bool set_health(size_t i, FolderHealth h) {
    bool changed = false;
    commit([&](DestStatus& st) {
      if (i < st.folders.size() && st.folders[i].health != h) {
        st.folders[i].health = h;
        changed = true;
      }
    });
    return changed;
  }
  void note_space(const std::optional<uint64_t>& free) {
    const uint64_t rate = k_.rate_bytes;
    commit([&](DestStatus& st) {
      if (c_.folder >= st.folders.size()) return;
      auto& f = st.folders[c_.folder];
      f.free_bytes = free;
      f.hours_left = free && rate ? double(*free) / double(rate) / 3600.0 : 0.0;
    });
  }

  // ---- opening and closing ----

  void begin() {
    const SampleIndex at = c_.session_start;
    if (!k_.seed.prev_manifest.empty()) {
      c_.manifest_path = k_.seed.prev_manifest;  // linked from a new manifest if it cannot be reused
      std::wstring err;
      const size_t folder = folder_of(k_.seed.prev_manifest);
      bool skip = false;
      {
        std::lock_guard<std::mutex> lk(k_.m);
        if (stale()) return;
        // An earlier generation hung reopening it (or hung in its folder): never again, or
        // every takeover would hang the same way. The new manifest still links it.
        skip = k_.skip_seed || k_.quarantined.count(folder) != 0;
        k_.seed_opening = !skip;
      }
      std::unique_ptr<Manifest> m;
      if (skip) {
        err = L"its folder hung";
      } else {
        IoScope io(*this, folder);
        m = Manifest::open_append(k_.fs, k_.seed.prev_manifest, &err);
      }
      if (gone()) {
        (void)m.release();
        return;
      }
      {
        std::lock_guard<std::mutex> lk(k_.m);
        if (!stale()) k_.seed_opening = false;
      }
      if (m) {
        man_ = std::move(m);
        man_folder_ = folder;
        man_->append(session_start_line(at, L""));
        if (!commit([&](DestStatus& st) { st.manifest_path = c_.manifest_path; })) return;
      } else {
        log(Severity::Warning, L"Cannot reopen manifest " + k_.seed.prev_manifest + L": " + err);
      }
    }
    c_.next_boundary = first_rotation_boundary(at, wall(at), k_.sp.rate, cfg_.rotation_minutes);
    c_.started = true;
    if (open_at(at, c_.n, 0, kNone, nullptr, kNone)) return;
    if (!gone()) enter_last_resort(at);
  }

  // This generation replaces one that hung in file I/O on folder stalled_. That one keeps
  // its encoder and manifest (never finalized); everything after the last confirmed sample
  // (this generation's cursor) is written again here.
  void take_over() {
    const std::wstring from = folder_path(stalled_);
    publish_cursor();
    if (c_.mode == Mode::LastResort) {
      // It hung in a retry (probe, open or manifest): retry on schedule without that folder.
      follow_capture();
      return;
    }
    if (c_.mode == Mode::GapClosed) {
      // No file was open (the last one closed normally). If it hung reopening for a
      // CmdResumeAt, that command is still queued and is handled again here.
      event(failover_line(cursor_, from, L"", "stall", L""));
      commit([&](DestStatus&) { k_.set_state_locked(DestState::GapClosed); });
      return;
    }
    const SampleIndex at = cursor_;
    const std::wstring old_path = c_.path;
    const LeadFn lead = [&](const std::wstring& to) {
      return std::vector<JsonLine>{failover_line(at, from, to, "stall", old_path)};
    };
    // No file opened yet (it hung in the first open): this is the first file, seg 0.
    const uint32_t seg = c_.folder == kNone ? 0 : c_.seg + 1;
    if (open_at(at, c_.n, seg, stalled_, lead, kNone)) {
      log(Severity::Warning, L"Failed over from " + from + L" to " + folder_path(c_.folder) + L" (stall)");
      return;
    }
    if (gone()) return;
    event(failover_line(at, from, L"", "stall", old_path));
    enter_last_resort(at);
  }

  // Opens file {n}{seg} at `at` in the first folder that passes the health check: `first`
  // if given, then the chain in order, never `skip` or a quarantined folder. The manifest
  // moves along if it lives elsewhere; `lead` gives the events that follow session_start
  // in a new manifest.
  bool open_at(SampleIndex at, uint32_t n, uint32_t seg, size_t skip, const LeadFn& lead, size_t first) {
    IoScope io(*this, kNone);
    const size_t count = folder_count();
    std::vector<size_t> order;
    if (first < count) order.push_back(first);
    for (size_t i = 0; i < count; ++i)
      if (i != first) order.push_back(i);
    for (size_t i : order) {
      if (i == skip) continue;
      std::wstring dir;
      {
        std::lock_guard<std::mutex> lk(k_.m);
        if (stale()) return false;
        if (k_.quarantined.count(i)) continue;
        // R14 per folder: each folder's I/O gets its own 2 s before it counts as a hang.
        if (k_.busy_folder != i) k_.io_since_ms = k_.clock.steady_ms();
        k_.busy_folder = i;
        dir = k_.st.config.folders[i];
      }
      const bool healthy = folder_healthy(k_.fs, dir, k_.low_water, cfg_.name);
      if (gone()) return false;
      if (healthy && (man_folder_ == i || move_manifest(i, at, lead)) && open_file(i, at, n, seg)) return true;
      if (gone()) return false;
      if (set_health(i, FolderHealth::Failed)) log(Severity::Warning, L"Folder unavailable: " + dir);
    }
    return false;
  }

  // A new manifest in folders[i]: session_start (naming the previous manifest), the lead
  // events, then whatever waited for a manifest. The old one is finished off the thread.
  bool move_manifest(size_t i, SampleIndex at, const LeadFn& lead) {
    const std::wstring dir = folder_path(i);
    const std::wstring stem = sanitize_component(k_.sp.session_name) + L"_" + sanitize_component(cfg_.name);
    std::wstring path, err;
    auto man = create_unique(k_.fs, dir, stem, L".jsonl", path,
                             [&](const std::wstring& p) { return Manifest::create(k_.fs, p, &err); });
    if (gone()) {
      (void)man.release();
      return false;
    }
    if (!man) {
      log(Severity::Error, L"Cannot create manifest " + path + L": " + err);
      return false;
    }
    if (man_) {
      Retired r;
      r.man = std::move(man_);
      retire(std::move(r));
    }
    const std::wstring prev = c_.manifest_path;
    man_ = std::move(man);
    man_folder_ = i;
    c_.manifest_path = path;
    man_->append(session_start_line(at, prev));
    if (lead)
      for (const auto& l : lead(dir)) man_->append(l);
    for (const auto& l : c_.backlog) man_->append(l);
    c_.backlog.clear();
    if (!commit([&](DestStatus& st) { st.manifest_path = path; })) return false;
    flush_manifest();  // the chain (prev_manifest, failover) must survive a crash right now
    return !gone();
  }

  // Opens file {n}{seg} in folders[folder] whose first sample is `at`.
  bool open_file(size_t folder, SampleIndex at, uint32_t n, uint32_t seg) {
    const std::wstring dir = folder_path(folder);
    const WallTime first = wall(at);
    const std::wstring stem =
        expand_name(cfg_.name_template, NameInputs{k_.sp.session_name, cfg_.name, first, n, seg});
    std::wstring path, err;
    SizeTap bytes = std::make_shared<std::atomic<uint64_t>>(0);
    TappingFs fs(k_.fs, bytes);
    std::unique_ptr<IEncoderFile> enc;
    if (cfg_.format == OutFormat::Wav) {
      WavParams wp{k_.sp.rate, uint16_t(out_ch_), cfg_.wav_depth, bext_for(first)};
      enc = create_unique(fs, dir, stem, L".wav", path,
                          [&](const std::wstring& p) { return open_wav(fs, p, wp, &err); });
    } else {
      const OpusParams op{k_.sp.rate, uint16_t(out_ch_), opus_bitrate(cfg_.opus_bitrate, out_ch_)};
      enc = create_unique(fs, dir, stem, L".opus", path,
                          [&](const std::wstring& p) { return open_opus(fs, p, op, &err); });
    }
    if (gone()) {
      (void)enc.release();  // leaked with this generation
      return false;
    }
    if (!enc) {
      log(Severity::Error, L"Cannot create " + path + L": " + err);
      return false;
    }
    enc_ = enc.release();
    c_.path = path;
    bytes_ = bytes;
    c_.mode = Mode::Writing;
    c_.file_start = at;
    c_.n = n;
    c_.seg = seg;
    c_.folder = folder;
    // Markers that fell before this file (a gap) stay in the manifest only.
    c_.markers.erase(std::remove_if(c_.markers.begin(), c_.markers.end(),
                                    [&](const auto& m) { return m.first < at; }),
                     c_.markers.end());
    event(line("file_open", at).str("path", path).num("n", n).num("seg", seg).num("folder", int64_t(folder)));
    if (!commit([&](DestStatus& st) {
          st.current_file = path;
          st.n = n;
          st.confirmed = at;
          k_.set_state_locked(DestState::Writing);
          for (size_t i = 0; i < st.folders.size(); ++i) {
            if (i == folder) {
              st.folders[i].health = FolderHealth::Active;
            } else if (st.folders[i].health == FolderHealth::Active) {
              st.folders[i].health = FolderHealth::Standby;
            }
          }
          k_.busy_folder = folder;
        }))
      return false;
    flush_manifest();  // crash repair finds the file only through its file_open
    if (gone()) return false;
    log(Severity::Info, L"Recording to " + path);
    return true;
  }

  // Best effort: a failure shows up again at the next periodic flush, which fails over.
  void flush_manifest() {
    if (!man_) return;
    IoScope io(*this, man_folder_);
    man_->flush();
  }

  BextInfo bext_for(WallTime first) const {
    const LocalParts p = local_parts(first);
    char date[16], time[16];
    std::snprintf(date, sizeof date, "%04d-%02d-%02d", p.year, p.month, p.day);
    std::snprintf(time, sizeof time, "%02d:%02d:%02d", p.hour, p.minute, p.second);
    BextInfo b;
    b.originator_reference = utf8_from_wide(k_.sp.session_id);
    b.date = date;
    b.time = time;
    int64_t since_midnight = (first.utc_ms + int64_t(first.local_offset_min) * 60000) % kDayMs;
    if (since_midnight < 0) since_midnight += kDayMs;
    b.time_reference = uint64_t(since_midnight) * k_.sp.rate / 1000;
    return b;
  }

  // The open file's markers in [file_start, end), relative to the file; every marker
  // before `end` leaves the pending list.
  std::vector<Marker> take_markers(SampleIndex end) {
    std::vector<Marker> ms;
    for (const auto& [s, name] : c_.markers)
      if (s >= c_.file_start && s < end) ms.push_back(Marker{uint64_t(s - c_.file_start), name});
    c_.markers.erase(std::remove_if(c_.markers.begin(), c_.markers.end(),
                                    [&](const auto& m) { return m.first < end; }),
                     c_.markers.end());
    return ms;
  }

  // Finalizes the open file so that it ends at `end` (<= cursor_). A WAV file is
  // truncated when `end` lies behind what was already written; Opus cannot be, so it
  // ends at the cursor. Returns the actual end sample.
  SampleIndex close_file(SampleIndex end) {
    std::optional<uint64_t> truncate_to;
    if (end < cursor_ && cfg_.format == OutFormat::Wav) {
      end = std::max(end, c_.file_start);
      truncate_to = uint64_t(end - c_.file_start);
    } else {
      end = cursor_;
    }
    const std::vector<Marker> ms = take_markers(end);
    bool ok;
    {
      IoScope io(*this, c_.folder);
      ok = enc_->finalize(ms, truncate_to);
    }
    if (gone()) return end;
    delete enc_;
    enc_ = nullptr;
    JsonLine l = line("file_close", end);
    l.str("path", c_.path).num("start", c_.file_start).num("end", end).num("bytes", int64_t(bytes_->load()));
    if (!ok) l.num("ok", 0);  // the header may be stale: a crash scan repairs it
    event(l);
    commit([&](DestStatus& st) {
      st.current_file.clear();
      if (ok) st.confirmed = end;
    });
    if (!ok) log(Severity::Error, L"Could not finalize " + c_.path);
    return end;
  }

  // Flushes the open file and publishes what it confirmed; a failure fails over.
  bool confirm_all() {
    bool ok;
    {
      IoScope io(*this, c_.folder);
      ok = enc_->flush();
    }
    if (gone()) return false;
    if (!ok) {
      failover("flush_error", L"Flush failed: " + c_.path);
      return false;
    }
    const SampleIndex confirmed = c_.file_start + SampleIndex(enc_->confirmed_frames());
    return commit([&](DestStatus& st) { st.confirmed = confirmed; });
  }

  void rotate(SampleIndex at) {
    // Confirm the tail first: if the drive died, fail over from what is really on disk
    // (the boundary then comes round again in the next folder).
    if (!confirm_all()) return;
    const bool timed = at == c_.next_boundary;
    while (!c_.splits.empty() && *c_.splits.begin() <= at) c_.splits.erase(c_.splits.begin());
    if (timed) c_.next_boundary = next_rotation_boundary(c_.next_boundary, k_.sp.rate, cfg_.rotation_minutes);
    event(line("rotation", at).str("reason", timed ? L"time" : L"split"));
    close_file(at);
    if (gone()) return;
    // Back to the highest-priority healthy folder (no flapping in between, §4.3).
    if (open_at(at, c_.n + 1, 0, kNone, nullptr, kNone)) return;
    if (!gone()) enter_last_resort(at);
  }

  // Keeps the session's rotation grid after the cursor jumped forward. The boundary is
  // never recomputed from wall time, which is rounded to milliseconds.
  void advance_grid() {
    while (c_.next_boundary <= cursor_)
      c_.next_boundary = next_rotation_boundary(c_.next_boundary, k_.sp.rate, cfg_.rotation_minutes);
    while (!c_.splits.empty() && *c_.splits.begin() <= cursor_) c_.splits.erase(c_.splits.begin());
  }

  // ---- failure handling ----

  // §4.3: a new generation; the open file ends at the last confirmed sample and is
  // finalized on a detached thread; the next healthy folder continues from there with
  // seg + 1. No healthy folder -> LastResort.
  void failover(std::string_view trigger, const std::wstring& what) {
    if (!enc_) return;
    log(Severity::Error, what);
    const size_t from = c_.folder;
    const std::wstring from_dir = folder_path(from);
    SampleIndex confirmed = c_.file_start;
    {
      std::unique_lock<std::shared_mutex> lv(k_.live);
      std::lock_guard<std::mutex> lk(k_.m);
      if (stale()) return;
      gen_ = ++k_.generation;
      confirmed = std::clamp(k_.st.confirmed, c_.file_start, cursor_);
      k_.set_state_locked(DestState::FailingOver);
      k_.st.needs_ack = true;
      k_.st.current_file.clear();
      if (from < k_.st.folders.size()) k_.st.folders[from].health = FolderHealth::Failed;
      k_.carry = c_;
    }
    Retired old;
    old.enc = enc_;
    enc_ = nullptr;
    old.man = std::move(man_);
    man_folder_ = kNone;
    const std::wstring old_path = c_.path;
    old.path = old_path;
    old.start = c_.file_start;
    old.bytes = bytes_;
    if (cfg_.format == OutFormat::Wav) old.truncate_to = uint64_t(confirmed - c_.file_start);
    old.markers = take_markers(confirmed);
    cursor_ = confirmed;
    publish_cursor();

    const LeadFn lead = [&](const std::wstring& to) {
      return std::vector<JsonLine>{failover_line(confirmed, from_dir, to, trigger, old_path)};
    };
    const bool opened = open_at(confirmed, c_.n, c_.seg + 1, from, lead, kNone);
    if (gone()) {
      (void)old.man.release();  // leaked with this generation, like old.enc
      return;
    }
    const std::wstring to_dir = opened ? folder_path(c_.folder) : std::wstring();
    old.after.push_back(failover_line(confirmed, from_dir, to_dir, trigger, old_path));
    retire(std::move(old));
    if (opened) {
      log(Severity::Warning, L"Failed over from " + from_dir + L" to " + to_dir + L" (" + widen(trigger) + L")");
      return;
    }
    event(failover_line(confirmed, from_dir, L"", trigger, old_path));
    enter_last_resort(confirmed);
  }

  // Hands r to a detached thread. Nobody waits for it, except stop() within its budget.
  void retire(Retired r) {
    {
      std::lock_guard<std::mutex> lk(k_.m);
      if (stale()) {
        (void)r.man.release();  // r.enc is a raw pointer: leaked as well
        return;
      }
      ++k_.finalizers;
    }
    try {
      std::thread([core = core_, r = std::move(r)]() mutable { core->finish(std::move(r)); }).detach();
    } catch (const std::system_error&) {
      std::lock_guard<std::mutex> lk(k_.m);
      --k_.finalizers;
    }
  }

  void check_space() {
    const std::wstring dir = folder_path(c_.folder);
    const std::optional<uint64_t> free = k_.fs.free_bytes(dir);
    if (gone()) return;
    note_space(free);
    if (!free) {
      failover("low_space", L"Folder unavailable: " + dir);
    } else if (*free < k_.low_water) {
      failover("low_space", L"Free space below the low-water mark in " + dir);
    }
  }

  // No folder can be written: keep the cursor on the live position and retry every 30 s.
  void enter_last_resort(SampleIndex from) {
    c_.mode = Mode::LastResort;
    c_.gap_from = from;
    c_.gap_in_manifest = man_ != nullptr;
    event(gap_line("gap_start", from, "last_resort"));
    next_retry_ms_ = now_ms() + kRetryMs;
    if (!commit([&](DestStatus& st) {
          k_.set_state_locked(DestState::LastResort);
          st.needs_ack = true;
          st.current_file.clear();
          ++st.gaps;
        }))
      return;
    log(Severity::Error, L"No folder can be written: recording stopped, retrying every 30 s");
    follow_capture();
  }

  void retry_last_resort() {
    follow_capture();
    if (gone()) return;
    const SampleIndex at = cursor_;
    advance_grid();
    const bool repeat_start = c_.gap_in_manifest;  // that manifest is not the one we resume in
    const SampleIndex from = c_.gap_from;
    const LeadFn lead = [&](const std::wstring&) {
      std::vector<JsonLine> v;
      if (repeat_start) v.push_back(gap_line("gap_start", from, "last_resort"));
      return v;
    };
    if (!open_at(at, c_.n + 1, 0, kNone, lead, kNone)) return;
    c_.gap_in_manifest = false;
    event(gap_line("gap_end", at, "last_resort"));
    log(Severity::Info, L"Recording again after last resort: " + c_.path);
  }

  void follow_capture() {
    SampleIndex wi = cursor_;
    live([&] { wi = k_.ring.write_index(); });
    if (wi > cursor_) {
      cursor_ = wi;
      publish_cursor();
    }
  }

  // §8.4: the writer fell out of the ring. The file ends at the cursor, the cursor jumps
  // to half a ring behind the capture position and a new file starts there.
  void on_overrun() {
    if (!confirm_all()) return;
    SampleIndex wi = cursor_, oldest = cursor_;
    if (!live([&] {
          wi = k_.ring.write_index();
          oldest = k_.ring.oldest_valid();
        }))
      return;
    const SampleIndex from = cursor_;
    // Never before the oldest valid frame: after a jump_to that floor lies past
    // write_index - capacity/2, and resyncing below it would overrun again.
    const SampleIndex to = std::max({from + 1, wi - SampleIndex(k_.ring_capacity / 2), oldest});
    event(gap_line("gap_start", from, "overrun"));
    if (!commit([](DestStatus& st) { ++st.gaps; })) return;
    wchar_t secs[32];
    std::swprintf(secs, 32, L"%.1f s", double(to - from) / double(k_.sp.rate));
    log(Severity::Error, L"Fell behind the capture ring (overrun): " + std::wstring(secs) + L" skipped");
    close_file(from);
    if (gone()) return;
    cursor_ = to;
    publish_cursor();
    advance_grid();
    event(gap_line("gap_end", to, "overrun"));
    if (open_at(to, c_.n + 1, 0, kNone, nullptr, c_.folder)) return;
    if (!gone()) enter_last_resort(to);
  }

  // ---- periodic work ----

  void housekeeping() {
    const uint64_t now = now_ms();
    if (gone()) return;
    if (now - last_flush_ms_ >= k_.sp.flush_ms) {
      last_flush_ms_ = now;
      flush();
      if (gone()) return;
    }
    if (enc_ && now - last_space_ms_ >= kSpaceCheckMs) {
      last_space_ms_ = now;
      check_space();
      if (gone()) return;
    }
    if (c_.mode == Mode::LastResort && (retry_now_ || now >= next_retry_ms_)) {
      retry_now_ = false;
      next_retry_ms_ = now + kRetryMs;
      retry_last_resort();
    }
  }

  void flush() {
    if (enc_ && !confirm_all()) return;
    if (man_) {
      bool ok;
      {
        IoScope io(*this, man_folder_);
        ok = man_->flush();
      }
      if (gone()) return;
      if (!ok && enc_ && man_folder_ == c_.folder) {
        failover("flush_error", L"Manifest flush failed: " + c_.manifest_path);
        return;
      }
      if (!ok && !manifest_warned_) log(Severity::Warning, L"Cannot write the manifest " + c_.manifest_path);
      manifest_warned_ = !ok;
    }
    if (!enc_) return;
    const std::optional<uint64_t> free = k_.fs.free_bytes(folder_path(c_.folder));
    if (gone()) return;
    note_space(free);
  }

  // False if the last flush failed and the writer failed over instead.
  bool end_session(SampleIndex stop_end, const std::string& reason) {
    if (enc_ && !confirm_all()) return gone();
    SampleIndex end = stop_end;
    if (enc_) end = close_file(stop_end);
    if (gone()) return true;
    event(line("session_end", end).str("reason", widen(reason)));
    if (man_) {
      IoScope io(*this, man_folder_);
      man_->flush();
    }
    if (!gone()) commit([&](DestStatus&) { k_.set_state_locked(DestState::Stopped); });
    return true;
  }

  // ---- commands ----

  // Only a close-for-gap waits: for the cursor to reach its sample.
  bool ready(const DestCommand& c) const {
    const auto* g = std::get_if<CmdCloseForGap>(&c);
    return !g || !enc_ || g->at <= cursor_;
  }

  void on(const CmdMarker& c) {
    event(line("marker", c.at).str("name", c.name));
    log(Severity::Info, L"Marker \"" + c.name + L"\" at " + hms(wall(c.at)));
    if (!enc_ || c.at >= c_.file_start) c_.markers.emplace_back(c.at, c.name);  // else manifest only
  }
  void on(const CmdSplit& c) {
    if (!enc_) return;
    const SampleIndex s = std::max(c.at, cursor_);  // never retroactive
    if (s > c_.file_start) c_.splits.insert(s);
  }
  void on(const CmdAppendFolder& c) {
    bool added = false;
    commit([&](DestStatus& st) {
      for (const auto& f : st.config.folders)
        if (folder_key(f) == folder_key(c.path)) return;
      st.config.folders.push_back(c.path);
      st.folders.push_back(FolderStatus{c.path, FolderHealth::Standby, std::nullopt, 0.0});
      added = true;
    });
    if (!added) return;
    log(Severity::Info, L"Folder added: " + c.path);
    if (c_.mode == Mode::LastResort) retry_now_ = true;  // tried at once (§4.3)
  }
  void on(const CmdGapStart& c) {
    event(gap_line("gap_start", c.at, c.reason));
    commit([](DestStatus& st) { ++st.gaps; });
  }
  void on(const CmdGapEnd& c) { event(gap_line("gap_end", c.at, c.reason)); }
  void on(const CmdCloseForGap& c) {
    if (!enc_) return;
    if (!confirm_all()) {
      redo_front_ = !gone();  // failed over to an earlier sample: close there once it gets to c.at
      return;
    }
    close_file(c.at);
    if (gone()) return;
    c_.splits.clear();
    c_.mode = Mode::GapClosed;
    commit([&](DestStatus&) { k_.set_state_locked(DestState::GapClosed); });
  }
  void on(const CmdResumeAt& c) {
    if (c_.mode == Mode::LastResort) return;  // the retry reopens at the live position
    if (enc_) close_file(cursor_);
    if (gone()) return;
    cursor_ = std::max(c.at, cursor_);
    publish_cursor();
    c_.splits.clear();
    advance_grid();
    if (open_at(cursor_, c_.n + 1, 0, kNone, nullptr, c_.folder)) return;
    if (!gone()) enter_last_resort(cursor_);
  }
  void on(const CmdClockPair& c) { event(JsonLine("clock_pair", c.at, c.wall)); }
  void on(const CmdAcknowledge&) {
    commit([](DestStatus& st) { st.needs_ack = false; });
  }

  // ---- state ----

  std::shared_ptr<Core> core_;  // keeps the shared state alive for an abandoned generation
  Core& k_;
  uint64_t gen_;
  const DestinationConfig cfg_;  // folders are read live from k_.st.config (CmdAppendFolder)
  const uint32_t out_ch_;
  const size_t stalled_;  // take_over only
  Carry c_;
  SampleIndex cursor_;
  bool dead_ = false;
  int io_depth_ = 0;  // nesting of IoScope

  std::unique_ptr<Manifest> man_;
  size_t man_folder_ = kNone;
  IEncoderFile* enc_ = nullptr;  // the open file, if any
  SizeTap bytes_;

  uint64_t last_flush_ms_ = 0, last_space_ms_ = 0, next_retry_ms_ = 0;
  bool retry_now_ = false, manifest_warned_ = false;
  bool redo_front_ = false;  // the command being handled stays queued (it must wait again)
  std::vector<float> in_, out_;
};

Destination::Destination(DestinationConfig cfg, const SessionParams& sp, SampleRing& ring, IFileSystem& fs,
                         IClock& clock, EventLog& log, DestResumeSeed seed)
    : core_(std::make_shared<Core>(sp, ring, fs, clock, log, std::move(seed), cfg.name)) {
  Core& k = *core_;
  if (cfg.channels.empty()) {
    for (uint32_t c = 0; c < ring.channels(); ++c) k.map.push_back(uint16_t(c));
  } else {
    k.map = cfg.channels;
  }
  k.rate_bytes = sl::data_rate(cfg, sp.rate, k.map.size());
  k.low_water = low_water_bytes(sp, k.rate_bytes);
  k.st.name = cfg.name;
  k.st.state = DestState::Idle;
  k.st.needs_ack = false;
  for (const auto& f : cfg.folders) k.st.folders.push_back(FolderStatus{f, FolderHealth::Standby, std::nullopt, 0.0});
  k.st.gaps = 0;
  k.st.cursor = k.st.confirmed = 0;
  k.st.n = 0;
  k.st.config = std::move(cfg);

  std::wstring problem;
  if (k.st.config.folders.empty()) problem = L"no folder configured";
  for (uint16_t c : k.map)
    if (problem.empty() && c >= ring.channels()) problem = L"channel " + std::to_wstring(c + 1) + L" is not an input channel";
  if (problem.empty() && k.st.config.format == OutFormat::Opus && k.map.size() > 2)
    problem = L"Opus records 1 or 2 channels per destination";
  if (!problem.empty()) disable(problem);
}

Destination::~Destination() {
  if (thread_.joinable()) stop(progress());
}

void Destination::start(SampleIndex at) {
  Core& k = *core_;
  Writer* w = nullptr;
  {
    std::lock_guard<std::mutex> lk(k.m);
    if (k.st.state != DestState::Idle || writer_) return;  // disabled, or already started
    k.st.confirmed = at;
    k.cursor.store(at, std::memory_order_release);
    Carry c;
    c.n = k.seed.first_n;
    c.session_start = at;
    k.carry = c;  // what a takeover starts from if the first writer hangs before any commit
    w = new Writer(core_, k.generation.load(), at, std::move(c), kNone);
    writer_ = w;
  }
  thread_ = std::thread([w] { w->run(); });
}

void Destination::post(DestCommand c) {
  std::lock_guard<std::mutex> lk(core_->m);
  core_->cmds.push_back(std::move(c));
}

void Destination::stop(SampleIndex end, std::string reason) {
  Core& k = *core_;
  if (!thread_.joinable()) {
    set_state(DestState::Stopped);
    return;
  }
  {
    std::lock_guard<std::mutex> lk(k.m);
    if (!k.stop_requested) {  // disable() may have asked first
      k.stop_requested = true;
      k.stop_end = end;
      k.stop_reason = std::move(reason);
    }
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kStopBudgetMs);
  if (WaitForSingleObject(thread_.native_handle(), kStopBudgetMs) == WAIT_OBJECT_0) {
    thread_.join();
    Writer* w = nullptr;
    {
      std::lock_guard<std::mutex> lk(k.m);
      std::swap(w, writer_);
    }
    delete w;
    // Files handed off by a failover finish on their own threads: give them what is left
    // of the budget, so a healthy one is complete when stop() returns.
    std::unique_lock<std::mutex> lk(k.m);
    k.fin_cv.wait_until(lk, deadline, [&] { return k.finalizers == 0; });
    return;
  }
  // Stuck in file I/O: leak the writer, its encoder and its thread.
  {
    std::unique_lock<std::shared_mutex> lv(k.live);
    std::lock_guard<std::mutex> lk(k.m);
    ++k.generation;
    writer_ = nullptr;
    k.set_state_locked(DestState::Stopped);
  }
  CancelSynchronousIo(thread_.native_handle());  // best effort
  thread_.detach();
  log(Severity::Error, L"Writer did not stop within 3 s and was abandoned");
}

void Destination::declare_stalled() {
  Core& k = *core_;
  Writer* w = nullptr;
  std::wstring dir;
  {
    std::unique_lock<std::shared_mutex> lv(k.live);
    std::lock_guard<std::mutex> lk(k.m);
    if (!writer_ || !thread_.joinable()) return;
    // A writer that writes (or fails over) can stall at any time. In the other states it
    // idles on purpose (GapClosed, LastResort) unless it has been inside one file I/O call
    // for kIdleStallMs (R14): a hang in the first open, a LastResort retry, a resume reopen
    // or a manifest flush is a stall; an idle writer, or a routine flush, is not and must
    // not cost a healthy folder.
    if (k.st.state == DestState::Stopped || k.st.state == DestState::Disabled) return;
    const bool writing = k.st.state == DestState::Writing || k.st.state == DestState::FailingOver;
    if (!writing && !(k.io_busy && k.clock.steady_ms() - k.io_since_ms >= kIdleStallMs)) return;
    // A generation that took over after a stall starts at least half a ring behind: it is
    // given until it reaches the capture position recorded at the takeover, or until the
    // grace deadline if it hangs too (R13).
    if (k.stall_grace_until >= 0) {
      const bool caught_up = k.cursor.load(std::memory_order_acquire) >= k.stall_grace_until;
      if (!caught_up && k.clock.steady_ms() < k.stall_grace_deadline_ms) return;
      k.stall_grace_until = -1;
    }
    const uint64_t gen = ++k.generation;  // from here on the stuck writer touches nothing shared
    const size_t stalled = k.busy_folder;
    if (stalled < k.st.folders.size()) {
      k.quarantined.insert(stalled);  // a probe there could hang the next writer too
      k.st.folders[stalled].health = FolderHealth::Failed;
      dir = k.st.config.folders[stalled];
    }
    if (k.carry.started && k.carry.mode == Mode::Writing) {
      k.set_state_locked(DestState::FailingOver);
      k.st.current_file.clear();
    }
    k.st.needs_ack = true;
    k.io_busy = false;  // the new generation starts outside file I/O
    if (k.seed_opening) k.skip_seed = true;
    k.seed_opening = false;
    k.stall_grace_until = k.ring.write_index();
    k.stall_grace_deadline_ms = k.clock.steady_ms() + stall_grace_ms(k.sp, k.ring_capacity);
    // The new generation continues from the last confirmed sample with the published carry.
    w = new Writer(core_, gen, k.st.confirmed, k.carry, stalled);
    writer_ = w;  // the old one is leaked: its thread may be stuck in the call that hung
  }
  CancelSynchronousIo(thread_.native_handle());  // best effort: fails a blocked call on a real device
  thread_.detach();
  thread_ = std::thread([w] { w->run(); });
  log(Severity::Error, L"Writer stalled" + (dir.empty() ? std::wstring() : L" on " + dir) +
                           L"; continuing from the last confirmed sample");
}

void Destination::abandon_for_test() {
  Core& k = *core_;
  {
    std::unique_lock<std::shared_mutex> lv(k.live);
    std::lock_guard<std::mutex> lk(k.m);
    ++k.generation;    // the writer is stale: it abandons its encoder and exits at its next check
    writer_ = nullptr;  // leaked with its encoder and manifest, like a stalled one
    k.set_state_locked(DestState::Stopped);
  }
  if (!thread_.joinable()) return;
  if (WaitForSingleObject(thread_.native_handle(), kStopBudgetMs) == WAIT_OBJECT_0) {
    thread_.join();
  } else {
    thread_.detach();  // stuck in file I/O
  }
}

void Destination::disable(std::wstring reason) {
  {
    std::lock_guard<std::mutex> lk(core_->m);
    if (core_->st.state == DestState::Disabled) return;
    core_->st.state = DestState::Disabled;
    if (writer_ && !core_->stop_requested) {  // running: close at the current position
      core_->stop_requested = true;
      core_->stop_end = progress();
      core_->stop_reason = "disabled";
    }
  }
  log(Severity::Error, L"Destination disabled: " + reason);
}

DestStatus Destination::status() const {
  std::lock_guard<std::mutex> lk(core_->m);
  DestStatus s = core_->st;
  s.cursor = progress();
  return s;
}

SampleIndex Destination::progress() const { return core_->cursor.load(std::memory_order_acquire); }

uint64_t Destination::data_rate() const { return core_->rate_bytes; }

void Destination::log(Severity s, std::wstring text) {
  core_->log.add(s, core_->clock.now(), core_->name, std::move(text));
}

void Destination::set_state(DestState s) {
  std::lock_guard<std::mutex> lk(core_->m);
  core_->set_state_locked(s);
}

uint64_t data_rate(const DestinationConfig& c, uint32_t rate, size_t channels) {
  if (c.format == OutFormat::Opus) return opus_bitrate(c.opus_bitrate, channels) / 8;
  return uint64_t(rate) * channels * bytes_per_sample(c.wav_depth);
}

bool is_stalled(SampleIndex write_index, SampleIndex progress, size_t capacity, uint32_t stall_percent) {
  const SampleIndex lag = write_index - progress;
  return lag > 0 && uint64_t(lag) * 100 > uint64_t(capacity) * stall_percent;
}

}  // namespace sl
