#include "format/wav_writer.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

#include "core/sample_convert.h"

namespace spool {

namespace {

constexpr size_t kJunkOffset = 12;  // JUNK (later ds64) chunk header
constexpr size_t kFmtOffset = 48;   // 12 + 8 + 28
constexpr uint32_t kBextPayload = 602;
constexpr uint32_t kMax32 = 0xFFFFFFFFu;

void put16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(uint8_t(x));
  v.push_back(uint8_t(x >> 8));
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
  for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
void put_id(std::vector<uint8_t>& v, const char* id) { v.insert(v.end(), id, id + 4); }
// Fixed-width, NUL-padded text field (truncated if too long).
void put_text(std::vector<uint8_t>& v, const std::string& s, size_t width) {
  const size_t n = std::min(s.size(), width);
  v.insert(v.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
  v.insert(v.end(), width - n, uint8_t(0));
}

void set32(uint8_t* p, uint32_t x) { std::memcpy(p, &x, 4); }
void set64(uint8_t* p, uint64_t x) { std::memcpy(p, &x, 8); }
uint32_t get32(const uint8_t* p) {
  uint32_t x;
  std::memcpy(&x, p, 4);
  return x;
}

std::string to_utf8(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(size_t(n > 0 ? n : 0), '\0');
  if (n > 0)
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
  return s;
}

uint16_t bytes_per_sample(WavDepth d) { return d == WavDepth::Int16 ? 2 : d == WavDepth::Int24 ? 3 : 4; }

// RIFF + JUNK(ds64 reserve) + fmt + [fact] + bext + data header, sizes all zero.
std::vector<uint8_t> build_header(const WavParams& p) {
  const uint16_t bps = bytes_per_sample(p.depth);
  const uint16_t block_align = uint16_t(p.channels * bps);
  const bool is_float = p.depth == WavDepth::Float32;
  const bool extensible = p.channels > 2;
  std::vector<uint8_t> h;
  h.reserve(768);
  put_id(h, "RIFF");
  put32(h, 0);
  put_id(h, "WAVE");
  put_id(h, "JUNK");
  put32(h, 28);
  h.insert(h.end(), 28, uint8_t(0));

  put_id(h, "fmt ");
  put32(h, extensible ? 40 : is_float ? 18 : 16);
  put16(h, extensible ? 0xFFFE : is_float ? 3 : 1);
  put16(h, p.channels);
  put32(h, p.sample_rate);
  put32(h, p.sample_rate * block_align);
  put16(h, block_align);
  put16(h, uint16_t(bps * 8));
  if (extensible) {
    put16(h, 22);                 // cbSize
    put16(h, uint16_t(bps * 8));  // valid bits
    put32(h, 0);                  // channel mask
    put16(h, is_float ? 3 : 1);   // sub-format GUID {0000000x-0000-0010-8000-00AA00389B71}
    static const uint8_t guid_tail[14] = {0, 0, 0, 0, 0x10, 0, 0x80, 0, 0, 0xAA, 0, 0x38, 0x9B, 0x71};
    h.insert(h.end(), guid_tail, guid_tail + 14);
  } else if (is_float) {
    put16(h, 0);  // cbSize
  }
  if (is_float) {
    put_id(h, "fact");
    put32(h, 4);
    put32(h, 0);
  }

  put_id(h, "bext");
  put32(h, kBextPayload);
  put_text(h, p.bext.description, 256);
  put_text(h, p.bext.originator, 32);
  put_text(h, p.bext.originator_reference, 32);
  put_text(h, p.bext.date, 10);
  put_text(h, p.bext.time, 8);
  put32(h, uint32_t(p.bext.time_reference));
  put32(h, uint32_t(p.bext.time_reference >> 32));
  put16(h, 2);                                   // BWF version
  h.insert(h.end(), 64, uint8_t(0));             // UMID
  for (int i = 0; i < 5; ++i) put16(h, 0x7FFF);  // loudness fields: not used
  h.insert(h.end(), 180, uint8_t(0));            // reserved

  put_id(h, "data");
  put32(h, 0);
  return h;
}

// Pad byte (odd data) + cue + LIST/adtl, to be placed right after the data chunk.
std::vector<uint8_t> build_tail(uint64_t data_bytes, const std::vector<Marker>& markers) {
  std::vector<uint8_t> t;
  if (data_bytes & 1) t.push_back(0);
  const std::vector<uint8_t> m = wav_marker_chunks(markers);
  t.insert(t.end(), m.begin(), m.end());
  return t;
}

class WavFile final : public IEncoderFile {
 public:
  WavFile(std::unique_ptr<IFile> f, const WavParams& p)
      : file_(std::move(f)),
        threshold_(p.rf64_threshold),
        depth_(p.depth),
        channels_(p.channels),
        block_align_(uint32_t(p.channels * bytes_per_sample(p.depth))),
        hdr_(build_header(p)) {}

  bool start() { return file_->write(hdr_.data(), hdr_.size()); }

  bool write(const float* in, size_t frames) override {
    if (!file_ || bad_) return false;
    if (frames == 0) return true;
    const size_t n = frames * channels_;
    const void* out = in;
    size_t bytes = n * 4;
    if (depth_ == WavDepth::Int16) {
      s16_.resize(n);
      float_to_int16_tpdf(in, s16_.data(), n, dither_);
      out = s16_.data();
      bytes = n * 2;
    } else if (depth_ == WavDepth::Int24) {
      s24_.resize(n * 3);
      float_to_int24_packed(in, s24_.data(), n);
      out = s24_.data();
      bytes = n * 3;
    }
    if (!file_->write(out, bytes)) {
      bad_ = true;  // the file now has unknown trailing bytes; refuse further audio
      return false;
    }
    frames_ += frames;
    return true;
  }

  bool flush() override {
    if (!file_) return false;
    const uint64_t data_bytes = frames_ * block_align_;
    if (!write_header(data_bytes, frames_, hdr_.size() + data_bytes) || !file_->flush()) return false;
    confirmed_ = frames_;
    return true;
  }

  uint64_t confirmed_frames() const override { return confirmed_; }
  uint64_t frames_written() const override { return frames_; }

  bool finalize(const std::vector<Marker>& markers, std::optional<uint64_t> truncate_to) override {
    if (!file_) return false;
    const uint64_t frames = truncate_to ? std::min(*truncate_to, frames_) : frames_;
    const uint64_t data_bytes = frames * block_align_;
    const uint64_t end = hdr_.size() + data_bytes;

    const std::vector<uint8_t> tail = build_tail(data_bytes, markers);
    const uint64_t tail_end = end + tail.size();
    if (!tail.empty() && !file_->write_at(end, tail.data(), tail.size())) return false;

    // Bytes past the claimed end (discarded audio, torn writes) are wrapped in a
    // JUNK chunk so that RIFF size, chunk walk and file length all agree.
    const uint64_t size = file_->size();
    if (size > tail_end) {
      uint64_t rest = size - tail_end;
      if (rest < 8) {
        const uint8_t zeros[8] = {};
        if (!file_->write(zeros, 8 - size_t(rest))) return false;
        rest = 8;
      } else if (rest & 1) {
        const uint8_t zero = 0;
        if (!file_->write(&zero, 1)) return false;
        ++rest;
      }
      std::vector<uint8_t> junk;
      put_id(junk, "JUNK");
      put32(junk, uint32_t(rest - 8));
      if (!file_->write_at(tail_end, junk.data(), junk.size())) return false;
    }

    if (!write_header(data_bytes, frames, file_->size()) || !file_->flush()) return false;
    confirmed_ = frames;
    file_->close();
    file_.reset();
    return true;
  }

  void abandon() override {
    // A stalled handle's close() can block forever: leak it on purpose.
    (void)file_.release();
  }

 private:
  bool write_header(uint64_t data_bytes, uint64_t frames, uint64_t file_bytes) {
    if (data_bytes > threshold_ || file_bytes - 8 > kMax32) rf64_ = true;
    if (!wav_patch_header(hdr_, data_bytes, frames, file_bytes, rf64_)) return false;
    return file_->write_at(0, hdr_.data(), hdr_.size());
  }

  std::unique_ptr<IFile> file_;
  uint64_t threshold_;
  WavDepth depth_;
  uint32_t channels_, block_align_;
  std::vector<uint8_t> hdr_;
  uint64_t frames_ = 0, confirmed_ = 0;
  bool rf64_ = false, bad_ = false;
  Tpdf dither_;
  std::vector<int16_t> s16_;
  std::vector<uint8_t> s24_;
};

}  // namespace

bool wav_patch_header(std::vector<uint8_t>& hdr, uint64_t data_bytes, uint64_t frames,
                      uint64_t file_bytes, bool rf64) {
  if (hdr.size() < kFmtOffset + 16 + 8 || file_bytes < 8) return false;
  uint8_t* h = hdr.data();
  if (std::memcmp(h + kJunkOffset, "JUNK", 4) != 0 && std::memcmp(h + kJunkOffset, "ds64", 4) != 0)
    return false;
  if (get32(h + kJunkOffset + 4) != 28) return false;
  const size_t data_hdr = hdr.size() - 8;
  if (std::memcmp(h + data_hdr, "data", 4) != 0) return false;
  if (std::memcmp(h, "RF64", 4) == 0) rf64 = true;

  size_t fact = 0;  // payload offset of the fact chunk, if any
  for (size_t pos = kFmtOffset; pos + 8 <= data_hdr;) {
    const uint32_t sz = get32(h + pos + 4);
    if (std::memcmp(h + pos, "fact", 4) == 0 && sz >= 4) fact = pos + 8;
    pos += 8 + size_t(sz) + (sz & 1);
  }

  const uint64_t riff = file_bytes - 8;
  if (!rf64 && (data_bytes > kMax32 || riff > kMax32)) return false;
  if (rf64) {
    std::memcpy(h, "RF64", 4);
    set32(h + 4, kMax32);
    std::memcpy(h + kJunkOffset, "ds64", 4);
    set64(h + kJunkOffset + 8, riff);
    set64(h + kJunkOffset + 16, data_bytes);
    set64(h + kJunkOffset + 24, frames);
    set32(h + kJunkOffset + 32, 0);  // table length
    set32(h + data_hdr + 4, kMax32);
  } else {
    set32(h + 4, uint32_t(riff));
    set32(h + data_hdr + 4, uint32_t(data_bytes));
  }
  if (fact) set32(h + fact, uint32_t(std::min<uint64_t>(frames, kMax32)));
  return true;
}

std::vector<uint8_t> wav_marker_chunks(const std::vector<Marker>& markers) {
  std::vector<uint8_t> t;
  if (markers.empty()) return t;
  put_id(t, "cue ");
  put32(t, uint32_t(4 + 24 * markers.size()));
  put32(t, uint32_t(markers.size()));
  for (size_t i = 0; i < markers.size(); ++i) {
    put32(t, uint32_t(i + 1));  // cue point id
    put32(t, uint32_t(i));      // play order position
    put_id(t, "data");
    put32(t, 0);  // chunk start
    put32(t, 0);  // block start
    put32(t, uint32_t(std::min<uint64_t>(markers[i].frame_in_file, kMax32)));
  }

  std::vector<uint8_t> labels;
  for (size_t i = 0; i < markers.size(); ++i) {
    const std::string name = to_utf8(markers[i].name);
    if (name.empty()) continue;
    put_id(labels, "labl");
    put32(labels, uint32_t(4 + name.size() + 1));
    put32(labels, uint32_t(i + 1));
    labels.insert(labels.end(), name.begin(), name.end());
    labels.push_back(0);
    if ((name.size() + 1) & 1) labels.push_back(0);
  }
  if (!labels.empty()) {
    put_id(t, "LIST");
    put32(t, uint32_t(4 + labels.size()));
    put_id(t, "adtl");
    t.insert(t.end(), labels.begin(), labels.end());
  }
  return t;
}

std::unique_ptr<IEncoderFile> open_wav(IFileSystem& fs, const std::wstring& path, const WavParams& p,
                                       std::wstring* err) {
  auto fail = [&](const wchar_t* msg) -> std::unique_ptr<IEncoderFile> {
    if (err) *err = msg;
    return nullptr;
  };
  if (p.channels == 0 || p.channels > 64) return fail(L"unsupported channel count");
  if (p.sample_rate == 0 || p.sample_rate > 768000) return fail(L"unsupported sample rate");
  std::wstring ferr;
  auto f = fs.create_new(path, &ferr);
  if (!f) {
    if (err) *err = ferr.empty() ? L"cannot create file" : ferr;
    return nullptr;
  }
  auto w = std::make_unique<WavFile>(std::move(f), p);
  if (!w->start()) {
    w->abandon();  // leave the handle alone; the volume is misbehaving
    return fail(L"cannot write WAV header");
  }
  return w;
}

}  // namespace spool
