#include "format/wav_reader.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace sl {

namespace {

constexpr uint32_t kMax32 = 0xFFFFFFFFu;
constexpr uint64_t kMaxMetaChunk = 16u << 20;  // cue / LIST payloads beyond this are ignored

uint16_t get16(const uint8_t* p) {
  uint16_t x;
  std::memcpy(&x, p, 2);
  return x;
}
uint32_t get32(const uint8_t* p) {
  uint32_t x;
  std::memcpy(&x, p, 4);
  return x;
}
uint64_t get64(const uint8_t* p) {
  uint64_t x;
  std::memcpy(&x, p, 8);
  return x;
}

std::string text_field(const uint8_t* p, size_t width) {
  size_t n = 0;
  while (n < width && p[n]) ++n;
  return std::string(reinterpret_cast<const char*>(p), n);
}

std::wstring from_utf8(const uint8_t* p, size_t n) {
  while (n && p[n - 1] == 0) --n;
  if (n == 0) return {};
  const int w = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(p), int(n), nullptr, 0);
  std::wstring s(size_t(w > 0 ? w : 0), L'\0');
  if (w > 0) MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(p), int(n), s.data(), w);
  return s;
}

void parse_bext(const std::vector<uint8_t>& b, BextInfo& out) {
  out.description = text_field(b.data(), 256);
  out.originator = text_field(b.data() + 256, 32);
  out.originator_reference = text_field(b.data() + 288, 32);
  out.date = text_field(b.data() + 320, 10);
  out.time = text_field(b.data() + 330, 8);
  out.time_reference = uint64_t(get32(b.data() + 338)) | (uint64_t(get32(b.data() + 342)) << 32);
}

}  // namespace

std::optional<WavInfo> read_wav_info(IFile& f) {
  const uint64_t fsize = f.size();
  uint8_t head[12];
  if (fsize < 12 || !f.read_at(0, head, 12)) return std::nullopt;
  const bool rf64 = std::memcmp(head, "RF64", 4) == 0;
  if ((!rf64 && std::memcmp(head, "RIFF", 4) != 0) || std::memcmp(head + 8, "WAVE", 4) != 0)
    return std::nullopt;

  WavInfo info{};
  info.riff_id = rf64 ? "RF64" : "RIFF";
  bool have_fmt = false, have_data = false, have_ds64 = false;
  uint64_t ds64_data = 0;
  std::vector<std::pair<uint32_t, uint32_t>> cues;       // id, sample offset
  std::vector<std::pair<uint32_t, std::wstring>> labels;  // cue id, text

  uint64_t pos = 12;
  while (pos + 8 <= fsize) {
    uint8_t ch[8];
    if (!f.read_at(pos, ch, 8)) break;
    const uint32_t size32 = get32(ch + 4);
    const uint64_t payload = pos + 8;
    const uint64_t avail = fsize - payload;

    if (std::memcmp(ch, "data", 4) == 0) {
      if (!have_data) {
        uint64_t claim = size32;
        if (rf64 && size32 == kMax32 && have_ds64) claim = ds64_data;
        info.data_offset = payload;
        info.data_bytes = std::min(claim, avail);
        have_data = true;
        pos = payload + info.data_bytes + (info.data_bytes & 1);
      } else {  // a second data chunk is not ours: skip it
        pos = payload + std::min<uint64_t>(size32, avail) + (size32 & 1);
      }
      continue;
    }

    const uint64_t size = size32;
    if (size > avail) break;  // truncated or garbage after the claimed data
    std::vector<uint8_t> body;
    auto load = [&](uint64_t want_min) {
      if (size < want_min || size > kMaxMetaChunk) return false;
      body.resize(size_t(size));
      return size == 0 || f.read_at(payload, body.data(), size_t(size));
    };

    if (std::memcmp(ch, "ds64", 4) == 0) {
      if (load(24)) {
        ds64_data = get64(body.data() + 8);
        have_ds64 = true;
      }
    } else if (std::memcmp(ch, "fmt ", 4) == 0) {
      if (load(16)) {
        info.format_tag = get16(body.data());
        info.channels = get16(body.data() + 2);
        info.sample_rate = get32(body.data() + 4);
        info.block_align = get16(body.data() + 12);
        info.bits = get16(body.data() + 14);
        have_fmt = true;
      }
    } else if (std::memcmp(ch, "bext", 4) == 0) {
      if (load(346)) parse_bext(body, info.bext);
    } else if (std::memcmp(ch, "cue ", 4) == 0) {
      if (load(4)) {
        const uint32_t n = get32(body.data());
        for (uint32_t i = 0; i < n && 4 + 24ull * (i + 1) <= body.size(); ++i) {
          const uint8_t* e = body.data() + 4 + 24ull * i;
          cues.emplace_back(get32(e), get32(e + 20));
        }
      }
    } else if (std::memcmp(ch, "LIST", 4) == 0) {
      if (load(4) && std::memcmp(body.data(), "adtl", 4) == 0) {
        for (size_t p = 4; p + 8 <= body.size();) {
          const uint32_t sz = get32(body.data() + p + 4);
          if (sz > body.size() - p - 8) break;
          if (std::memcmp(body.data() + p, "labl", 4) == 0 && sz >= 4)
            labels.emplace_back(get32(body.data() + p + 8), from_utf8(body.data() + p + 12, sz - 4));
          p += 8 + size_t(sz) + (sz & 1);
        }
      }
    }
    pos = payload + size + (size & 1);
  }

  if (!have_fmt || !have_data) return std::nullopt;
  info.frames = info.block_align ? info.data_bytes / info.block_align : 0;
  for (const auto& c : cues) {
    Marker m{c.second, {}};
    for (const auto& l : labels)
      if (l.first == c.first) {
        m.name = l.second;
        break;
      }
    info.markers.push_back(std::move(m));
  }
  return info;
}

}  // namespace sl
