#include "mde/moldudp64/packet.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace mde::moldudp64 {
namespace {

Header read_header(const std::uint8_t* p) noexcept {
  Header h;
  std::copy_n(p, h.session.size(), h.session.begin());
  h.sequence = load_be64(p + 10);
  h.count = load_be16(p + 18);
  return h;
}

void write_header(std::uint8_t* p, const Header& h) noexcept {
  std::copy_n(h.session.begin(), h.session.size(), p);
  store_be64(p + 10, h.sequence);
  store_be16(p + 18, h.count);
}

std::vector<std::uint8_t> header_only(const Session& session, std::uint64_t sequence, std::uint16_t count) {
  std::vector<std::uint8_t> out(kHeaderSize);
  write_header(out.data(), {session, sequence, count});
  return out;
}

}  // namespace

Session make_session(std::string_view name) {
  Session s;
  for (std::size_t i = 0; i < s.size(); ++i) s[i] = i < name.size() ? name[i] : ' ';
  return s;
}

std::string_view to_string(PacketStatus s) noexcept {
  switch (s) {
    case PacketStatus::kOk: return "ok";
    case PacketStatus::kTooShort: return "too_short";
    case PacketStatus::kTruncatedBlock: return "truncated_block";
    case PacketStatus::kTrailingBytes: return "trailing_bytes";
    case PacketStatus::kBadSequence: return "bad_sequence";
  }
  return "?";
}

PacketStatus parse(Bytes datagram, Packet& out) noexcept {
  if (datagram.size() < kHeaderSize) return PacketStatus::kTooShort;
  out.header = read_header(datagram.data());
  const Bytes blocks = datagram.subspan(kHeaderSize);
  out.blocks = blocks;
  const std::size_t count = out.message_count();
  if (out.header.sequence > std::numeric_limits<std::uint64_t>::max() - count) return PacketStatus::kBadSequence;

  FramedReader reader{blocks};
  Bytes msg;
  for (std::size_t i = 0; i < count; ++i) {
    if (reader.next(msg) != FramedReader::Status::kOk) return PacketStatus::kTruncatedBlock;
  }
  if (reader.offset() != blocks.size()) return PacketStatus::kTrailingBytes;
  return PacketStatus::kOk;
}

PacketBuilder::PacketBuilder(const Session& session, std::uint64_t first_sequence, std::size_t max_size)
    : buf_(header_only(session, first_sequence, 0)), max_size_(max_size) {
  if (max_size < kHeaderSize + 2) throw std::invalid_argument("max packet size too small");
}

bool PacketBuilder::try_add(Bytes message) {
  if (count_ == kMaxMessagesPerPacket || message.size() > 0xFFFF) return false;
  if (buf_.size() + 2 + message.size() > max_size_) return false;
  const std::size_t at = buf_.size();
  buf_.resize(at + 2 + message.size());
  store_be16(buf_.data() + at, static_cast<std::uint16_t>(message.size()));
  std::copy(message.begin(), message.end(), buf_.begin() + static_cast<std::ptrdiff_t>(at + 2));
  ++count_;
  return true;
}

std::vector<std::uint8_t> PacketBuilder::finish() {
  store_be16(buf_.data() + 18, count_);
  return std::move(buf_);
}

std::vector<std::uint8_t> make_heartbeat(const Session& session, std::uint64_t next_sequence) {
  return header_only(session, next_sequence, 0);
}

std::vector<std::uint8_t> make_end_of_session(const Session& session, std::uint64_t next_sequence) {
  return header_only(session, next_sequence, kEndOfSession);
}

std::array<std::uint8_t, kHeaderSize> encode(const Request& r) noexcept {
  std::array<std::uint8_t, kHeaderSize> out{};
  write_header(out.data(), {r.session, r.sequence, r.count});
  return out;
}

std::optional<Request> parse_request(Bytes datagram) noexcept {
  if (datagram.size() != kHeaderSize) return std::nullopt;
  const Header h = read_header(datagram.data());
  return Request{h.session, h.sequence, h.count};
}

}  // namespace mde::moldudp64
