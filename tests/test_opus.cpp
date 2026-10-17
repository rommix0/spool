#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "format/encoder.h"
#include "format/opus_writer.h"
#include "io/fault_file_system.h"
#include "test.h"

using namespace sl;

namespace {

const std::wstring kDir = L"D:\rec";
const std::wstring kPath = L"D:\rec\a.opus";

std::unique_ptr<IEncoderFile> open(FaultFileSystem& fs, uint32_t rate, uint16_t ch,
                                   std::wstring* err = nullptr) {
  std::wstring e;
  fs.ensure_dir(kDir, &e);
  return open_opus(fs, kPath, OpusParams{rate, ch}, err ? err : &e);
}

// Writes `frames` of a 1 kHz sine in 1024-frame blocks, advancing `pos`.
bool feed(IEncoderFile& w, uint32_t rate, uint16_t ch, uint64_t frames, uint64_t& pos) {
  std::vector<float> buf(1024 * ch);
  for (uint64_t done = 0; done < frames;) {
    const size_t n = size_t(std::min<uint64_t>(1024, frames - done));
    for (size_t i = 0; i < n; ++i) {
      const float v = 0.5f * std::sin(6.2831853f * 1000.0f * float((pos + i) % rate) / float(rate));
      for (uint16_t c = 0; c < ch; ++c) buf[i * ch + c] = v;
    }
    if (!w.write(buf.data(), n)) return false;
    done += n;
    pos += n;
  }
  return true;
}

bool contains(const std::vector<uint8_t>& b, const char* s) {
  return std::search(b.begin(), b.end(), s, s + std::strlen(s)) != b.end();
}

// Walks the Ogg pages; returns the last non-negative granule, and the preskip from OpusHead.
struct OggInfo {
  int64_t last_granule = -1;
  int preskip = -1;
  size_t pages = 0;
  bool ok = true;
};
OggInfo scan(const std::vector<uint8_t>& b) {
  OggInfo r;
  size_t pos = 0;
  while (pos + 27 <= b.size()) {
    if (std::memcmp(&b[pos], "OggS", 4) != 0) {
      r.ok = false;
      break;
    }
    int64_t g;
    std::memcpy(&g, &b[pos + 6], 8);
    const size_t ns = b[pos + 26];
    if (pos + 27 + ns > b.size()) {
      r.ok = false;
      break;
    }
    size_t body = 0;
    for (size_t i = 0; i < ns; ++i) body += b[pos + 27 + i];
    const size_t data = pos + 27 + ns;
    if (data + body > b.size()) {
      r.ok = false;
      break;
    }
    if (r.pages == 0 && body >= 12 && std::memcmp(&b[data], "OpusHead", 8) == 0)
      r.preskip = b[data + 10] | (b[data + 11] << 8);
    if (g >= 0) r.last_granule = g;
    ++r.pages;
    pos = data + body;
  }
  return r;
}

}  // namespace

TEST(opus_valid_stream_and_size) {
  FaultFileSystem fs;
  auto w = open(fs, 48000, 2);
  CHECK(w != nullptr);
  if (!w) return;
  uint64_t pos = 0;
  CHECK(feed(*w, 48000, 2, 480000, pos));
  CHECK(w->finalize({}, std::nullopt));
  const auto b = fs.contents(kPath);
  CHECK(b.size() >= 4 && std::memcmp(b.data(), "OggS", 4) == 0);
  CHECK(contains(b, "OpusHead"));
  CHECK(contains(b, "ENCODER=SoundLogger"));
  CHECK(b.size() >= 120 * 1000);
  CHECK(b.size() <= 200 * 1000);
  CHECK(scan(b).ok);
}

TEST(opus_final_granule_matches_length) {
  FaultFileSystem fs;
  auto w = open(fs, 48000, 2);
  CHECK(w != nullptr);
  if (!w) return;
  uint64_t pos = 0;
  CHECK(feed(*w, 48000, 2, 480000, pos));
  CHECK(w->finalize({}, std::nullopt));
  const OggInfo i = scan(fs.contents(kPath));
  CHECK(i.preskip > 0);
  CHECK_EQ(i.last_granule - i.preskip, int64_t(480000));
  CHECK_EQ(w->frames_written(), uint64_t(480000));
  CHECK_EQ(w->confirmed_frames(), uint64_t(480000));
}

