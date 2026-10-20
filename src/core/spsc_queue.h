#pragma once
// Bounded single-producer / single-consumer queue (UI -> manager commands).
#include <array>
#include <atomic>
#include <cstddef>
#include <optional>

#pragma warning(push)
#pragma warning(disable : 4324)  // padding from alignas is intentional

namespace spool {

template <class T, size_t N>
class SpscQueue {
  static_assert(N >= 2 && (N & (N - 1)) == 0, "N must be a power of two");

 public:
  // Producer thread only. Returns false when full.
  bool push(const T& v) {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - head_.load(std::memory_order_acquire) == N) return false;
    items_[tail & (N - 1)] = v;
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  // Consumer thread only.
  std::optional<T> pop() {
    const size_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_.load(std::memory_order_acquire)) return std::nullopt;
    std::optional<T> v(std::move(items_[head & (N - 1)]));
    head_.store(head + 1, std::memory_order_release);
    return v;
  }

 private:
  alignas(64) std::atomic<size_t> head_{0};
  alignas(64) std::atomic<size_t> tail_{0};
  std::array<T, N> items_{};
};

}  // namespace spool
#pragma warning(pop)
