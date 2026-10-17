#include <optional>
#include <string>
#include <vector>

#include "app/settings_model.h"
#include "app/text_util.h"
#include "test.h"

using namespace sl;

namespace {
// 2026-10-16 14:30:05 local, UTC+2.
WallTime sample_time() {
  constexpr int64_t kDay = 20742;
  WallTime w;
  w.local_offset_min = 120;
  w.utc_ms = kDay * 86400000LL + ((14 * 60LL + 30) * 60 + 5) * 1000 - 120 * 60000LL;
  return w;
}
}  // namespace

TEST(model_rotation_choices) {
  CHECK((rotation_choices(30) == std::vector<uint32_t>{0, 5, 10, 15, 30, 60}));
  CHECK((rotation_choices(0) == std::vector<uint32_t>{0, 5, 10, 15, 30, 60}));
  // A hand-edited value stays selectable, in order.
  CHECK((rotation_choices(45) == std::vector<uint32_t>{0, 5, 10, 15, 30, 45, 60}));
  CHECK((rotation_choices(120) == std::vector<uint32_t>{0, 5, 10, 15, 30, 60, 120}));
  CHECK(rotation_label(0) == L"Off");
  CHECK(rotation_label(5) == L"5 min");
  CHECK(rotation_label(60) == L"60 min");
}

TEST(model_bitrate_choices) {
  const std::vector<uint32_t> std_rates{32000, 48000, 64000, 96000, 128000, 160000, 192000, 256000};
  CHECK(bitrate_choices(64000) == std_rates);
  CHECK(bitrate_choices(0) == std_rates);
  CHECK((bitrate_choices(20000) ==
         std::vector<uint32_t>{20000, 32000, 48000, 64000, 96000, 128000, 160000, 192000, 256000}));
  CHECK(bitrate_label(64000) == L"64k");
  CHECK(bitrate_label(256000) == L"256k");
  CHECK_EQ(effective_bitrate(0, 1), 64000u);
  CHECK_EQ(effective_bitrate(0, 2), 128000u);
  CHECK_EQ(effective_bitrate(96000, 1), 96000u);
}

TEST(model_wav_depth_index) {
  CHECK_EQ(depth_index(WavDepth::Int16), 0);
  CHECK_EQ(depth_index(WavDepth::Int24), 1);
  CHECK_EQ(depth_index(WavDepth::Float32), 2);
  CHECK(depth_from_index(0) == WavDepth::Int16);
  CHECK(depth_from_index(1) == WavDepth::Int24);
  CHECK(depth_from_index(2) == WavDepth::Float32);
  CHECK(depth_from_index(-1) == WavDepth::Int16);
}

TEST(model_channel_checks) {
  CHECK((channel_checks({}, 2) == std::vector<bool>{true, true}));  // empty = all
  CHECK((channel_checks({1}, 4) == std::vector<bool>{false, true, false, false}));
  // A channel the device lacks still shows (checked), so validation can name it.
  CHECK((channel_checks({0, 4}, 2) == std::vector<bool>{true, false, false, false, true}));
  CHECK(channel_checks({}, 0).empty());
}

TEST(model_channels_from_checks) {
  constexpr OutFormat kWav = OutFormat::Wav;
  CHECK((channels_from_checks({true, false, true}, 3, false, kWav) == std::vector<uint16_t>{0, 2}));
  CHECK(!channels_from_checks({false, false}, 2, true, kWav).has_value());
  CHECK(!channels_from_checks({}, 0, true, kWav).has_value());
  // Rows beyond the device: an explicit list even when all are checked.
  CHECK((channels_from_checks({true, true, true}, 2, true, kWav) == std::vector<uint16_t>{0, 1, 2}));
  // Round trip.
  CHECK((channels_from_checks(channel_checks({1, 3}, 4), 4, false, kWav) == std::vector<uint16_t>{1, 3}));
}

TEST(model_channels_explicit_stays_explicit) {
  // {0,1} on a 2-ch device, every row checked: still {0,1}, so a later 8-ch device doesn't
  // make it record all 8.
  const std::vector<uint16_t> explicit_list{0, 1};
  const auto checks = channel_checks(explicit_list, 2);
  CHECK((channels_from_checks(checks, 2, false, OutFormat::Wav) == explicit_list));
  CHECK((channels_from_checks(checks, 2, false, OutFormat::Opus) == explicit_list));
}

