#pragma once
// Capture side of the pipeline: a source delivers float32 interleaved frames to its
// callbacks (§4.1) and reports device loss and return (§5.3).
#include <cstddef>
#include <cstdint>
#include <string>

#include "core/types.h"

namespace sl {

enum class CaptureMode { Raw, Exclusive };

struct CaptureConfig {
  std::wstring device_id;
  CaptureMode mode = CaptureMode::Raw;
  uint32_t sample_rate = 0;  // used only for Exclusive
};

struct CaptureCallbacks {
  virtual ~CaptureCallbacks() = default;
  // Capture thread: must not allocate, lock or do file I/O.
  virtual void on_frames(const float* interleaved, size_t frames, bool discontinuity) = 0;
  // The source stopped producing; it is closed until on_device_returned.
  virtual void on_device_lost() = 0;
  // The source is open again (possibly with a new format) but not started.
  virtual void on_device_returned(StreamFormat fmt) = 0;
};

class ICaptureSource {
 public:
  virtual ~ICaptureSource() = default;
  virtual bool open(const CaptureConfig&, std::wstring* err) = 0;
  virtual StreamFormat format() const = 0;
  // After a loss the manager calls start() again once the device returned.
  virtual bool start(CaptureCallbacks*) = 0;
  // Returns once no further on_frames call can happen.
  virtual void stop() = 0;
};

}  // namespace sl
