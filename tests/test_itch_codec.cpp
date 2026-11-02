#include <gtest/gtest.h>

#include <optional>

#include "mde/itch/codec.hpp"
#include "mde/itch/format.hpp"
#include "test_util.hpp"

namespace mde::itch {
namespace {

template <class T>
std::optional<T> decode_one(Bytes msg, DecodeStatus* status = nullptr) {
  std::optional<T> out;
  const DecodeStatus s = decode(msg, Overloaded{
                                         [&](const T& m) { out = m; },
                                         [](const auto&) {},
                                     });
  if (status) *status = s;
  return out;
}

// Laid out field by field from spec section 1.3.1; values chosen so a wrong offset or width
// produces a visibly wrong number rather than a coincidental match.
TEST(ItchDecode, AddOrderFieldsMatchSpecOffsets) {
  const auto bytes = test::hex(
      "41"                       // type 'A'
      "0001"                     // stock locate
      "0002"                     // tracking number
      "1f1ad635bd15"             // timestamp 34200123456789 ns (09:30:00.123456789)
      "0102030405060708"         // order reference number
      "42"                       // 'B'
      "000001f4"                 // shares 500
      "4141504c20202020"         // "AAPL    "
      "0016ed24");               // price 150.2500
  ASSERT_EQ(bytes.size(), AddOrder::kSize);

  const auto m = decode_one<AddOrder>(bytes);
  ASSERT_TRUE(m);
  EXPECT_EQ(m->header.stock_locate, 1);
  EXPECT_EQ(m->header.tracking_number, 2);
  EXPECT_EQ(m->header.timestamp.ns, 34'200'123'456'789ULL);
  EXPECT_EQ(m->order_ref, 0x0102030405060708ULL);
  EXPECT_EQ(m->side, 'B');
  EXPECT_EQ(m->shares, 500u);
  EXPECT_EQ(trim(m->stock), "AAPL");
  EXPECT_EQ(m->price, 1'502'500u);

  EXPECT_EQ(to_string(*m),
            "AddOrder stock_locate=1 tracking_number=2 timestamp=34200123456789 "
            "order_ref=72623859790382856 side=B shares=500 stock=\"AAPL\" price=1502500");
}

TEST(ItchDecode, RejectsEmptyAndUnknownTypes) {
  DecodeStatus s{};
  decode_one<AddOrder>({}, &s);
  EXPECT_EQ(s, DecodeStatus::kEmpty);
  const auto unknown = test::hex("5a 0000");
  decode_one<AddOrder>(unknown, &s);
  EXPECT_EQ(s, DecodeStatus::kUnknownType);
}

TEST(ItchDecode, RejectsWrongLengthInEitherDirection) {
  std::vector<std::uint8_t> buf(AddOrder::kSize + 1);
  AddOrder m{};
  m.side = 'S';
  encode(m, buf.data());
  DecodeStatus s{};
  EXPECT_FALSE(decode_one<AddOrder>(Bytes(buf.data(), AddOrder::kSize - 1), &s));
  EXPECT_EQ(s, DecodeStatus::kBadLength);
  EXPECT_FALSE(decode_one<AddOrder>(Bytes(buf.data(), AddOrder::kSize + 1), &s));
  EXPECT_EQ(s, DecodeStatus::kBadLength);
  EXPECT_TRUE(decode_one<AddOrder>(Bytes(buf.data(), AddOrder::kSize), &s));
}

TEST(ItchDecode, RejectsInvalidSideAndPrintableFlag) {
  std::vector<std::uint8_t> buf(64);
  AddOrder add{};
  add.side = 'X';
  encode(add, buf.data());
  DecodeStatus s{};
  EXPECT_FALSE(decode_one<AddOrder>(Bytes(buf.data(), AddOrder::kSize), &s));
  EXPECT_EQ(s, DecodeStatus::kBadField);

  OrderExecutedWithPrice exec{};
  exec.printable = 'y';  // case matters
  encode(exec, buf.data());
  EXPECT_FALSE(decode_one<OrderExecutedWithPrice>(Bytes(buf.data(), exec.kSize), &s));
  EXPECT_EQ(s, DecodeStatus::kBadField);
}

// encode -> read is the identity for every message type and any field values.
template <class T>
class ItchRoundTrip : public ::testing::Test {};

template <class Tuple>
struct to_gtest_types;
template <class... T>
struct to_gtest_types<std::tuple<T...>> {
  using type = ::testing::Types<T...>;
};
TYPED_TEST_SUITE(ItchRoundTrip, to_gtest_types<test::AllMessages>::type);

TYPED_TEST(ItchRoundTrip, EncodeThenReadIsIdentity) {
  Rng rng{static_cast<std::uint64_t>(TypeParam::kType)};
  std::uint8_t buf[kMaxMessageSize];
  for (int i = 0; i < 2000; ++i) {
    TypeParam original{};
    test::fill_random(original, rng);
    ASSERT_EQ(encode(original, buf), TypeParam::kSize);
    EXPECT_EQ(buf[0], static_cast<std::uint8_t>(TypeParam::kType));
    EXPECT_EQ(read<TypeParam>(buf), original);
  }
}

}  // namespace
}  // namespace mde::itch
