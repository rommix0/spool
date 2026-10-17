#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "format/encoder.h"
#include "format/opus_writer.h"
#include "io/fault_file_system.h"
#include "io/file_system.h"
#include "test.h"

using namespace sl;

namespace {

const std::wstring kDir = L"D:\\rec";
const std::wstring kPath = L"D:\\rec\\a.opus";

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
  fs.add_rule({kDir, FaultFileSystem::Kind::FailWrite, 20000, 0});
  auto w = open(fs, 48000, 2);
  CHECK(w != nullptr);
  if (!w) return;
  uint64_t pos = 0;
  bool ok = true;
  for (int i = 0; i < 100 && ok; ++i) ok = feed(*w, 48000, 2, 48000, pos);
  CHECK(!ok);
  CHECK(!w->write(std::vector<float>(2048).data(), 1024));
  CHECK(!w->flush());
  CHECK(!w->finalize({}, std::nullopt));  // must return, not hang
}

TEST(opus_flush_failure_is_sticky) {
  FaultFileSystem fs;
  auto w = open(fs, 48000, 2);
  CHECK(w != nullptr);
  if (!w) return;
  uint64_t pos = 0;
  CHECK(feed(*w, 48000, 2, 96000, pos));
  CHECK(w->flush());
  const uint64_t confirmed = w->confirmed_frames();
  CHECK(confirmed > 0);
  fs.add_rule({kDir, FaultFileSystem::Kind::FailFlush, 0, 0});
  CHECK(feed(*w, 48000, 2, 96000, pos));
  CHECK(!w->flush());
  fs.clear_rules();  // the device recovers, but the data's durability is unknown
  CHECK(!w->flush());
  CHECK(!w->write(std::vector<float>(2048).data(), 1024));
  CHECK_EQ(w->confirmed_frames(), confirmed);
  CHECK(!w->finalize({}, std::nullopt));
}

namespace {

// Counts the calls that reach the file after a mark; delegates everything.
struct Counts {
  std::atomic<int> flushes{0}, closes{0}, writes_started{0}, writes_done{0};
};

class CountingFile : public IFile {
 public:
  CountingFile(std::unique_ptr<IFile> f, Counts& c) : f_(std::move(f)), c_(c) {}
  bool write(const void* p, size_t n) override {
    ++c_.writes_started;
    const bool ok = f_->write(p, n);
    ++c_.writes_done;
    return ok;
  }
  bool write_at(uint64_t o, const void* p, size_t n) override { return f_->write_at(o, p, n); }
  bool read_at(uint64_t o, void* p, size_t n) override { return f_->read_at(o, p, n); }
  bool flush() override {
    ++c_.flushes;
    return f_->flush();
  }
  uint64_t size() const override { return f_->size(); }
  void close() override {
    ++c_.closes;
    f_->close();
  }

 private:
  std::unique_ptr<IFile> f_;
  Counts& c_;
};

class CountingFs : public IFileSystem {
 public:
  CountingFs(FaultFileSystem& in, Counts& c) : in_(in), c_(c) {}
  std::unique_ptr<IFile> create_new(const std::wstring& p, std::wstring* e) override {
    auto f = in_.create_new(p, e);
    return f ? std::make_unique<CountingFile>(std::move(f), c_) : nullptr;
  }
  std::unique_ptr<IFile> open_existing(const std::wstring& p, std::wstring* e) override {
    return in_.open_existing(p, e);
  }
  bool exists(const std::wstring& p) override { return in_.exists(p); }
  bool ensure_dir(const std::wstring& d, std::wstring* e) override { return in_.ensure_dir(d, e); }
  bool remove(const std::wstring& p) override { return in_.remove(p); }
  std::vector<std::wstring> list(const std::wstring& d, const std::wstring& s) override {
    return in_.list(d, s);
  }
  std::optional<uint64_t> free_bytes(const std::wstring& d) override { return in_.free_bytes(d); }
  std::wstring volume_id(const std::wstring& d) override { return in_.volume_id(d); }

 private:
  FaultFileSystem& in_;
  Counts& c_;
};

}  // namespace

TEST(opus_abandon_does_not_close_or_flush_a_stalled_file) {
  FaultFileSystem fs;
  std::wstring err;
  fs.ensure_dir(kDir, &err);
  fs.add_rule({kDir, FaultFileSystem::Kind::HangWrite, 20000, 0});
  Counts counts;
  CountingFs cfs(fs, counts);
  auto w = open_opus(cfs, kPath, OpusParams{48000, 2}, &err);
  CHECK(w != nullptr);
  if (!w) return;

  std::atomic<bool> wrote_ok{true};
  std::thread writer([&] {
    uint64_t pos = 0;
    for (int i = 0; i < 100 && wrote_ok; ++i) wrote_ok = feed(*w, 48000, 2, 48000, pos);
  });
  // Wait until the writer thread is stuck inside a file write.
  for (int i = 0; i < 5000 && counts.writes_started == counts.writes_done; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  CHECK(counts.writes_started > counts.writes_done);

  const int flushes = counts.flushes, closes = counts.closes;
  w->abandon();  // from another thread, while the writer is inside the callback
  fs.release_hangs();
  writer.join();

  CHECK(!wrote_ok);  // the released write failed
  CHECK_EQ(counts.flushes.load(), flushes);
  CHECK_EQ(counts.closes.load(), closes);
  CHECK_EQ(counts.closes.load(), 0);
  CHECK(!w->flush());
  CHECK(!w->finalize({}, std::nullopt));
  w->abandon();
  w.reset();
  CHECK_EQ(counts.flushes.load(), flushes);
  CHECK_EQ(counts.closes.load(), 0);
}
