// Win32 implementation of IFileSystem.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <mutex>

#include "io/file_system.h"

namespace sl {
namespace {

// Paths this long or longer get the \\?\ prefix. Below MAX_PATH (260) for files
// but CreateDirectoryW's limit is 248, so use a threshold under both.
constexpr size_t kLongPathThreshold = MAX_PATH - 20;

std::wstring win_error(const wchar_t* what, DWORD code) {
  wchar_t buf[512];
  const DWORD len = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                                   code, 0, buf, 512, nullptr);
  std::wstring msg(buf, len);
  while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n' || msg.back() == L' ')) msg.pop_back();
  return std::wstring(what) + L" failed (error " + std::to_wstring(code) + L")" +
         (msg.empty() ? L"" : L": " + msg);
}

void set_err(std::wstring* err, const wchar_t* what, DWORD code) {
  if (err) *err = win_error(what, code);
}

bool has_prefix(const std::wstring& s, const wchar_t* p) { return s.rfind(p, 0) == 0; }

// Returns a path safe to hand to the Win32 API: long paths become absolute
// "\\?\" paths (which also disables '/' handling, so GetFullPathNameW normalizes).
std::wstring api_path(const std::wstring& p) {
  if (p.size() < kLongPathThreshold || has_prefix(p, L"\\\\?\\") || has_prefix(p, L"\\\\.\\")) return p;
  const DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
  if (n == 0) return p;
  std::wstring full(n, L'\0');
  const DWORD m = GetFullPathNameW(p.c_str(), n, full.data(), nullptr);
  if (m == 0 || m >= n) return p;
  full.resize(m);
  if (has_prefix(full, L"\\\\")) return L"\\\\?\\UNC\\" + full.substr(2);
  return L"\\\\?\\" + full;
}

bool is_sep(wchar_t c) { return c == L'\\' || c == L'/'; }

// Writes/reads in chunks so n may exceed what a single DWORD call can take.
constexpr size_t kMaxIo = size_t(1) << 30;

class WinFile : public IFile {
 public:
  WinFile(HANDLE h, uint64_t initial_size) : h_(h), size_(initial_size) {}
  ~WinFile() override { close(); }

  bool write(const void* p, size_t n) override {
    std::lock_guard<std::mutex> lk(mu_);
    if (h_ == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER zero{}, end{};
    if (!SetFilePointerEx(h_, zero, &end, FILE_END)) return false;
    return write_all(p, n) ? publish_size(static_cast<uint64_t>(end.QuadPart) + n) : resync_size();
  }

  bool write_at(uint64_t off, const void* p, size_t n) override {
    std::lock_guard<std::mutex> lk(mu_);
    if (h_ == INVALID_HANDLE_VALUE || !seek(off)) return false;
    return write_all(p, n) ? publish_size(off + n) : resync_size();
  }

  bool read_at(uint64_t off, void* p, size_t n) override {
    std::lock_guard<std::mutex> lk(mu_);
    if (h_ == INVALID_HANDLE_VALUE || !seek(off)) return false;
    auto* dst = static_cast<char*>(p);
    while (n > 0) {
      const DWORD chunk = static_cast<DWORD>(std::min(n, kMaxIo));
      DWORD got = 0;
      if (!ReadFile(h_, dst, chunk, &got, nullptr) || got != chunk) return false;  // short read = failure
      dst += chunk;
      n -= chunk;
    }
    return true;
  }

  bool flush() override {
    std::lock_guard<std::mutex> lk(mu_);
    return h_ != INVALID_HANDLE_VALUE && FlushFileBuffers(h_) != 0;
  }

  // Lock-free so a stalled write cannot block size() callers (e.g. a watchdog).
  uint64_t size() const override { return size_.load(std::memory_order_acquire); }

  void close() override {
    std::lock_guard<std::mutex> lk(mu_);
    if (h_ != INVALID_HANDLE_VALUE) {
      CloseHandle(h_);
      h_ = INVALID_HANDLE_VALUE;
    }
    size_.store(0, std::memory_order_release);
  }

 private:
  bool seek(uint64_t off) {
    if (off > static_cast<uint64_t>(INT64_MAX)) return false;
    LARGE_INTEGER li;
    li.QuadPart = static_cast<LONGLONG>(off);
    return SetFilePointerEx(h_, li, nullptr, FILE_BEGIN) != 0;
  }

  bool write_all(const void* p, size_t n) {
    const auto* src = static_cast<const char*>(p);
    while (n > 0) {
      const DWORD chunk = static_cast<DWORD>(std::min(n, kMaxIo));
      DWORD put = 0;
      if (!WriteFile(h_, src, chunk, &put, nullptr) || put != chunk) return false;
      src += chunk;
      n -= chunk;
    }
    return true;
  }

  // Records a successful write ending at `end`; always returns true.
  bool publish_size(uint64_t end) {
    if (end > size_.load(std::memory_order_relaxed)) size_.store(end, std::memory_order_release);
    return true;
  }

  // After a failed write the file length is uncertain: re-read it. Returns false.
  bool resync_size() {
    LARGE_INTEGER sz{};
    if (GetFileSizeEx(h_, &sz)) size_.store(static_cast<uint64_t>(sz.QuadPart), std::memory_order_release);
    return false;
  }

  // Held across blocking I/O (seek + transfer must not interleave), so a stalled
  // device blocks every other call that takes it. size() deliberately does not.
  std::mutex mu_;
  HANDLE h_;
  std::atomic<uint64_t> size_;
};

class WinFileSystem : public IFileSystem {
 public:
  std::unique_ptr<IFile> create_new(const std::wstring& path, std::wstring* err) override {
    return open(path, CREATE_NEW, err);
  }

