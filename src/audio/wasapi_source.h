#pragma once
// WASAPI capture source (§5): event-driven capture in raw shared mode (no Windows
// processing) or exclusive mode, delivered as float32. Device loss (also a stream that
// delivers nothing for 5 s) is reported once per loss; when the device comes back the
// source reopens it (not started) and reports on_device_returned. The callbacks must
// outlive the source and must not call back into it.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "audio/capture_source.h"

namespace sl {

struct DeviceInfo {
  std::wstring id, name;
  StreamFormat mix_format;
};

// Active capture endpoints. Empty (never throws) when there are none or enumeration fails.
std::vector<DeviceInfo> list_capture_devices();
// The default capture endpoint's id; empty when there is none.
std::wstring default_capture_device_id();
// Which of 44100, 48000, 88200, 96000, 176400, 192000 the device accepts in exclusive mode
// (an empty id means the default device).
std::vector<uint32_t> exclusive_rates(const std::wstring& id);

// A running capture stream that delivers no packet for this long has gone silent without an
// error: it is treated as device loss (closed, reported, reopened).
constexpr uint64_t kCaptureSilenceTimeoutMs = 5000;
// True when the last packet arrived at least kCaptureSilenceTimeoutMs before now_ms.
bool capture_timed_out(uint64_t last_packet_ms, uint64_t now_ms);

class WasapiImpl;

class WasapiSource : public ICaptureSource {
 public:
  WasapiSource();
  ~WasapiSource() override;
  WasapiSource(const WasapiSource&) = delete;
  WasapiSource& operator=(const WasapiSource&) = delete;

  // Raw mode with an endpoint that lacks raw support falls back to normal shared mode
  // (see raw_active()). Fails for more than 8 channels. An empty device id means the
  // default capture device.
  bool open(const CaptureConfig&, std::wstring* err) override;
  StreamFormat format() const override;
  bool start(CaptureCallbacks*) override;  // false while lost or not open
  void stop() override;

  // True when the stream is unprocessed: RAW shared mode took effect, or exclusive mode.
  // False after a fallback to normal shared mode (the app should warn).
  bool raw_active() const;

  // The endpoint this source is bound to: resolved at open() (so an empty config id becomes
  // the then-default device) and kept across loss and return; empty when not open. It never
  // follows another device, so on_device_returned always means the same device.
  std::wstring device_id() const;

 private:
  std::unique_ptr<WasapiImpl> p_;
};

}  // namespace sl
