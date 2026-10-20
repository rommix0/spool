#pragma once
// Per-destination configuration and per-session parameters shared by the writers.
#include <cstdint>
#include <string>
#include <vector>

#include "core/types.h"
#include "format/wav_writer.h"

namespace spool {

enum class OutFormat { Wav, Opus };

struct DestinationConfig {
  std::wstring name = L"main";
  OutFormat format = OutFormat::Wav;
  WavDepth wav_depth = WavDepth::Int16;  // WAV only
  uint32_t opus_bitrate = 0;             // Opus only; 0 -> 128 kbps stereo / 64 kbps mono
  std::vector<uint16_t> channels;        // 0-based input channels; empty = all input channels
  std::vector<std::wstring> folders;     // failover chain, highest priority first
  uint32_t rotation_minutes = 30;        // 0 = no rotation
  std::wstring name_template = L"{session}_{date}_{time}_{n}{seg}";
};

struct SessionParams {
  std::wstring session_name = L"session", session_id;
  uint32_t rate = 48000;
  uint16_t input_channels = 2;
  SampleIndex start_sample = 0;
  WallTime start_wall;  // wall_for(s) = start_wall + (s - start_sample)*1000/rate
  uint32_t flush_ms = 2000, low_water_minutes = 10;
  uint64_t low_water_min_bytes = 1ull << 30;
  uint32_t stall_percent = 50;
  std::wstring prev_session;
};

// Wall-clock time of sample s, derived from the session start (not the PC clock).
inline WallTime wall_for(const SessionParams& p, SampleIndex s) {
  return WallTime{p.start_wall.utc_ms + (s - p.start_sample) * 1000 / int64_t(p.rate),
                  p.start_wall.local_offset_min};
}

}  // namespace spool
