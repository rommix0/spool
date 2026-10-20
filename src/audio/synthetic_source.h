#pragma once
// Test double for the capture side (§11): a ramp, sine or silence pattern, delivered
// either by pump() (manual) or by a real-time thread in 10 ms blocks. Device loss and
// return (also with a new format) are simulated on demand.
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include "audio/capture_source.h"

namespace spool {

class SyntheticSource : public ICaptureSource {
 public:
  enum class Pattern { Ramp, Sine, Silence };
  // The source is open after construction. manual: frames only via pump(); otherwise a
  // real-time thread delivers 10 ms blocks while started.
  SyntheticSource(StreamFormat, Pattern, bool manual);
  ~SyntheticSource() override;
  SyntheticSource(const SyntheticSource&) = delete;
  SyntheticSource& operator=(const SyntheticSource&) = delete;

  bool open(const CaptureConfig&, std::wstring* err) override;  // the device id is ignored
  StreamFormat format() const override;
  bool start(CaptureCallbacks*) override;  // false while lost
  void stop() override;

  // Delivers `frames` on the calling thread (which then plays the capture thread);
  // nothing while not started. The pattern's frame counter continues across loss and
  // return, so a ramp shows exactly which source frames reached a file.
  void pump(size_t frames);
  // Stops producing, then reports on_device_lost.
  void simulate_loss();
  // Opens again with fmt (not started), then reports on_device_returned(fmt).
  void simulate_return(StreamFormat fmt);
  // The next delivered block carries discontinuity = true.
  void simulate_discontinuity() { discontinuity_.store(true); }

 private:
  void deliver(size_t frames);  // with m_ held
  void join_thread();

  const Pattern pattern_;
  const bool manual_;
  mutable std::mutex m_;  // serializes delivery against stop/loss/return
  std::mutex thread_m_;   // serializes starting and joining thread_
  StreamFormat fmt_;
  bool open_ = true, started_ = false;
  CaptureCallbacks* cb_ = nullptr;  // the last start()'s callbacks; also gets loss/return
  uint64_t pos_ = 0;                // pattern frame counter
  std::vector<float> buf_;
  std::atomic<bool> discontinuity_{false};
  std::atomic<bool> run_{false};
  std::thread thread_;
};

}  // namespace spool