TEST(model_channels_all_stays_all) {
  // Empty ("all channels") with every row still checked stays empty for WAV.
  const auto checks = channel_checks({}, 4);
  CHECK((channels_from_checks(checks, 4, true, OutFormat::Wav) == std::vector<uint16_t>{}));
  // Unchecking one makes it explicit.
  CHECK((channels_from_checks({true, true, false, true}, 4, true, OutFormat::Wav) ==
         std::vector<uint16_t>{0, 1, 3}));
}

TEST(model_channels_opus_never_all) {
  // Opus is 1-2 channels: "all" would break on a bigger device, so it is always explicit.
  const auto checks = channel_checks({}, 2);
  CHECK((channels_from_checks(checks, 2, true, OutFormat::Opus) == std::vector<uint16_t>{0, 1}));
  CHECK((channels_from_checks({true}, 1, true, OutFormat::Opus) == std::vector<uint16_t>{0}));
}

TEST(model_text_helpers) {
  CHECK(trim(L"  a b \t") == L"a b");
  CHECK(trim(L" \t ").empty());
  CHECK(eq_nocase(L"Destination 3", L"destination 3"));
  CHECK(!eq_nocase(L"main", L"mains"));
  CHECK(join_lines({L"a", L"b", L"c"}) == L"a\nb\nc");
  CHECK(join_lines({}).empty());
}

TEST(model_name_preview) {
  const WallTime t = sample_time();
  CHECK(name_preview(L"{session}_{date}_{time}_{n}{seg}", L"gig", L"main", OutFormat::Wav, t) ==
        L"gig_2026-10-16_143005_001.wav");
  CHECK(name_preview(L"{dest}-{n}", L"gig", L"phones", OutFormat::Opus, t) == L"phones-001.opus");
  CHECK(name_preview(L"", L"gig", L"main", OutFormat::Wav, t) == L"_.wav");
}

TEST(model_new_destination_name) {
  std::vector<DestinationConfig> d(1);
  d[0].name = L"main";
  CHECK(new_destination_name(d) == L"Destination 2");
  d.push_back(DestinationConfig{});
  d[1].name = L"destination 3";  // taken, case-insensitively
  d.push_back(DestinationConfig{});
  d[2].name = L"x";
  CHECK(new_destination_name(d) == L"Destination 4");
  CHECK(new_destination_name({}) == L"Destination 1");
}

TEST(model_parse_number) {
  CHECK(parse_number(L"30", 1, 3600) == std::optional<uint32_t>(30));
  CHECK(parse_number(L" 30 ", 1, 3600) == std::optional<uint32_t>(30));  // spaces trimmed
  CHECK(parse_number(L"0", 0, 10) == std::optional<uint32_t>(0));
  CHECK(!parse_number(L"", 1, 3600));
  CHECK(!parse_number(L"0", 1, 3600));
  CHECK(!parse_number(L"3601", 1, 3600));
  CHECK(!parse_number(L"12a", 1, 3600));
  CHECK(!parse_number(L"-1", 0, 3600));
  CHECK(!parse_number(L"99999999999999999999", 0, 4000000000u));
  CHECK(range_message(L"Ring length", 1, 3600, L" seconds") == L"Ring length must be 1–3600 seconds.");
  CHECK(range_message(L"Stall threshold", 1, 100, L"%") == L"Stall threshold must be 1–100%.");
}

TEST(model_destination_problems) {
  DestinationConfig d;
  d.name = L"phones";
  d.folders = {L"C:\\rec"};
  CHECK(destination_problems(d, 2).empty());

  d.format = OutFormat::Opus;
  d.channels = {0, 1, 2};
  const auto p = destination_problems(d, 4);
  CHECK_EQ(p.size(), size_t(1));
  CHECK(!p.empty() && p[0] == L"\"phones\": Opus records 1 or 2 channels.");

  DestinationConfig e;
  e.name = L"  ";
  e.folders = {};
  const auto q = destination_problems(e, 2);
  CHECK_EQ(q.size(), size_t(2));
  CHECK(!q.empty() && q[0] == L"Enter a destination name.");
}

TEST(model_raw_rate_note) {
  CHECK(raw_rate_note(48000) == L"48000 Hz, set in Windows sound settings");
  CHECK(raw_rate_note(0) == L"Set in Windows sound settings");
}
