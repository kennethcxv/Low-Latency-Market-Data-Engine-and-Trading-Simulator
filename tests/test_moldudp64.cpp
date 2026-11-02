#include <gtest/gtest.h>

#include "mde/moldudp64/packet.hpp"
#include "test_util.hpp"

namespace mde::moldudp64 {
namespace {

// Laid out from the Downstream Packet Header and Message Block tables of the spec.
const auto kTwoMessages = test::hex(
    "4142434445464748494a"  // 0  session "ABCDEFGHIJ"
    "0102030405060708"      // 10 sequence number of the first message
    "0002"                  // 18 message count
    "0003 aabbcc"           // 20 block 1: length 3
    "0000");                // 25 block 2: zero-length message (legal)

TEST(MoldPacket, ParsesHeaderAndBlocks) {
  Packet p;
  ASSERT_EQ(parse(kTwoMessages, p), PacketStatus::kOk);
  EXPECT_EQ(p.header.session, make_session("ABCDEFGHIJ"));
  EXPECT_EQ(p.header.sequence, 0x0102030405060708ULL);
  EXPECT_EQ(p.message_count(), 2u);
  EXPECT_EQ(p.next_sequence(), 0x010203040506070aULL);
  EXPECT_FALSE(p.is_heartbeat());

  FramedReader blocks{p.blocks};
  Bytes m;
  ASSERT_EQ(blocks.next(m), FramedReader::Status::kOk);
  EXPECT_EQ(std::vector<std::uint8_t>(m.begin(), m.end()), test::hex("aabbcc"));
  ASSERT_EQ(blocks.next(m), FramedReader::Status::kOk);
  EXPECT_TRUE(m.empty());
}

TEST(MoldPacket, HeartbeatAndEndOfSessionCarryNextSequence) {
  Packet p;
  const auto hb = make_heartbeat(make_session("S"), 500);
  ASSERT_EQ(parse(hb, p), PacketStatus::kOk);
  EXPECT_TRUE(p.is_heartbeat());
  EXPECT_EQ(p.message_count(), 0u);
  EXPECT_EQ(p.next_sequence(), 500u);

  const auto eos = make_end_of_session(make_session("S"), 500);
  EXPECT_EQ(eos, test::hex("53202020202020202020 00000000000001f4 ffff"));
  ASSERT_EQ(parse(eos, p), PacketStatus::kOk);
  EXPECT_TRUE(p.is_end_of_session());
  EXPECT_EQ(p.message_count(), 0u);
}

TEST(MoldPacket, RejectsMalformedPackets) {
  Packet p;
  auto bytes = kTwoMessages;
  EXPECT_EQ(parse(Bytes(bytes.data(), 19), p), PacketStatus::kTooShort);

  bytes[19] = 3;  // claims a third block that is not there
  EXPECT_EQ(parse(bytes, p), PacketStatus::kTruncatedBlock);
  bytes[19] = 1;  // claims one block but carries two
  EXPECT_EQ(parse(bytes, p), PacketStatus::kTrailingBytes);

  auto heartbeat_with_payload = make_heartbeat(make_session("S"), 1);
  heartbeat_with_payload.push_back(0);
  EXPECT_EQ(parse(heartbeat_with_payload, p), PacketStatus::kTrailingBytes);

  auto overflow = kTwoMessages;
  store_be64(overflow.data() + 10, ~0ULL);
  EXPECT_EQ(parse(overflow, p), PacketStatus::kBadSequence);
}

TEST(MoldPacket, EveryTruncationIsRejected) {
  for (std::size_t len = 0; len < kTwoMessages.size(); ++len) {
    std::vector<std::uint8_t> cut(kTwoMessages.begin(), kTwoMessages.begin() + static_cast<std::ptrdiff_t>(len));
    Packet p;
    EXPECT_NE(parse(cut, p), PacketStatus::kOk) << "length " << len;
  }
}

TEST(MoldPacket, BuilderRespectsSizeLimit) {
  PacketBuilder b(make_session("S"), 7, kHeaderSize + 2 * (2 + 10));
  const std::vector<std::uint8_t> msg(10, 0x42);
  EXPECT_TRUE(b.try_add(msg));
  EXPECT_TRUE(b.try_add(msg));
  EXPECT_FALSE(b.try_add(msg));
  const auto packet = b.finish();
  EXPECT_EQ(packet.size(), kHeaderSize + 24);
  Packet p;
  ASSERT_EQ(parse(packet, p), PacketStatus::kOk);
  EXPECT_EQ(p.header.sequence, 7u);
  EXPECT_EQ(p.message_count(), 2u);
}

TEST(MoldRequest, MatchesSpecLayoutAndRoundTrips) {
  const Request r{make_session("ABCDEFGHIJ"), 0x1122334455667788ULL, 0x00ff};
  const auto bytes = encode(r);
  EXPECT_EQ(std::vector<std::uint8_t>(bytes.begin(), bytes.end()),
            test::hex("4142434445464748494a 1122334455667788 00ff"));
  EXPECT_EQ(parse_request(bytes), r);
  EXPECT_FALSE(parse_request(Bytes(bytes.data(), 19)));
}

}  // namespace
}  // namespace mde::moldudp64
