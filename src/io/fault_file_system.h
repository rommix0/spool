#pragma once
// In-memory IFileSystem that injects faults, for testing failover without
// hardware. Thread-safe: writer threads may use it concurrently, and a
// HangWrite blocks only the calling thread.
#include <memory>

#include "io/file_system.h"

namespace spool {

struct FaultState;  // internal, defined in the .cpp

class FaultFileSystem : public IFileSystem {
 public:
  enum class Kind {
    FailOpen,    // create_new / open_existing / ensure_dir fail under the prefix; the
                 // volume looks vanished (exists false, free_bytes nullopt, list empty)
    FailWrite,   // write / write_at return false once after_bytes would be exceeded
    FailFlush,   // flush() returns false
    HangWrite,   // writes block (once past after_bytes) until release_hangs(), then return false
    SlowWrite    // each write call sleeps delay_ms (once past after_bytes)
  };
  // prefix: case-insensitive string prefix of the (full) path. For write rules,
  // after_bytes counts bytes successfully written (write + write_at) to each
  // file under the prefix since the rule was added, counted per file; a write
  // that would push that count past after_bytes is affected entirely.
  struct Rule {
    std::wstring prefix;
    Kind kind;
    uint64_t after_bytes = 0;
    uint32_t delay_ms = 0;
  };

  FaultFileSystem();
  ~FaultFileSystem() override;  // releases hung writers
  // Handles: size() and close() never block here, even while another thread is
  // hung in write(). Callers must not rely on close() being non-blocking: the
  // real IFile::close() may block behind an in-flight operation.

  void add_rule(Rule r);
  void clear_rules();                // does not wake hung writers; see release_hangs
  void release_hangs();              // wakes writes blocked right now; they return false
  void set_free_bytes(const std::wstring& prefix, uint64_t bytes);  // default 1 TB; longest prefix wins
  void set_volume(const std::wstring& prefix, const std::wstring& id);
  std::vector<uint8_t> contents(const std::wstring& path) const;    // empty if missing

  std::unique_ptr<IFile> create_new(const std::wstring& path, std::wstring* err) override;
  std::unique_ptr<IFile> open_existing(const std::wstring& path, std::wstring* err) override;
  bool exists(const std::wstring& path) override;
  bool ensure_dir(const std::wstring& dir, std::wstring* err) override;
  bool remove(const std::wstring& path) override;
  std::vector<std::wstring> list(const std::wstring& dir, const std::wstring& suffix) override;
  std::optional<uint64_t> free_bytes(const std::wstring& dir) override;
  std::wstring volume_id(const std::wstring& dir) override;

 private:
  std::shared_ptr<FaultState> st_;  // shared with open handles so they may outlive *this
};

}  // namespace spool
