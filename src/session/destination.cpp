#include "session/destination.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <set>
#include <utility>

#include "core/sample_convert.h"
#include "format/opus_writer.h"
#include "format/wav_writer.h"
#include "session/manifest.h"
#include "session/naming.h"

namespace sl {

namespace {

constexpr size_t kChunkFrames = 4096;
constexpr int kIdleMs = 5;
constexpr int kNameAttempts = 100;
constexpr int64_t kDayMs = 86400000;

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

}  // namespace

// One generation of the writer thread. Everything the thread touches while it may be
// stuck in file I/O lives here, so an abandoned generation (Task 10) can be leaked whole:
// the encoder is a raw pointer that is deleted only after a normal finalize.
class Destination::Writer {
 public:
  Writer(Destination& d, SampleIndex at)
      : d_(d),
        cfg_(d.st_.config),
        out_ch_(uint32_t(d.map_.size())),
        cursor_(at),
        start_(at),
        n_(d.seed_.first_n),
        in_(kChunkFrames * d.ring_.channels()),
        out_(kChunkFrames * d.map_.size()) {}
  ~Writer() { delete enc_; }  // null after a normal stop; never runs for an abandoned writer

  std::atomic<bool> abandoned{false};

  void run() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    last_flush_ms_ = d_.clock_.steady_ms();
    if (!begin()) mode_ = Mode::Failed;
    while (!abandoned) {
      bool stop;
      SampleIndex stop_end;
      std::string stop_reason;
      {
        std::lock_guard<std::mutex> lk(d_.m_);
        while (!d_.cmds_.empty()) {
          cmds_.push_back(std::move(d_.cmds_.front()));
          d_.cmds_.pop_front();
        }
        stop = d_.stop_requested_;
        stop_end = d_.stop_end_;
        stop_reason = d_.stop_reason_;
      }
      while (!cmds_.empty() && ready(cmds_.front())) {
        std::visit([this](const auto& c) { on(c); }, cmds_.front());
        cmds_.pop_front();
        if (abandoned) return;
      }
      if (stop && (!enc_ || cursor_ >= stop_end)) {
        end_session(stop_end, stop_reason);
        return;
      }
      maybe_flush();
      if (abandoned) return;
      if (!enc_) {
        idle();
        continue;
      }

      // Write up to the next cut: rotation boundary, split point, a pending close-for-gap
      // (the only command that waits for the cursor) or the stop sample.
      const SampleIndex cut = std::min(next_boundary_, splits_.empty() ? kNoRotation : *splits_.begin());
      if (cut <= cursor_) {
        rotate(cut);
        continue;
      }
      SampleIndex limit = stop ? stop_end : kNoRotation;
      if (!cmds_.empty()) limit = std::min(limit, std::get<CmdCloseForGap>(cmds_.front()).at);
      const SampleIndex want = std::min({SampleIndex(kChunkFrames), cut - cursor_, limit - cursor_});
      if (want <= 0) {
        idle();
        continue;
      }
      const auto r = d_.ring_.read(cursor_, in_.data(), size_t(want));
      if (r.frames == 0) {
        // Overrun recovery (resync, gap) is Task 10; here the writer just must not spin.
        if (r.overrun && !overrun_logged_) {
          d_.log(Severity::Error, L"Fell behind the capture ring (overrun)");
          overrun_logged_ = true;
        }
        idle();
        continue;
      }
      overrun_logged_ = false;
      extract_channels(in_.data(), d_.ring_.channels(), d_.map_.data(), out_ch_, out_.data(), r.frames);
      const bool ok = enc_->write(out_.data(), r.frames);
      if (abandoned) return;
      if (!ok) {
        fail(L"Write failed: " + path_);
        continue;
      }
      cursor_ += SampleIndex(r.frames);
      d_.cursor_.store(cursor_, std::memory_order_release);
    }
  }

 private:
  enum class Mode { Writing, GapClosed, Failed };

  WallTime wall(SampleIndex s) const { return wall_for(d_.sp_, s); }
  JsonLine line(std::string_view ev, SampleIndex s) const { return JsonLine(ev, s, wall(s)); }
  void event(const JsonLine& l) {
    if (man_) man_->append(l);
  }
  std::wstring folder_path(size_t i) const {
    std::lock_guard<std::mutex> lk(d_.m_);
    return i < d_.st_.config.folders.size() ? d_.st_.config.folders[i] : std::wstring();
  }
  static void idle() { std::this_thread::sleep_for(std::chrono::milliseconds(kIdleMs)); }

