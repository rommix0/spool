#include "format/wav_markers.h"

#include <cstring>

#include "format/wav_reader.h"
#include "format/wav_writer.h"

namespace spool {

namespace {

constexpr uint32_t kMax32 = 0xFFFFFFFFu;

uint32_t get32(const uint8_t* p) {
  uint32_t x;
  std::memcpy(&x, p, 4);
  return x;
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}

bool tail_chunk(const uint8_t* id) {
  return std::memcmp(id, "cue ", 4) == 0 || std::memcmp(id, "LIST", 4) == 0 || std::memcmp(id, "JUNK", 4) == 0;
}

}  // namespace

bool rewrite_wav_markers(IFileSystem& fs, const std::wstring& path, const std::vector<Marker>& markers,
                         std::wstring* err) {
  std::wstring e;
  std::unique_ptr<IFile> f = fs.open_existing(path, &e);
  if (!f) {
    if (err) *err = e.empty() ? L"can't open the file" : e;
    return false;
  }
  const auto fail = [&](const wchar_t* why) {
    if (err) *err = why;
    f->close();
    return false;
  };
  const std::optional<WavInfo> info = read_wav_info(*f);
  if (!info) return fail(L"not a WAV file");
  const uint64_t size = f->size();
  const uint64_t tail = info->data_offset + info->data_bytes + (info->data_bytes & 1);
  if (tail > size) return fail(L"the file is cut short");
  for (uint64_t pos = tail; pos < size;) {
    uint8_t ch[8];
    if (pos + 8 > size || !f->read_at(pos, ch, 8)) return fail(L"the file ends inside a chunk");
    if (!tail_chunk(ch)) return fail(L"the file has other chunks after the audio");
    const uint64_t sz = get32(ch + 4);
    pos += 8 + sz + (sz & 1);
    if (pos > size) return fail(L"the file ends inside a chunk");
  }

  std::vector<uint8_t> t = wav_marker_chunks(markers);
  uint64_t end = tail + t.size();
  if (end < size) {
    // The old tail was longer: the rest becomes a JUNK chunk, at least its 8-byte header.
    uint64_t rest = size - end;
    if (rest < 8) {
      const uint8_t zeros[8] = {};
      if (!f->write_at(size, zeros, size_t(8 - rest))) return fail(L"a write failed");
      rest = 8;
    }
    t.insert(t.end(), {'J', 'U', 'N', 'K'});
    put32(t, uint32_t(rest - 8));
    end = tail + (t.size() - 8) + rest;
  }
  if (!t.empty() && !f->write_at(tail, t.data(), t.size())) return fail(L"a write failed");

  std::vector<uint8_t> hdr(size_t(info->data_offset));
  if (!f->read_at(0, hdr.data(), hdr.size())) return fail(L"can't read the header");
  const bool rf64 = info->riff_id == "RF64" || end - 8 > kMax32;
  if (!wav_patch_header(hdr, info->data_bytes, info->frames, end, rf64))
    return fail(L"the header isn't in the layout Spool writes");
  if (!f->write_at(0, hdr.data(), hdr.size()) || !f->flush()) return fail(L"a write failed");
  f->close();
  return true;
}

}  // namespace spool
