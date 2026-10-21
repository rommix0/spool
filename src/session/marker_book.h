#pragma once
// The marker names of one recording (start() to stop(), across linked sessions) and the
// finished WAV files each marker went into. A rename reaches a file still being written
// at its finalize (the writer asks name()), and a finished file through a rewrite of its
// cue labels on the book's worker thread. The worker also appends the marker_rename
// records to the manifests of a session that has stopped. Thread-safe.
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <variant>
#include <vector>

#include "core/clock.h"
#include "io/file_system.h"
#include "session/event_log.h"
#include "session/manifest.h"

namespace spool {

struct PlacedMarker {
  uint32_t id;
  uint64_t frame_in_file;
  std::wstring written;  // the name in the file's cue labels
};

class MarkerBook : public std::enable_shared_from_this<MarkerBook> {
 public:
  // The file system, clock and log must outlive every job; shutdown() ends the logging.
  static std::shared_ptr<MarkerBook> create(IFileSystem&, IClock&, EventLog&);
  ~MarkerBook();

  void add(uint32_t id, std::wstring name);
  // The current name of marker id; `fallback` for an id the book doesn't know (0).
  std::wstring name(uint32_t id, const std::wstring& fallback) const;
  // A finished WAV file of destination `dest` holds these markers, with the names written.
  // One renamed meanwhile gets the file rewritten.
  void placed(std::wstring dest, std::wstring path, std::vector<PlacedMarker>);
  // False for an unknown id or the same name. Every finished file holding the marker is
  // rewritten (in the background, in order); a failure is logged as an error.
  bool rename(uint32_t id, std::wstring name);
  // Appends `line` to an existing manifest, on the worker.
  void append_to_manifest(std::wstring path, const JsonLine& line);
  // Waits up to ms for the worker to finish what is queued; true if nothing is left.
  bool wait_idle(uint64_t ms);
  // Nothing is logged afterwards (the event log may go away). A job in progress finishes;
  // queued ones are dropped.
  void shutdown();

 private:
  struct PlacedFile {
    std::wstring dest, path;
    std::vector<PlacedMarker> markers;
  };
  struct AppendJob {
    std::wstring path;
    JsonLine line;
  };
  using Job = std::variant<size_t /*PlacedFile index*/, AppendJob>;

  MarkerBook(IFileSystem&, IClock&, EventLog&);
  bool stale_locked(const PlacedFile&) const;  // a written name differs from the current one
  void queue_file_locked(size_t);
  void queue_locked(Job);
  void run();  // the worker thread
  void log_locked(Severity, const std::wstring& dest, std::wstring text);

  IFileSystem& fs_;
  IClock& clock_;
  EventLog& log_;
  mutable std::mutex m_;  // never held across file I/O
  std::condition_variable idle_cv_;
  std::map<uint32_t, std::wstring> names_;
  std::vector<PlacedFile> files_;
  std::set<size_t> queued_files_;
  std::deque<Job> jobs_;
  bool running_ = false, shut_ = false;
};

}  // namespace spool
