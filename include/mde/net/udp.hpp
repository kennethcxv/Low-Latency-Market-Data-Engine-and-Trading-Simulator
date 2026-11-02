// Minimal POSIX UDP socket (IPv4, unicast and multicast). Used for live MoldUDP64 reception,
// re-requests, and the `mde serve` test publisher. Linux and macOS.
#pragma once

#include <netinet/in.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "mde/common.hpp"

namespace mde::net {

struct Endpoint {
  sockaddr_in address{};
  std::string to_string() const;
};

// "host:port" with a dotted-quad IPv4 host.
std::optional<Endpoint> parse_endpoint(const std::string& text);

class UdpSocket {
 public:
  // Binds to bind_address:port (port 0 picks an ephemeral port). With a multicast group, the
  // socket also joins it on `interface_address`. Throws std::system_error on failure.
  UdpSocket(const std::string& bind_address, std::uint16_t port, const std::string& multicast_group = {},
            const std::string& interface_address = "0.0.0.0");
  ~UdpSocket();
  UdpSocket(UdpSocket&& other) noexcept;
  UdpSocket& operator=(UdpSocket&&) = delete;
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;

  // Waits up to timeout_ms for one datagram; returns its size, or nothing on timeout.
  std::optional<std::size_t> receive(std::span<std::uint8_t> buffer, int timeout_ms, Endpoint* from = nullptr);
  void send_to(Bytes data, const Endpoint& to);

  std::uint16_t local_port() const;
  // Kernel receive buffer actually granted (Linux caps requests at net.core.rmem_max; it also
  // reports double the requested value to account for bookkeeping).
  int receive_buffer_bytes() const;
  void set_receive_buffer(int bytes);
  // For multicast publishing: TTL and whether our own host receives what we send.
  void set_multicast(int ttl, bool loopback, const std::string& interface_address = "0.0.0.0");

 private:
  int fd_ = -1;
};

}  // namespace mde::net
