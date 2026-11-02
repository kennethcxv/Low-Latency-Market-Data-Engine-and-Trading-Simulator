#include <gtest/gtest.h>

#include "mde/common.hpp"
#include "test_util.hpp"

namespace mde {
namespace {

TEST(ByteOrder, LoadsAreBigEndian) {
  const auto b = test::hex("01 02 03 04 05 06 07 08");
  EXPECT_EQ(load_be16(b.data()), 0x0102u);
  EXPECT_EQ(load_be32(b.data()), 0x01020304u);
  EXPECT_EQ(load_be48(b.data()), 0x010203040506ULL);
  EXPECT_EQ(load_be64(b.data()), 0x0102030405060708ULL);
}

TEST(ByteOrder, StoresRoundTrip) {
  std::uint8_t b[8] = {};
  store_be48(b, 0xA1B2C3D4E5F6ULL);
  EXPECT_EQ(test::hex("a1 b2 c3 d4 e5 f6"), std::vector<std::uint8_t>(b, b + 6));
  store_be64(b, 0xFFEEDDCCBBAA9988ULL);
  EXPECT_EQ(load_be64(b), 0xFFEEDDCCBBAA9988ULL);
  store_be32(b, 0x80000001u);
  EXPECT_EQ(load_be32(b), 0x80000001u);
  store_be16(b, 0xFFFE);
  EXPECT_EQ(load_be16(b), 0xFFFEu);
}

TEST(FramedReader, IteratesRecordsIncludingEmptyOnes) {
  const auto buf = test::hex("0002 aabb 0000 0001 cc");
  FramedReader r{buf};
  Bytes m;
  ASSERT_EQ(r.next(m), FramedReader::Status::kOk);
  EXPECT_EQ(m.size(), 2u);
  EXPECT_EQ(m[1], 0xbb);
  ASSERT_EQ(r.next(m), FramedReader::Status::kOk);
  EXPECT_TRUE(m.empty());
  ASSERT_EQ(r.next(m), FramedReader::Status::kOk);
  EXPECT_EQ(m[0], 0xcc);
  EXPECT_EQ(r.next(m), FramedReader::Status::kEnd);
}

TEST(FramedReader, ReportsTruncationWithoutAdvancing) {
  const auto partial_length = test::hex("0002 aabb 00");
  FramedReader r1{partial_length};
  Bytes m;
  ASSERT_EQ(r1.next(m), FramedReader::Status::kOk);
  EXPECT_EQ(r1.next(m), FramedReader::Status::kTruncated);
  EXPECT_EQ(r1.offset(), 4u);

  const auto short_body = test::hex("0005 aabbcc");
  FramedReader r2{short_body};
  EXPECT_EQ(r2.next(m), FramedReader::Status::kTruncated);
  EXPECT_EQ(r2.offset(), 0u);
}

TEST(Rng, IsDeterministicForAGivenSeed) {
  Rng a{42}, b{42}, c{43};
  for (int i = 0; i < 100; ++i) {
    const auto x = a.next();
    EXPECT_EQ(x, b.next());
    EXPECT_NE(x, c.next());
  }
}

}  // namespace
}  // namespace mde
