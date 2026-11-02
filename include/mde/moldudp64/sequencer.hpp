// MoldUDP64 receive-side sequencing: turns datagrams that may be lost, duplicated, reordered or
// corrupt into an exactly-once, in-order message stream, and decides when to re-request.
//
// It is a pure state machine. It owns no socket and reads no clock; the caller passes the current
// time to on_timer(). That makes every loss pattern unit-testable and replay deterministic.
//
// Gap handling (MoldUDP64 leaves policy to the receiver; these are this project's choices):
//  * A packet that starts beyond the next expected sequence is buffered (bounded), and the hole
//    before it is re-requested. Heartbeats and end-of-session packets carry the server's next
//    sequence number, so losses at the tail of a burst are detected too.
//  * A request covers only the hole up to the first buffered packet, capped at
//    max_request_count; the server may answer with fewer messages than asked (the spec allows it),
//    so requests repeat until the hole is filled. Any progress resets the attempt budget.
//  * If a hole cannot be filled after max_request_attempts timeouts, or after one timeout when
//    re-requests are disabled, it is skipped: counted in messages_lost, and delivery resumes from
//    the buffered packets. Downstream state is then known to be incomplete. Without re-requests,
//    the timeout runs from when the hole was revealed (the arrival of the packet after it), not
//    from when it reached the head of the line, so several holes age in parallel.
#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <vector>

#include "mde/moldudp64/packet.hpp"

namespace mde::moldudp64 {

struct SequencerConfig {
  std::optional<Session> session;  // expected session; if unset, the first valid packet's is used
  std::uint64_t next_sequence = 1;
  std::size_t max_buffered_packets = 4096;
  std::uint16_t max_request_count = 1024;
  std::uint64_t request_timeout_ns = 20'000'000;
  std::uint32_t max_request_attempts = 5;  // 0 disables re-requests
};

struct SequencerStats {
  std::uint64_t packets = 0;
  std::uint64_t malformed_packets = 0;
  std::uint64_t session_mismatches = 0;
  std::uint64_t heartbeats = 0;            // includes end-of-session packets
  std::uint64_t delivered = 0;
  std::uint64_t duplicates = 0;            // messages received again after delivery or buffering
  std::uint64_t out_of_order_packets = 0;  // buffered because a hole preceded them
  std::uint64_t buffer_overflows = 0;      // out-of-order packets dropped because the buffer was full
  std::uint64_t gaps_detected = 0;          // holes: times the stream jumped past the highest sequence seen
  std::uint64_t requests_sent = 0;
  std::uint64_t gaps_skipped = 0;
  std::uint64_t messages_lost = 0;
};

class Sequencer {
 public:
  explicit Sequencer(SequencerConfig config = {})
      : config_(config), next_(config.next_sequence), upstream_next_(config.next_sequence), session_(config.session) {}

  // Calls deliver(sequence, message) for every message that becomes deliverable: strictly in
  // sequence order, each sequence number exactly once. `now_ns` is the receive time.
  template <class Deliver>
  void on_packet(Bytes datagram, std::uint64_t now_ns, Deliver&& deliver) {
    ++stats_.packets;
    Packet p;
    if (parse(datagram, p) != PacketStatus::kOk) {
      ++stats_.malformed_packets;
      return;
    }
    if (!session_) {
      session_ = p.header.session;
    } else if (p.header.session != *session_) {
      ++stats_.session_mismatches;
      return;
    }

    if (p.header.sequence > upstream_next_) ++stats_.gaps_detected;
    if (p.next_sequence() > upstream_next_) {
      upstream_next_ = p.next_sequence();
      upstream_advanced_ns_ = now_ns;
    }
    if (p.message_count() == 0) {
      ++stats_.heartbeats;
      end_of_session_ |= p.is_end_of_session();
    } else if (p.next_sequence() <= next_) {
      stats_.duplicates += p.message_count();
    } else if (p.header.sequence > next_) {
      buffer(datagram, p, now_ns);
    } else {
      deliver_packet(p, deliver);
      drain(deliver);
    }
  }

