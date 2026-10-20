#pragma once
// WAV/BWF/RF64 header parser (used by repair and tests). Skips unknown chunks.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "format/wav_writer.h"
#include "io/file_system.h"

namespace spool {

struct WavInfo {
  std::string riff_id;  // "RIFF" or "RF64"
  uint16_t format_tag, channels, bits, block_align;
  uint32_t sample_rate;
  uint64_t data_offset, data_bytes /*as claimed, clamped to the file*/, frames;
  BextInfo bext;
  std::vector<Marker> markers;
};

// nullopt if the file is not a WAV with a fmt and data chunk.
std::optional<WavInfo> read_wav_info(IFile&);

}  // namespace spool
