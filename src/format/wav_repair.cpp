#include "format/wav_repair.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "format/wav_reader.h"
#include "format/wav_writer.h"

namespace spool {

namespace {

// True if the bytes from `pos` to the end of the file are a clean run of the chunks
// our finalize() writes (cue, LIST, JUNK) and nothing else, i.e. the file was finalized.
// Audio never parses as such: silence has chunk id 0 and anything else won't match.
bool is_finalized_tail(IFile& f, uint64_t pos, uint64_t fsize) {
  static const char* const kKnown[] = {"cue ", "LIST", "JUNK"};
  while (pos < fsize) {
    uint8_t h[8];
    if (pos + 8 > fsize || !f.read_at(pos, h, 8)) return false;
    bool known = false;
    for (const char* id : kKnown) known = known || std::memcmp(h, id, 4) == 0;
    uint32_t size;
    std::memcpy(&size, h + 4, 4);
    if (!known || pos + 8 + size > fsize) return false;
    pos += 8 + uint64_t(size) + (size & 1);
  }
  return true;
}

// Makes [pos, end of file) one JUNK chunk (at least 8 bytes, even length) and returns the
// new file length. pos == file length leaves the file alone.
std::optional<uint64_t> wrap_in_junk(IFile& f, uint64_t pos) {
  const uint64_t fsize = f.size();
  if (pos >= fsize) {
    if (pos > fsize) {  // the data chunk's pad byte is missing
      const uint8_t zero = 0;
      if (!f.write(&zero, 1)) return std::nullopt;
    }
    return f.size();
  }
  uint64_t rest = fsize - pos;
  const uint8_t zeros[8] = {};
  if (rest < 8 || (rest & 1)) {
    const size_t add = rest < 8 ? size_t(8 - rest) : 1;
    if (!f.write(zeros, add)) return std::nullopt;
    rest += add;
  }
  uint8_t h[8] = {'J', 'U', 'N', 'K'};
  const uint32_t size = uint32_t(std::min<uint64_t>(rest - 8, 0xFFFFFFFFu));
  std::memcpy(h + 4, &size, 4);
  if (!f.write_at(pos, h, 8)) return std::nullopt;
  return f.size();
}

}  // namespace

std::optional<uint64_t> repair_wav(IFileSystem& fs, const std::wstring& path, std::wstring* err,
                                   std::optional<uint64_t> max_frames) {
  auto fail = [&](const wchar_t* msg) -> std::optional<uint64_t> {
    if (err) *err = msg;
    return std::nullopt;
  };
  std::wstring ferr;
  auto f = fs.open_existing(path, &ferr);
  if (!f) {
    if (err) *err = ferr.empty() ? L"cannot open file" : ferr;
    return std::nullopt;
  }
  const auto info = read_wav_info(*f);
  if (!info || info->block_align == 0) return fail(L"not a readable WAV file");
  const uint64_t fsize = f->size();
  if (fsize < info->data_offset) return fail(L"file ends inside the header");

  const uint64_t claimed_end = info->data_offset + info->data_bytes + (info->data_bytes & 1);
  if (claimed_end < fsize && is_finalized_tail(*f, claimed_end, fsize)) return info->frames;

  uint64_t frames = (fsize - info->data_offset) / info->block_align;
  uint64_t file_bytes = fsize;
  if (max_frames && *max_frames < frames) frames = *max_frames;
  const uint64_t data_bytes = frames * info->block_align;
  if (max_frames) {
    const uint64_t data_end = info->data_offset + data_bytes;
    if ((data_bytes & 1) && data_end < fsize) {  // zero the pad byte the JUNK chunk follows
      const uint8_t zero = 0;
      if (!f->write_at(data_end, &zero, 1)) return fail(L"cannot write the file");
    }
    const auto len = wrap_in_junk(*f, data_end + (data_bytes & 1));
    if (!len) return fail(L"cannot write the file");
    file_bytes = *len;
  }
  std::vector<uint8_t> hdr(static_cast<size_t>(info->data_offset));
  if (!f->read_at(0, hdr.data(), hdr.size())) return fail(L"cannot read header");
  const bool rf64 = info->riff_id == "RF64" || data_bytes > WavParams{1, 1}.rf64_threshold ||
                    file_bytes - 8 > 0xFFFFFFFFull;
  if (!wav_patch_header(hdr, data_bytes, frames, file_bytes, rf64))
    return fail(L"unrecognised header layout");
  if (!f->write_at(0, hdr.data(), hdr.size()) || !f->flush()) return fail(L"cannot rewrite header");
  f->close();
  return frames;
}

}  // namespace spool
