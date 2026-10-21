#pragma once
// Rewrites the markers of a finished WAV file in place (a marker renamed after its file
// was finalized).
#include <string>
#include <vector>

#include "format/encoder.h"
#include "io/file_system.h"

namespace spool {

// Replaces the cue and LIST/adtl chunks after the audio with `markers`; the audio is
// never touched. Only a file in this writer's layout whose data chunk is followed by
// nothing but cue, LIST and JUNK chunks is changed. A shorter tail leaves its leftover
// bytes in a JUNK chunk (the file never shrinks); a longer one extends the file. The
// RIFF (or RF64 ds64) size is patched last, then the file is flushed. False (and *err)
// if the file can't be opened, isn't such a WAV, or a write fails.
bool rewrite_wav_markers(IFileSystem&, const std::wstring& path, const std::vector<Marker>& markers,
                         std::wstring* err);

}  // namespace spool
