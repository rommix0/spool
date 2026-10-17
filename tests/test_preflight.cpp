#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "io/fault_file_system.h"
#include "session/config.h"
#include "session/preflight.h"
#include "test.h"

using namespace sl;
using Kind = FaultFileSystem::Kind;

namespace {

DestinationConfig dest(const std::wstring& name, std::vector<std::wstring> folders) {
  DestinationConfig c;
  c.name = name;
  c.folders = std::move(folders);
  return c;  // default WAV 16-bit, all (2) input channels at 48 kHz: 192000 B/s
}

bool any_mentions(const std::vector<std::wstring>& v, const std::wstring& a, const std::wstring& b = L"") {
  for (const auto& s : v)
    if (s.find(a) != std::wstring::npos && (b.empty() || s.find(b) != std::wstring::npos)) return true;
  return false;
}

}  // namespace

TEST(preflight_same_volume_warns) {
  FaultFileSystem fs;
  fs.set_volume(L"D:\\p", L"VOL-D");
  fs.set_volume(L"D:\\q", L"VOL-D");
  const auto r = preflight({dest(L"main", {L"D:\\p", L"D:\\q"})}, SessionParams{}, fs);
  CHECK(r.can_start);
  CHECK(r.errors.empty());
  CHECK(any_mentions(r.warnings, L"D:\\p", L"D:\\q"));
}

TEST(preflight_different_volumes_no_warning) {
  FaultFileSystem fs;
  fs.set_volume(L"D:\\", L"VOL-D");
  fs.set_volume(L"E:\\", L"VOL-E");
  const auto r = preflight({dest(L"main", {L"D:\\p", L"E:\\s"})}, SessionParams{}, fs);
  CHECK(r.can_start);
  CHECK(r.warnings.empty());
  CHECK(r.errors.empty());
}

TEST(preflight_one_dest_unwritable_can_start) {
  FaultFileSystem fs;
  fs.add_rule({L"X:\\", Kind::FailOpen});
  fs.add_rule({L"Y:\\", Kind::FailOpen});
  const auto r = preflight({dest(L"main", {L"D:\\p"}), dest(L"copy", {L"X:\\a", L"Y:\\b"})}, SessionParams{}, fs);
  CHECK(r.can_start);
  CHECK_EQ(r.errors.size(), size_t(1));
  CHECK(any_mentions(r.errors, L"copy"));
  CHECK(any_mentions(r.warnings, L"X:\\a"));
}

TEST(preflight_all_unwritable_blocks) {
  FaultFileSystem fs;
  fs.add_rule({L"X:\\", Kind::FailOpen});
  fs.add_rule({L"Y:\\", Kind::FailOpen});
  const auto r = preflight({dest(L"main", {L"X:\\a"}), dest(L"copy", {L"Y:\\b"})}, SessionParams{}, fs);
  CHECK(!r.can_start);
  CHECK_EQ(r.errors.size(), size_t(2));
}

TEST(preflight_short_primary_warns) {
  FaultFileSystem fs;
  fs.set_free_bytes(L"D:\\", 192000ull * 1800);  // 30 min at 192000 B/s
  SessionParams sp;
  sp.low_water_min_bytes = 0;  // keep the low-water warning out of the way
  sp.low_water_minutes = 1;
  const auto r = preflight({dest(L"main", {L"D:\\p", L"E:\\s"})}, sp, fs);
  CHECK(r.can_start);
  CHECK_EQ(r.warnings.size(), size_t(1));
  CHECK(any_mentions(r.warnings, L"D:\\p"));
}

TEST(preflight_low_water_bytes) {
  SessionParams sp;
  CHECK_EQ(low_water_bytes(sp, 192000), uint64_t(1) << 30);  // 10 min = 115.2 MB < 1 GB
  CHECK_EQ(low_water_bytes(sp, 4000000), uint64_t(10) * 60 * 4000000);
  sp.low_water_min_bytes = 0;
  CHECK_EQ(low_water_bytes(sp, 192000), uint64_t(10) * 60 * 192000);
}

