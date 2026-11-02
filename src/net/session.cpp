#include "mde/net/session.hpp"

#include <thread>

namespace mde::net {
namespace {

void answer_requests(UdpSocket& socket, const moldudp64::MessageLog& log, const PublishOptions& options,
                     PublishStats& stats, int timeout_ms) {
  std::uint8_t buf[512];
  Endpoint from;
  // Bounded per call so a stream of requests cannot starve publishing, linger or stop.
  for (int handled = 0; handled < 64; ++handled) {
    const auto n = socket.receive(buf, timeout_ms, &from);
    if (!n) return;
    timeout_ms = 0;  // drain whatever else is queued without waiting
    const auto request = moldudp64::parse_request(Bytes(buf, *n));
    if (!request) continue;
    ++stats.requests;
    if (auto response = log.answer(*request, options.max_packet_size)) {
      socket.send_to(*response, from);  // unicast back to the requesting socket
      ++stats.retransmissions;
    }
  }
}

}  // namespace

PublishStats publish(UdpSocket& socket, const moldudp64::MessageLog& log, const PublishOptions& options,
                     const std::atomic<bool>& stop) {
  using Clock = std::chrono::steady_clock;
  PublishStats stats;
  Rng rng(options.seed);
  const auto start = Clock::now();
  const auto packets = log.packetize(options.max_packet_size);
  for (std::size_t i = 0; i < packets.size() && !stop.load(std::memory_order_relaxed); ++i) {
    if (options.packets_per_second > 0) {
      std::this_thread::sleep_until(start + std::chrono::nanoseconds(i * 1'000'000'000ULL / options.packets_per_second));
    }
    if (rng.chance(options.drop)) {
      ++stats.packets_dropped;
    } else {
      socket.send_to(packets[i], options.destination);
      ++stats.packets_sent;
    }
    answer_requests(socket, log, options, stats, 0);
  }

  const auto end_of_session = moldudp64::make_end_of_session(log.session(), log.size() + 1);
  const auto linger_until = Clock::now() + options.linger;
  do {  // always at least one end-of-session packet, even with no linger
    socket.send_to(end_of_session, options.destination);
    answer_requests(socket, log, options, stats, 100);
  } while (Clock::now() < linger_until && !stop.load(std::memory_order_relaxed));
  return stats;
}

}  // namespace mde::net
