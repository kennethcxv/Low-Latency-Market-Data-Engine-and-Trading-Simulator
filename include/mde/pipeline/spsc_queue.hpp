// Bounded single-producer/single-consumer ring buffer, plus the mutex-protected queue it is
// benchmarked against. Both expose the same non-blocking try_push/try_pop interface.
#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <deque>
#include <mutex>
#include <type_traits>

#include "mde/common.hpp"

namespace mde::pipeline {

// Exactly one thread may call try_push and exactly one (other) thread try_pop.
//
// head_ and tail_ are monotonically increasing positions (never wrapped), so full is
// tail - head == Capacity and empty is tail == head with no ambiguity; size_t positions cannot
// overflow in practice (2^64 messages).
//
// Memory ordering: the producer writes the slot and then publishes it with a release store of
// tail_; the consumer's acquire load of tail_ therefore sees the slot contents. Symmetrically, the
// consumer's release store of head_ (after copying the slot out) is acquired by the producer
// before it reuses that slot. Each side's own index is read relaxed because only it writes it.
//
// Each side caches the other's index and only reloads it (one cross-core cache-line transfer)
// when the ring looks full or empty. Producer-owned and consumer-owned fields live on separate
// cache lines so the two threads do not invalidate each other's lines on every operation.
template <class T, std::size_t Capacity>
class SpscQueue {
  static_assert(std::has_single_bit(Capacity), "capacity must be a power of two");
  static_assert(std::is_trivially_copyable_v<T>, "slots are overwritten in place");

 public:
  bool try_push(const T& value) noexcept {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - head_cache_ == Capacity) {
      head_cache_ = head_.load(std::memory_order_acquire);
      if (tail - head_cache_ == Capacity) return false;
    }
    slots_[tail & kMask] = value;
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  bool try_pop(T& out) noexcept {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_cache_) {
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (head == tail_cache_) return false;
    }
    out = slots_[head & kMask];
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  static constexpr std::size_t capacity() { return Capacity; }

 private:
  static constexpr std::size_t kMask = Capacity - 1;

  alignas(kCacheLine) std::atomic<std::size_t> head_{0};  // consumer writes
  std::size_t tail_cache_ = 0;                             // consumer-private
  alignas(kCacheLine) std::atomic<std::size_t> tail_{0};  // producer writes
  std::size_t head_cache_ = 0;                             // producer-private
  alignas(kCacheLine) std::array<T, Capacity> slots_{};
};

// Baseline: a bounded std::deque under a std::mutex, with the same non-blocking interface so the
// pipeline and benchmarks can swap one for the other.
template <class T, std::size_t Capacity>
class MutexQueue {
 public:
  bool try_push(const T& value) {
    std::lock_guard lock(mutex_);
    if (queue_.size() == Capacity) return false;
    queue_.push_back(value);
    return true;
  }

  bool try_pop(T& out) {
    std::lock_guard lock(mutex_);
    if (queue_.empty()) return false;
    out = queue_.front();
    queue_.pop_front();
    return true;
  }

  static constexpr std::size_t capacity() { return Capacity; }

 private:
  std::mutex mutex_;
  std::deque<T> queue_;
};

// Spin-wait hint: lets the sibling hyperthread run (x86 PAUSE) or saves power (ARM YIELD).
inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__)
  asm volatile("yield" ::: "memory");
#endif
}

}  // namespace mde::pipeline
