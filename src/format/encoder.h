#pragma once
// Common interface of the per-file encoders (WAV, Opus) the destination writer drives.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sl {

struct Marker {
  uint64_t frame_in_file;
  std::wstring name;
};

class IEncoderFile {
 public:
  virtual ~IEncoderFile() = default;
  virtual bool write(const float* interleaved, size_t frames) = 0;  // destination channel count
  virtual bool flush() = 0;  // on disk + header updated; sets confirmed_frames()
  virtual uint64_t confirmed_frames() const = 0;
  virtual uint64_t frames_written() const = 0;
  // truncate_to: the header claims only this many frames (failover).
  virtual bool finalize(const std::vector<Marker>& markers, std::optional<uint64_t> truncate_to) = 0;
  // Releases the handle without touching the file (stalled device). Never closes it.
  virtual void abandon() = 0;
};

}  // namespace sl
