#include <gtest/gtest.h>

#include "mde/moldudp64/replay.hpp"
#include "mde/moldudp64/sequencer.hpp"

namespace mde::moldudp64 {
namespace {

// Message N carries N as 8 big-endian bytes, so delivery order and content are checkable.
std::vector<std::uint8_t> numbered_stream(std::uint64_t n) {
  std::vector<std::uint8_t> out;
  for (std::uint64_t i = 1; i <= n; ++i) {
    std::uint8_t m[10];
    store_be16(m, 8);
    store_be64(m + 2, i);
    out.insert(out.end(), m, m + 10);
  }
  return out;
}

class SequencerTest : public ::testing::Test {
 protected:
  SequencerTest() : stream(numbered_stream(100)), log(make_session("TEST"), stream) {}

  // Each test packet holds `count` messages starting at `first`.
  std::vector<std::uint8_t> pkt(std::uint64_t first, std::uint64_t count) { return log.packet(first, count, 1400); }

  void feed(Sequencer& s, const std::vector<std::uint8_t>& datagram, std::uint64_t now = 0) { s.on_packet(datagram, now, collect); }
  std::optional<Request> timer(Sequencer& s, std::uint64_t now) { return s.on_timer(now, collect); }

  std::vector<std::uint8_t> stream;
  MessageLog log;
  std::vector<std::uint64_t> delivered;
  std::function<void(std::uint64_t, Bytes)> collect = [this](std::uint64_t seq, Bytes msg) {
    ASSERT_EQ(msg.size(), 8u);
    ASSERT_EQ(load_be64(msg.data()), seq);
    delivered.push_back(seq);
  };