  // Returns a retransmission request to send now, if one is due. May skip an unrecoverable hole,
  // in which case buffered messages after it are delivered through `deliver`.
  template <class Deliver>
  std::optional<Request> on_timer(std::uint64_t now_ns, Deliver&& deliver) {
    while (has_gap()) {
      if (attempts_for_ != next_) {  // a new hole, or progress on the old one
        attempts_for_ = next_;
        attempts_ = 0;
      }
      if (attempts_ < config_.max_request_attempts) {
        if (attempts_ > 0 && now_ns - last_request_ns_ < config_.request_timeout_ns) return std::nullopt;
        ++attempts_;
        ++stats_.requests_sent;
        last_request_ns_ = now_ns;
        const std::uint64_t count = std::min<std::uint64_t>(gap_end() - next_, config_.max_request_count);
        return Request{*session_, next_, static_cast<std::uint16_t>(count)};
      }
      // Give the last request, or plain reordering when re-requests are off, one timeout to land.
      const std::uint64_t revealed = pending_.empty() ? upstream_advanced_ns_ : pending_.begin()->second.arrival_ns;
      const std::uint64_t since = attempts_ > 0 ? last_request_ns_ : revealed;
      if (now_ns - since < config_.request_timeout_ns) return std::nullopt;
      const std::uint64_t end = gap_end();
      ++stats_.gaps_skipped;
      stats_.messages_lost += end - next_;
      next_ = end;
      drain(deliver);
    }
    return std::nullopt;
  }

  bool has_gap() const { return next_ < upstream_next_; }
  // End of session seen and nothing outstanding.
  bool complete() const { return end_of_session_ && !has_gap(); }
  std::uint64_t next_sequence() const { return next_; }
  const std::optional<Session>& session() const { return session_; }
  const SequencerStats& stats() const { return stats_; }

 private:
  void buffer(Bytes datagram, const Packet& p, std::uint64_t now_ns) {
    if (pending_.contains(p.header.sequence)) {
      stats_.duplicates += p.message_count();
    } else if (pending_.size() >= config_.max_buffered_packets) {
      ++stats_.buffer_overflows;  // still covered by upstream_next_, so it will be re-requested
    } else {
      ++stats_.out_of_order_packets;
      pending_.emplace(p.header.sequence, Buffered{{datagram.begin(), datagram.end()}, now_ns});
    }
  }

  template <class Deliver>
  void deliver_packet(const Packet& p, Deliver& deliver) {
    std::uint64_t seq = p.header.sequence;
    FramedReader blocks{p.blocks};
    Bytes msg;
    while (blocks.next(msg) == FramedReader::Status::kOk) {  // already validated by parse()
      if (seq >= next_) {
        deliver(seq, msg);
        ++stats_.delivered;
        next_ = seq + 1;
      } else {
        ++stats_.duplicates;
      }
      ++seq;
    }
  }

  template <class Deliver>
  void drain(Deliver& deliver) {
    while (!pending_.empty() && pending_.begin()->first <= next_) {
      auto node = pending_.extract(pending_.begin());
      Packet p;
      (void)parse(node.mapped().datagram, p);  // validated before it was buffered
      if (p.next_sequence() <= next_) {
        stats_.duplicates += p.message_count();
      } else {
        deliver_packet(p, deliver);
      }
    }
  }

  std::uint64_t gap_end() const {
    return pending_.empty() ? upstream_next_ : std::min(upstream_next_, pending_.begin()->first);
  }

  SequencerConfig config_;
  SequencerStats stats_;
  std::uint64_t next_;
  std::uint64_t upstream_next_;  // highest "next sequence" the server has shown us
  std::optional<Session> session_;
  struct Buffered {
    std::vector<std::uint8_t> datagram;
    std::uint64_t arrival_ns;
  };
  std::map<std::uint64_t, Buffered> pending_;  // out-of-order packets by first sequence
  std::uint64_t upstream_advanced_ns_ = 0;
  bool end_of_session_ = false;
  std::uint64_t attempts_for_ = std::numeric_limits<std::uint64_t>::max();
  std::uint32_t attempts_ = 0;
  std::uint64_t last_request_ns_ = 0;
};

}  // namespace mde::moldudp64
