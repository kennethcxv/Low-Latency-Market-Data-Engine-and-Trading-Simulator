// Offline MoldUDP64 replay: the publishing side of a session held in memory, a seeded lossy
// channel, and a deterministic loop that drives a Sequencer through loss and retransmission.
// The same MessageLog backs `mde serve` when replaying over real UDP.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "mde/moldudp64/packet.hpp"
#include "mde/moldudp64/sequencer.hpp"

namespace mde::moldudp64 {

// A sequenced message stream as a MoldUDP64 server holds it: message N has sequence number N.
class MessageLog {
 public:
  // `framed` is [u16 length][message]... (an ITCH file); it must outlive the log.
  MessageLog(const Session& session, Bytes framed);

  const Session& session() const { return session_; }
  std::uint64_t size() const { return offsets_.size(); }
  Bytes message(std::uint64_t sequence) const;

  // One packet of consecutive messages starting at `first`: at most `limit` messages and
  // `max_size` bytes, but always at least one message if any remain.
  std::vector<std::uint8_t> packet(std::uint64_t first, std::uint64_t limit, std::size_t max_size) const;
  std::vector<std::vector<std::uint8_t>> packetize(std::size_t max_size) const;
  // Response to a re-request, or nothing if it names another session or lies outside the log.
  std::optional<std::vector<std::uint8_t>> answer(const Request& r, std::size_t max_size) const;

 private:
  Session session_;
  Bytes framed_;
  std::vector<std::size_t> offsets_;  // byte offset of each message's length prefix
};

struct Impairments {
  double drop = 0;       // probability a packet (including a retransmission) is lost
  double duplicate = 0;  // probability a packet is delivered twice
  double reorder = 0;    // probability a packet is held back behind up to 8 later packets
  std::uint64_t seed = 1;
  std::size_t burst = 1;  // each drop loses this many consecutive packets
  double corrupt = 0;     // probability a packet arrives truncated at a random byte
  double garbage = 0;     // probability a random datagram is injected (almost always malformed)
};

// Applies seeded drop/duplicate/reorder/corruption to a packet sequence, in order.
std::vector<std::vector<std::uint8_t>> impair(const std::vector<std::vector<std::uint8_t>>& packets,
                                              const Impairments& impairments);

struct ReplayOptions {
  Impairments impairments;
  bool retransmit = true;  // false: no re-request server, holes are skipped after a timeout
  std::size_t max_packet_size = 1400;
  SequencerConfig sequencer;
};

struct ReplayResult {
  SequencerStats stats;
  std::uint64_t packets_sent = 0;
  std::uint64_t retransmissions = 0;
  bool complete = false;
};

// Publishes the whole log through the impaired channel, answers re-requests from the log, and
// finishes with end-of-session packets until the sequencer is complete. Retransmissions are
// subject to `drop` only (not duplication, reordering or corruption). Simulated time advances
// 10 us per packet, so the result is a pure function of the inputs.
template <class Deliver>
ReplayResult replay(const MessageLog& log, ReplayOptions options, Deliver&& deliver) {
  if (!options.retransmit) options.sequencer.max_request_attempts = 0;
  options.sequencer.session = log.session();
  Sequencer seq(options.sequencer);
  Rng response_loss(options.impairments.seed ^ 0x5eed);
  ReplayResult result;
  std::uint64_t now_ns = 0;

  auto service_timers = [&] {
    while (auto request = seq.on_timer(now_ns, deliver)) {
      auto response = log.answer(*request, options.max_packet_size);
      if (!response || response_loss.chance(options.impairments.drop)) continue;
      ++result.retransmissions;
      seq.on_packet(*response, now_ns, deliver);
    }
  };

  for (const auto& packet : impair(log.packetize(options.max_packet_size), options.impairments)) {
    now_ns += 10'000;
    ++result.packets_sent;
    seq.on_packet(packet, now_ns, deliver);
    service_timers();
  }
  const auto end_of_session = make_end_of_session(log.session(), log.size() + 1);
  for (int i = 0; i < 10'000 && !seq.complete(); ++i) {
    now_ns += options.sequencer.request_timeout_ns;
    if (!response_loss.chance(options.impairments.drop)) seq.on_packet(end_of_session, now_ns, deliver);
    service_timers();
  }
  result.stats = seq.stats();
  result.complete = seq.complete();
  return result;
}

}  // namespace mde::moldudp64
