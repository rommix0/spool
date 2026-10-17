#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

#include "audio/wasapi_source.h"
#include "test.h"

using sl::CaptureConfig;
using sl::CaptureMode;
using sl::WasapiSource;

namespace {

struct Counter : sl::CaptureCallbacks {
  std::atomic<uint64_t> frames{0};
  std::atomic<int> lost{0}, returned{0}, disc{0};
  std::atomic<bool> bad{false};
  uint32_t channels = 0;
  void on_frames(const float* p, size_t n, bool discontinuity) override {
    for (size_t i = 0; i < n * channels; ++i)
      if (!std::isfinite(p[i])) bad = true;
    frames += n;
    if (discontinuity) ++disc;
  }
  void on_device_lost() override { ++lost; }
  void on_device_returned(sl::StreamFormat) override { ++returned; }
};

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

}  // namespace

TEST(wasapi_list_devices_does_not_throw) {
  auto d = sl::list_capture_devices();
  CHECK(d.size() < 64);
  for (const auto& i : d) CHECK(!i.id.empty());
}

// A running stream that delivers no packet for 5 s (silent without an error) counts as
// device loss: the worker closes it, reports the loss and reopens it.
TEST(wasapi_capture_silence_timeout) {
  CHECK_EQ(sl::kCaptureSilenceTimeoutMs, uint64_t(5000));
  CHECK(!sl::capture_timed_out(1000, 1000));
  CHECK(!sl::capture_timed_out(1000, 5999));
  CHECK(sl::capture_timed_out(1000, 6000));
  CHECK(sl::capture_timed_out(1000, 60000));
  CHECK(!sl::capture_timed_out(1000, 500));  // never on a clock that reads earlier
}

TEST(wasapi_open_unknown_device_fails_cleanly) {
  WasapiSource src;
  CaptureConfig cfg;
  cfg.device_id = L"{0.0.1.00000000}.{00000000-0000-0000-0000-000000000000}";
  std::wstring err;
  CHECK(!src.open(cfg, &err));
  CHECK(!err.empty());
  Counter cb;
  CHECK(!src.start(&cb));
  src.stop();
  CHECK(sl::exclusive_rates(cfg.device_id).empty());
}

namespace {

// Captures `ms` of audio from an already opened source and checks the frame count.
void check_capture(WasapiSource& src, int ms) {
  const sl::StreamFormat f = src.format();
  Counter cb;
  cb.channels = f.channels;
  CHECK(src.start(&cb));
  sleep_ms(ms);
  src.stop();
  const uint64_t got = cb.frames.load();
  const uint64_t expect = uint64_t(f.sample_rate) * ms / 1000;
  CHECK(got > expect * 9 / 10);
  CHECK(got < expect * 11 / 10);
  CHECK(!cb.bad.load());
  CHECK_EQ(cb.lost.load(), 0);
  CHECK_EQ(cb.returned.load(), 0);
  // WASAPI flags the first packet after every start as a discontinuity: not reported.
  CHECK_EQ(cb.disc.load(), 0);

  // No frames after stop(), and the source can be started again.
  const uint64_t after_stop = cb.frames.load();
  sleep_ms(50);
  CHECK_EQ(cb.frames.load(), after_stop);
  CHECK(src.start(&cb));
  sleep_ms(100);
  src.stop();
  CHECK(cb.frames.load() > after_stop);
  CHECK_EQ(cb.disc.load(), 0);
  CHECK(src.format() == f);
}

}  // namespace

TEST(wasapi_raw_capture_one_second) {
  const std::wstring def = sl::default_capture_device_id();
  if (def.empty()) SKIP("no capture device");
  WasapiSource src;
  CaptureConfig cfg;  // default device, raw
  std::wstring err;
  if (!src.open(cfg, &err)) SKIP("default capture device cannot be opened");
  const sl::StreamFormat f = src.format();
  CHECK(f.sample_rate >= 8000);
  CHECK(f.channels >= 1 && f.channels <= 8);
  // The source is bound to the endpoint that was default at open() (not to "whatever is default").
  CHECK(src.device_id() == def);
  CHECK(!src.exclusive());
  std::printf("  raw active: %s\n", src.raw_active() ? "yes" : "no (endpoint reports no raw support)");
  for (const auto& d : sl::list_capture_devices())
    if (d.id == def && d.mix_format.channels != 0) CHECK_EQ(f.channels, d.mix_format.channels);
  check_capture(src, 1000);
}

TEST(wasapi_exclusive_capture_one_second) {
  const std::wstring def = sl::default_capture_device_id();
  if (def.empty()) SKIP("no capture device");
  const auto rates = sl::exclusive_rates(def);
  if (rates.empty()) SKIP("exclusive mode not available");
  WasapiSource src;
  CaptureConfig cfg;
  cfg.mode = CaptureMode::Exclusive;
  cfg.sample_rate = rates[0];
  std::wstring err;
  if (!src.open(cfg, &err)) SKIP("exclusive open failed (device busy or not allowed)");
  CHECK_EQ(src.format().sample_rate, rates[0]);
  CHECK(src.raw_active());
  CHECK(src.exclusive());
  CHECK(src.device_id() == def);
  check_capture(src, 1000);
}

TEST(wasapi_failed_open_clears_state) {
  WasapiSource src;
  std::wstring err;
  src.open(CaptureConfig{}, &err);  // may fail without a device; then there is no earlier state to clear
  CaptureConfig cfg;
  cfg.device_id = L"{0.0.1.00000000}.{00000000-0000-0000-0000-000000000000}";
  CHECK(!src.open(cfg, &err));
  CHECK(src.format() == sl::StreamFormat{});
  CHECK(!src.raw_active());
  CHECK(!src.exclusive());
  CHECK(src.device_id().empty());
}
