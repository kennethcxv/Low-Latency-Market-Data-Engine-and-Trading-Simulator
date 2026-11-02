// Malformed-input behaviour of the ITCH decoder. Buffers are heap-allocated at their exact size so
// that, under AddressSanitizer, any read past the end of a truncated message is a hard failure.
#include <gtest/gtest.h>

#include <array>
#include <set>

#include "mde/itch/codec.hpp"
#include "test_util.hpp"

namespace mde::itch {
namespace {

const auto kIgnore = [](const auto&) {};

TEST(ItchMalformed, EveryWrongLengthIsRejectedForEveryType) {
  test::for_each_message_type([](auto proto) {
    using T = decltype(proto);
    std::uint8_t full[kMaxMessageSize + 8] = {};
    T m{};
    if constexpr (requires { m.side; }) m.side = 'B';
    if constexpr (requires { m.printable; }) m.printable = 'Y';
    encode(m, full);
    for (std::size_t len = 0; len <= T::kSize + 8; ++len) {
      if (len == T::kSize) continue;
      std::vector<std::uint8_t> buf(full, full + len);
      const auto expected = len == 0 ? DecodeStatus::kEmpty : DecodeStatus::kBadLength;
      EXPECT_EQ(decode(buf, kIgnore), expected) << T::kName << " len=" << len;
    }
    std::vector<std::uint8_t> exact(full, full + T::kSize);
    EXPECT_EQ(decode(exact, kIgnore), DecodeStatus::kOk) << T::kName;
  });
}

TEST(ItchMalformed, OnlySpecifiedTypeBytesAreAccepted) {
  std::set<char> known;
  test::for_each_message_type([&](auto m) { known.insert(decltype(m)::kType); });
  ASSERT_EQ(known.size(), 23u);

  for (int b = 0; b < 256; ++b) {
    if (known.count(static_cast<char>(b))) continue;
    for (std::size_t len : {1u, 12u, 36u, 50u}) {
      std::vector<std::uint8_t> buf(len, 0);
      buf[0] = static_cast<std::uint8_t>(b);
      EXPECT_EQ(decode(buf, kIgnore), DecodeStatus::kUnknownType) << "type byte " << b;
    }
  }
}

// Buy/Sell and Printable change book and volume semantics, so every other byte value is rejected.
TEST(ItchMalformed, EnumeratedFieldsRejectEveryInvalidValue) {
  struct Case {
    char type;
    std::size_t size;
    std::size_t offset;
    std::string_view valid;
  };
  const std::array cases = {
      Case{'A', AddOrder::kSize, 19, "BS"},
      Case{'F', AddOrderMpid::kSize, 19, "BS"},
      Case{'P', Trade::kSize, 19, "BS"},
      Case{'C', OrderExecutedWithPrice::kSize, 31, "YN"},
  };
  for (const auto& c : cases) {
    for (int v = 0; v < 256; ++v) {
      std::vector<std::uint8_t> buf(c.size, 0);
      buf[0] = static_cast<std::uint8_t>(c.type);
      buf[c.offset] = static_cast<std::uint8_t>(v);
      const bool ok = c.valid.find(static_cast<char>(v)) != std::string_view::npos;
      EXPECT_EQ(decode(buf, kIgnore), ok ? DecodeStatus::kOk : DecodeStatus::kBadField)
          << c.type << " value " << v;
    }
  }
}

// Mutation fuzzing that runs everywhere (the libFuzzer target needs upstream clang). A valid framed
// stream is corrupted in seeded, reproducible ways and decoded. Properties checked:
//  * nothing crashes or reads out of bounds (enforced by ASan/UBSan in the sanitizer builds);
//  * any message the decoder accepts re-encodes to exactly the bytes it was decoded from.
TEST(ItchMalformed, MutatedStreamsAreHandledSafely) {
  Rng rng{20261102};
  std::vector<std::uint8_t> stream;
  for (int i = 0; i < 400; ++i) {
    std::size_t pick = rng.below(23);
    test::for_each_message_type([&](auto proto) {
      if (pick-- != 0) return;
      append_framed(stream, test::random_valid<decltype(proto)>(rng));
    });
  }

  std::array<std::uint64_t, 5> seen{};
  std::uint64_t truncated_streams = 0;
  for (int round = 0; round < 3000; ++round) {
    std::vector<std::uint8_t> buf = stream;
    const int mutations = 1 + static_cast<int>(rng.below(8));
    for (int k = 0; k < mutations; ++k) {
      const std::size_t at = rng.below(buf.size());
      switch (rng.below(4)) {
        case 0: buf[at] = static_cast<std::uint8_t>(rng.next()); break;
        case 1: buf[at] ^= static_cast<std::uint8_t>(1u << rng.below(8)); break;
        case 2: buf.insert(buf.begin() + static_cast<std::ptrdiff_t>(at), static_cast<std::uint8_t>(rng.next())); break;
        case 3: buf.resize(at); break;
      }
      if (buf.empty()) break;
    }

    FramedReader reader{buf};
    Bytes msg;
    FramedReader::Status framing;
    while ((framing = reader.next(msg)) == FramedReader::Status::kOk) {
      const auto status = decode(msg, [&](const auto& m) {
        std::uint8_t out[kMaxMessageSize];
        const std::size_t n = encode(m, out);
        ASSERT_EQ(n, msg.size());
        ASSERT_TRUE(std::equal(msg.begin(), msg.end(), out));
      });
      ++seen[static_cast<std::size_t>(status)];
    }
    if (framing == FramedReader::Status::kTruncated) ++truncated_streams;
  }

  // Guard against a vacuous test: every outcome must actually have been exercised.
  for (std::size_t s = 0; s < seen.size(); ++s) {
    EXPECT_GT(seen[s], 0u) << to_string(static_cast<DecodeStatus>(s));
  }
  EXPECT_GT(truncated_streams, 0u);
}

}  // namespace
}  // namespace mde::itch
