#pragma once
#include <cstdint>
#include <optional>
#include <string>

#include "io/file_system.h"

namespace sl {

// Fixes the header of a WAV left by a crash: data = everything after the data
// header (whole frames), RF64 if needed. A cleanly finalized file is left alone.
// max_frames (a file that failed over): data ends there at most; the bytes after it
// (already in the next file) are wrapped in a JUNK chunk, as finalize() does.
// Returns the frame count, nullopt (and *err) on failure.
std::optional<uint64_t> repair_wav(IFileSystem&, const std::wstring& path, std::wstring* err,
                                   std::optional<uint64_t> max_frames = std::nullopt);

}  // namespace sl