TEST(opus_confirmed_after_flush) {
  FaultFileSystem fs;
  auto w = open(fs, 48000, 2);
  CHECK(w != nullptr);
  if (!w) return;
  uint64_t pos = 0;
  CHECK_EQ(w->confirmed_frames(), uint64_t(0));
  CHECK(feed(*w, 48000, 2, 240000, pos));
  CHECK(w->flush());
  CHECK(w->confirmed_frames() <= 240000);
  CHECK(w->confirmed_frames() >= 240000 - 48000);
  // Provably on disk: the pages already in the file cover at least what is confirmed.
  const OggInfo i = scan(fs.contents(kPath));
  CHECK(i.ok);
  CHECK(uint64_t(std::max<int64_t>(0, i.last_granule - i.preskip)) >= w->confirmed_frames());
  const uint64_t first = w->confirmed_frames();
  CHECK(feed(*w, 48000, 2, 48000, pos));
  CHECK(w->flush());
  CHECK(w->confirmed_frames() >= first);
  CHECK(w->finalize({}, std::nullopt));
}

TEST(opus_44100_input_resampled) {
  FaultFileSystem fs;
  auto w = open(fs, 44100, 2);
  CHECK(w != nullptr);
  if (!w) return;
  uint64_t pos = 0;
  CHECK(feed(*w, 44100, 2, 441000, pos));
  CHECK(w->finalize({}, std::nullopt));
  const OggInfo i = scan(fs.contents(kPath));
  CHECK_NEAR(i.last_granule - i.preskip, 480000, 960);
  CHECK_NEAR(double(w->confirmed_frames()), 441000.0, 960.0);
}

TEST(opus_rejects_3_channels) {
  FaultFileSystem fs;
  std::wstring err;
  auto w = open(fs, 48000, 3, &err);
  CHECK(w == nullptr);
  CHECK(!err.empty());
}

TEST(opus_mono_roundtrip) {
  FaultFileSystem fs;
  auto w = open(fs, 48000, 1);
  CHECK(w != nullptr);
  if (!w) return;
  uint64_t pos = 0;
  CHECK(feed(*w, 48000, 1, 96000, pos));
  CHECK(w->finalize({}, std::nullopt));
  const auto b = fs.contents(kPath);
  const OggInfo i = scan(b);
  CHECK(i.ok);
  CHECK_EQ(i.last_granule - i.preskip, int64_t(96000));
  CHECK(b.size() < 40 * 1000);  // 64 kbps * 2 s = 16 KB
}

TEST(opus_never_overwrites) {
  FaultFileSystem fs;
  auto w = open(fs, 48000, 2);
  CHECK(w != nullptr);
  std::wstring err;
  auto w2 = open_opus(fs, kPath, OpusParams{48000, 2}, &err);
  CHECK(w2 == nullptr);
  CHECK(!err.empty());
}

TEST(opus_write_failure_is_sticky) {
  FaultFileSystem fs;
  fs.add_rule({L"D:\rec", FaultFileSystem::Kind::FailWrite, 20000, 0});
  auto w = open(fs, 48000, 2);
  CHECK(w != nullptr);
  if (!w) return;
  uint64_t pos = 0;
  bool ok = true;
  for (int i = 0; i < 100 && ok; ++i) ok = feed(*w, 48000, 2, 48000, pos);
  CHECK(!ok);
  CHECK(!w->write(std::vector<float>(2048).data(), 1024));
  CHECK(!w->flush());
  const uint64_t confirmed = w->confirmed_frames();
  CHECK(!w->flush());
  CHECK_EQ(w->confirmed_frames(), confirmed);
  CHECK(!w->finalize({}, std::nullopt));  // must return, not hang
}

TEST(opus_abandon_does_not_close_or_write) {
  FaultFileSystem fs;
  auto w = open(fs, 48000, 2);
  CHECK(w != nullptr);
  if (!w) return;
  uint64_t pos = 0;
  CHECK(feed(*w, 48000, 2, 96000, pos));
  CHECK(w->flush());
  const auto before = fs.contents(kPath);
  w->abandon();
  CHECK(!w->write(std::vector<float>(2048).data(), 1024));
  CHECK(!w->flush());
  CHECK(!w->finalize({}, std::nullopt));
  w->abandon();
  w.reset();
  CHECK(fs.contents(kPath) == before);
}
