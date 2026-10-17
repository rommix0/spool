#pragma once
// Abstract file layer. Every writer (WAV, Opus, manifest) goes through
// IFileSystem so failover can be tested with FaultFileSystem.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sl {

class IFile {
 public:
  virtual ~IFile() = default;
  virtual bool write(const void* p, size_t n) = 0;                  // append at end
  virtual bool write_at(uint64_t off, const void* p, size_t n) = 0;
  virtual bool read_at(uint64_t off, void* p, size_t n) = 0;        // false on short read
  virtual bool flush() = 0;                                          // data on stable storage (FlushFileBuffers)
  // Never blocks, even while another thread is stuck inside write/flush on this
  // handle (a stalled device); returns the last known length, 0 once closed.
  virtual uint64_t size() const = 0;
  // May block behind an in-flight operation on the same handle: never call it on a
  // handle whose writer is stalled. Stalled writers are abandoned (leaked) by design.
  virtual void close() = 0;
};

class IFileSystem {
 public:
  virtual ~IFileSystem() = default;
  virtual std::unique_ptr<IFile> create_new(const std::wstring& path, std::wstring* err) = 0;     // fails if it exists
  virtual std::unique_ptr<IFile> open_existing(const std::wstring& path, std::wstring* err) = 0;  // read/write
  virtual bool exists(const std::wstring& path) = 0;                                              // file or directory
  virtual bool ensure_dir(const std::wstring& dir, std::wstring* err) = 0;                        // creates parents
  virtual bool remove(const std::wstring& path) = 0;                                              // a file
  // Full paths of the files (not directories) directly in dir whose names end
  // with suffix (case-insensitive; empty matches all), sorted by name.
  virtual std::vector<std::wstring> list(const std::wstring& dir, const std::wstring& suffix) = 0;
  virtual std::optional<uint64_t> free_bytes(const std::wstring& dir) = 0;
  virtual std::wstring volume_id(const std::wstring& dir) = 0;                                    // "" if unknown
};

// The process-wide Win32 implementation.
IFileSystem& real_fs();

}  // namespace sl
