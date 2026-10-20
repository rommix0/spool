#include "format/opus_writer.h"

#include <opusenc.h>

#include <algorithm>
#include <atomic>
#include <cstring>

namespace spool {

namespace {

constexpr int kFamily = 0;               // mono / stereo
constexpr uint32_t kOutputRate = 48000;  // Ogg Opus granules always count 48 kHz samples
constexpr int kMuxingDelay = 24000;      // 48 kHz samples: 0.5 s of audio per page at most
// libopusenc holds back 2 s of input before encoding (a lookahead only used to pick
// stream-chaining points, which we never use). Drop it so flush() confirms recent audio.
constexpr int kDecisionDelay = 0;
constexpr size_t kMaxBlock = 1 << 20;    // frames per ope_encoder_write_float call (int-safe)

// Everything the encoder callbacks touch. abandon() leaks it (with the encoder and the
// file) so a thread stuck inside a stalled write never sees freed memory.
struct State {
  std::unique_ptr<IFile> file;
  OggOpusEnc* enc = nullptr;
  std::atomic<bool> abandoned{false};  // set by the watchdog thread while a callback may run
  int64_t granule = -1;   // of the last page successfully written to the file
  int64_t preskip = 0;    // from the OpusHead packet
};

// libopusenc hands over exactly one Ogg page per call.
int write_cb(void* user, const unsigned char* p, opus_int32 len) {
  State& s = *static_cast<State*>(user);
  if (s.abandoned || !s.file || len < 27) return 1;
  if (!s.file->write(p, size_t(len))) return 1;
  if (std::memcmp(p, "OggS", 4) != 0) return 0;
  int64_t g;
  std::memcpy(&g, p + 6, 8);
  if (g >= 0) s.granule = std::max(s.granule, g);
  const size_t ns = p[26];
  const size_t data = 27 + ns;
  if ((p[5] & 0x02) && data + 12 <= size_t(len) && std::memcmp(p + data, "OpusHead", 8) == 0)
    s.preskip = p[data + 10] | (p[data + 11] << 8);
  return 0;
}

// Called when the stream ends (drain) or the encoder is destroyed. Idempotent.
int close_cb(void* user) {
  State& s = *static_cast<State*>(user);
  if (s.abandoned || !s.file) return 0;  // never touch a stalled handle
  const bool ok = s.file->flush();
  s.file->close();
  s.file.reset();
  return ok ? 0 : 1;
}

const OpusEncCallbacks kCallbacks = {write_cb, close_cb};

class OpusFile final : public IEncoderFile {
 public:
  OpusFile(std::unique_ptr<State> s, uint32_t rate, uint16_t channels)
      : s_(std::move(s)), rate_(rate), channels_(channels) {}

  ~OpusFile() override {
    if (s_ && s_->enc) ope_encoder_destroy(s_->enc);  // closes the file via close_cb
  }

  bool write(const float* in, size_t frames) override {
    if (!s_ || !s_->enc || bad_) return false;
    // Locals: abandon() may null s_ from another thread while this call is stalled in a write.
    OggOpusEnc* const enc = s_->enc;
    for (size_t done = 0; done < frames;) {
      const size_t n = std::min(kMaxBlock, frames - done);
      if (ope_encoder_write_float(enc, in + done * channels_, int(n)) != OPE_OK) {
        bad_ = true;  // libopusenc is unrecoverable after a failed page write
        return false;
      }
      done += n;
      frames_ += n;
    }
    return true;
  }

  bool flush() override {
    if (!s_ || !s_->file || bad_) return false;
    if (!s_->file->flush()) {
      bad_ = true;  // durability of the pending data is unknown from here on
      return false;
    }
    confirmed_ = std::max(confirmed_, confirmed_from_granule());
    return true;
  }

  uint64_t confirmed_frames() const override { return confirmed_; }
  uint64_t frames_written() const override { return frames_; }

  bool finalize(const std::vector<Marker>&, std::optional<uint64_t>) override {
    if (!s_ || !s_->enc) return false;
    // After a failure drain returns at once; destroy then closes the file.
    const bool ok = !bad_ && ope_encoder_drain(s_->enc) == OPE_OK;
    ope_encoder_destroy(s_->enc);
    s_->enc = nullptr;
    if (ok) confirmed_ = confirmed_from_granule();
    s_.reset();
    return ok;
  }

  void abandon() override {
    if (!s_) return;
    s_->abandoned = true;  // from now on the callbacks touch nothing
    (void)s_.release();    // leak state, encoder and handle: close() may block forever
  }

 private:
  uint64_t confirmed_from_granule() const {
    const int64_t g = s_->granule - s_->preskip;
    return g > 0 ? uint64_t(g) * rate_ / kOutputRate : 0;
  }

  std::unique_ptr<State> s_;
  uint32_t rate_;
  uint16_t channels_;
  uint64_t frames_ = 0, confirmed_ = 0;
  bool bad_ = false;
};

}  // namespace

std::unique_ptr<IEncoderFile> open_opus(IFileSystem& fs, const std::wstring& path, const OpusParams& p,
                                        std::wstring* err) {
  auto fail = [&](const wchar_t* msg) -> std::unique_ptr<IEncoderFile> {
    if (err) *err = msg;
    return nullptr;
  };
  if (p.channels != 1 && p.channels != 2) return fail(L"Opus supports 1 or 2 channels");
  if (p.input_rate == 0 || p.input_rate > 768000) return fail(L"unsupported sample rate");

  std::wstring ferr;
  auto f = fs.create_new(path, &ferr);
  if (!f) {
    if (err) *err = ferr.empty() ? L"cannot create file" : ferr;
    return nullptr;
  }
  auto s = std::make_unique<State>();
  s->file = std::move(f);

  auto drop_file = [&](const wchar_t* msg) {
    s->file->close();
    s->file.reset();
    fs.remove(path);  // we created it a moment ago and it holds nothing
    return fail(msg);
  };

  OggOpusComments* comments = ope_comments_create();
  if (!comments) return drop_file(L"out of memory");
  ope_comments_add(comments, "ENCODER", "Spool");
  int e = OPE_OK;
  s->enc = ope_encoder_create_callbacks(&kCallbacks, s.get(), comments, opus_int32(p.input_rate),
                                        p.channels, kFamily, &e);
  ope_comments_destroy(comments);
  if (!s->enc) return drop_file(L"cannot create Opus encoder");

  const opus_int32 bitrate = p.bitrate ? opus_int32(p.bitrate) : (p.channels == 1 ? 64000 : 128000);
  if (ope_encoder_ctl(s->enc, OPUS_SET_BITRATE(bitrate)) != OPE_OK ||
      ope_encoder_ctl(s->enc, OPUS_SET_VBR(1)) != OPE_OK ||
      ope_encoder_ctl(s->enc, OPE_SET_MUXING_DELAY(kMuxingDelay)) != OPE_OK ||
      ope_encoder_ctl(s->enc, OPE_SET_DECISION_DELAY(kDecisionDelay)) != OPE_OK) {
    s->abandoned = true;
    ope_encoder_destroy(s->enc);
    s->enc = nullptr;
    s->abandoned = false;
    return drop_file(L"cannot configure Opus encoder");
  }
  return std::make_unique<OpusFile>(std::move(s), p.input_rate, p.channels);
}

}  // namespace spool
