#include <chrono>
#include <thread>

#include "core/clock.h"
#include "io/fault_file_system.h"
#include "session/naming.h"
#include "test.h"

using namespace spool;

namespace {
// Build a WallTime whose local time is the given civil time (UTC + offset).
WallTime local_at(int64_t days_since_epoch, int h, int m, int s, int ms, int32_t offset_min) {
  WallTime w;
  w.local_offset_min = offset_min;
  w.utc_ms = days_since_epoch * 86400000LL + ((h * 60LL + m) * 60 + s) * 1000 + ms -
             static_cast<int64_t>(offset_min) * 60000;
  return w;
}
// 2026-10-16 is day 20742 since 1970-01-01.
constexpr int64_t kDay = 20742;
}  // namespace

TEST(name_default_template) {
  NameInputs in;
  in.session = L"gig";
  in.dest = L"main";
  in.first_sample = local_at(kDay, 14, 30, 0, 0, -240);
  in.n = 3;
  in.seg = 0;
  CHECK(expand_name(L"{session}_{date}_{time}_{n}{seg}", in) == L"gig_2026-10-16_143000_003");
}

TEST(name_segment_letter) {
  NameInputs in;
  in.session = L"gig";
  in.dest = L"main";
  in.first_sample = local_at(kDay, 14, 30, 0, 0, 0);
  in.n = 3;
  in.seg = 1;
  const std::wstring a = expand_name(L"{session}_{date}_{time}_{n}{seg}", in);
  CHECK(a.size() >= 6 && a.substr(a.size() - 6) == L"_003_b");
  in.seg = 2;
  const std::wstring b = expand_name(L"{n}{seg}", in);
  CHECK(b == L"003_c");
}

TEST(name_tokens_dest_and_unknown) {
  NameInputs in;
  in.session = L"Set 1: Main/Stage";
  in.dest = L"usb?";
  in.first_sample = local_at(kDay, 1, 2, 3, 0, 0);
  CHECK(expand_name(L"{dest}-{session}-{bogus}", in) == L"usb_-Set 1_ Main_Stage-{bogus}");
  CHECK(expand_name(L"a{unterminated", in) == L"a{unterminated");
  CHECK(expand_name(L"x/y", in) == L"x_y");  // final stem is sanitized too
  CHECK(expand_name(L"", in) == L"_");
}

TEST(name_sanitizes_illegal_chars) {
  CHECK(sanitize_component(L"Set 1: Main/Stage") == L"Set 1_ Main_Stage");
  CHECK(sanitize_component(L"x. ") == L"x");
  CHECK(sanitize_component(L"") == L"_");
  CHECK(sanitize_component(L"...") == L"_");
  CHECK(sanitize_component(L"a<b>c\"d\\e|f?g*h\ti") == L"a_b_c_d_e_f_g_h_i");
}

// Windows reserves these device names, with any extension: "nul.wav" is the NUL device.
TEST(name_sanitizes_reserved_device_names) {
  for (const wchar_t* r : {L"CON", L"prn", L"Aux", L"NUL", L"COM1", L"com9", L"LPT1", L"lpt9"})
    CHECK(sanitize_component(r) == std::wstring(L"_") + r);
  CHECK(sanitize_component(L"nul.backup") == L"_nul.backup");
  CHECK(sanitize_component(L"Con.") == L"_Con");  // the trailing dot goes first
  CHECK(sanitize_component(L"COM0") == L"COM0");
  CHECK(sanitize_component(L"COM10") == L"COM10");
  CHECK(sanitize_component(L"console") == L"console");
  CHECK(sanitize_component(L"aux_1") == L"aux_1");
  CHECK(sanitize_component(L"my.nul") == L"my.nul");
  NameInputs in;
  in.dest = L"aux";
  CHECK(expand_name(L"{dest}", in) == L"_aux");
}

TEST(name_unique_path_on_collision) {
  FaultFileSystem fs;
  std::wstring err;
  CHECK(fs.ensure_dir(L"D:\\rec", &err));
  CHECK(unique_path(fs, L"D:\\rec", L"a", L".wav") == L"D:\\rec\\a.wav");
  CHECK(fs.create_new(L"D:\\rec\\a.wav", &err) != nullptr);
  CHECK(fs.create_new(L"D:\\rec\\a_1.wav", &err) != nullptr);
  CHECK(unique_path(fs, L"D:\\rec", L"a", L".wav") == L"D:\\rec\\a_2.wav");
  CHECK(unique_path(fs, L"D:\\rec\\", L"b", L".wav") == L"D:\\rec\\b.wav");
}

