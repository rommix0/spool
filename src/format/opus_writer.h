#pragma once
// Ogg Opus writer on libopusenc. confirmed_frames() is derived from the granule
// position of the last Ogg page that was handed to the file before the last
// flush(): it rounds down, so a failover seam can repeat up to one page of
// audio but never drops any. Markers and truncate_to are ignored (Opus markers
// are recorded in the manifest only).
#include <cstdint>
#include <memory>
#include <string>

#include "format/encoder.h"
#include "io/file_system.h"

namespace spool {

struct OpusParams {
  uint32_t input_rate;
  uint16_t channels;      // 1 or 2
  uint32_t bitrate = 0;   // 0 -> 128000 stereo / 64000 mono
};

// Fails (nullptr, *err set) on bad params or if the file cannot be created (never overwrites).
std::unique_ptr<IEncoderFile> open_opus(IFileSystem&, const std::wstring& path, const OpusParams&,
                                        std::wstring* err);

}  // namespace spool
