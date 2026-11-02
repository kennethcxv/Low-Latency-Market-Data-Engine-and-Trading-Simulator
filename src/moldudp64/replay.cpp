#include "mde/moldudp64/replay.hpp"

#include <stdexcept>

namespace mde::moldudp64 {

MessageLog::MessageLog(const Session& session, Bytes framed) : session_(session), framed_(framed) {
  FramedReader reader{framed};
  Bytes msg;
  std::size_t at = 0;
  FramedReader::Status status;
  while ((status = reader.next(msg)) == FramedReader::Status::kOk) {
    offsets_.push_back(at);
    at = reader.offset();
  }
  if (status == FramedReader::Status::kTruncated) throw std::invalid_argument("truncated record in message stream");
}

Bytes MessageLog::message(std::uint64_t sequence) const {
  const std::size_t at = offsets_.at(sequence - 1);
  return framed_.subspan(at + 2, load_be16(framed_.data() + at));
}

std::vector<std::uint8_t> MessageLog::packet(std::uint64_t first, std::uint64_t limit, std::size_t max_size) const {
  PacketBuilder builder(session_, first, max_size);
  for (std::uint64_t seq = first; seq <= size() && seq - first < limit; ++seq) {
    if (!builder.try_add(message(seq))) {
      if (builder.count() == 0) throw std::invalid_argument("message larger than max packet size");
      break;
    }
  }
  return builder.finish();
}

std::vector<std::vector<std::uint8_t>> MessageLog::packetize(std::size_t max_size) const {
  std::vector<std::vector<std::uint8_t>> out;
  for (std::uint64_t seq = 1; seq <= size();) {
    out.push_back(packet(seq, kMaxMessagesPerPacket, max_size));
    seq += load_be16(out.back().data() + 18);
  }
  return out;
}

std::optional<std::vector<std::uint8_t>> MessageLog::answer(const Request& r, std::size_t max_size) const {
  if (r.session != session_ || r.count == 0 || r.sequence == 0 || r.sequence > size()) return std::nullopt;
  return packet(r.sequence, r.count, max_size);
}

std::vector<std::vector<std::uint8_t>> impair(const std::vector<std::vector<std::uint8_t>>& packets,
                                              const Impairments& impairments) {
  Rng rng(impairments.seed);
  std::vector<std::vector<std::uint8_t>> out;
  std::vector<std::pair<std::size_t, std::size_t>> held;  // (release after this many emitted, packet index)
  for (std::size_t i = 0; i < packets.size(); ++i) {
    if (rng.chance(impairments.drop)) continue;
    if (rng.chance(impairments.reorder)) {
      held.emplace_back(out.size() + 1 + rng.below(8), i);
      continue;
    }
    out.push_back(packets[i]);
    if (rng.chance(impairments.duplicate)) out.push_back(packets[i]);
    for (std::size_t h = 0; h < held.size();) {
      if (held[h].first <= out.size()) {
        out.push_back(packets[held[h].second]);
        held.erase(held.begin() + static_cast<std::ptrdiff_t>(h));
      } else {
        ++h;
      }
    }
  }
  for (const auto& h : held) out.push_back(packets[h.second]);
  return out;
}

}  // namespace mde::moldudp64
