#include "session/marker_book.h"

#include <chrono>
#include <system_error>
#include <thread>

#include "format/wav_markers.h"

namespace spool {

std::shared_ptr<MarkerBook> MarkerBook::create(IFileSystem& fs, IClock& clock, EventLog& log) {
  return std::shared_ptr<MarkerBook>(new MarkerBook(fs, clock, log));
}

MarkerBook::MarkerBook(IFileSystem& fs, IClock& clock, EventLog& log) : fs_(fs), clock_(clock), log_(log) {}

MarkerBook::~MarkerBook() = default;  // the worker holds a reference: it has ended by now

void MarkerBook::add(uint32_t id, std::wstring name) {
  std::lock_guard<std::mutex> lk(m_);
  names_[id] = std::move(name);
}

std::wstring MarkerBook::name(uint32_t id, const std::wstring& fallback) const {
  std::lock_guard<std::mutex> lk(m_);
  const auto it = names_.find(id);
  return it == names_.end() ? fallback : it->second;
}

void MarkerBook::placed(std::wstring dest, std::wstring path, std::vector<PlacedMarker> markers) {
  std::lock_guard<std::mutex> lk(m_);
  files_.push_back(PlacedFile{std::move(dest), std::move(path), std::move(markers)});
  if (stale_locked(files_.back())) queue_file_locked(files_.size() - 1);
}

bool MarkerBook::rename(uint32_t id, std::wstring name) {
  std::lock_guard<std::mutex> lk(m_);
  const auto it = names_.find(id);
  if (it == names_.end() || it->second == name) return false;
  it->second = std::move(name);
  for (size_t i = 0; i < files_.size(); ++i)
    if (stale_locked(files_[i])) queue_file_locked(i);
  return true;
}

void MarkerBook::append_to_manifest(std::wstring path, const JsonLine& line) {
  std::lock_guard<std::mutex> lk(m_);
  queue_locked(AppendJob{std::move(path), line});
}

bool MarkerBook::wait_idle(uint64_t ms) {
  std::unique_lock<std::mutex> lk(m_);
  return idle_cv_.wait_for(lk, std::chrono::milliseconds(ms), [&] { return !running_ && jobs_.empty(); });
}

void MarkerBook::shutdown() {
  std::lock_guard<std::mutex> lk(m_);
  shut_ = true;
  jobs_.clear();
  queued_files_.clear();
  if (!running_) idle_cv_.notify_all();
}

bool MarkerBook::stale_locked(const PlacedFile& f) const {
  for (const PlacedMarker& p : f.markers) {
    const auto it = names_.find(p.id);
    if (it != names_.end() && it->second != p.written) return true;
  }
  return false;
}

void MarkerBook::queue_file_locked(size_t i) {
  if (queued_files_.insert(i).second) queue_locked(i);
}

void MarkerBook::queue_locked(Job j) {
  if (shut_) return;
  jobs_.push_back(std::move(j));
  if (running_) return;
  try {
    std::thread([self = shared_from_this()] { self->run(); }).detach();
    running_ = true;
  } catch (const std::system_error&) {
    jobs_.clear();
    queued_files_.clear();
    log_locked(Severity::Error, L"", L"Couldn't start renaming the marker: no thread");
  }
}

void MarkerBook::log_locked(Severity sev, const std::wstring& dest, std::wstring text) {
  if (!shut_) log_.add(sev, clock_.now(), dest, std::move(text));
}

void MarkerBook::run() {
  std::unique_lock<std::mutex> lk(m_);
  while (!jobs_.empty()) {
    Job job = std::move(jobs_.front());
    jobs_.pop_front();
    std::wstring err;
    if (const auto* i = std::get_if<size_t>(&job)) {
      queued_files_.erase(*i);
      // The names as of now; a rename during the write queues the file again.
      const PlacedFile f = files_[*i];
      std::vector<Marker> want;
      for (const PlacedMarker& p : f.markers) {
        const auto it = names_.find(p.id);
        want.push_back(Marker{p.frame_in_file, it == names_.end() ? p.written : it->second});
      }
      lk.unlock();
      const bool ok = rewrite_wav_markers(fs_, f.path, want, &err);
      lk.lock();
      if (!ok) {
        log_locked(Severity::Error, f.dest, L"Couldn't rename the marker in " + f.path + L": " + err);
        continue;
      }
      for (size_t k = 0; k < want.size(); ++k) files_[*i].markers[k].written = want[k].name;
      if (stale_locked(files_[*i])) queue_file_locked(*i);
    } else {
      const AppendJob& a = std::get<AppendJob>(job);
      const std::wstring path = a.path;
      const JsonLine line = a.line;
      lk.unlock();
      std::unique_ptr<Manifest> man = Manifest::open_append(fs_, path, &err);
      const bool ok = man && man->append(line) && man->flush();
      man.reset();
      lk.lock();
      if (!ok)
        log_locked(Severity::Error, L"", L"Couldn't record the marker rename in " + path + (err.empty() ? L"" : L": " + err));
    }
  }
  running_ = false;
  idle_cv_.notify_all();
}

}  // namespace spool
