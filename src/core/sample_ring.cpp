#include "core/sample_ring.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <new>

namespace sl {

namespace {
size_t next_pow2(size_t v) {
  size_t p = 1;
  while (p < v) p <<= 1;
  return p;
}
}  // namespace

SampleRing::SampleRing(uint32_t channels, size_t min_frames, SampleIndex start)
    : channels_(channels ? channels : 1),
      capacity_(next_pow2(min_frames ? min_frames : 1)),
      mask_(capacity_ - 1),
      write_index_(start),
      pending_index_(start),
      jump_floor_(start) {
  const size_t bytes = capacity_ * channels_ * sizeof(float);
  void* p = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (!p) throw std::bad_alloc();
  data_ = static_cast<float*>(p);
  // Pre-fault every page so the capture thread never takes a page fault.
  volatile char* bytes_p = static_cast<volatile char*>(p);
  for (size_t off = 0; off < bytes; off += 4096) bytes_p[off] = 0;
  locked_ = VirtualLock(p, bytes) != FALSE;  // best effort; may fail against the working-set quota
}

SampleRing::~SampleRing() {
  if (data_) {
    if (locked_) VirtualUnlock(data_, capacity_ * channels_ * sizeof(float));
    VirtualFree(data_, 0, MEM_RELEASE);
  }
}

SampleIndex SampleRing::oldest_valid() const {
  const int64_t w = write_index_.load(std::memory_order_acquire);
  const int64_t floor = jump_floor_.load(std::memory_order_acquire);
  return std::max<int64_t>(w - static_cast<int64_t>(capacity_), floor);
}

void SampleRing::write(const float* interleaved, size_t frames) {
  if (frames == 0) return;
  const int64_t w = write_index_.load(std::memory_order_relaxed);
  const int64_t end = w + static_cast<int64_t>(frames);
  // Announce the new extent before touching any slot (pairs with the fence in read()).
  pending_index_.store(end, std::memory_order_relaxed);
  std::atomic_thread_fence(std::memory_order_release);

  // A write larger than the ring only keeps its newest `capacity_` frames.
  int64_t first = w;
  if (frames > capacity_) {
    const size_t skip = frames - capacity_;
    interleaved += skip * channels_;
    first += static_cast<int64_t>(skip);
    frames = capacity_;
  }
  const size_t pos = static_cast<size_t>(first) & mask_;
  const size_t n1 = std::min(frames, capacity_ - pos);
  std::memcpy(data_ + pos * channels_, interleaved, n1 * channels_ * sizeof(float));
  if (n1 < frames)
    std::memcpy(data_, interleaved + n1 * channels_, (frames - n1) * channels_ * sizeof(float));

  write_index_.store(end, std::memory_order_release);
}

void SampleRing::jump_to(SampleIndex next) {
  if (next <= write_index_.load(std::memory_order_relaxed)) return;  // forward only
  jump_floor_.store(next, std::memory_order_release);
  pending_index_.store(next, std::memory_order_relaxed);
  write_index_.store(next, std::memory_order_release);
}

SampleRing::ReadResult SampleRing::read(SampleIndex from, float* out, size_t max_frames) const {
  const int64_t cap = static_cast<int64_t>(capacity_);
  const int64_t w = write_index_.load(std::memory_order_acquire);
  const int64_t floor = jump_floor_.load(std::memory_order_acquire);
  if (from < std::max<int64_t>(w - cap, floor)) return {0, true};
  if (from >= w || max_frames == 0) return {0, false};

  const size_t n = static_cast<size_t>(std::min<int64_t>(w - from, static_cast<int64_t>(max_frames)));
  const size_t pos = static_cast<size_t>(from) & mask_;
  const size_t n1 = std::min(n, capacity_ - pos);
  std::memcpy(out, data_ + pos * channels_, n1 * channels_ * sizeof(float));
  if (n1 < n) std::memcpy(out + n1 * channels_, data_, (n - n1) * channels_ * sizeof(float));

  // Seqlock re-check: if the producer started overwriting any slot we copied (even
  // without having published yet), pending_index_ is already visible here.
  std::atomic_thread_fence(std::memory_order_acquire);
  const int64_t pending = pending_index_.load(std::memory_order_relaxed);
  const int64_t floor2 = jump_floor_.load(std::memory_order_relaxed);
  if (from < std::max<int64_t>(pending - cap, floor2)) return {0, true};
  return {n, false};
}

}  // namespace sl