  bool begin() {
    const std::wstring dir = folder_path(0);
    std::wstring err;
    if (!d_.fs_.ensure_dir(dir, &err)) {
      fail(L"Cannot create folder " + dir + L": " + err);
      return false;
    }
    std::wstring path;
    if (!d_.seed_.prev_manifest.empty()) {
      path = d_.seed_.prev_manifest;
      man_ = Manifest::open_append(d_.fs_, path, &err);
    }
    if (!man_) {
      const std::wstring stem = sanitize_component(d_.sp_.session_name) + L"_" + sanitize_component(cfg_.name);
      man_ = create_unique(d_.fs_, dir, stem, L".jsonl", path,
                           [&](const std::wstring& p) { return Manifest::create(d_.fs_, p, &err); });
    }
    if (!man_) {
      fail(L"Cannot create manifest " + path + L": " + err);
      return false;
    }
    {
      std::lock_guard<std::mutex> lk(d_.m_);
      d_.st_.manifest_path = path;
    }
    std::wstring map;
    for (uint16_t c : d_.map_) map += (map.empty() ? L"" : L",") + std::to_wstring(c);
    JsonLine l = line("session_start", start_);
    l.str("session", d_.sp_.session_name)
        .str("session_id", d_.sp_.session_id)
        .str("dest", cfg_.name)
        .str("format", cfg_.format == OutFormat::Wav ? L"wav" : L"opus")
        .num("rate", d_.sp_.rate)
        .num("channels", out_ch_)
        .str("channel_map", map);
    if (!d_.sp_.prev_session.empty()) l.str("prev_session", d_.sp_.prev_session);
    event(l);
    next_boundary_ = first_rotation_boundary(start_, wall(start_), d_.sp_.rate, cfg_.rotation_minutes);
    return open_file(0, start_, d_.seed_.first_n, 0);
  }

  // The reusable step: open file {n}{seg} in folders[folder] whose first sample is `at`.
  // Task 10 failover calls it with the next folder, the confirmed sample and seg + 1.
  bool open_file(size_t folder, SampleIndex at, uint32_t n, uint32_t seg) {
    const std::wstring dir = folder_path(folder);
    std::wstring err;
    if (dir.empty() || !d_.fs_.ensure_dir(dir, &err)) {
      fail(L"Cannot create folder " + dir + L": " + err);
      return false;
    }
    const WallTime first = wall(at);
    const std::wstring stem =
        expand_name(cfg_.name_template, NameInputs{d_.sp_.session_name, cfg_.name, first, n, seg});
    std::wstring path;
    std::unique_ptr<IEncoderFile> enc;
    if (cfg_.format == OutFormat::Wav) {
      WavParams wp{d_.sp_.rate, uint16_t(out_ch_), cfg_.wav_depth, bext_for(first)};
      enc = create_unique(d_.fs_, dir, stem, L".wav", path,
                          [&](const std::wstring& p) { return open_wav(d_.fs_, p, wp, &err); });
    } else {
      const OpusParams op{d_.sp_.rate, uint16_t(out_ch_), opus_bitrate(cfg_.opus_bitrate, out_ch_)};
      enc = create_unique(d_.fs_, dir, stem, L".opus", path,
                          [&](const std::wstring& p) { return open_opus(d_.fs_, p, op, &err); });
    }
    if (abandoned) {
      (void)enc.release();  // leaked with this generation
      return false;
    }
    if (!enc) {
      fail(L"Cannot create " + path + L": " + err);
      return false;
    }
    enc_ = enc.release();
    path_ = path;
    file_start_ = at;
    n_ = n;
    seg_ = seg;
    folder_ = folder;
    // Markers that fell before this file (a gap) stay in the manifest only.
    markers_.erase(std::remove_if(markers_.begin(), markers_.end(),
                                  [&](const auto& m) { return m.first < at; }),
                   markers_.end());
    event(line("file_open", at).str("path", path).num("n", n).num("seg", seg).num("folder", int64_t(folder)));
    {
      std::lock_guard<std::mutex> lk(d_.m_);
      d_.st_.current_file = path;
      d_.st_.n = n;
      d_.st_.confirmed = at;
      if (d_.st_.state != DestState::Disabled) d_.st_.state = DestState::Writing;
      for (size_t i = 0; i < d_.st_.folders.size(); ++i)
        if (d_.st_.folders[i].health != FolderHealth::Failed)
          d_.st_.folders[i].health = i == folder ? FolderHealth::Active : FolderHealth::Standby;
    }
    d_.log(Severity::Info, L"Recording to " + path);
    return true;
  }

