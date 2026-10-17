#include "session/event_log.h"

#include <cstdio>

#include "core/clock.h"
#include "session/manifest.h"  // utf8_from_wide

namespace sl {

EventLog::EventLog(IFileSystem& fs, std::wstring path) {
  std::wstring err;
  file_ = fs.create_new(path, &err);
  if (!file_) file_ = fs.open_existing(path, &err);  // keep appending to an earlier log
}

void EventLog::add(Severity sev, WallTime wall, std::wstring dest, std::wstring text) {
  const char* sev_text = sev == Severity::Error ? "ERROR" : sev == Severity::Warning ? "WARN" : "INFO";
  const LocalParts t = local_parts(wall);
  char head[64];
  std::snprintf(head, sizeof head, "%04d-%02d-%02d %02d:%02d:%02d  %s  ", t.year, t.month, t.day,
                t.hour, t.minute, t.second, sev_text);
  std::string line = head;
  const auto one_line = [](std::string s) {
    for (char& c : s)
      if (c == '\r' || c == '\n') c = ' ';
    return s;
  };
  if (!dest.empty()) line += "[" + one_line(utf8_from_wide(dest)) + "] ";
  line += one_line(utf8_from_wide(text));
  line += "\r\n";

  {
    std::lock_guard<std::mutex> lk(m_);
    entries_.push_back({wall, sev, std::move(dest), std::move(text)});
    if (entries_.size() > kMaxEntries) entries_.pop_front();
  }
  std::lock_guard<std::mutex> io(io_m_);  // file I/O may stall; recent() never waits on it
  if (file_ && file_->write(line.data(), line.size())) file_->flush();
}

std::vector<LogEntry> EventLog::recent(size_t max) const {
  std::lock_guard<std::mutex> lk(m_);
  std::vector<LogEntry> out;
  for (auto it = entries_.rbegin(); it != entries_.rend() && out.size() < max; ++it) out.push_back(*it);
  return out;
}

}  // namespace sl
