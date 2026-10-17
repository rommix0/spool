#pragma once
// Capture ring: one real-time producer (the capture thread), many readers.
// Readers detect overrun themselves (seqlock style), so the producer never waits.
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "core/types.h"

#pragma warning(push)
#pragma warning(disable : 4324)  // padding from alignas is intentional

namespace sl {

class SampleRing {
 public:
  // capacity = next power of two >= min_frames. Memory is VirtualAlloc'd,
  // touched page by page, and best-effort VirtualLock'd. Throws std::bad_alloc.
  SampleRing(uint32_t channels, size_t min_frames, SampleIndex start = 0);
  ~SampleRing();
  SampleRing(const SampleRing&) = delete;
  SampleRing& operator=(const SampleRing&) = delete;

  uint32_t channels() const { return channels_; }
  size_t capacity() const { return capacity_; }

  // Single producer; no allocation, locks or syscalls. Overwrites the oldest frames.
  void write(const float* interleaved, size_t frames);
  // Producer only, forward only. Frames in the skipped range become invalid.
  void jump_to(SampleIndex next);

  SampleIndex write_index() const { return write_index_.load(std::memory_order_acquire); }
  // max(write_index - capacity, last jump target, start)
  SampleIndex oldest_valid() const;

  struct ReadResult {
    size_t frames;
    bool overrun;
  };
  // Copies [from, min(from + max_frames, write_index)). Returns {0, true} if `from` was
  // (or became, during the copy) older than the oldest valid frame; torn data is never
  // reported as valid. Reading at or past write_index returns {0, false}.
  ReadResult read(SampleIndex from, float* out, size_t max_frames) const;

 private:
  uint32_t channels_;
  size_t capacity_;
  size_t mask_;
  float* data_ = nullptr;
  alignas(64) std::atomic<int64_t> write_index_;  // published after the data is copied
  // Index the producer is about to publish; stored before any slot is overwritten.
  // Readers use it after copying to detect slots overwritten while they were reading.
  alignas(64) std::atomic<int64_t> pending_index_;
  alignas(64) std::atomic<int64_t> jump_floor_;
};

}  // namespace sl
#pragma warning(pop)