  BextInfo bext_for(WallTime first) const {
    const LocalParts p = local_parts(first);
    char date[16], time[16];
    std::snprintf(date, sizeof date, "%04d-%02d-%02d", p.year, p.month, p.day);
    std::snprintf(time, sizeof time, "%02d:%02d:%02d", p.hour, p.minute, p.second);
    BextInfo b;
    b.originator_reference = utf8_from_wide(d_.sp_.session_id);
    b.date = date;
    b.time = time;
    int64_t since_midnight = (first.utc_ms + int64_t(first.local_offset_min) * 60000) % kDayMs;
    if (since_midnight < 0) since_midnight += kDayMs;
    b.time_reference = uint64_t(since_midnight) * d_.sp_.rate / 1000;
    return b;
  }

  // Finalizes the open file so that it ends at `end` (<= cursor_). A WAV file is
  // truncated when `end` lies behind what was already written; Opus cannot be, so it
  // ends at the cursor. Returns the actual end sample.
  SampleIndex close_file(SampleIndex end) {
    std::optional<uint64_t> truncate_to;
    if (end < cursor_ && cfg_.format == OutFormat::Wav) {
      end = std::max(end, file_start_);
      truncate_to = uint64_t(end - file_start_);
    } else {
      end = cursor_;
    }
    std::vector<Marker> ms;
    for (const auto& [s, name] : markers_)
      if (s >= file_start_ && s < end) ms.push_back(Marker{uint64_t(s - file_start_), name});
    markers_.erase(std::remove_if(markers_.begin(), markers_.end(),
                                  [&](const auto& m) { return m.first < end; }),
                   markers_.end());
    const bool ok = enc_->finalize(ms, truncate_to);
    if (abandoned) return end;  // the encoder is leaked with this generation
    delete enc_;
    enc_ = nullptr;
    event(line("file_close", end)
              .str("path", path_)
              .num("start", file_start_)
              .num("end", end)
              .num("bytes", int64_t(file_bytes(path_))));
    {
      std::lock_guard<std::mutex> lk(d_.m_);
      d_.st_.current_file.clear();
      if (ok) d_.st_.confirmed = end;
    }
    if (!ok) d_.log(Severity::Error, L"Could not finalize " + path_);
    return end;
  }

  uint64_t file_bytes(const std::wstring& path) {
    std::wstring err;
    auto f = d_.fs_.open_existing(path, &err);
    if (!f) return 0;
    const uint64_t n = f->size();
    f->close();
    return n;
  }

  void rotate(SampleIndex at) {
    const bool timed = at == next_boundary_;
    while (!splits_.empty() && *splits_.begin() <= at) splits_.erase(splits_.begin());
    if (timed) next_boundary_ = next_rotation_boundary(next_boundary_, d_.sp_.rate, cfg_.rotation_minutes);
    event(line("rotation", at).str("reason", timed ? L"time" : L"split"));
    close_file(at);
    if (abandoned) return;
    open_file(0, at, n_ + 1, 0);  // Task 10: return to the highest-priority healthy folder
  }

  void maybe_flush() {
    const uint64_t now = d_.clock_.steady_ms();
    if (now - last_flush_ms_ < d_.sp_.flush_ms) return;
    last_flush_ms_ = now;
    SampleIndex confirmed = -1;
    if (enc_) {
      const bool ok = enc_->flush();
      if (abandoned) return;
      if (!ok) {
        fail(L"Flush failed: " + path_);
        return;
      }
      confirmed = file_start_ + SampleIndex(enc_->confirmed_frames());
    }
    if (man_ && !man_->flush()) {
      fail(L"Manifest flush failed");
      return;
    }
    const std::optional<uint64_t> free = d_.fs_.free_bytes(folder_path(folder_));
    const uint64_t rate = d_.data_rate();
    std::lock_guard<std::mutex> lk(d_.m_);
    if (confirmed >= 0) d_.st_.confirmed = confirmed;
    if (folder_ < d_.st_.folders.size()) {
      auto& f = d_.st_.folders[folder_];
      f.free_bytes = free;
      f.hours_left = free && rate ? double(*free) / double(rate) / 3600.0 : 0.0;
    }
  }

