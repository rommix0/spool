#pragma once
// Human-readable event log: appended to a text file and kept in memory for the UI.
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/types.h"
#include "io/file_system.h"

namespace sl {

enum class Severity { Info, Warning, Error };

struct LogEntry {
  WallTime wall;
  Severity sev;
  std::wstring dest, text;
};

class EventLog {
 public:
  // If the file cannot be opened the log keeps working in memory only.
  EventLog(IFileSystem&, std::wstring path);
  // Thread-safe. Appends "2026-10-16 14:30:00  WARN  [main] text\r\n" (local time;
  // "[dest] " omitted when dest is empty) and flushes.
  void add(Severity, WallTime, std::wstring dest, std::wstring text);
  std::vector<LogEntry> recent(size_t max) const;  // newest first; the last 1000 are kept

 private:
  static constexpr size_t kMaxEntries = 1000;

  mutable std::mutex m_;
  std::unique_ptr<IFile> file_;   // null: memory only
  std::deque<LogEntry> entries_;  // oldest first
};

}  // namespace sl
