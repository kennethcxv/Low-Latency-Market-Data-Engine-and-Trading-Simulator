// libFuzzer target: the input is a framed list of datagrams ([u16 length][datagram]...) fed to a
// Sequencer with timers firing between them. Invariants, whatever the datagrams contain: delivered
// sequence numbers strictly increase (in order, never twice), and every sequence below the next
// expected one is accounted for as delivered or lost.
#include <cstdint>
#include <cstdlib>

#include "mde/moldudp64/sequencer.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  mde::moldudp64::SequencerConfig config;
  config.max_buffered_packets = 16;
  config.request_timeout_ns = 3;
  config.max_request_attempts = 2;
  mde::moldudp64::Sequencer seq(config);

  std::uint64_t last = 0;
  bool any = false;
  auto deliver = [&](std::uint64_t s, mde::Bytes) {
    if (any && s <= last) std::abort();
    last = s;
    any = true;
  };

  mde::FramedReader reader{mde::Bytes(data, size)};
  mde::Bytes datagram;
  std::uint64_t now = 0;
  while (reader.next(datagram) == mde::FramedReader::Status::kOk) {
    mde::moldudp64::Packet p;
    (void)mde::moldudp64::parse(datagram, p);
    seq.on_packet(datagram, now, deliver);
    now += 2;
    while (seq.on_timer(now, deliver)) {
    }
    // Every sequence below next_sequence() was either delivered once or counted as lost.
    if (seq.stats().delivered + seq.stats().messages_lost != seq.next_sequence() - 1) std::abort();
  }
  return 0;
}