  void end_session(SampleIndex stop_end, const std::string& reason) {
    SampleIndex end = stop_end;
    if (enc_) end = close_file(stop_end);
    if (abandoned) return;
    event(line("session_end", end).str("reason", std::wstring(reason.begin(), reason.end())));
    if (man_) man_->flush();
    d_.set_state(DestState::Stopped);
  }

  // Task 9: an I/O failure disables the destination. Task 10 replaces this with failover.
  void fail(const std::wstring& what) {
    d_.log(Severity::Error, what);
    event(line("warning", cursor_).str("text", what));
    if (enc_) {
      SampleIndex confirmed;
      {
        std::lock_guard<std::mutex> lk(d_.m_);
        confirmed = d_.st_.confirmed;
      }
      close_file(confirmed);  // best effort
    }
    mode_ = Mode::Failed;
    d_.set_state(DestState::Disabled);
  }

  // Only a close-for-gap waits: for the cursor to reach its sample.
  bool ready(const DestCommand& c) const {
    const auto* g = std::get_if<CmdCloseForGap>(&c);
    return !g || !enc_ || g->at <= cursor_;
  }

  void on(const CmdMarker& c) {
    event(line("marker", c.at).str("name", c.name));
    d_.log(Severity::Info, L"Marker \"" + c.name + L"\" at " + hms(wall(c.at)));
    if (!enc_ || c.at >= file_start_) markers_.emplace_back(c.at, c.name);  // else manifest only
  }
  void on(const CmdSplit& c) {
    if (!enc_) return;
    const SampleIndex s = std::max(c.at, cursor_);  // never retroactive
    if (s > file_start_) splits_.insert(s);
  }
  void on(const CmdAppendFolder& c) {
    {
      std::lock_guard<std::mutex> lk(d_.m_);
      d_.st_.config.folders.push_back(c.path);
      d_.st_.folders.push_back(FolderStatus{c.path, FolderHealth::Standby, std::nullopt, 0.0});
    }
    d_.log(Severity::Info, L"Folder added: " + c.path);
  }
  void on(const CmdGapStart& c) {
    event(line("gap_start", c.at).str("reason", std::wstring(c.reason.begin(), c.reason.end())));
    std::lock_guard<std::mutex> lk(d_.m_);
    ++d_.st_.gaps;
  }
  void on(const CmdGapEnd& c) {
    event(line("gap_end", c.at).str("reason", std::wstring(c.reason.begin(), c.reason.end())));
  }
  void on(const CmdCloseForGap& c) {
    if (!enc_) return;
    close_file(c.at);
    if (abandoned) return;
    splits_.clear();
    mode_ = Mode::GapClosed;
    d_.set_state(DestState::GapClosed);
  }
  void on(const CmdResumeAt& c) {
    if (mode_ == Mode::Failed) return;
    if (enc_) close_file(cursor_);
    if (abandoned) return;
    cursor_ = std::max(c.at, cursor_);
    d_.cursor_.store(cursor_, std::memory_order_release);
    splits_.clear();
    next_boundary_ = first_rotation_boundary(cursor_, wall(cursor_), d_.sp_.rate, cfg_.rotation_minutes);
    if (open_file(0, cursor_, n_ + 1, 0)) mode_ = Mode::Writing;
  }
  void on(const CmdClockPair& c) { event(JsonLine("clock_pair", c.at, c.wall)); }
  void on(const CmdAcknowledge&) {
    std::lock_guard<std::mutex> lk(d_.m_);
    d_.st_.needs_ack = false;
  }

  Destination& d_;
  const DestinationConfig cfg_;  // folders are read live from d_.st_.config (CmdAppendFolder)
  const uint32_t out_ch_;
  Mode mode_ = Mode::Writing;
  std::deque<DestCommand> cmds_;  // drained from d_.cmds_; the front may wait (close-for-gap)

  SampleIndex cursor_, start_;
  SampleIndex next_boundary_ = kNoRotation;
  std::set<SampleIndex> splits_;
  std::vector<std::pair<SampleIndex, std::wstring>> markers_;  // not yet in a finalized file

  std::unique_ptr<Manifest> man_;
  IEncoderFile* enc_ = nullptr;  // the open file, if any
  std::wstring path_;
  SampleIndex file_start_ = 0;
  uint32_t n_, seg_ = 0;
  size_t folder_ = 0;

  uint64_t last_flush_ms_ = 0;
  bool overrun_logged_ = false;
  std::vector<float> in_, out_;
};

