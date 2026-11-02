#include <gtest/gtest.h>

#include <set>

#include "mde/book/book_builder.hpp"
#include "mde/book/reference_market.hpp"
#include "mde/synth/generator.hpp"

namespace mde {
namespace {

std::uint64_t hash_bytes(const std::vector<std::uint8_t>& data) {
  Digest d;
  for (std::uint8_t b : data) d.add(b);
  return d.value();
}

TEST(Synth, StreamIsConsistentAndCoversEveryMessageType) {
  synth::Config config;
  config.seed = 11;
  config.messages = 30'000;
  config.symbols = 8;
  const auto data = synth::generate(config);

  book::ReferenceMarket market;
  book::BookBuilder builder(market, /*check_invariants=*/true);
  std::set<std::uint8_t> types;
  FramedReader reader{data};
  Bytes msg;
  while (reader.next(msg) == FramedReader::Status::kOk) {
    types.insert(msg[0]);
    ASSERT_EQ(builder.apply(msg), itch::DecodeStatus::kOk);
  }
  EXPECT_EQ(reader.offset(), data.size());
  EXPECT_EQ(builder.counters().messages, config.messages);
  EXPECT_EQ(types.size(), 23u);
  EXPECT_EQ(builder.counters().book_anomalies(), 0u);
  EXPECT_EQ(builder.counters().invariant_violations, 0u) << builder.first_violation();
  EXPECT_EQ(market.check_all_invariants(), "");
  EXPECT_EQ(builder.system_event(), 'C');  // stream ends with End of Messages
}

TEST(Synth, OutputIsAPureFunctionOfTheConfig) {
  synth::Config config;
  config.seed = 5;
  config.messages = 5'000;
  config.symbols = 4;
  const auto a = synth::generate(config);
  EXPECT_EQ(a, synth::generate(config));
  config.seed = 6;
  EXPECT_NE(a, synth::generate(config));
}

// Pins the exact bytes for one config. The suite runs against libc++ (macOS) and libstdc++ (Linux
// CI), so this fails if generation or replay ever becomes platform-dependent.
TEST(Synth, OutputIsIdenticalAcrossPlatforms) {
  synth::Config config;
  config.seed = 2026;
  config.messages = 20'000;
  config.symbols = 6;
  const auto data = synth::generate(config);
  EXPECT_EQ(hash_bytes(data), 0x18438224d3ebbfb7ULL) << std::hex << hash_bytes(data);

  book::ReferenceMarket market;
  book::BookBuilder builder(market);
  FramedReader reader{data};
  Bytes msg;
  while (reader.next(msg) == FramedReader::Status::kOk) builder.apply(msg);
  EXPECT_EQ(book::state_digest(market), 0x957f5e6f309af593ULL) << std::hex << book::state_digest(market);
}

TEST(Synth, RejectsUnusableConfigs) {
  synth::Config config;
  config.symbols = 0;
  EXPECT_THROW(synth::generate(config), std::invalid_argument);
  config.symbols = 10;
  config.messages = 50;
  EXPECT_THROW(synth::generate(config), std::invalid_argument);
}

}  // namespace
}  // namespace mde
