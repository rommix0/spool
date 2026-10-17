#include <string>
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
