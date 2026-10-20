#pragma once
// Human-readable event log: appended to a text file and kept in memory for the UI.
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/types.h"
#include "io/file_system.h"

namespace spool {

enum class Severity { Info, Warning, Error };

struct LogEntry {
  WallTime wall;
  Severity sev;
  std::wstring dest, text;
};

class EventLog {
 public:
  // Creates the file, or appends to it if it already exists (a crash-resumed session
  // reuses its session id and so its log). If neither works the log keeps working in
  // memory only.
  EventLog(IFileSystem&, std::wstring path);
  // Thread-safe. Later lines go to `path` (created, or appended to), or stay in memory
  // only if it cannot be opened; the in-memory list is kept. Nothing happens if `path` is
  // the file already in use. The app switches between app.log and per-session logs.
  void set_file(std::wstring path);
  // Thread-safe. Appends "2026-10-16 14:30:00  WARN  [main] text\r\n" (local time;
  // "[dest] " omitted when dest is empty) and flushes. CR/LF in dest/text become spaces
  // in the file line only.
  void add(Severity, WallTime, std::wstring dest, std::wstring text);
  std::vector<LogEntry> recent(size_t max) const;  // newest first; the last 1000 are kept

 private:
  static constexpr size_t kMaxEntries = 1000;

  IFileSystem& fs_;
  std::mutex io_m_;               // serializes file writes and set_file; recent() never takes it
  std::wstring path_;             // of file_
  std::unique_ptr<IFile> file_;   // null: memory only
  mutable std::mutex m_;          // guards entries_ only
  std::deque<LogEntry> entries_;  // oldest first
};

}  // namespace spool
