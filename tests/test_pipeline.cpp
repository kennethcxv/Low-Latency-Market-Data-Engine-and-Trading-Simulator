// Queue semantics, cross-thread ordering under contention (run under TSan in CI), and the
// pipeline's equivalence to single-threaded replay.
#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>

#include "mde/book/book_builder.hpp"
#include "mde/book/reference_market.hpp"
#include "mde/moldudp64/replay.hpp"
#include "mde/pipeline/pipeline.hpp"
#include "mde/synth/generator.hpp"

namespace mde::pipeline {
namespace {

template <class Q>
class QueueTest : public ::testing::Test {};
using Queues = ::testing::Types<SpscQueue<std::uint64_t, 8>, MutexQueue<std::uint64_t, 8>>;
TYPED_TEST_SUITE(QueueTest, Queues);

TYPED_TEST(QueueTest, FifoBoundedAndReusable) {
  auto q = std::make_unique<TypeParam>();
  std::uint64_t v = 0;
  EXPECT_FALSE(q->try_pop(v));
  std::uint64_t next_in = 0, next_out = 0;
  for (int round = 0; round < 100; ++round) {  // wraps the ring many times
    while (q->try_push(next_in)) ++next_in;
    EXPECT_EQ(next_in - next_out, 8u);  // exactly Capacity items fit
    const int take = 1 + round % 8;
    for (int i = 0; i < take; ++i) {
      ASSERT_TRUE(q->try_pop(v));
      EXPECT_EQ(v, next_out++);
    }
  }
  while (q->try_pop(v)) EXPECT_EQ(v, next_out++);
  EXPECT_EQ(next_out, next_in);
}

// A tiny ring keeps both threads constantly at the full/empty boundary, where ordering bugs show.
TYPED_TEST(QueueTest, TwoThreadsPreserveOrderUnderContention) {
  constexpr std::uint64_t kItems = 300'000;
  auto q = std::make_unique<TypeParam>();
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kItems; ++i) {
      while (!q->try_push(i)) cpu_relax();
    }
  });
  std::uint64_t expected = 0, v = 0;
  while (expected < kItems) {
    if (q->try_pop(v)) {
      ASSERT_EQ(v, expected);
      ++expected;
    }
  }
  producer.join();
  EXPECT_FALSE(q->try_pop(v));
}

// The slot payload (not just an index) must arrive intact: the consumer checks every byte.
TEST(SpscQueue, PayloadIsFullyPublishedBeforeTheIndex) {
  auto q = std::make_unique<SpscQueue<MessageSlot, 16>>();
  constexpr std::uint64_t kItems = 200'000;
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kItems; ++i) {
      MessageSlot s;
      s.ingress_ns = i;
      s.length = static_cast<std::uint8_t>(i % 56);
      std::memset(s.data, static_cast<int>(i & 0xff), sizeof s.data);
      while (!q->try_push(s)) cpu_relax();
    }
  });
  MessageSlot s;
  for (std::uint64_t i = 0; i < kItems;) {
    if (!q->try_pop(s)) continue;
    ASSERT_EQ(s.ingress_ns, i);
    ASSERT_EQ(s.length, i % 56);
    for (std::uint8_t b : s.data) ASSERT_EQ(b, i & 0xff);
    ++i;
  }
  producer.join();
}

const std::vector<std::uint8_t>& fixture() {
  static const auto data = [] {
    synth::Config c;
    c.seed = 314;
    c.messages = 50'000;
    c.symbols = 10;
    return synth::generate(c);
  }();
  return data;
}

std::uint64_t direct_digest() {
  book::ReferenceMarket market;
  book::BookBuilder builder(market);
  FramedReader reader{fixture()};
  Bytes msg;
  while (reader.next(msg) == FramedReader::Status::kOk) builder.apply(msg);
  return book::state_digest(market);
}

template <class Queue>
std::pair<std::uint64_t, PipelineStats> run_pipeline(const moldudp64::ReplayOptions& options) {
  const moldudp64::MessageLog log(moldudp64::make_session("PIPE"), fixture());
  auto queue = std::make_unique<Queue>();
  book::ReferenceMarket market;
  book::BookBuilder builder(market);
  const auto stats = run(
      *queue,
      [&](auto& push) {
        const auto r = moldudp64::replay(log, options, [&](std::uint64_t seq, Bytes msg) { push(seq, msg); });
        ASSERT_TRUE(r.complete);
      },
      [&](const MessageSlot& slot) { builder.apply(Bytes(slot.data, slot.length)); });
  EXPECT_EQ(stats.messages, log.size());
  EXPECT_EQ(builder.counters().book_anomalies(), 0u);
  return {book::state_digest(market), stats};
}

TEST(Pipeline, ThreadedReplayMatchesSingleThreaded) {
  const std::uint64_t expected = direct_digest();
  moldudp64::ReplayOptions lossy;
  lossy.impairments = {0.02, 0.01, 0.02, 9};
  using Spsc = SpscQueue<MessageSlot, 1024>;
  using Mutex = MutexQueue<MessageSlot, 1024>;
  EXPECT_EQ(run_pipeline<Spsc>({}).first, expected);
  EXPECT_EQ(run_pipeline<Spsc>(lossy).first, expected);
  EXPECT_EQ(run_pipeline<Mutex>(lossy).first, expected);
}

TEST(Pipeline, TinyQueueForcesBackpressureWithoutLoss) {
  const auto [digest, stats] = run_pipeline<SpscQueue<MessageSlot, 4>>({});
  EXPECT_EQ(digest, direct_digest());
  EXPECT_GT(stats.full_waits, 0u);
}

TEST(Pipeline, ProducerExceptionStopsTheBookThread) {
  auto queue = std::make_unique<SpscQueue<MessageSlot, 8>>();
  std::uint64_t consumed = 0;
  const std::uint8_t msg[3] = {1, 2, 3};
  EXPECT_THROW(run(
                   *queue,
                   [&](auto& push) {
                     for (int i = 0; i < 100; ++i) push(0, Bytes(msg, 3));
                     throw std::runtime_error("feed failed");
                   },
                   [&](const MessageSlot&) { ++consumed; }),
               std::runtime_error);
  EXPECT_EQ(consumed, 100u);  // everything pushed before the failure was still applied
}

}  // namespace
}  // namespace mde::pipeline
