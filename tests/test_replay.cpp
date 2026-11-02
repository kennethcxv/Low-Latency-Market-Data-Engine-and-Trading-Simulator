// End-to-end replay: synthetic ITCH -> MoldUDP64 -> (loss, retransmission) -> book.
#include <gtest/gtest.h>

#include <thread>

#include "mde/book/book_builder.hpp"
#include "mde/book/reference_market.hpp"
#include "mde/moldudp64/replay.hpp"
#include "mde/net/session.hpp"
#include "mde/synth/generator.hpp"

namespace mde {
namespace {

const std::vector<std::uint8_t>& fixture() {
  static const auto data = [] {
    synth::Config c;
    c.seed = 99;
    c.messages = 40'000;
    c.symbols = 12;
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

TEST(Replay, LossyReplayWithRetransmissionRebuildsTheSameBook) {
  const moldudp64::MessageLog log(moldudp64::make_session("SYNTH"), fixture());
  const std::uint64_t expected = direct_digest();
  for (std::uint64_t seed : {1ULL, 2ULL, 3ULL}) {
    moldudp64::ReplayOptions opt;
    opt.impairments = {0.02, 0.01, 0.02, seed};
    book::ReferenceMarket market;
    book::BookBuilder builder(market);
    const auto result = moldudp64::replay(log, opt, [&](std::uint64_t, Bytes msg) { builder.apply(msg); });
    ASSERT_TRUE(result.complete);
    EXPECT_GT(result.stats.gaps_detected, 0u);
    EXPECT_EQ(result.stats.messages_lost, 0u);
    EXPECT_EQ(builder.counters().book_anomalies(), 0u);
    EXPECT_EQ(book::state_digest(market), expected) << "seed " << seed;
  }
}

TEST(Replay, IsDeterministic) {
  const moldudp64::MessageLog log(moldudp64::make_session("SYNTH"), fixture());
  moldudp64::ReplayOptions opt;
  opt.impairments = {0.05, 0.05, 0.05, 7};
  std::uint64_t digests[2];
  moldudp64::SequencerStats stats[2];
  for (int run = 0; run < 2; ++run) {
    book::ReferenceMarket market;
    book::BookBuilder builder(market);
    stats[run] = moldudp64::replay(log, opt, [&](std::uint64_t, Bytes msg) { builder.apply(msg); }).stats;
    digests[run] = book::state_digest(market);
  }
  EXPECT_EQ(digests[0], digests[1]);
  EXPECT_EQ(stats[0].requests_sent, stats[1].requests_sent);
  EXPECT_EQ(stats[0].duplicates, stats[1].duplicates);
}

// Without a re-request server, lost messages stay lost: the sequencer must say so, and the book
// must stay structurally valid while reporting the inconsistencies it saw.
TEST(Replay, UnrecoverableLossIsReportedAndBookStaysValid) {
  const moldudp64::MessageLog log(moldudp64::make_session("SYNTH"), fixture());
  moldudp64::ReplayOptions opt;
  opt.impairments = {0.02, 0, 0, 5};
  opt.retransmit = false;
  book::ReferenceMarket market;
  book::BookBuilder builder(market, /*check_invariants=*/true);
  const auto result = moldudp64::replay(log, opt, [&](std::uint64_t, Bytes msg) { builder.apply(msg); });
  EXPECT_TRUE(result.complete);
  EXPECT_GT(result.stats.messages_lost, 0u);
  EXPECT_EQ(result.stats.delivered + result.stats.messages_lost, log.size());
  EXPECT_GT(builder.counters().book_anomalies(), 0u);
  EXPECT_EQ(builder.counters().invariant_violations, 0u) << builder.first_violation();
  EXPECT_EQ(market.check_all_invariants(), "");
}

TEST(Net, ParseEndpointIsStrict) {
  EXPECT_TRUE(net::parse_endpoint("127.0.0.1:31001"));
  EXPECT_EQ(net::parse_endpoint("10.1.2.3:65535")->to_string(), "10.1.2.3:65535");
  for (const char* bad : {"127.0.0.1", "127.0.0.1:", "127.0.0.1:80abc", "127.0.0.1: 80", "127.0.0.1:+80",
                          "127.0.0.1:65536", "localhost:80", "1.2.3:80"}) {
    EXPECT_FALSE(net::parse_endpoint(bad)) << bad;
  }
}

// Real sockets on loopback: the publisher drops 3% of first transmissions on purpose, and the
// receiver must recover them through re-requests sent from its own socket.
TEST(Replay, UdpLoopbackRecoversDeliberateLoss) {
  const moldudp64::MessageLog log(moldudp64::make_session("UDPTEST"), fixture());
  net::UdpSocket receiver("127.0.0.1", 0);
  net::UdpSocket publisher_socket("127.0.0.1", 0);

  net::PublishOptions options;
  options.destination = *net::parse_endpoint("127.0.0.1:" + std::to_string(receiver.local_port()));
  options.drop = 0.03;
  options.packets_per_second = 50'000;  // keep loopback from overflowing the receive buffer
  options.linger = std::chrono::milliseconds(5000);
  std::atomic<bool> stop{false};
  net::PublishStats published;
  std::thread server([&] { published = net::publish(publisher_socket, log, options, stop); });

  book::ReferenceMarket market;
  book::BookBuilder builder(market);
  const auto rerequest = net::parse_endpoint("127.0.0.1:" + std::to_string(publisher_socket.local_port()));
  const auto [stats, complete] = net::receive_session(receiver, rerequest, {}, std::chrono::milliseconds(3000),
                                                      [&](std::uint64_t, Bytes msg) { builder.apply(msg); });
  stop = true;
  server.join();

  EXPECT_GT(published.packets_dropped, 0u);
  EXPECT_GT(published.retransmissions, 0u);
  EXPECT_TRUE(complete);
  EXPECT_EQ(stats.messages_lost, 0u);
  EXPECT_EQ(stats.delivered, log.size());
  EXPECT_EQ(book::state_digest(market), direct_digest());
}

}  // namespace
}  // namespace mde
