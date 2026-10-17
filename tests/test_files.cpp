#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "io/fault_file_system.h"
#include "io/file_system.h"
#include "test.h"

using sl::FaultFileSystem;
using sl::IFile;
using sl::IFileSystem;
using Kind = sl::FaultFileSystem::Kind;

namespace {
using Clock = std::chrono::steady_clock;

long long ms_since(Clock::time_point t0) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
}

// Polls until pred() holds or timeout_ms elapses.
template <class Pred>
bool wait_for(Pred pred, int timeout_ms) {
  const auto t0 = Clock::now();
  while (!pred()) {
    if (ms_since(t0) > timeout_ms) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return true;
}

// Ensures dir and creates a file inside it; returns the open file.
std::unique_ptr<IFile> make_file(FaultFileSystem& fs, const std::wstring& dir,
                                 const std::wstring& name) {
  std::wstring err;
  CHECK(fs.ensure_dir(dir, &err));
  return fs.create_new(dir + L"\\" + name, &err);
}

// ---- Win32 helpers ----
std::wstring ext(const std::wstring& p) { return L"\\\\?\\" + p; }

void remove_tree(const std::wstring& dir) {
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(ext(dir + L"\\*").c_str(), &fd);
  if (h != INVALID_HANDLE_VALUE) {
    do {
      const std::wstring name = fd.cFileName;
      if (name == L"." || name == L"..") continue;
      const std::wstring full = dir + L"\\" + name;
      if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) remove_tree(full);
      else DeleteFileW(ext(full).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
  }
  RemoveDirectoryW(ext(dir).c_str());
}

// Unique temp directory, removed (recursively) on scope exit.
struct TempDir {
  std::wstring path;
  TempDir() {
    static std::atomic<int> counter{0};
    wchar_t tmp[MAX_PATH + 1];
    const DWORD n = GetTempPathW(MAX_PATH, tmp);
    std::wstring base(tmp, n);
    if (!base.empty() && base.back() == L'\\') base.pop_back();
    path = base + L"\\sl_test_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
           std::to_wstring(GetTickCount64()) + L"_" + std::to_wstring(counter++);
    CreateDirectoryW(path.c_str(), nullptr);
  }
  ~TempDir() { remove_tree(path); }
};
}  // namespace

// ---------------- FaultFileSystem ----------------

TEST(fault_fail_write_after_bytes) {
  FaultFileSystem fs;
  fs.add_rule({L"D:\\", Kind::FailWrite, 100});
  auto f = make_file(fs, L"D:\\rec", L"a.bin");
  CHECK(f != nullptr);
  if (!f) return;
  std::vector<uint8_t> buf(60, 0xAB);
  CHECK(f->write(buf.data(), 60));
  CHECK(!f->write(buf.data(), 60));  // would cross 100: fails entirely
  CHECK_EQ(f->size(), uint64_t(60));
  CHECK_EQ(fs.contents(L"D:\\rec\\a.bin").size(), size_t(60));
  CHECK(f->write(buf.data(), 40));  // exactly reaching the limit is fine
  CHECK(!f->write(buf.data(), 1));
  CHECK(!f->write_at(0, buf.data(), 1));  // write_at counts too
  CHECK_EQ(f->size(), uint64_t(100));

  // Other volumes are unaffected; a fresh file starts its own count.
  auto g = make_file(fs, L"E:\\rec", L"a.bin");
  CHECK(g != nullptr);
  if (g) CHECK(g->write(buf.data(), 60));
  auto h = make_file(fs, L"D:\\rec", L"b.bin");
  CHECK(h != nullptr);
  if (h) CHECK(h->write(buf.data(), 60));
}

TEST(fault_fail_write_after_zero_fails_at_once) {
  FaultFileSystem fs;
  auto f = make_file(fs, L"D:\\rec", L"a.bin");
  CHECK(f != nullptr);
  if (!f) return;
  CHECK(f->write("abc", 3));
  fs.add_rule({L"D:\\", Kind::FailWrite, 0});
  CHECK(!f->write("x", 1));  // counts from the moment the rule is added
  fs.clear_rules();
  CHECK(f->write("x", 1));
  CHECK_EQ(f->size(), uint64_t(4));
}

TEST(fault_hang_until_release) {
  FaultFileSystem fs;
  auto hung = make_file(fs, L"H:\\rec", L"a.bin");
  auto ok = make_file(fs, L"D:\\rec", L"a.bin");
  CHECK(hung != nullptr);
  CHECK(ok != nullptr);
  if (!hung || !ok) return;
  fs.add_rule({L"H:\\", Kind::HangWrite});

  std::atomic<bool> done{false};
  std::atomic<bool> result{true};
  std::thread t([&] {
    result = hung->write("data", 4);
    done = true;
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(!done.load());  // blocked
  CHECK(ok->write("fine", 4));  // other files proceed meanwhile
  CHECK(!done.load());

  const auto t0 = Clock::now();
  fs.release_hangs();
  CHECK(wait_for([&] { return done.load(); }, 1000));
  CHECK(ms_since(t0) < 1000);
  CHECK(!result.load());
  t.join();
  CHECK_EQ(fs.contents(L"H:\\rec\\a.bin").size(), size_t(0));  // hung write wrote nothing
  CHECK_EQ(fs.contents(L"D:\\rec\\a.bin").size(), size_t(4));
}

TEST(fault_hang_releases_several_writers) {
  FaultFileSystem fs;
  auto a = make_file(fs, L"H:\\rec", L"a.bin");
  auto b = make_file(fs, L"H:\\rec", L"b.bin");
  CHECK(a != nullptr);
  CHECK(b != nullptr);
  if (!a || !b) return;
  fs.add_rule({L"H:\\", Kind::HangWrite});
  std::atomic<int> done{0};
  std::thread ta([&] { a->write("x", 1); ++done; });
  std::thread tb([&] { b->write("x", 1); ++done; });
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  CHECK_EQ(done.load(), 0);
  fs.release_hangs();
  CHECK(wait_for([&] { return done.load() == 2; }, 1000));
  ta.join();
  tb.join();
}

TEST(fault_slow_write_delays_each_call) {
  FaultFileSystem fs;
  auto f = make_file(fs, L"S:\\rec", L"a.bin");
  CHECK(f != nullptr);
  if (!f) return;
  fs.add_rule({L"S:\\", Kind::SlowWrite, 0, 60});
  const auto t0 = Clock::now();
  CHECK(f->write("a", 1));
  CHECK(f->write("b", 1));
  CHECK(ms_since(t0) >= 110);  // two calls at 60 ms (timer slack allowed)
  CHECK_EQ(f->size(), uint64_t(2));
}

TEST(fault_fail_open_and_ensure_dir) {
  FaultFileSystem fs;
  std::wstring err;
  CHECK(fs.ensure_dir(L"D:\\rec", &err));
  auto f = fs.create_new(L"D:\\rec\\a.wav", &err);
  CHECK(f != nullptr);
  if (f) f->close();

  fs.add_rule({L"D:\\", Kind::FailOpen});
  err.clear();
  CHECK(fs.create_new(L"D:\\rec\\b.wav", &err) == nullptr);
  CHECK(!err.empty());
  CHECK(fs.open_existing(L"D:\\rec\\a.wav", &err) == nullptr);
  CHECK(!fs.ensure_dir(L"D:\\rec\\sub", &err));
  CHECK(!fs.ensure_dir(L"D:\\", &err));
  CHECK(fs.ensure_dir(L"E:\\rec", &err));  // unaffected drive
  CHECK(fs.create_new(L"E:\\rec\\a.wav", &err) != nullptr);

  fs.clear_rules();
  CHECK(fs.open_existing(L"D:\\rec\\a.wav", &err) != nullptr);
}

TEST(fault_fail_flush) {
  FaultFileSystem fs;
  auto f = make_file(fs, L"D:\\rec", L"a.bin");
  CHECK(f != nullptr);
  if (!f) return;
  CHECK(f->flush());
  fs.add_rule({L"D:\\", Kind::FailFlush});
  CHECK(!f->flush());
  CHECK(f->write("ok", 2));  // only flush is affected
  fs.clear_rules();
  CHECK(f->flush());
}

TEST(fault_free_bytes_and_volume) {
  FaultFileSystem fs;
  CHECK_EQ(*fs.free_bytes(L"C:\\x"), uint64_t(1) << 40);  // default 1 TB
  fs.set_free_bytes(L"E:\\", 5);
  CHECK_EQ(*fs.free_bytes(L"E:\\x"), uint64_t(5));
  CHECK_EQ(*fs.free_bytes(L"D:\\x"), uint64_t(1) << 40);
  fs.set_free_bytes(L"E:\\big", 99);  // longest prefix wins
  CHECK_EQ(*fs.free_bytes(L"E:\\big"), uint64_t(99));
  CHECK_EQ(*fs.free_bytes(L"e:\\Big\\deep"), uint64_t(99));  // case-insensitive
  CHECK_EQ(*fs.free_bytes(L"E:\\other"), uint64_t(5));

  CHECK(fs.volume_id(L"D:\\a").empty());
  fs.set_volume(L"D:\\a", L"v1");
  fs.set_volume(L"D:\\b", L"v1");
  CHECK(!fs.volume_id(L"D:\\a").empty());
  CHECK(fs.volume_id(L"D:\\a") == fs.volume_id(L"D:\\b"));
  CHECK(fs.volume_id(L"D:\\a") == L"v1");
  CHECK(fs.volume_id(L"D:\\c").empty());

  fs.add_rule({L"E:\\", Kind::FailOpen});  // vanished drive
  CHECK(!fs.free_bytes(L"E:\\x").has_value());
}

TEST(fault_directories_and_listing) {
  FaultFileSystem fs;
  std::wstring err;
  // Parent never ensured: create_new fails like Win32.
  CHECK(fs.create_new(L"D:\\rec\\a.wav", &err) == nullptr);
  CHECK(!err.empty());
  CHECK(!fs.exists(L"D:\\rec"));
  CHECK(fs.ensure_dir(L"D:\\rec\\deep\\er", &err));  // creates parents
  CHECK(fs.exists(L"D:\\rec"));
  CHECK(fs.exists(L"d:\\REC\\Deep"));
  CHECK(fs.ensure_dir(L"D:\\rec\\", &err));  // idempotent, trailing slash ok

  for (const wchar_t* n : {L"D:\\rec\\b.WAV", L"D:\\rec\\a.wav", L"D:\\rec\\c.opus",
                           L"D:\\rec\\deep\\x.wav"}) {
    auto f = fs.create_new(n, &err);
    CHECK(f != nullptr);
  }
  auto wavs = fs.list(L"D:\\rec", L".wav");
  CHECK_EQ(wavs.size(), size_t(2));  // direct children only, suffix case-insensitive
  if (wavs.size() == 2) {
    CHECK(wavs[0] == L"D:\\rec\\a.wav");
    CHECK(wavs[1] == L"D:\\rec\\b.WAV");
  }
  CHECK_EQ(fs.list(L"D:\\rec", L"").size(), size_t(3));
  CHECK_EQ(fs.list(L"D:\\nope", L"").size(), size_t(0));

  // Case-insensitive identity and "never overwrite".
  CHECK(fs.create_new(L"d:\\REC\\A.WAV", &err) == nullptr);
  CHECK(fs.create_new(L"D:/rec/a.wav", &err) == nullptr);
  CHECK(fs.exists(L"D:\\rec\\a.wav"));
  CHECK(fs.remove(L"D:\\rec\\A.wav"));
  CHECK(!fs.exists(L"D:\\rec\\a.wav"));
  CHECK(!fs.remove(L"D:\\rec\\a.wav"));
  CHECK_EQ(fs.list(L"D:\\rec", L".wav").size(), size_t(1));
}

TEST(fault_read_write_at_and_reopen) {
  FaultFileSystem fs;
  auto f = make_file(fs, L"D:\\rec", L"a.bin");
  CHECK(f != nullptr);
  if (!f) return;
  CHECK(f->write("0123456789", 10));
  CHECK(f->write_at(2, "XY", 2));
  CHECK(f->write("AB", 2));  // still appends at the end
  CHECK_EQ(f->size(), uint64_t(12));
  char buf[12] = {};
  CHECK(f->read_at(0, buf, 12));
  CHECK(std::memcmp(buf, "01XY456789AB", 12) == 0);
  CHECK(!f->read_at(8, buf, 8));  // past the end
  CHECK(f->write_at(14, "Z", 1));  // extends, zero-filling the gap
  CHECK_EQ(f->size(), uint64_t(15));
  const std::vector<uint8_t> all = fs.contents(L"D:\\rec\\a.bin");
  CHECK_EQ(all.size(), size_t(15));
  if (all.size() == 15) CHECK_EQ(all[12], uint8_t(0));  // the gap

  f->close();
  CHECK(!f->write("x", 1));  // closed handle
  std::wstring err;
  auto g = fs.open_existing(L"D:\\rec\\a.bin", &err);
  CHECK(g != nullptr);
  if (g) {
    CHECK_EQ(g->size(), uint64_t(15));
    CHECK(g->write("!", 1));
    CHECK_EQ(g->size(), uint64_t(16));
  }
  CHECK(fs.open_existing(L"D:\\rec\\missing.bin", &err) == nullptr);
  CHECK(fs.contents(L"D:\\rec\\missing.bin").empty());
}

TEST(fault_concurrent_writers) {
  FaultFileSystem fs;
  auto a = make_file(fs, L"D:\\rec", L"a.bin");
  auto b = make_file(fs, L"D:\\rec", L"b.bin");
  CHECK(a != nullptr);
  CHECK(b != nullptr);
  if (!a || !b) return;
  auto run = [](IFile* f) {
    for (int i = 0; i < 2000; ++i) f->write("xy", 2);
  };
  std::thread ta(run, a.get());
  std::thread tb(run, b.get());
  for (int i = 0; i < 200; ++i) fs.contents(L"D:\\rec\\a.bin");
  ta.join();
  tb.join();
  CHECK_EQ(a->size(), uint64_t(4000));
  CHECK_EQ(b->size(), uint64_t(4000));
}

// ---------------- Win32 ----------------

TEST(win_create_new_refuses_existing) {
  TempDir td;
  sl::IFileSystem& fs = sl::real_fs();
  std::wstring err;
  const std::wstring p = td.path + L"\\a.bin";
  auto f = fs.create_new(p, &err);
  CHECK(f != nullptr);
  CHECK(fs.exists(p));
  if (f) f->close();
  err.clear();
  auto g = fs.create_new(p, &err);
  CHECK(g == nullptr);
  CHECK(!err.empty());
  // Missing parent directory fails too.
  CHECK(fs.create_new(td.path + L"\\nodir\\b.bin", &err) == nullptr);
  CHECK(fs.open_existing(td.path + L"\\none.bin", &err) == nullptr);
  CHECK(fs.remove(p));
  CHECK(!fs.exists(p));
  CHECK(!fs.remove(p));
}

TEST(win_write_at_flush_size) {
  TempDir td;
  sl::IFileSystem& fs = sl::real_fs();
  std::wstring err;
  const std::wstring p = td.path + L"\\a.bin";
  auto f = fs.create_new(p, &err);
  CHECK(f != nullptr);
  if (!f) return;
  CHECK(f->write("0123456789", 10));
  CHECK(f->write_at(2, "XY", 2));
  CHECK(f->flush());
  CHECK_EQ(f->size(), uint64_t(10));
  char buf[10] = {};
  CHECK(f->read_at(0, buf, 10));
  CHECK(std::memcmp(buf, "01XY456789", 10) == 0);
  CHECK(f->write("AB", 2));  // append after a write_at
  CHECK_EQ(f->size(), uint64_t(12));
  CHECK(!f->read_at(8, buf, 8));  // short read reports false
  f->close();
  CHECK(!f->write("x", 1));

  auto g = fs.open_existing(p, &err);
  CHECK(g != nullptr);
  if (g) {
    CHECK_EQ(g->size(), uint64_t(12));
    char b2[2] = {};
    CHECK(g->read_at(10, b2, 2));
    CHECK(std::memcmp(b2, "AB", 2) == 0);
  }
}

TEST(win_free_bytes_and_volume) {
  TempDir td;
  sl::IFileSystem& fs = sl::real_fs();
  auto fb = fs.free_bytes(td.path);
  CHECK(fb.has_value());
  if (fb) CHECK(*fb > 0);
  const std::wstring v = fs.volume_id(td.path);
  CHECK(!v.empty());
  CHECK(v == fs.volume_id(td.path + L"\\sub"));
}

TEST(win_ensure_dir_and_list) {
  TempDir td;
  sl::IFileSystem& fs = sl::real_fs();
  std::wstring err;
  const std::wstring d = td.path + L"\\a\\b\\c";
  CHECK(fs.ensure_dir(d, &err));
  CHECK(fs.exists(d));
  CHECK(fs.ensure_dir(d + L"\\", &err));  // idempotent
  for (const wchar_t* n : {L"\\a\\b\\c\\y.WAV", L"\\a\\b\\c\\x.wav", L"\\a\\b\\c\\z.opus"}) {
    auto f = fs.create_new(td.path + n, &err);
    CHECK(f != nullptr);
  }
  auto wavs = fs.list(d, L".wav");
  CHECK_EQ(wavs.size(), size_t(2));
  if (wavs.size() == 2) {
    CHECK(wavs[0] == d + L"\\x.wav");
    CHECK(wavs[1] == d + L"\\y.WAV");
  }
  CHECK_EQ(fs.list(td.path + L"\\a\\b", L"").size(), size_t(0));  // only a subdir inside
  CHECK_EQ(fs.list(td.path + L"\\none", L"").size(), size_t(0));
  // A file in the way of a directory is an error.
  err.clear();
  CHECK(!fs.ensure_dir(d + L"\\x.wav\\sub", &err));
  CHECK(!err.empty());
}

TEST(win_long_path) {
  TempDir td;
  sl::IFileSystem& fs = sl::real_fs();
  std::wstring err;
  std::wstring d = td.path;
  while (d.size() < 330) d += L"\\long_directory_name_segment";
  CHECK(fs.ensure_dir(d, &err));
  CHECK(fs.exists(d));
  const std::wstring p = d + L"\\file.wav";
  CHECK(p.size() > MAX_PATH);
  auto f = fs.create_new(p, &err);
  CHECK(f != nullptr);
  if (f) {
    CHECK(f->write("abc", 3));
    CHECK_EQ(f->size(), uint64_t(3));
    f->close();
  }
  CHECK_EQ(fs.list(d, L".wav").size(), size_t(1));
  CHECK(!fs.volume_id(d).empty());
  CHECK(fs.free_bytes(d).has_value());
  CHECK(fs.remove(p));
  CHECK(!fs.exists(p));
}
