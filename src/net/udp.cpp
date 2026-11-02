#include "mde/net/udp.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <string>
#include <system_error>

namespace mde::net {
namespace {

[[noreturn]] void fail(const char* what) { throw std::system_error(errno, std::generic_category(), what); }

in_addr parse_ipv4(const std::string& text) {
  in_addr a{};
  if (inet_pton(AF_INET, text.c_str(), &a) != 1) throw std::invalid_argument("invalid IPv4 address: " + text);
  return a;
}

}  // namespace

std::string Endpoint::to_string() const {
  char host[INET_ADDRSTRLEN] = {};
  inet_ntop(AF_INET, &address.sin_addr, host, sizeof host);
  return std::string(host) + ":" + std::to_string(ntohs(address.sin_port));
}

std::optional<Endpoint> parse_endpoint(const std::string& text) {
  const auto colon = text.rfind(':');
  if (colon == std::string::npos) return std::nullopt;
  Endpoint e;
  e.address.sin_family = AF_INET;
  if (inet_pton(AF_INET, text.substr(0, colon).c_str(), &e.address.sin_addr) != 1) return std::nullopt;
  const std::string port_text = text.substr(colon + 1);
  if (port_text.empty() || port_text.size() > 5 ||
      port_text.find_first_not_of("0123456789") != std::string::npos) {
    return std::nullopt;
  }
  const unsigned long port = std::stoul(port_text);
  if (port > 65535) return std::nullopt;
  e.address.sin_port = htons(static_cast<std::uint16_t>(port));
  return e;
}

UdpSocket::UdpSocket(const std::string& bind_address, std::uint16_t port, const std::string& multicast_group,
                     const std::string& interface_address) {
  fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd_ < 0) fail("socket");
  try {
    // Only multicast receivers share a port; a unicast socket must own its port outright.
    if (!multicast_group.empty()) {
      const int on = 1;
      if (setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) != 0) fail("SO_REUSEADDR");
#ifdef SO_REUSEPORT
      if (setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &on, sizeof on) != 0) fail("SO_REUSEPORT");  // macOS/BSD
#endif
    }
    // Ask for a large buffer: the socket buffer is what absorbs bursts while the feed thread is busy.
    const int want = 8 << 20;
    setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &want, sizeof want);  // best effort; see receive_buffer_bytes()

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr = parse_ipv4(bind_address);
    if (::bind(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) fail("bind");

    if (!multicast_group.empty()) {
      ip_mreq mreq{};
      mreq.imr_multiaddr = parse_ipv4(multicast_group);
      mreq.imr_interface = parse_ipv4(interface_address);
      if (setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq) != 0) fail("IP_ADD_MEMBERSHIP");
    }
  } catch (...) {
    ::close(fd_);
    throw;
  }
}

UdpSocket::~UdpSocket() {
  if (fd_ >= 0) ::close(fd_);
}

UdpSocket::UdpSocket(UdpSocket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

std::optional<std::size_t> UdpSocket::receive(std::span<std::uint8_t> buffer, int timeout_ms, Endpoint* from) {
  pollfd p{fd_, POLLIN, 0};
  for (;;) {
    const int ready = ::poll(&p, 1, timeout_ms);
    if (ready == 0) return std::nullopt;
    if (ready < 0) {
      if (errno == EINTR) continue;
      fail("poll");
    }
    sockaddr_in peer{};
    socklen_t peer_len = sizeof peer;
    // Non-blocking: Linux can report a datagram readable and then discard it (bad checksum), and a
    // blocking read here would then hang past the caller's timeout.
    const ssize_t n = ::recvfrom(fd_, buffer.data(), buffer.size(), MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&peer), &peer_len);
    if (n < 0) {
      if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
      fail("recvfrom");
    }
    if (from) from->address = peer;
    return static_cast<std::size_t>(n);
  }
}

bool UdpSocket::send_to(Bytes data, const Endpoint& to) {
  for (;;) {
    const ssize_t n = ::sendto(fd_, data.data(), data.size(), 0, reinterpret_cast<const sockaddr*>(&to.address),
                               sizeof to.address);
    if (n >= 0) return true;
    if (errno == EINTR) continue;
    // Transient conditions are just loss for UDP, which MoldUDP64 recovers from. Anything else
    // (EMSGSIZE, EBADF, ...) is a programming or configuration error.
    if (errno == ENOBUFS || errno == EAGAIN || errno == EWOULDBLOCK || errno == EHOSTUNREACH ||
        errno == ENETUNREACH || errno == ENETDOWN || errno == ECONNREFUSED || errno == EPERM) {
      return false;
    }
    fail("sendto");
  }
}

std::uint16_t UdpSocket::local_port() const {
  sockaddr_in addr{};
  socklen_t len = sizeof addr;
  if (getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) fail("getsockname");
  return ntohs(addr.sin_port);
}

int UdpSocket::receive_buffer_bytes() const {
  int size = 0;
  socklen_t len = sizeof size;
  getsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &size, &len);
  return size;
}

void UdpSocket::set_receive_buffer(int bytes) {
  if (setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof bytes) != 0) fail("SO_RCVBUF");
}

void UdpSocket::set_multicast(int ttl, bool loopback, const std::string& interface_address) {
  const unsigned char ttl_byte = static_cast<unsigned char>(ttl);
  const unsigned char loop_byte = loopback ? 1 : 0;
  const in_addr iface = parse_ipv4(interface_address);
  if (setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl_byte, sizeof ttl_byte) != 0) fail("IP_MULTICAST_TTL");
  if (setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &loop_byte, sizeof loop_byte) != 0) fail("IP_MULTICAST_LOOP");
  if (setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &iface, sizeof iface) != 0) fail("IP_MULTICAST_IF");
}

}  // namespace mde::net
