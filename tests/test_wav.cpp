#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "audio/ramp.h"
#include "format/encoder.h"
#include "format/wav_reader.h"
#include "format/wav_repair.h"
#include "format/wav_writer.h"
#include "io/fault_file_system.h"
#include "test.h"

using namespace sl;

namespace {

const std::wstring kDir = L"D:\\rec";
const std::wstring kPath = L"D:\\rec\\a.wav";

std::unique_ptr<IEncoderFile> open(FaultFileSystem& fs, const WavParams& p) {
  std::wstring err;
  fs.ensure_dir(kDir, &err);
  auto w = open_wav(fs, kPath, p, &err);
  CHECK(w != nullptr);
  return w;
}

// Reads the WAV header of the file's current contents.
std::optional<WavInfo> info_of(FaultFileSystem& fs, const std::wstring& path = kPath) {
  std::wstring err;
  auto f = fs.open_existing(path, &err);
  if (!f) return std::nullopt;
  return read_wav_info(*f);
}

std::vector<float> ramp(uint64_t first, size_t frames, uint32_t ch) {
  std::vector<float> v(frames * ch);
  for (size_t f = 0; f < frames; ++f)
    for (uint32_t c = 0; c < ch; ++c) v[f * ch + c] = ramp_value(SampleIndex(first + f), c);
  return v;
}

uint32_t le32(const std::vector<uint8_t>& b, size_t off) {
  uint32_t v;
  std::memcpy(&v, b.data() + off, 4);
  return v;
}

}  // namespace

TEST(wav_16bit_stereo_basic) {
  FaultFileSystem fs;
  WavParams p{48000, 2};
  p.bext.description = "unit test";
  p.bext.date = "2026-10-16";
  p.bext.time = "12:34:56";
  p.bext.time_reference = 0x1'0000'0002ull;
  auto w = open(fs, p);
  if (!w) return;
  const auto in = ramp(0, 48000, 2);
  CHECK(w->write(in.data(), 48000));
  CHECK_EQ(w->frames_written(), 48000u);
  CHECK(w->finalize({}, std::nullopt));
  const auto info = info_of(fs);
  CHECK(info.has_value());
  if (!info) return;
  CHECK_EQ(info->riff_id, std::string("RIFF"));
  CHECK_EQ(info->format_tag, 1);
  CHECK_EQ(info->channels, 2);
  CHECK_EQ(info->bits, 16);
  CHECK_EQ(info->block_align, 4);
  CHECK_EQ(info->sample_rate, 48000u);
  CHECK_EQ(info->frames, 48000u);
  CHECK_EQ(info->data_bytes, 192000u);
  CHECK_EQ(info->bext.originator, std::string("SoundLogger"));
  CHECK_EQ(info->bext.description, std::string("unit test"));
  CHECK_EQ(info->bext.date, std::string("2026-10-16"));
  CHECK_EQ(info->bext.time, std::string("12:34:56"));
  CHECK_EQ(info->bext.time_reference, 0x1'0000'0002ull);
  const auto bytes = fs.contents(kPath);
  CHECK_EQ(le32(bytes, 4), uint32_t(bytes.size() - 8));
  CHECK_EQ(info->data_offset, uint64_t(48 + 8 + 16 + 8 + 602 + 8));
}

TEST(wav_flush_updates_header_before_finalize) {
  FaultFileSystem fs;
  auto w = open(fs, WavParams{48000, 2});
  if (!w) return;
  const auto in = ramp(0, 1000, 2);
  CHECK(w->write(in.data(), 1000));
  CHECK_EQ(w->confirmed_frames(), 0u);
  CHECK(w->flush());
  CHECK_EQ(w->confirmed_frames(), 1000u);
  const auto info = info_of(fs);
  CHECK(info.has_value());
  if (!info) return;
  CHECK_EQ(info->data_bytes, 4000u);
  CHECK_EQ(info->frames, 1000u);
}

TEST(wav_markers_to_cue_labl) {
  FaultFileSystem fs;
  auto w = open(fs, WavParams{48000, 2});
  if (!w) return;
  const auto in = ramp(0, 3000, 2);
  CHECK(w->write(in.data(), 3000));
  CHECK(w->finalize({{100, L"intro"}, {2000, L"set 2 \u00e9"}}, std::nullopt));
  const auto info = info_of(fs);
  CHECK(info.has_value());
  if (!info) return;
  CHECK_EQ(info->frames, 3000u);
  CHECK_EQ(info->markers.size(), size_t(2));
  if (info->markers.size() != 2) return;
  CHECK_EQ(info->markers[0].frame_in_file, 100u);
  CHECK(info->markers[0].name == L"intro");
  CHECK_EQ(info->markers[1].frame_in_file, 2000u);
  CHECK(info->markers[1].name == L"set 2 \u00e9");
}

