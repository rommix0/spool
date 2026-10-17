#include <cstdint>
#include <vector>

#include "core/sample_convert.h"
#include "test.h"

namespace {
void put24(uint8_t* p, int32_t v) {
  p[0] = uint8_t(v & 0xFF);
  p[1] = uint8_t((v >> 8) & 0xFF);
  p[2] = uint8_t((v >> 16) & 0xFF);
}
int32_t get24(const uint8_t* p) {
  int32_t v = int32_t(uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16));
  return (v << 8) >> 8;  // sign-extend
}
}  // namespace

TEST(convert_int24_roundtrip_exact) {
  const int32_t vals[] = {-8388608, -1, 0, 1, 8388607};
  uint8_t in[15];
  for (int i = 0; i < 5; ++i) put24(in + 3 * i, vals[i]);
  float f[5];
  sl::int24_packed_to_float(in, f, 5);
  CHECK_NEAR(f[0], -1.0f, 0.0f);
  uint8_t out[15];
  sl::float_to_int24_packed(f, out, 5);
  for (int i = 0; i < 5; ++i) CHECK_EQ(get24(out + 3 * i), vals[i]);
}

TEST(convert_int24_clamps) {
  const float in[] = {1.0f, -1.5f};
  uint8_t out[6];
  sl::float_to_int24_packed(in, out, 2);
  CHECK_EQ(get24(out), 8388607);
  CHECK_EQ(get24(out + 3), -8388608);
}

TEST(convert_int16_tpdf_silence_small) {
  std::vector<float> in(10000, 0.0f);
  std::vector<int16_t> out(10000);
  sl::Tpdf d;
  sl::float_to_int16_tpdf(in.data(), out.data(), in.size(), d);
  for (int16_t s : out) CHECK(s >= -1 && s <= 1);
}

TEST(convert_int16_tpdf_unbiased) {
  std::vector<float> in(10000, 0.25f);
  std::vector<int16_t> out(10000);
  sl::Tpdf d;
  sl::float_to_int16_tpdf(in.data(), out.data(), in.size(), d);
  double sum = 0;
  for (int16_t s : out) sum += s;
  CHECK_NEAR(sum / double(out.size()), 8192.0, 1.0);
}

TEST(convert_int16_clamps) {
  const float in[] = {2.0f, -2.0f};
  int16_t out[2];
  sl::Tpdf d;
  sl::float_to_int16_tpdf(in, out, 2, d);
  CHECK_EQ(int(out[0]), 32767);
  CHECK_EQ(int(out[1]), -32768);
}

TEST(convert_extract_channels) {
  const uint32_t in_ch = 8, frames = 4;
  std::vector<float> in(in_ch * frames);
  for (uint32_t i = 0; i < in.size(); ++i) in[i] = float(i);
  const uint16_t map[] = {6, 1};
  std::vector<float> out(2 * frames);
  sl::extract_channels(in.data(), in_ch, map, 2, out.data(), frames);
  for (uint32_t f = 0; f < frames; ++f) {
    CHECK_EQ(out[f * 2 + 0], in[f * in_ch + 6]);
    CHECK_EQ(out[f * 2 + 1], in[f * in_ch + 1]);
  }
}
