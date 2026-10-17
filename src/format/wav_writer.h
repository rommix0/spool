#pragma once
// BWF writer with RF64 upgrade. The header is rewritten at every flush(), so a
// crash leaves a playable file; repair_wav() recovers the unflushed tail.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "format/encoder.h"
#include "io/file_system.h"

namespace sl {

enum class WavDepth { Int16, Int24, Float32 };

struct BextInfo {
  std::string description, originator = "SoundLogger", originator_reference;
  std::string date /*yyyy-mm-dd*/, time /*hh:mm:ss*/;
  uint64_t time_reference = 0;
};

struct WavParams {
  uint32_t sample_rate;
  uint16_t channels;
  WavDepth depth = WavDepth::Int16;
  BextInfo bext;
  uint64_t rf64_threshold = 0xFFFFFFFFull - 1024 * 1024;  // tests lower it
};

// Fails (nullptr, *err set) on bad params or if the file cannot be created (never overwrites).
std::unique_ptr<IEncoderFile> open_wav(IFileSystem&, const std::wstring& path, const WavParams&,
                                       std::wstring* err);

// Shared with repair_wav. `hdr` is the file image [0, data_offset) in this writer's layout
// (RIFF | JUNK or ds64 | fmt | [fact] | bext | data header). Patches the RIFF/data sizes, the
// fact sample count and the ds64 payload; upgrades to RF64 when `rf64`. file_bytes is the
// total file length. False if the image is not in the expected layout.
bool wav_patch_header(std::vector<uint8_t>& hdr, uint64_t data_bytes, uint64_t frames,
                      uint64_t file_bytes, bool rf64);

}  // namespace sl