TEST(wav_8ch_24bit_extensible) {
  FaultFileSystem fs;
  auto w = open(fs, WavParams{96000, 8, WavDepth::Int24});
  if (!w) return;
  const auto in = ramp(0, 10, 8);
  CHECK(w->write(in.data(), 10));
  CHECK(w->finalize({}, std::nullopt));
  const auto info = info_of(fs);
  CHECK(info.has_value());
  if (!info) return;
  CHECK_EQ(info->format_tag, 0xFFFE);
  CHECK_EQ(info->channels, 8);
  CHECK_EQ(info->bits, 24);
  CHECK_EQ(info->block_align, 24);
  CHECK_EQ(info->frames, 10u);
  // 24-bit is exact: ramp values are multiples of 2^-20, so sample = v * 2^23.
  const auto bytes = fs.contents(kPath);
  const size_t off = size_t(info->data_offset) + 24 * 3 + 3 * 5;  // frame 3, channel 5
  const int32_t s = int32_t(bytes[off] | (bytes[off + 1] << 8) | (bytes[off + 2] << 16) |
                            ((bytes[off + 2] & 0x80) ? 0xFF000000u : 0u));
  CHECK_EQ(s, int32_t(ramp_value(3, 5) * 8388608.0f));
}

TEST(wav_float32_ramp_exact) {
  FaultFileSystem fs;
  auto w = open(fs, WavParams{48000, 3, WavDepth::Float32});
  if (!w) return;
  const auto in = ramp(777, 2000, 3);
  CHECK(w->write(in.data(), 2000));
  CHECK(w->finalize({}, std::nullopt));
  const auto info = info_of(fs);
  CHECK(info.has_value());
  if (!info) return;
  CHECK_EQ(info->format_tag, 0xFFFE);
  CHECK_EQ(info->bits, 32);
  CHECK_EQ(info->frames, 2000u);
  const auto bytes = fs.contents(kPath);
  CHECK(bytes.size() >= info->data_offset + in.size() * 4);
  CHECK(std::memcmp(bytes.data() + info->data_offset, in.data(), in.size() * 4) == 0);

  // Mono/stereo float uses plain tag 3.
  FaultFileSystem fs2;
  std::wstring err;
  fs2.ensure_dir(kDir, &err);
  auto w2 = open_wav(fs2, kPath, WavParams{48000, 2, WavDepth::Float32}, &err);
  CHECK(w2 != nullptr);
  if (!w2) return;
  CHECK(w2->finalize({}, std::nullopt));
  const auto info2 = info_of(fs2);
  CHECK(info2.has_value());
  if (info2) CHECK_EQ(info2->format_tag, 3);
}

TEST(wav_rf64_upgrade) {
  FaultFileSystem fs;
  WavParams p{48000, 2};
  p.rf64_threshold = 1000;
  auto w = open(fs, p);
  if (!w) return;
  const auto in = ramp(0, 1000, 2);
  CHECK(w->write(in.data(), 1000));
  CHECK(w->flush());
  auto info = info_of(fs);
  CHECK(info.has_value());
  if (!info) return;
  CHECK_EQ(info->riff_id, std::string("RF64"));
  CHECK_EQ(info->data_bytes, 4000u);
  CHECK_EQ(info->frames, 1000u);
  const auto b = fs.contents(kPath);
  CHECK_EQ(le32(b, 4), 0xFFFFFFFFu);
  CHECK_EQ(le32(b, size_t(info->data_offset) - 4), 0xFFFFFFFFu);
  CHECK(std::memcmp(b.data() + 12, "ds64", 4) == 0);
  CHECK(w->finalize({{5, L"m"}}, std::nullopt));
  info = info_of(fs);
  CHECK(info.has_value());
  if (!info) return;
  CHECK_EQ(info->riff_id, std::string("RF64"));
  CHECK_EQ(info->data_bytes, 4000u);
  CHECK_EQ(info->frames, 1000u);
  CHECK_EQ(info->markers.size(), size_t(1));
}

TEST(wav_truncate_on_finalize) {
  FaultFileSystem fs;
  auto w = open(fs, WavParams{48000, 2});
  if (!w) return;
  const auto a = ramp(0, 1000, 2), b = ramp(1000, 500, 2);
  CHECK(w->write(a.data(), 1000));
  CHECK(w->flush());
  CHECK(w->write(b.data(), 500));
  CHECK(w->finalize({{10, L"x"}}, 1000));
  const auto info = info_of(fs);
  CHECK(info.has_value());
  if (!info) return;
  CHECK_EQ(info->frames, 1000u);
  CHECK_EQ(info->markers.size(), size_t(1));
  // The discarded tail is wrapped in a JUNK chunk: header and file length agree.
  const auto bytes = fs.contents(kPath);
  CHECK_EQ(le32(bytes, 4), uint32_t(bytes.size() - 8));
  // Repair leaves a finalized file alone.
  std::wstring err;
  const auto r = repair_wav(fs, kPath, &err);
  CHECK(r.has_value());
  if (r) CHECK_EQ(*r, 1000u);
  CHECK(fs.contents(kPath) == bytes);
}

