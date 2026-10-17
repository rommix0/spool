#pragma once
// Level meters for the dashboard. Reads the shared capture ring from its own
// cursor; update() and get() are called from the UI thread.
#include <cstdint>
#include <vector>

#include "core/clock.h"
#include "core/sample_ring.h"

namespace sl {

struct ChannelMeter {
  float peak_db;  // max |x| over the frames seen by the latest update(), floor -60
  float hold_db;  // highest peak held for 2 s, then falls back to the current peak
  float rms_db;   // RMS over the last 300 ms
  uint64_t clips; // session clip count; each run of |x| >= -0.1 dBFS counts once
};

class MeterReader {
 public:
  MeterReader(SampleRing& ring, uint32_t rate, IClock& clock);
  // Consumes the frames written since the previous call. If the reader is more
  // than a ring behind it skips to write_index - rate/10.
  void update();
  std::vector<ChannelMeter> get() const { return out_; }
  void reset_clips();

 private:
  struct Channel {
    std::vector<float> squares;  // ring of the last 300 ms of x*x
    double sum = 0;              // running sum of `squares`
    float peak = 0;              // linear
    float hold = 0;              // linear
    uint64_t hold_set_ms = 0;
    bool in_clip = false;
  };

  void resync(SampleIndex to);

  SampleRing& ring_;
  uint32_t rate_;
  IClock& clock_;
  uint32_t channels_;
  SampleIndex cursor_;
  size_t window_;        // frames in the RMS window
  size_t filled_ = 0;    // frames currently in the window (<= window_)
  size_t pos_ = 0;       // next slot in each channel's squares ring
  std::vector<float> scratch_;
  std::vector<Channel> ch_;
  std::vector<ChannelMeter> out_;
};

}  // namespace sl
