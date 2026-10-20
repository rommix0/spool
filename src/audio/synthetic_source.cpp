#include "audio/synthetic_source.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "audio/ramp.h"

namespace spool {

namespace {
constexpr size_t kBlockFrames = 4096;  // pump() delivers in blocks of at most this
constexpr double kSineHz = 1000.0, kSineAmp = 0.5;
}  // namespace

SyntheticSource::SyntheticSource(StreamFormat fmt, Pattern pattern, bool manual)
    : pattern_(pattern), manual_(manual), fmt_(fmt) {
  buf_.resize(kBlockFrames * std::max<uint16_t>(fmt.channels, 1));
}

SyntheticSource::~SyntheticSource() { stop(); }

bool SyntheticSource::open(const CaptureConfig&, std::wstring*) {
  std::lock_guard<std::mutex> lk(m_);
  open_ = true;
  return true;
}

StreamFormat SyntheticSource::format() const {
  std::lock_guard<std::mutex> lk(m_);
  return fmt_;
}

bool SyntheticSource::start(CaptureCallbacks* cb) {
  {
    std::lock_guard<std::mutex> lk(m_);
    if (!open_ || !cb) return false;
    cb_ = cb;
    if (started_) return true;
    started_ = true;
  }
  if (!manual_) {
    std::lock_guard<std::mutex> tl(thread_m_);
    run_.store(false);
    if (thread_.joinable()) thread_.join();  // one that ended on its own (loss or stop)
    run_.store(true);
    thread_ = std::thread([this] {
      auto next = std::chrono::steady_clock::now();
      while (run_.load()) {
        {
          std::lock_guard<std::mutex> lk(m_);
          if (!started_) break;
          deliver(fmt_.sample_rate / 100);  // 10 ms
        }
        next += std::chrono::milliseconds(10);
        std::this_thread::sleep_until(next);
      }
    });
  }
  return true;
}

void SyntheticSource::stop() {
  {
    std::lock_guard<std::mutex> lk(m_);
    started_ = false;
  }
  join_thread();
}

void SyntheticSource::join_thread() {
  std::lock_guard<std::mutex> tl(thread_m_);
  run_.store(false);
  if (thread_.joinable()) thread_.join();
}

void SyntheticSource::pump(size_t frames) {
  std::lock_guard<std::mutex> lk(m_);
  if (!started_) return;
  deliver(frames);
}

void SyntheticSource::deliver(size_t frames) {
  const uint32_t ch = fmt_.channels;
  while (frames > 0 && cb_) {
    const size_t n = std::min(frames, kBlockFrames);
    for (size_t i = 0; i < n; ++i) {
      const uint64_t f = pos_ + i;
      for (uint32_t c = 0; c < ch; ++c) {
        float v = 0.0f;
        if (pattern_ == Pattern::Ramp) {
          v = ramp_value(SampleIndex(f), c);
        } else if (pattern_ == Pattern::Sine) {
          v = float(kSineAmp * std::sin(2.0 * 3.14159265358979323846 * kSineHz * double(f) / fmt_.sample_rate));
        }
        buf_[i * ch + c] = v;
      }
    }
    pos_ += n;
    cb_->on_frames(buf_.data(), n, discontinuity_.exchange(false));
    frames -= n;
  }
}

void SyntheticSource::simulate_loss() {
  CaptureCallbacks* cb;
  {
    std::lock_guard<std::mutex> lk(m_);
    open_ = started_ = false;
    cb = cb_;
  }
  join_thread();  // no frame is delivered after this
  if (cb) cb->on_device_lost();
}

void SyntheticSource::simulate_return(StreamFormat fmt) {
  CaptureCallbacks* cb;
  {
    std::lock_guard<std::mutex> lk(m_);
    fmt_ = fmt;
    buf_.assign(kBlockFrames * std::max<uint16_t>(fmt.channels, 1), 0.0f);
    open_ = true;
    started_ = false;
    cb = cb_;
  }
  if (cb) cb->on_device_returned(fmt);
}

}  // namespace spool