TEST(clock_local_parts_and_iso) {
  WallTime w = local_at(kDay, 14, 0, 0, 0, 0);
  CHECK(iso_utc(w) == "2026-10-16T14:00:00.000Z");
  // Same instant seen from US Eastern daylight (-240): local 10:00, iso stays UTC.
  w.local_offset_min = -240;
  const LocalParts p = local_parts(w);
  CHECK_EQ(p.hour, 10);
  CHECK_EQ(p.day, 16);
  CHECK(iso_utc(w) == "2026-10-16T14:00:00.000Z");
  // Crossing midnight with a positive offset, and a leap day.
  WallTime x;
  x.utc_ms = 951782400000LL + 23 * 3600000LL + 30 * 60000LL + 123;  // 2000-02-29T23:30:00.123Z
  x.local_offset_min = 90;
  const LocalParts q = local_parts(x);
  CHECK_EQ(q.year, 2000);
  CHECK_EQ(q.month, 3);
  CHECK_EQ(q.day, 1);
  CHECK_EQ(q.hour, 1);
  CHECK_EQ(q.minute, 0);
  CHECK_EQ(q.ms, 123);
  // Before 1970.
  WallTime y;
  y.utc_ms = -1;
  const LocalParts r = local_parts(y);
  CHECK_EQ(r.year, 1969);
  CHECK_EQ(r.month, 12);
  CHECK_EQ(r.day, 31);
  CHECK_EQ(r.hour, 23);
  CHECK_EQ(r.ms, 999);
}

TEST(clock_fake_set_vs_advance) {
  FakeClock c(local_at(kDay, 0, 0, 0, 0, 0), 100);
  const int64_t t0 = c.now().utc_ms;
  c.advance_ms(1500);
  CHECK_EQ(c.now().utc_ms, t0 + 1500);
  CHECK_EQ(c.steady_ms(), 1600u);
  c.set(local_at(kDay + 1, 0, 0, 0, 0, 0));
  CHECK_EQ(c.steady_ms(), 1600u);  // set() leaves steady time alone
  CHECK_EQ(c.now().utc_ms, (kDay + 1) * 86400000LL);
}

TEST(clock_system_is_sane) {
  IClock& c = system_clock();
  const WallTime w = c.now();
  CHECK(w.utc_ms > 1700000000000LL);  // after 2023
  CHECK(w.local_offset_min >= -14 * 60 && w.local_offset_min <= 14 * 60);
  const uint64_t a = c.steady_ms();
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  CHECK(c.steady_ms() >= a + 15);
}

TEST(rotation_aligned_to_half_hour) {
  const SampleIndex now = 123456;
  const WallTime w = local_at(kDay, 14, 12, 0, 0, -240);
  CHECK_EQ(first_rotation_boundary(now, w, 48000, 30), now + 18LL * 60 * 48000);
}

TEST(rotation_exactly_on_boundary_goes_to_next) {
  const SampleIndex now = 0;
  const WallTime w = local_at(kDay, 14, 30, 0, 0, 330);
  CHECK_EQ(first_rotation_boundary(now, w, 48000, 30), now + 30LL * 60 * 48000);
}

TEST(rotation_mid_second) {
  const SampleIndex now = 1000;
  const WallTime w = local_at(kDay, 14, 12, 0, 500, 0);
  CHECK_EQ(first_rotation_boundary(now, w, 48000, 30), now + (17LL * 60 + 59) * 48000 + 24000);
}

TEST(rotation_across_midnight_and_negative_local) {
  // 23:50 local, 30 min period -> 00:00 next day, 10 minutes away.
  const WallTime w = local_at(kDay, 23, 50, 0, 0, -300);
  CHECK_EQ(first_rotation_boundary(0, w, 1000, 30), 10LL * 60 * 1000);
  // Pre-1970 local time still aligns (negative modulo handled).
  WallTime old;
  old.utc_ms = -(7 * 60000LL);  // 23:53 local on 1969-12-31
  CHECK_EQ(first_rotation_boundary(0, old, 1000, 30), 7LL * 60 * 1000);
}

TEST(rotation_next_is_sample_based) {
  CHECK_EQ(next_rotation_boundary(1000, 48000, 30), 1000 + 30LL * 60 * 48000);
}

TEST(rotation_off) { CHECK_EQ(first_rotation_boundary(0, {}, 48000, 0), kNoRotation); }
