#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

#include "audio/wasapi_source.h"
#include "test.h"

using sl::CaptureConfig;
using sl::CaptureMode;
using sl::WasapiSource;

namespace {

struct Counter : sl::CaptureCallbacks {
  std::atomic<uint64_t> frames{0};
  std::atomic<int> lost{0}, returned{0};
  std::atomic<bool> bad{false};
  uint32_t channels = 0;
  void on_frames(const float* p, size_t n, bool) override {
    for (size_t i = 0; i < n * channels; ++i)
      if (!std::isfinite(p[i])) bad = true;
    frames += n;
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

TEST(wasapi_raw_capture_one_second) {
  if (sl::default_capture_device_id().empty()) SKIP("no capture device");
  WasapiSource src;
  CaptureConfig cfg;  // default device, raw
  std::wstring err;
  if (!src.open(cfg, &err)) SKIP("default capture device cannot be opened");
  const sl::StreamFormat f = src.format();
  CHECK(f.sample_rate >= 8000);
  CHECK(f.channels >= 1 && f.channels <= 8);

  Counter cb;
  cb.channels = f.channels;
  CHECK(src.start(&cb));
  sleep_ms(1000);
  src.stop();
  const uint64_t got = cb.frames.load();
  CHECK(got > f.sample_rate * 9 / 10);
  CHECK(got < f.sample_rate * 11 / 10);
  CHECK(!cb.bad.load());
  CHECK_EQ(cb.lost.load(), 0);

  // No frames after stop(), and the source can be started again.
  const uint64_t after_stop = cb.frames.load();
  sleep_ms(50);
  CHECK_EQ(cb.frames.load(), after_stop);
  CHECK(src.start(&cb));
  sleep_ms(150);
  src.stop();
  CHECK(cb.frames.load() > after_stop);
  CHECK(src.format() == f);
}
