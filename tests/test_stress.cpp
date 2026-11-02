// Stress and failure handling: heavy and bursty loss, corrupt and foreign datagrams, a reorder
// buffer far too small for the disorder, a dead re-request server, absurd sequence jumps, sustained
// backpressure, and real kernel drops on a deliberately tiny socket buffer.
#include <gtest/gtest.h>

#include <thread>

#include "mde/book/book_builder.hpp"
#include "mde/book/fast_market.hpp"
#include "mde/book/reference_market.hpp"
#include "mde/moldudp64/replay.hpp"
#include "mde/net/session.hpp"
#include "mde/pipeline/pipeline.hpp"
#include "mde/synth/generator.hpp"

namespace mde {
namespace {

#ifdef NDEBUG
constexpr std::size_t kScale = 10;  // optimized builds run a larger stream
#else
constexpr std::size_t kScale = 1;
#endif

const std::vector<std::uint8_t>& fixture() {
  static const auto data = [] {
    synth::Config c;
    c.seed = 777;
    c.messages = 60'000 * kScale;
    c.symbols = 20;
    return synth::generate(c);
  }();
  return data;
}

std::uint64_t direct_digest() {
  static const std::uint64_t digest = [] {
    book::FastMarket market;
    book::BookBuilder builder(market);
    FramedReader reader{fixture()};
    Bytes msg;
    while (reader.next(msg) == FramedReader::Status::kOk) builder.apply(msg);
    return book::state_digest(market);
  }();
  return digest;
}

struct Outcome {
  moldudp64::ReplayResult replay;
  std::uint64_t digest;
  std::uint64_t anomalies;
  std::string violation;
};

Outcome replay_into_book(const moldudp64::ReplayOptions& options) {
  const moldudp64::MessageLog log(moldudp64::make_session("STRESS"), fixture());
  book::FastMarket market;
  book::BookBuilder builder(market);
  Outcome out;
  out.replay = moldudp64::replay(log, options, [&](std::uint64_t, Bytes m) { builder.apply(m); });
  out.digest = book::state_digest(market);
  out.anomalies = builder.counters().book_anomalies();
  out.violation = market.check_all_invariants();
  return out;
}

// At 25% loss each retransmission is itself lost a quarter of the time, so the default five
// attempts per hole leave about 0.25^5 = 0.1% of holes unrecovered (165 messages in this stream).
// The retry budget has to be sized to the loss rate; with 20 attempts recovery is exact.
TEST(Stress, HeavyLossDuplicationAndReorderingRecoverExactly) {
  moldudp64::ReplayOptions opt;
  opt.impairments = {0.25, 0.10, 0.10, 1};
  opt.sequencer.max_request_attempts = 20;
  const auto o = replay_into_book(opt);
  ASSERT_TRUE(o.replay.complete);
  EXPECT_EQ(o.replay.stats.messages_lost, 0u);
  EXPECT_EQ(o.digest, direct_digest());
}

TEST(Stress, BurstLossRecoversExactly) {
  moldudp64::ReplayOptions opt;
  opt.impairments.drop = 0.01;
  opt.impairments.burst = 50;  // each loss event takes out 50 consecutive packets
  opt.impairments.seed = 2;
  const auto o = replay_into_book(opt);
  ASSERT_TRUE(o.replay.complete);
  EXPECT_GT(o.replay.stats.requests_sent, o.replay.stats.gaps_detected);  // big holes need several requests
  EXPECT_EQ(o.replay.stats.messages_lost, 0u);
  EXPECT_EQ(o.digest, direct_digest());
}

TEST(Stress, CorruptAndForeignDatagramsAreRejectedAndRecovered) {
  moldudp64::ReplayOptions opt;
  opt.impairments.corrupt = 0.05;
  opt.impairments.garbage = 0.05;
  opt.impairments.seed = 3;
  const auto o = replay_into_book(opt);
  ASSERT_TRUE(o.replay.complete);
  EXPECT_GT(o.replay.stats.malformed_packets + o.replay.stats.session_mismatches, 0u);
  EXPECT_EQ(o.replay.stats.messages_lost, 0u);
  EXPECT_EQ(o.anomalies, 0u);
  EXPECT_EQ(o.digest, direct_digest());
}

TEST(Stress, ReorderBufferFarTooSmallStillRecovers) {
  moldudp64::ReplayOptions opt;
  opt.impairments = {0.10, 0.05, 0.30, 4};
  opt.sequencer.max_buffered_packets = 4;
  opt.sequencer.max_request_count = 16;  // and a stingy server-side request size
  const auto o = replay_into_book(opt);
  ASSERT_TRUE(o.replay.complete);
  EXPECT_GT(o.replay.stats.buffer_overflows, 0u);
  EXPECT_EQ(o.replay.stats.messages_lost, 0u);
  EXPECT_EQ(o.digest, direct_digest());
}

// No re-request server: loss is permanent. The accounting must be exact and the book must stay
// structurally valid, reporting the inconsistencies it was fed rather than crashing.
TEST(Stress, DeadRerequestServerLosesDataButStaysConsistent) {
  moldudp64::ReplayOptions opt;
  opt.impairments = {0.05, 0.02, 0.05, 5};
  opt.retransmit = false;
  const auto o = replay_into_book(opt);
  const moldudp64::MessageLog log(moldudp64::make_session("STRESS"), fixture());
  ASSERT_TRUE(o.replay.complete);
  EXPECT_EQ(o.replay.stats.delivered + o.replay.stats.messages_lost, log.size());
  EXPECT_GT(o.replay.stats.messages_lost, 0u);
  EXPECT_GT(o.anomalies, 0u);
  EXPECT_EQ(o.violation, "");
  EXPECT_NE(o.digest, direct_digest());
}

// A heartbeat claiming a sequence 10^12 ahead is beyond max_sequence_jump (2^32) and rejected
// outright. A large but plausible jump (10^9) is accepted: requests stay capped, the hole is
// skipped after the retry budget, and nothing is allocated in proportion to the hole.
TEST(Stress, AbsurdSequenceJumpIsRejectedAndLargeJumpIsBoundedWork) {
  moldudp64::SequencerConfig cfg;
  cfg.request_timeout_ns = 1;
  cfg.max_request_attempts = 3;
  moldudp64::Sequencer seq(cfg);
  const auto session = moldudp64::make_session("JUMP");
  std::vector<std::uint8_t> stream(10, 0);
  store_be16(stream.data(), 8);
  const moldudp64::MessageLog log(session, stream);
  std::uint64_t delivered = 0;
  auto deliver = [&](std::uint64_t, Bytes) { ++delivered; };
  seq.on_packet(log.packet(1, 1, 1400), 0, deliver);

  seq.on_packet(moldudp64::make_heartbeat(session, 1'000'000'000'002ULL), 0, deliver);
  EXPECT_EQ(seq.stats().implausible_sequences, 1u);
  EXPECT_FALSE(seq.has_gap());

  seq.on_packet(moldudp64::make_heartbeat(session, 1'000'000'002ULL), 0, deliver);
  ASSERT_TRUE(seq.has_gap());
  int requests = 0;
  for (std::uint64_t t = 1; t < 100 && seq.has_gap(); ++t) {
    if (auto r = seq.on_timer(t, deliver)) {
      EXPECT_LE(r->count, cfg.max_request_count);
      ++requests;
    }
  }
  EXPECT_EQ(requests, 3);
  EXPECT_FALSE(seq.has_gap());
  EXPECT_EQ(seq.stats().messages_lost, 1'000'000'000ULL);
  EXPECT_EQ(delivered, 1u);
}

// Book thread deliberately slower than the feed and a two-slot ring: the feed thread spends most
// of its time in backpressure, and nothing may be lost or reordered.
TEST(Stress, SustainedBackpressureThroughAThreadedPipeline) {
  const moldudp64::MessageLog log(moldudp64::make_session("STRESS"), fixture());
  auto queue = std::make_unique<pipeline::SpscQueue<pipeline::MessageSlot, 2>>();
  book::FastMarket market;
  book::BookBuilder builder(market);
  moldudp64::ReplayOptions opt;
  opt.impairments = {0.05, 0.05, 0.05, 6};
  moldudp64::ReplayResult result;
  const auto stats = pipeline::run(
      *queue, [&](auto& push) { result = moldudp64::replay(log, opt, [&](std::uint64_t s, Bytes m) { push(s, m); }); },
      [&](const pipeline::MessageSlot& slot) {
        builder.apply(Bytes(slot.data, slot.length));
        if (slot.ingress_ns % 64 == 0) std::this_thread::yield();  // make the book thread the bottleneck
      });
  ASSERT_TRUE(result.complete);
  EXPECT_GT(stats.full_waits, stats.messages / 10);
  EXPECT_EQ(stats.messages, log.size());
  EXPECT_EQ(book::state_digest(market), direct_digest());
}

// Real kernel loss: a receiver with a tiny socket buffer that sleeps between reads, against a
// publisher sending as fast as it can. The kernel drops datagrams; re-requests must recover them.
TEST(Stress, RealKernelDropsOnATinySocketBufferAreRecovered) {
  const moldudp64::MessageLog log(moldudp64::make_session("KERNEL"), fixture());
  net::UdpSocket receiver("127.0.0.1", 0);
  receiver.set_receive_buffer(16 * 1024);
  net::UdpSocket publisher_socket("127.0.0.1", 0);
  net::PublishOptions options;
  options.destination = *net::parse_endpoint("127.0.0.1:" + std::to_string(receiver.local_port()));
  options.packets_per_second = 0;  // unthrottled
  options.linger = std::chrono::milliseconds(20'000);
  std::atomic<bool> stop{false};
  net::PublishStats published;
  std::thread server([&] { published = net::publish(publisher_socket, log, options, stop); });

  book::FastMarket market;
  book::BookBuilder builder(market);
  const auto rerequest = net::parse_endpoint("127.0.0.1:" + std::to_string(publisher_socket.local_port()));
  std::uint64_t n = 0;
  const auto [stats, complete] = net::receive_session(receiver, rerequest, {}, std::chrono::milliseconds(5000),
                                                      [&](std::uint64_t, Bytes m) {
                                                        builder.apply(m);
                                                        if (++n % 2000 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                                      });
  stop = true;
  server.join();

  EXPECT_TRUE(complete);
  EXPECT_EQ(published.packets_dropped, 0u);  // every loss below happened in the kernel
  EXPECT_GT(stats.gaps_detected, 0u) << "the kernel dropped nothing; the test did not exercise recovery";
  EXPECT_EQ(stats.messages_lost, 0u);
  EXPECT_EQ(book::state_digest(market), direct_digest());
}

}  // namespace
}  // namespace mde
