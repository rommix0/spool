#include "core/sample_convert.h"

#include <cmath>

namespace sl {

namespace {
constexpr float kInv16 = 1.0f / 32768.0f;
constexpr float kInv24 = 1.0f / 8388608.0f;
constexpr double kInv32 = 1.0 / 2147483648.0;

// Round to nearest and clamp to [lo, hi]. NaN maps to 0. Clamping happens in the
// float domain first so the integer conversion cannot overflow.
inline int32_t quantize(float v, float lo, float hi) {
  if (!(v == v)) return 0;
  if (v <= lo) return int32_t(lo);
  if (v >= hi) return int32_t(hi);
  return int32_t(std::lrintf(v));
}
}  // namespace

void int16_to_float(const int16_t* in, float* out, size_t n) {
  for (size_t i = 0; i < n; ++i) out[i] = float(in[i]) * kInv16;
}

void int24_packed_to_float(const uint8_t* in, float* out, size_t n) {
  for (size_t i = 0; i < n; ++i, in += 3) {
    // Place the 24 bits in the top of an int32, then shift down to sign-extend.
    const uint32_t u = (uint32_t(in[0]) << 8) | (uint32_t(in[1]) << 16) | (uint32_t(in[2]) << 24);
    out[i] = float(int32_t(u) >> 8) * kInv24;
  }
}

void int32_to_float(const int32_t* in, float* out, size_t n) {
  for (size_t i = 0; i < n; ++i) out[i] = float(double(in[i]) * kInv32);
}

float Tpdf::next() {
  // Two uniforms in (0, 1) from the top 24 bits; their sum minus 1 is in (-1, 1).
  const double u1 = (double(step() >> 8) + 0.5) / 16777216.0;
  const double u2 = (double(step() >> 8) + 0.5) / 16777216.0;
  return float(u1 + u2 - 1.0);
}

void float_to_int16_tpdf(const float* in, int16_t* out, size_t n, Tpdf& d) {
  for (size_t i = 0; i < n; ++i)
    out[i] = int16_t(quantize(in[i] * 32768.0f + d.next(), -32768.0f, 32767.0f));
}

void float_to_int24_packed(const float* in, uint8_t* out, size_t n) {
  for (size_t i = 0; i < n; ++i, out += 3) {
    const int32_t v = quantize(in[i] * 8388608.0f, -8388608.0f, 8388607.0f);
    out[0] = uint8_t(v & 0xFF);
    out[1] = uint8_t((v >> 8) & 0xFF);
    out[2] = uint8_t((v >> 16) & 0xFF);
  }
}

void extract_channels(const float* in, uint32_t in_ch, const uint16_t* map, uint32_t out_ch,
                      float* out, size_t frames) {
  for (size_t f = 0; f < frames; ++f) {
    const float* src = in + f * in_ch;
    float* dst = out + f * out_ch;
    for (uint32_t k = 0; k < out_ch; ++k) dst[k] = src[map[k]];
  }
}

}  // namespace sl
