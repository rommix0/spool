#include "session/meter.h"

#include <algorithm>
#include <cmath>

namespace sl {
namespace {
constexpr float kFloorDb = -60.0f;
constexpr float kClipLevel = 0.988553f;  // -0.1 dBFS
constexpr uint64_t kHoldMs = 2000;
constexpr uint64_t kDeadMs = 300;  // no new frames for this long: capture is dead
constexpr size_t kChunkFrames = 4096;

float to_db(double lin) {
  if (!(lin > 0.0)) return kFloorDb;
  const double db = 20.0 * std::log10(lin);
  return db < kFloorDb ? kFloorDb : static_cast<float>(db);
}
}  // namespace

MeterReader::MeterReader(SampleRing& ring, uint32_t rate, IClock& clock)
    : ring_(ring),
      rate_(rate),
      clock_(clock),
      channels_(ring.channels()),
      cursor_(ring.write_index()),
      window_(std::max<size_t>(1, static_cast<size_t>(rate) * 3 / 10)),
      scratch_(kChunkFrames * ring.channels()),
      ch_(ring.channels()),
      out_(ring.channels(), ChannelMeter{kFloorDb, kFloorDb, kFloorDb, 0}) {
  for (auto& c : ch_) c.squares.assign(window_, 0.0f);
  last_frames_ms_ = clock_.steady_ms();
}

void MeterReader::resync(SampleIndex to) {
  // Never below the oldest valid frame (a jump_to raises that floor past write_index - margin).
  cursor_ = std::max<SampleIndex>({to, ring_.oldest_valid(), 0});
  for (auto& c : ch_) c.in_clip = false;  // the gap breaks any clip run
}

void MeterReader::update() {
  const SampleIndex margin = rate_ / 10;
  SampleIndex end = ring_.write_index();
  if (end - cursor_ > static_cast<SampleIndex>(ring_.capacity())) resync(end - margin);

  bool saw_frames = false;
  bool resynced_in_loop = false;

  while (cursor_ < end) {
    const size_t want = static_cast<size_t>(std::min<SampleIndex>(end - cursor_, kChunkFrames));
    const auto r = ring_.read(cursor_, scratch_.data(), want);
    if (r.overrun) {
      // A second overrun in one update means the producer is still racing us: try next update.
      if (resynced_in_loop) break;
      resynced_in_loop = true;
      resync(ring_.write_index() - margin);
      end = ring_.write_index();
      continue;
    }
    if (r.frames == 0) break;
    if (!saw_frames) {
      saw_frames = true;
      for (auto& c : ch_) c.peak = 0.0f;  // new window; with no new frames keep the last reading
    }
    for (uint32_t k = 0; k < channels_; ++k) {
      Channel& c = ch_[k];
      size_t pos = pos_;
      for (size_t i = 0; i < r.frames; ++i) {
        float a = std::fabs(scratch_[i * channels_ + k]);
        if (!std::isfinite(a)) a = 0.0f;  // NaN/Inf must not poison the running sum
        if (a > c.peak) c.peak = a;
        if (a >= kClipLevel) {
          if (!c.in_clip) { ++out_[k].clips; c.in_clip = true; }
        } else {
          c.in_clip = false;
        }
        const float sq = a * a;
        c.sum += static_cast<double>(sq) - static_cast<double>(c.squares[pos]);
        c.squares[pos] = sq;
        if (++pos == window_) {
          pos = 0;
          double sum = 0;  // exact recompute once per window wrap bounds running-sum drift
          for (const float v : c.squares) sum += static_cast<double>(v);
          c.sum = sum;
        }
      }
    }
    pos_ = (pos_ + r.frames) % window_;
    filled_ = std::min(window_, filled_ + r.frames);
    cursor_ += static_cast<SampleIndex>(r.frames);
  }

  const uint64_t now = clock_.steady_ms();
  if (saw_frames) {
    last_frames_ms_ = now;
  } else if (now - last_frames_ms_ > kDeadMs) {
    // Capture is dead: report the floor instead of freezing the last levels.
    for (auto& c : ch_) {
      c.peak = 0.0f;
      c.sum = 0.0;
      std::fill(c.squares.begin(), c.squares.end(), 0.0f);
    }
    filled_ = 0;
  }
  for (uint32_t k = 0; k < channels_; ++k) {
    Channel& c = ch_[k];
    ChannelMeter& o = out_[k];
    o.peak_db = to_db(c.peak);
    if (c.peak >= c.hold || now - c.hold_set_ms >= kHoldMs) {
      c.hold = c.peak;
      c.hold_set_ms = now;
    }
    o.hold_db = to_db(c.hold);
    const double mean = filled_ ? std::max(0.0, c.sum) / static_cast<double>(filled_) : 0.0;
    o.rms_db = to_db(std::sqrt(mean));
  }
}

void MeterReader::reset_clips() {
  for (auto& o : out_) o.clips = 0;
}

}  // namespace sl
