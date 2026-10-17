#pragma once
// Per-destination JSON Lines manifest: one record per file, marker, gap and
// failover, with sample indices and wall-clock times. Crash recovery reads it
// back with read_manifest.
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/types.h"
#include "io/file_system.h"

namespace sl {

// UTF-16 -> UTF-8 (lone surrogates become U+FFFD).
std::string utf8_from_wide(std::wstring_view w);

// Builds one flat JSON object. Base fields, in order: ev, sample, utc, local_offset.
class JsonLine {
 public:
  JsonLine(std::string_view ev, SampleIndex sample, WallTime wall);
  JsonLine& str(std::string_view key, std::wstring_view v);  // UTF-8, escaped
  JsonLine& num(std::string_view key, int64_t v);
  std::string finish() const;  // one line, no trailing newline

 private:
  std::string s_;
};

class Manifest {
 public:
  // create fails (nullptr, *err set) if the file exists; open_append fails if it does not.
  static std::unique_ptr<Manifest> create(IFileSystem&, const std::wstring& path, std::wstring* err);
  // Appends to an existing manifest (after crash recovery). Nothing is truncated; if the
  // last line is torn (no trailing newline) a newline precedes the next write so each
  // record stays on its own line.
  static std::unique_ptr<Manifest> open_append(IFileSystem&, const std::wstring& path, std::wstring* err);

  bool append(const JsonLine&);  // buffered in memory; thread-safe
  // Writes the buffered lines, then IFile::flush. On failure the buffer is kept and
  // false is returned, so the caller can decide on failover.
  bool flush();

 private:
  explicit Manifest(std::unique_ptr<IFile> f) : file_(std::move(f)) {}

  std::mutex m_;
  std::unique_ptr<IFile> file_;
  std::string buf_;
  bool lead_newline_ = false;  // next write must begin with '\n' (torn last line)
};

using ManifestRecord = std::map<std::string, std::string>;  // values unescaped; numbers as text
// Flat objects only. Lines that do not parse (a torn line from a crash) are skipped.
// Missing file -> empty.
std::vector<ManifestRecord> read_manifest(IFileSystem&, const std::wstring& path);

}  // namespace sl