  std::unique_ptr<IFile> open_existing(const std::wstring& path, std::wstring* err) override {
    return open(path, OPEN_EXISTING, err);
  }

  bool exists(const std::wstring& path) override {
    return GetFileAttributesW(api_path(path).c_str()) != INVALID_FILE_ATTRIBUTES;
  }

  bool ensure_dir(const std::wstring& dir, std::wstring* err) override {
    if (dir.empty()) {
      if (err) *err = L"empty directory path";
      return false;
    }
    std::wstring d = dir;
    // Drop trailing separators, but keep a drive root like "C:\".
    while (d.size() > 1 && is_sep(d.back()) && !(d.size() == 3 && d[1] == L':')) d.pop_back();
    return make_dirs(d, err);
  }

  bool remove(const std::wstring& path) override { return DeleteFileW(api_path(path).c_str()) != 0; }

  std::vector<std::wstring> list(const std::wstring& dir, const std::wstring& suffix) override {
    std::vector<std::wstring> out;
    std::wstring base = dir;
    while (base.size() > 1 && is_sep(base.back())) base.pop_back();
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(api_path(base + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      const std::wstring name = fd.cFileName;
      if (!ends_with_nocase(name, suffix)) continue;
      out.push_back(base + (is_sep(base.back()) ? L"" : L"\\") + name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) {
      return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    return out;
  }

  std::optional<uint64_t> free_bytes(const std::wstring& dir) override {
    // A not-yet-created folder lives on the volume of its nearest existing ancestor.
    std::wstring p = dir;
    for (;;) {
      ULARGE_INTEGER avail{};
      if (GetDiskFreeSpaceExW(api_path(p).c_str(), &avail, nullptr, nullptr))
        return static_cast<uint64_t>(avail.QuadPart);
      while (!p.empty() && is_sep(p.back())) p.pop_back();
      const size_t cut = p.find_last_of(L"\\/");
      if (cut == std::wstring::npos || cut < 2) return std::nullopt;  // never fall to "\\" or "\\server"
      p.resize(cut);
      if (p.back() == L':') p += L'\\';  // "C:" means the current dir; use the root
    }
  }

  std::wstring volume_id(const std::wstring& dir) override {
    const std::wstring p = api_path(dir);
    std::wstring root(p.size() + MAX_PATH, L'\0');
    if (!GetVolumePathNameW(p.c_str(), root.data(), static_cast<DWORD>(root.size()))) return {};
    root.resize(wcslen(root.c_str()));
    wchar_t name[MAX_PATH];
    if (GetVolumeNameForVolumeMountPointW(root.c_str(), name, MAX_PATH)) return name;  // \\?\Volume{guid}\.
    return root;  // e.g. a network share: the mount root still identifies it
  }

 private:
  static bool ends_with_nocase(const std::wstring& s, const std::wstring& suffix) {
    if (suffix.empty()) return true;
    if (s.size() < suffix.size()) return false;
    return CompareStringOrdinal(s.c_str() + (s.size() - suffix.size()), static_cast<int>(suffix.size()),
                                suffix.c_str(), static_cast<int>(suffix.size()), TRUE) == CSTR_EQUAL;
  }

  static std::unique_ptr<IFile> open(const std::wstring& path, DWORD disposition, std::wstring* err) {
    HANDLE h = CreateFileW(api_path(path).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           disposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
      set_err(err, L"CreateFileW", GetLastError());
      return nullptr;
    }
    LARGE_INTEGER sz{};
    const uint64_t initial = GetFileSizeEx(h, &sz) ? static_cast<uint64_t>(sz.QuadPart) : 0;
    return std::make_unique<WinFile>(h, initial);
  }

  // Creates d and any missing parents. (SHCreateDirectoryExW cannot take \\?\ paths.)
  static bool make_dirs(const std::wstring& d, std::wstring* err) {
    const DWORD attrs = GetFileAttributesW(api_path(d).c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
      if (attrs & FILE_ATTRIBUTE_DIRECTORY) return true;
      if (err) *err = L"a file with that name already exists: " + d;
      return false;
    }
    const size_t pos = d.find_last_of(L"\\/");
    if (pos != std::wstring::npos && pos > 0) {
      const std::wstring parent = d.substr(0, pos);
      if (!make_dirs(parent, err)) return false;
    }
    if (CreateDirectoryW(api_path(d).c_str(), nullptr)) return true;
    const DWORD code = GetLastError();
    if (code == ERROR_ALREADY_EXISTS) {
      const DWORD a = GetFileAttributesW(api_path(d).c_str());
      if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return true;  // lost a race
    }
    set_err(err, L"CreateDirectoryW", code);
    return false;
  }
};

}  // namespace

IFileSystem& real_fs() {
  static WinFileSystem* const fs = new WinFileSystem;  // never destroyed: see file_system.h
  return *fs;
}

}  // namespace sl
