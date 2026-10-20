#pragma once
// Format conversion between device sample formats and the float32 ring format.
// The *_to_float functions are called on the capture thread: no allocation, no locks.
#include <cstddef>
#include <cstdint>

namespace spool {

void int16_to_float(const int16_t* in, float* out, size_t n);
void int24_packed_to_float(const uint8_t* in, float* out, size_t n);  // 3-byte little-endian
void int32_to_float(const int32_t* in, float* out, size_t n);         // also 24-in-32 left-justified

// xorshift32 TPDF dither source: next() is triangular in (-1, 1) LSB.
class Tpdf {
 public:
  explicit Tpdf(uint32_t seed = 0x9E3779B9u) : s_(seed ? seed : 0x9E3779B9u) {}
  float next();

 private:
  uint32_t step() {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 17;
    s_ ^= s_ << 5;
    return s_;
  }
  uint32_t s_;
};

// Round to nearest, then clamp. Dither only on the 16-bit path.
void float_to_int16_tpdf(const float* in, int16_t* out, size_t n, Tpdf& d);
void float_to_int24_packed(const float* in, uint8_t* out, size_t n);

// Copies out_ch channels per frame: out[f*out_ch + k] = in[f*in_ch + map[k]].
void extract_channels(const float* in, uint32_t in_ch, const uint16_t* map, uint32_t out_ch,
                      float* out, size_t frames);

}  // namespace spool