TEST(wav_repair_after_crash) {
  FaultFileSystem fs;
  auto w = open(fs, WavParams{48000, 2});
  if (!w) return;
  const auto a = ramp(0, 1000, 2), b = ramp(1000, 333, 2);
  CHECK(w->write(a.data(), 1000));
  CHECK(w->flush());
  CHECK(w->write(b.data(), 333));
  w->abandon();
  CHECK(!w->write(b.data(), 1));
  CHECK(!w->flush());
  CHECK(!w->finalize({}, std::nullopt));
  CHECK_EQ(info_of(fs)->frames, 1000u);
  std::wstring err;
  const auto r = repair_wav(fs, kPath, &err);
  CHECK(r.has_value());
  if (r) CHECK_EQ(*r, 1333u);
  const auto info = info_of(fs);
  CHECK(info.has_value());
  if (info) CHECK_EQ(info->frames, 1333u);
  const auto bytes = fs.contents(kPath);
  CHECK_EQ(le32(bytes, 4), uint32_t(bytes.size() - 8));
}

TEST(wav_repair_crash_before_first_flush_and_partial_frame) {
  FaultFileSystem fs;
  auto w = open(fs, WavParams{48000, 2});
  if (!w) return;
  const auto a = ramp(0, 50, 2);
  CHECK(w->write(a.data(), 50));
  w->abandon();
  // Simulate a torn final frame.
  std::wstring err;
  auto f = fs.open_existing(kPath, &err);
  CHECK(f != nullptr);
  if (!f) return;
  const uint8_t junk[3] = {1, 2, 3};
  CHECK(f->write(junk, 3));
  const auto r = repair_wav(fs, kPath, &err);
  CHECK(r.has_value());
  if (r) CHECK_EQ(*r, 50u);
  CHECK_EQ(info_of(fs)->frames, 50u);
}

TEST(wav_repair_keeps_rf64) {
  // A crashed file that was already upgraded stays RF64 after repair.
  FaultFileSystem fs;
  WavParams p{48000, 2};
  p.rf64_threshold = 1000;
  auto w = open(fs, p);
  if (!w) return;
  const auto a = ramp(0, 1000, 2), b = ramp(1000, 10, 2);
  CHECK(w->write(a.data(), 1000));
  CHECK(w->flush());
  CHECK(w->write(b.data(), 10));
  w->abandon();
  std::wstring err;
  const auto r = repair_wav(fs, kPath, &err);
  CHECK(r.has_value());
  if (r) CHECK_EQ(*r, 1010u);
  const auto info = info_of(fs);
  CHECK(info.has_value());
  if (!info) return;
  CHECK_EQ(info->riff_id, std::string("RF64"));
  CHECK_EQ(info->frames, 1010u);
}

TEST(wav_odd_data_pads_before_cue) {
  FaultFileSystem fs;
  auto w = open(fs, WavParams{48000, 1, WavDepth::Int24});
  if (!w) return;
  const auto in = ramp(0, 3, 1);
  CHECK(w->write(in.data(), 3));
  CHECK(w->finalize({{2, L"end"}}, std::nullopt));
  const auto info = info_of(fs);
  CHECK(info.has_value());
  if (!info) return;
  CHECK_EQ(info->data_bytes, 9u);
  CHECK_EQ(info->frames, 3u);
  const auto bytes = fs.contents(kPath);
  const size_t cue = size_t(info->data_offset) + 10;
  CHECK(cue % 2 == 0);
  CHECK(bytes.size() > cue + 4 && std::memcmp(bytes.data() + cue, "cue ", 4) == 0);
  CHECK_EQ(info->markers.size(), size_t(1));
  if (!info->markers.empty()) {
    CHECK_EQ(info->markers[0].frame_in_file, 2u);
    CHECK(info->markers[0].name == L"end");
  }
}

TEST(wav_open_rejects_bad_params_and_existing_file) {
  FaultFileSystem fs;
  std::wstring err;
  fs.ensure_dir(kDir, &err);
  CHECK(open_wav(fs, kPath, WavParams{48000, 0}, &err) == nullptr);
  CHECK(open_wav(fs, kPath, WavParams{0, 2}, &err) == nullptr);
  auto w = open_wav(fs, kPath, WavParams{48000, 2}, &err);
  CHECK(w != nullptr);
  err.clear();
  CHECK(open_wav(fs, kPath, WavParams{48000, 2}, &err) == nullptr);  // never overwrite
  CHECK(!err.empty());
}
