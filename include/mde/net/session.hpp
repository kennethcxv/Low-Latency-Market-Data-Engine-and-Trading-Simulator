// MoldUDP64 over real UDP: a publisher (the role of Nasdaq's server and re-request server, used
// for local testing) and a receive loop that drives a Sequencer from a socket.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#include "mde/moldudp64/replay.hpp"
#include "mde/moldudp64/sequencer.hpp"
#include "mde/net/udp.hpp"

namespace mde::net {

struct PublishOptions {
  Endpoint destination;
  std::size_t max_packet_size = 1400;
  std::uint64_t packets_per_second = 0;  // 0 = as fast as the socket accepts
  double drop = 0;                       // deliberately skip this fraction of first transmissions
  std::uint64_t seed = 1;
  std::chrono::milliseconds linger{2000};  // end-of-session phase; re-requests are still answered
};

struct PublishStats {
  std::uint64_t packets_sent = 0;
  std::uint64_t packets_dropped = 0;
  std::uint64_t requests = 0;
  std::uint64_t retransmissions = 0;
};

// Sends every message of `log`, answering re-requests that arrive on `socket` throughout, then
// sends end-of-session packets (about every 100 ms, sooner while requests are arriving) until
// `linger` elapses or `stop` is set. A local test tool: it answers any requester, so do not expose
// it on an untrusted network (a 20-byte request yields up to max_packet_size bytes).
PublishStats publish(UdpSocket& socket, const moldudp64::MessageLog& log, const PublishOptions& options,
                     const std::atomic<bool>& stop);

struct ReceiveResult {
  moldudp64::SequencerStats stats;
  bool complete = false;  // end of session seen and nothing missing (lost messages were skipped)
};

// Receives one session on `socket` until end of session with nothing missing, or until no packet
// has arrived for `idle_timeout`. Re-requests go to `rerequest_server` from the same socket, as
// the spec intends, so retransmissions arrive where the data does. Without a server, holes are
// skipped after the sequencer's timeout.
template <class Deliver>
ReceiveResult receive_session(UdpSocket& socket, const std::optional<Endpoint>& rerequest_server,
                                          moldudp64::SequencerConfig config, std::chrono::milliseconds idle_timeout,
                                          Deliver&& deliver) {
  using Clock = std::chrono::steady_clock;
  if (!rerequest_server) config.max_request_attempts = 0;
  moldudp64::Sequencer seq(config);
  std::vector<std::uint8_t> buffer(65536);  // larger than any UDP payload, so nothing is truncated
  const auto start = Clock::now();
  auto last_packet = start;
  while (!seq.complete()) {
    const auto n = socket.receive(buffer, 5);
    const auto now = Clock::now();
    const auto now_ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count());
    if (n) {
      seq.on_packet(Bytes(buffer.data(), *n), now_ns, deliver);
      last_packet = now;
    } else if (now - last_packet > idle_timeout) {
      break;
    }
    while (auto request = seq.on_timer(now_ns, deliver)) socket.send_to(moldudp64::encode(*request), *rerequest_server);
  }
  return {seq.stats(), seq.complete()};
}

}  // namespace mde::net