  static std::vector<std::uint64_t> range(std::uint64_t first, std::uint64_t last) {
    std::vector<std::uint64_t> v;
    for (auto i = first; i <= last; ++i) v.push_back(i);
    return v;
  }
};

TEST_F(SequencerTest, DeliversInOrderAndDropsDuplicatesAndOverlaps) {
  Sequencer s;
  feed(s, pkt(1, 5));
  feed(s, pkt(1, 5));  // exact duplicate
  feed(s, pkt(3, 6));  // overlaps 3..5, new 6..8 (retransmission with different boundaries)
  EXPECT_EQ(delivered, range(1, 8));
  EXPECT_EQ(s.stats().duplicates, 8u);
  EXPECT_FALSE(s.has_gap());
  EXPECT_FALSE(timer(s, 0));
}

TEST_F(SequencerTest, BuffersAfterHoleRequestsOnlyTheHoleThenDrains) {
  Sequencer s;
  feed(s, pkt(1, 3));
  feed(s, pkt(7, 3));   // 4..6 missing
  feed(s, pkt(10, 2));  // contiguous with the buffered packet
  EXPECT_EQ(delivered, range(1, 3));
  EXPECT_EQ(s.stats().gaps_detected, 1u);

  const auto req = timer(s, 0);
  ASSERT_TRUE(req);
  EXPECT_EQ(req->sequence, 4u);
  EXPECT_EQ(req->count, 3u);  // only the hole, not the buffered 7..11
  feed(s, *log.answer(*req, 1400));
  EXPECT_EQ(delivered, range(1, 11));
  EXPECT_FALSE(s.has_gap());
  EXPECT_EQ(s.stats().out_of_order_packets, 2u);
}

TEST_F(SequencerTest, HeartbeatRevealsTailLoss) {
  Sequencer s;
  feed(s, pkt(1, 5));
  EXPECT_FALSE(s.has_gap());
  feed(s, make_heartbeat(log.session(), 9));  // server has sent through 8
  ASSERT_TRUE(s.has_gap());
  const auto req = timer(s, 0);
  ASSERT_TRUE(req);
  EXPECT_EQ(req->sequence, 6u);
  EXPECT_EQ(req->count, 3u);
}

TEST_F(SequencerTest, RepeatsRequestsAfterTimeoutAndFollowsPartialAnswers) {
  SequencerConfig cfg;
  cfg.request_timeout_ns = 100;
  Sequencer s(cfg);
  feed(s, pkt(1, 1));
  feed(s, pkt(50, 1));
  auto req = timer(s, 1000);
  ASSERT_TRUE(req);
  EXPECT_EQ(req->count, 48u);
  EXPECT_FALSE(timer(s, 1050));  // still waiting
  req = timer(s, 1100);          // timed out: ask again
  ASSERT_TRUE(req);
  EXPECT_EQ(s.stats().requests_sent, 2u);

  feed(s, log.packet(2, 10, 1400));  // server returned only part of the range
  EXPECT_EQ(delivered.back(), 11u);
  req = timer(s, 1101);  // progress resets the attempt budget: ask for the rest immediately
  ASSERT_TRUE(req);
  EXPECT_EQ(req->sequence, 12u);
  EXPECT_EQ(req->count, 38u);
}

TEST_F(SequencerTest, SkipsHoleAfterAttemptsAreExhausted) {
  SequencerConfig cfg;
  cfg.request_timeout_ns = 10;
  cfg.max_request_attempts = 2;
  Sequencer s(cfg);
  feed(s, pkt(1, 2));
  feed(s, pkt(6, 2));
  EXPECT_TRUE(timer(s, 0));
  EXPECT_TRUE(timer(s, 10));
  EXPECT_FALSE(timer(s, 15));  // last attempt gets one timeout
  EXPECT_FALSE(timer(s, 20));  // gives up on 3..5, delivers the buffered 6..7
  EXPECT_EQ(delivered, (std::vector<std::uint64_t>{1, 2, 6, 7}));
  EXPECT_EQ(s.stats().gaps_skipped, 1u);
  EXPECT_EQ(s.stats().messages_lost, 3u);
  EXPECT_FALSE(s.has_gap());

  feed(s, pkt(3, 3));  // the late retransmission is now just a duplicate
  EXPECT_EQ(delivered.size(), 4u);
}

TEST_F(SequencerTest, WithoutRetransmissionReorderingStillGetsOneTimeout) {
  SequencerConfig cfg;
  cfg.max_request_attempts = 0;
  cfg.request_timeout_ns = 100;
  Sequencer s(cfg);
  feed(s, pkt(1, 2));
  feed(s, pkt(5, 2));
  EXPECT_FALSE(timer(s, 0));
  feed(s, pkt(3, 2));  // arrives late but within the timeout
  EXPECT_EQ(delivered, range(1, 6));
  EXPECT_EQ(s.stats().requests_sent, 0u);
  EXPECT_EQ(s.stats().messages_lost, 0u);
}

TEST_F(SequencerTest, WithoutRetransmissionHolesAgeInParallel) {
  SequencerConfig cfg;
  cfg.max_request_attempts = 0;
  cfg.request_timeout_ns = 100;
  Sequencer s(cfg);
  feed(s, pkt(1, 1), 0);
  feed(s, pkt(3, 1), 10);  // hole at 2
  feed(s, pkt(5, 1), 20);  // hole at 4
  feed(s, pkt(7, 1), 30);  // hole at 6
  EXPECT_EQ(s.stats().gaps_detected, 3u);
  EXPECT_FALSE(timer(s, 110));  // only the first hole (revealed at t=10) has waited a full timeout
  EXPECT_EQ(delivered, (std::vector<std::uint64_t>{1, 3}));
  EXPECT_FALSE(timer(s, 130));  // the other two expire together, not one timeout after another
  EXPECT_EQ(delivered, (std::vector<std::uint64_t>{1, 3, 5, 7}));
  EXPECT_EQ(s.stats().messages_lost, 3u);
  EXPECT_EQ(s.stats().gaps_skipped, 3u);
}

TEST_F(SequencerTest, RejectsOtherSessionsAndMalformedPackets) {
  SequencerConfig cfg;
  cfg.session = make_session("TEST");
  Sequencer s(cfg);
  MessageLog other(make_session("OTHER"), stream);
  feed(s, other.packet(1, 5, 1400));
  auto bad = pkt(1, 5);
  bad.pop_back();
  feed(s, bad);
  EXPECT_TRUE(delivered.empty());
  EXPECT_EQ(s.stats().session_mismatches, 1u);
  EXPECT_EQ(s.stats().malformed_packets, 1u);
  feed(s, pkt(1, 5));
  EXPECT_EQ(delivered, range(1, 5));
}

TEST_F(SequencerTest, BufferOverflowIsRecoveredByLaterRequests) {
  SequencerConfig cfg;
  cfg.max_buffered_packets = 2;
  Sequencer s(cfg);
  feed(s, pkt(1, 1));
  for (std::uint64_t first = 3; first <= 9; first += 2) feed(s, pkt(first, 2));  // 2 is missing
  EXPECT_EQ(s.stats().buffer_overflows, 2u);
  for (int i = 0; i < 10 && s.has_gap(); ++i) {
    if (auto req = timer(s, static_cast<std::uint64_t>(i) * 1'000'000'000)) feed(s, *log.answer(*req, 1400));
  }
  EXPECT_EQ(delivered, range(1, 10));
}

TEST_F(SequencerTest, CompleteOnlyAfterEndOfSessionWithNothingMissing) {
  Sequencer s;
  feed(s, pkt(1, 4));
  feed(s, make_end_of_session(log.session(), 6));
  EXPECT_FALSE(s.complete());  // message 5 is still missing
  auto req = timer(s, 0);
  ASSERT_TRUE(req);
  feed(s, *log.answer(*req, 1400));
  EXPECT_TRUE(s.complete());
}

// Exactly-once, in-order delivery of the whole stream through random loss, duplication and
// reordering (including lost retransmissions), for many seeds.
TEST(SequencerProperty, ReplayRecoversEveryMessageUnderImpairment) {
  const auto stream = numbered_stream(5000);
  const MessageLog log(make_session("PROP"), stream);
  for (std::uint64_t seed = 1; seed <= 40; ++seed) {
    ReplayOptions opt;
    opt.impairments = {0.05 + 0.005 * static_cast<double>(seed % 10), 0.05, 0.05, seed};
    opt.max_packet_size = 20 + 10 * (1 + seed % 30);  // 1..30 messages per packet
    std::uint64_t expected = 1;
    const auto result = replay(log, opt, [&](std::uint64_t seq, Bytes msg) {
      ASSERT_EQ(seq, expected);
      ASSERT_EQ(load_be64(msg.data()), seq);
      ++expected;
    });
    ASSERT_TRUE(result.complete) << "seed " << seed;
    EXPECT_EQ(expected, 5001u) << "seed " << seed;
    EXPECT_EQ(result.stats.messages_lost, 0u);
    EXPECT_GT(result.stats.gaps_detected, 0u);
    EXPECT_GT(result.retransmissions, 0u);
  }
}

}  // namespace
}  // namespace mde::moldudp64
