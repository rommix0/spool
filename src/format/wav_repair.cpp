#include "format/wav_repair.h"

#include <cstring>
#include <vector>

#include "format/wav_reader.h"
#include "format/wav_writer.h"

namespace sl {

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

}  // namespace

std::optional<uint64_t> repair_wav(IFileSystem& fs, const std::wstring& path, std::wstring* err) {
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

  const uint64_t frames = (fsize - info->data_offset) / info->block_align;
  const uint64_t data_bytes = frames * info->block_align;
  std::vector<uint8_t> hdr(static_cast<size_t>(info->data_offset));
  if (!f->read_at(0, hdr.data(), hdr.size())) return fail(L"cannot read header");
  const bool rf64 = info->riff_id == "RF64" || data_bytes > WavParams{1, 1}.rf64_threshold;
  if (!wav_patch_header(hdr, data_bytes, frames, fsize, rf64))
    return fail(L"unrecognised header layout");
  if (!f->write_at(0, hdr.data(), hdr.size()) || !f->flush()) return fail(L"cannot rewrite header");
  f->close();
  return frames;
}

}  // namespace sl