TEST(preflight_folder_healthy_probe) {
  FaultFileSystem fs;
  CHECK(folder_healthy(fs, L"D:\\new\\dir", 1000));
  CHECK(fs.exists(L"D:\\new\\dir"));
  CHECK(fs.list(L"D:\\new\\dir", L"").empty());  // the probe file is gone
  fs.set_free_bytes(L"D:\\", 999);
  CHECK(!folder_healthy(fs, L"D:\\new\\dir", 1000));
  CHECK(folder_healthy(fs, L"D:\\new\\dir", 999));
  fs.add_rule({L"D:\\", Kind::FailWrite});
  CHECK(!folder_healthy(fs, L"D:\\new\\dir", 0));
  fs.clear_rules();
  fs.add_rule({L"D:\\", Kind::FailFlush});  // what confirms audio must work for the probe too
  CHECK(!folder_healthy(fs, L"D:\\new\\dir", 0));
  fs.clear_rules();
  fs.add_rule({L"D:\\", Kind::FailOpen});
  CHECK(!folder_healthy(fs, L"D:\\new\\dir", 0));
}


namespace {
// Records the files created through it; everything else goes to the FaultFileSystem.
class RecordingFs : public IFileSystem {
 public:
  explicit RecordingFs(FaultFileSystem& fs) : fs_(fs) {}
  std::unique_ptr<IFile> create_new(const std::wstring& p, std::wstring* err) override {
    created.push_back(p);
    return fs_.create_new(p, err);
  }
  std::unique_ptr<IFile> open_existing(const std::wstring& p, std::wstring* err) override {
    return fs_.open_existing(p, err);
  }
  bool exists(const std::wstring& p) override { return fs_.exists(p); }
  bool ensure_dir(const std::wstring& d, std::wstring* err) override { return fs_.ensure_dir(d, err); }
  bool remove(const std::wstring& p) override { return fs_.remove(p); }
  std::vector<std::wstring> list(const std::wstring& d, const std::wstring& s) override { return fs_.list(d, s); }
  std::optional<uint64_t> free_bytes(const std::wstring& d) override { return fs_.free_bytes(d); }
  std::wstring volume_id(const std::wstring& d) override { return fs_.volume_id(d); }

  std::vector<std::wstring> created;  // single-threaded use only

 private:
  FaultFileSystem& fs_;
};
}  // namespace

// R12: every probe has a name of its own, so destinations probing one folder at the same
// time (or a probe left open by a hung writer) never make each other fail.
TEST(preflight_probe_names_are_unique) {
  FaultFileSystem fault;
  RecordingFs fs(fault);
  CHECK(folder_healthy(fs, L"D:\\shared", 0, L"main"));
  CHECK(folder_healthy(fs, L"D:\\shared", 0, L"copy: 2"));
  CHECK(folder_healthy(fs, L"D:\\shared", 0, L"main"));
  CHECK_EQ(fs.created.size(), size_t(3));
  if (fs.created.size() != 3) return;
  CHECK(fs.created[0].rfind(L"D:\\shared\\.sl_probe_main_", 0) == 0);
  CHECK(fs.created[1].rfind(L"D:\\shared\\.sl_probe_copy_ 2_", 0) == 0);  // sanitized
  CHECK(fs.created[0] != fs.created[2]);
  for (const auto& p : fs.created) CHECK(p.size() > 4 && p.compare(p.size() - 4, 4, L".tmp") == 0);

  // A probe file still open elsewhere does not get in the way.
  std::wstring err;
  auto held = fault.create_new(fs.created[0], &err);
  CHECK(held != nullptr);
  CHECK(folder_healthy(fs, L"D:\\shared", 0, L"main"));

  // Two destinations probing the same folder at once.
  std::atomic<int> failures{0};
  auto probe = [&](const wchar_t* who) {
    for (int i = 0; i < 200; ++i)
      if (!folder_healthy(fault, L"D:\\shared", 0, who)) ++failures;
  };
  std::thread a(probe, L"main"), b(probe, L"copy");
  a.join();
  b.join();
  CHECK_EQ(failures.load(), 0);
  held->close();
  CHECK_EQ(fault.list(L"D:\\shared", L"").size(), size_t(1));  // only the file held above
}

// Preflight applies the writer's own rule: a destination whose folders are all below the
// low-water mark starts in LastResort.
TEST(preflight_below_low_water_is_last_resort) {
  FaultFileSystem fs;
  fs.set_free_bytes(L"D:\\", 100ull << 20);  // below the 1 GB default
  const auto one = preflight({dest(L"main", {L"D:\\p"})}, SessionParams{}, fs);
  CHECK(!one.can_start);
  CHECK_EQ(one.errors.size(), size_t(1));
  CHECK(any_mentions(one.warnings, L"low-water", L"D:\\p"));
  const auto two = preflight({dest(L"main", {L"D:\\p"}), dest(L"copy", {L"E:\\c"})}, SessionParams{}, fs);
  CHECK(two.can_start);
  CHECK_EQ(two.errors.size(), size_t(1));
  CHECK(any_mentions(two.errors, L"main"));
}
