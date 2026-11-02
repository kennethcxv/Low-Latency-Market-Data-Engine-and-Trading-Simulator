// Two-thread market-data pipeline: a feed thread (receive + sequence) hands in-order ITCH
// messages to a book thread (decode + book update) through a bounded queue.
//
// Thread ownership: the book thread is the only thread that touches whatever `consume` uses (the
// book builder and its market) until run() returns; the join at the end of run() makes those
// writes visible to the caller. The queue is the only shared structure.
//
// Backpressure: when the queue is full the feed thread spins until the book thread frees a slot,
// and counts the episode in full_waits. Nothing is dropped inside the process. Upstream of the
// feed thread, a live receiver's kernel socket buffer absorbs bursts; if it overflows the kernel
// drops datagrams, the sequencer sees gaps, and re-requests recover them.
#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <thread>

#include "mde/common.hpp"
#include "mde/pipeline/spsc_queue.hpp"

namespace mde::pipeline {

// One message in flight: exactly one 64-byte line, copied by value.
struct alignas(64) MessageSlot {
  std::uint64_t ingress_ns = 0;  // receive time of the datagram that carried it
  std::uint8_t length = 0;
  std::uint8_t data[55] = {};
};
static_assert(sizeof(MessageSlot) == 64);
inline constexpr std::size_t kMaxSlotMessage = sizeof(MessageSlot::data);  // largest ITCH message is 50

struct PipelineStats {
  std::uint64_t messages = 0;
  std::uint64_t full_waits = 0;  // pushes that found the queue full and had to spin
  std::uint64_t oversized = 0;   // longer than any ITCH message; dropped at the feed side
};

// Runs produce(push) on the calling thread, where push(ingress_ns, message) enqueues one message,
// and consume(const MessageSlot&) on a new book thread for every message, in order. Returns once
// produce has returned and the book thread has drained the queue. consume must not throw.
template <class Queue, class Produce, class Consume>
PipelineStats run(Queue& queue, Produce&& produce, Consume&& consume) {
  std::atomic<bool> done{false};
  std::thread book([&] {
    MessageSlot slot;
    for (;;) {
      if (queue.try_pop(slot)) {
        consume(slot);
      } else if (done.load(std::memory_order_acquire)) {
        // Every push happened before `done` was released, so this drain sees all of them.
        while (queue.try_pop(slot)) consume(slot);
        return;
      } else {
        cpu_relax();
      }
    }
  });

  PipelineStats stats;
  auto push = [&](std::uint64_t ingress_ns, Bytes message) {
    if (message.size() > kMaxSlotMessage) {
      ++stats.oversized;
      return;
    }
    MessageSlot slot;
    slot.ingress_ns = ingress_ns;
    slot.length = static_cast<std::uint8_t>(message.size());
    std::memcpy(slot.data, message.data(), message.size());
    if (!queue.try_push(slot)) {
      ++stats.full_waits;
      do cpu_relax();
      while (!queue.try_push(slot));
    }
    ++stats.messages;
  };

  try {
    produce(push);
  } catch (...) {
    done.store(true, std::memory_order_release);
    book.join();
    throw;
  }
  done.store(true, std::memory_order_release);
  book.join();
  return stats;
}

}  // namespace mde::pipeline
