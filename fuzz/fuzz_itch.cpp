// libFuzzer target: arbitrary bytes as a framed ITCH stream and as a single message.
// Invariant: an accepted message re-encodes to exactly its input bytes.
#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include "mde/itch/codec.hpp"

namespace {

void check_roundtrip(mde::Bytes msg) {
  mde::itch::decode(msg, [msg](const auto& m) {
    std::uint8_t out[mde::itch::kMaxMessageSize];
    const std::size_t n = mde::itch::encode(m, out);
    if (n != msg.size() || !std::equal(msg.begin(), msg.end(), out)) std::abort();
  });
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const mde::Bytes input(data, size);
  check_roundtrip(input);
  mde::FramedReader reader{input};
  mde::Bytes msg;
  while (reader.next(msg) == mde::FramedReader::Status::kOk) check_roundtrip(msg);
  return 0;
}
