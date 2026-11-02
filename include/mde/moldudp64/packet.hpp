// MoldUDP64 packet encoding and decoding (MoldUDP64 Protocol Specification v1.00).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "mde/common.hpp"

namespace mde::moldudp64 {

inline constexpr std::size_t kHeaderSize = 20;
inline constexpr std::uint16_t kEndOfSession = 0xFFFF;
inline constexpr std::uint16_t kMaxMessagesPerPacket = 0xFFFE;  // 0xFFFF is reserved for end of session

using Session = std::array<char, 10>;

[[nodiscard]] Session make_session(std::string_view name);

struct Header {
  Session session{};
  std::uint64_t sequence = 0;  // first message's sequence; for heartbeat/end of session, the next one
  std::uint16_t count = 0;     // 0 = heartbeat, 0xFFFF = end of session
};

enum class PacketStatus : std::uint8_t {
  kOk,
  kTooShort,        // fewer than 20 bytes
  kTruncatedBlock,  // a message block runs past the end of the datagram
  kTrailingBytes,   // bytes left after the declared message blocks (or any payload on a heartbeat)
  kBadSequence,     // sequence + count overflows 64 bits
};

[[nodiscard]] std::string_view to_string(PacketStatus s) noexcept;

struct Packet {
  Header header;
  Bytes blocks;  // validated message blocks: [u16 length][data] x message_count()

  bool is_heartbeat() const { return header.count == 0; }
  bool is_end_of_session() const { return header.count == kEndOfSession; }
  std::size_t message_count() const { return is_end_of_session() ? 0 : header.count; }
  // Sequence number of the message after this packet's last one.
  std::uint64_t next_sequence() const { return header.sequence + message_count(); }
};

// Validates the header and walks every message block before returning kOk, so a damaged packet
// is rejected whole and never partially delivered.
[[nodiscard]] PacketStatus parse(Bytes datagram, Packet& out) noexcept;

// Builds one downstream packet, adding messages until the next would exceed max_size.
class PacketBuilder {
 public:
  PacketBuilder(const Session& session, std::uint64_t first_sequence, std::size_t max_size);
  bool try_add(Bytes message);
  std::uint16_t count() const { return count_; }
  std::vector<std::uint8_t> finish();

 private:
  std::vector<std::uint8_t> buf_;
  std::size_t max_size_;
  std::uint16_t count_ = 0;
};

[[nodiscard]] std::vector<std::uint8_t> make_heartbeat(const Session& session, std::uint64_t next_sequence);
[[nodiscard]] std::vector<std::uint8_t> make_end_of_session(const Session& session, std::uint64_t next_sequence);

// Retransmission request, sent to a re-request server.
struct Request {
  Session session{};
  std::uint64_t sequence = 0;
  std::uint16_t count = 0;
  bool operator==(const Request&) const = default;
};

[[nodiscard]] std::array<std::uint8_t, kHeaderSize> encode(const Request& r) noexcept;
[[nodiscard]] std::optional<Request> parse_request(Bytes datagram) noexcept;

}  // namespace mde::moldudp64