Destination::Destination(DestinationConfig cfg, const SessionParams& sp, SampleRing& ring, IFileSystem& fs,
                         IClock& clock, EventLog& log, DestResumeSeed seed)
    : sp_(sp), ring_(ring), fs_(fs), clock_(clock), log_(log), seed_(std::move(seed)), name_(cfg.name) {
  if (cfg.channels.empty()) {
    for (uint32_t c = 0; c < ring.channels(); ++c) map_.push_back(uint16_t(c));
  } else {
    map_ = cfg.channels;
  }
  st_.name = cfg.name;
  st_.state = DestState::Idle;
  st_.needs_ack = false;
  for (const auto& f : cfg.folders) st_.folders.push_back(FolderStatus{f, FolderHealth::Standby, std::nullopt, 0.0});
  st_.gaps = 0;
  st_.cursor = st_.confirmed = 0;
  st_.n = 0;
  st_.config = std::move(cfg);

  std::wstring problem;
  if (st_.config.folders.empty()) problem = L"no folder configured";
  for (uint16_t c : map_)
    if (problem.empty() && c >= ring.channels()) problem = L"channel " + std::to_wstring(c + 1) + L" is not an input channel";
  if (problem.empty() && st_.config.format == OutFormat::Opus && map_.size() > 2)
    problem = L"Opus records 1 or 2 channels per destination";
  if (!problem.empty()) disable(problem);
}

Destination::~Destination() {
  if (thread_.joinable()) stop(progress());
}

void Destination::start(SampleIndex at) {
  {
    std::lock_guard<std::mutex> lk(m_);
    if (st_.state != DestState::Idle || writer_) return;  // disabled, or already started
  }
  cursor_.store(at, std::memory_order_release);
  writer_ = new Writer(*this, at);
  thread_ = std::thread([w = writer_] { w->run(); });
}

void Destination::post(DestCommand c) {
  std::lock_guard<std::mutex> lk(m_);
  cmds_.push_back(std::move(c));
}

void Destination::stop(SampleIndex end) {
  if (!thread_.joinable()) {
    set_state(DestState::Stopped);
    return;
  }
  {
    std::lock_guard<std::mutex> lk(m_);
    if (!stop_requested_) {  // disable() may have asked first
      stop_requested_ = true;
      stop_end_ = end;
      stop_reason_ = "stop";
    }
  }
  if (WaitForSingleObject(thread_.native_handle(), 3000) == WAIT_OBJECT_0) {
    thread_.join();
    delete writer_;
  } else {
    // Stuck in file I/O: leak the writer, its encoder and its thread (Task 10 completes this).
    writer_->abandoned = true;
    thread_.detach();
    log(Severity::Error, L"Writer did not stop within 3 s and was abandoned");
    set_state(DestState::Stopped);
  }
  writer_ = nullptr;
}

void Destination::declare_stalled() {
  // Task 10: fail over away from a stalled writer.
}

void Destination::disable(std::wstring reason) {
  {
    std::lock_guard<std::mutex> lk(m_);
    if (st_.state == DestState::Disabled) return;
    st_.state = DestState::Disabled;
    if (writer_ && !stop_requested_) {  // running: close at the current position
      stop_requested_ = true;
      stop_end_ = cursor_.load(std::memory_order_acquire);
      stop_reason_ = "disabled";
    }
  }
  log(Severity::Error, L"Destination disabled: " + reason);
}

DestStatus Destination::status() const {
  std::lock_guard<std::mutex> lk(m_);
  DestStatus s = st_;
  s.cursor = progress();
  return s;
}

uint64_t Destination::data_rate() const {
  if (st_.config.format == OutFormat::Opus) return opus_bitrate(st_.config.opus_bitrate, map_.size()) / 8;
  return uint64_t(sp_.rate) * map_.size() * bytes_per_sample(st_.config.wav_depth);
}

void Destination::log(Severity s, std::wstring text) { log_.add(s, clock_.now(), name_, std::move(text)); }

void Destination::set_state(DestState s) {
  std::lock_guard<std::mutex> lk(m_);
  if (st_.state != DestState::Disabled) st_.state = s;
}

bool is_stalled(SampleIndex write_index, SampleIndex progress, size_t capacity, uint32_t stall_percent) {
  const SampleIndex lag = write_index - progress;
  return lag > 0 && uint64_t(lag) * 100 > uint64_t(capacity) * stall_percent;
}

}  // namespace sl
