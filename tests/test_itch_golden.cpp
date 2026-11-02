// Golden vectors for every ITCH 5.0 message type, laid out field by field from the offset tables
// in the specification (offsets in the comments). Every integer byte holds its own offset, so a
// field decoded from the wrong position or with the wrong width yields a visibly wrong number.
// Alpha and enumerated fields carry valid ASCII values.
#include <gtest/gtest.h>

#include <algorithm>

#include "mde/itch/codec.hpp"
#include "test_util.hpp"

namespace mde::itch {
namespace {

template <class T>
T decode_exact(std::string_view hex_bytes) {
  const auto bytes = test::hex(hex_bytes);
  EXPECT_EQ(bytes.size(), T::kSize);
  T out{};
  int calls = 0;
  const auto status = decode(bytes, Overloaded{
                                        [&](const T& m) {
                                          out = m;
                                          ++calls;
                                        },
                                        [&](const auto&) { ADD_FAILURE() << "dispatched to wrong type"; },
                                    });
  EXPECT_EQ(status, DecodeStatus::kOk);
  EXPECT_EQ(calls, 1);

  // Every input byte belongs to some field, so re-encoding must reproduce the input exactly.
  std::uint8_t reencoded[kMaxMessageSize];
  encode(out, reencoded);
  EXPECT_TRUE(std::equal(bytes.begin(), bytes.end(), reencoded));
  return out;
}

void expect_header(const Header& h) {
  EXPECT_EQ(h.stock_locate, 0x0102);
  EXPECT_EQ(h.tracking_number, 0x0304);
  EXPECT_EQ(h.timestamp.ns, 0x05060708090aULL);
}

TEST(ItchGolden, SystemEvent) {
  const auto m = decode_exact<SystemEvent>(
      "53"            // 0  'S'
      "0102"          // 1  Stock Locate
      "0304"          // 3  Tracking Number
      "05060708090a"  // 5  Timestamp
      "4f");          // 11 event_code
  expect_header(m.header);
  EXPECT_EQ(m.event_code, 'O');
}

TEST(ItchGolden, StockDirectory) {
  const auto m = decode_exact<StockDirectory>(
      "52"                // 0  'R'
      "0102"              // 1  Stock Locate
      "0304"              // 3  Tracking Number
      "05060708090a"      // 5  Timestamp
      "4141504c20202020"  // 11 stock
      "51"                // 19 market_category
      "4e"                // 20 financial_status_indicator
      "15161718"          // 21 round_lot_size
      "4e"                // 25 round_lots_only
      "43"                // 26 issue_classification
      "5a20"              // 27 issue_sub_type
      "50"                // 29 authenticity
      "4e"                // 30 short_sale_threshold_indicator
      "4e"                // 31 ipo_flag
      "31"                // 32 luld_reference_price_tier
      "4e"                // 33 etp_flag
      "22232425"          // 34 etp_leverage_factor
      "4e");              // 38 inverse_indicator
  expect_header(m.header);
  EXPECT_EQ(trim(m.stock), "AAPL");
  EXPECT_EQ(m.market_category, 'Q');
  EXPECT_EQ(m.financial_status_indicator, 'N');
  EXPECT_EQ(m.round_lot_size, 0x15161718u);
  EXPECT_EQ(m.round_lots_only, 'N');
  EXPECT_EQ(m.issue_classification, 'C');
  EXPECT_EQ(trim(m.issue_sub_type), "Z");
  EXPECT_EQ(m.authenticity, 'P');
  EXPECT_EQ(m.short_sale_threshold_indicator, 'N');
  EXPECT_EQ(m.ipo_flag, 'N');
  EXPECT_EQ(m.luld_reference_price_tier, '1');
  EXPECT_EQ(m.etp_flag, 'N');
  EXPECT_EQ(m.etp_leverage_factor, 0x22232425u);
  EXPECT_EQ(m.inverse_indicator, 'N');
}

TEST(ItchGolden, StockTradingAction) {
  const auto m = decode_exact<StockTradingAction>(
      "48"                // 0  'H'
      "0102"              // 1  Stock Locate
      "0304"              // 3  Tracking Number
      "05060708090a"      // 5  Timestamp
      "4d53465420202020"  // 11 stock
      "54"                // 19 trading_state
      "20"                // 20 reserved
      "4d574331");        // 21 reason
  expect_header(m.header);
  EXPECT_EQ(trim(m.stock), "MSFT");
  EXPECT_EQ(m.trading_state, 'T');
  EXPECT_EQ(m.reserved, ' ');
  EXPECT_EQ(trim(m.reason), "MWC1");
}

TEST(ItchGolden, RegShoRestriction) {
  const auto m = decode_exact<RegShoRestriction>(
      "59"                // 0  'Y'
      "0102"              // 1  Stock Locate
      "0304"              // 3  Tracking Number
      "05060708090a"      // 5  Timestamp
      "54534c4120202020"  // 11 stock
      "31");              // 19 reg_sho_action
  expect_header(m.header);
  EXPECT_EQ(trim(m.stock), "TSLA");
  EXPECT_EQ(m.reg_sho_action, '1');
}

TEST(ItchGolden, MarketParticipantPosition) {
  const auto m = decode_exact<MarketParticipantPosition>(
      "4c"                // 0  'L'
      "0102"              // 1  Stock Locate
      "0304"              // 3  Tracking Number
      "05060708090a"      // 5  Timestamp
      "4e534451"          // 11 mpid
      "414d5a4e20202020"  // 15 stock
      "59"                // 23 primary_market_maker
      "4e"                // 24 market_maker_mode
      "41");              // 25 market_participant_state
  expect_header(m.header);
  EXPECT_EQ(trim(m.mpid), "NSDQ");
  EXPECT_EQ(trim(m.stock), "AMZN");
  EXPECT_EQ(m.primary_market_maker, 'Y');
  EXPECT_EQ(m.market_maker_mode, 'N');
  EXPECT_EQ(m.market_participant_state, 'A');
}

TEST(ItchGolden, MwcbDeclineLevel) {
  const auto m = decode_exact<MwcbDeclineLevel>(
      "56"                  // 0  'V'
      "0102"                // 1  Stock Locate
      "0304"                // 3  Tracking Number
      "05060708090a"        // 5  Timestamp
      "0b0c0d0e0f101112"    // 11 level1
      "131415161718191a"    // 19 level2
      "1b1c1d1e1f202122");  // 27 level3
  expect_header(m.header);
  EXPECT_EQ(m.level1, 0x0b0c0d0e0f101112ULL);
  EXPECT_EQ(m.level2, 0x131415161718191aULL);
  EXPECT_EQ(m.level3, 0x1b1c1d1e1f202122ULL);
}

TEST(ItchGolden, MwcbStatus) {
  const auto m = decode_exact<MwcbStatus>(
      "57"            // 0  'W'
      "0102"          // 1  Stock Locate
      "0304"          // 3  Tracking Number
      "05060708090a"  // 5  Timestamp
      "32");          // 11 breached_level
  expect_header(m.header);
  EXPECT_EQ(m.breached_level, '2');
}

TEST(ItchGolden, IpoQuotingPeriodUpdate) {
  const auto m = decode_exact<IpoQuotingPeriodUpdate>(
      "4b"                // 0  'K'
      "0102"              // 1  Stock Locate
      "0304"              // 3  Tracking Number
      "05060708090a"      // 5  Timestamp
      "4e4557434f202020"  // 11 stock
      "13141516"          // 19 ipo_quotation_release_time
      "41"                // 23 ipo_quotation_release_qualifier
      "18191a1b");        // 24 ipo_price
  expect_header(m.header);
  EXPECT_EQ(trim(m.stock), "NEWCO");
  EXPECT_EQ(m.ipo_quotation_release_time, 0x13141516u);
  EXPECT_EQ(m.ipo_quotation_release_qualifier, 'A');
  EXPECT_EQ(m.ipo_price, 0x18191a1bu);
}

TEST(ItchGolden, LuldAuctionCollar) {
  const auto m = decode_exact<LuldAuctionCollar>(
      "4a"                // 0  'J'
      "0102"              // 1  Stock Locate
      "0304"              // 3  Tracking Number
      "05060708090a"      // 5  Timestamp
      "474d452020202020"  // 11 stock
      "13141516"          // 19 auction_collar_reference_price
      "1718191a"          // 23 upper_auction_collar_price
      "1b1c1d1e"          // 27 lower_auction_collar_price
      "1f202122");        // 31 auction_collar_extension
  expect_header(m.header);
  EXPECT_EQ(trim(m.stock), "GME");
  EXPECT_EQ(m.auction_collar_reference_price, 0x13141516u);
  EXPECT_EQ(m.upper_auction_collar_price, 0x1718191au);
  EXPECT_EQ(m.lower_auction_collar_price, 0x1b1c1d1eu);
  EXPECT_EQ(m.auction_collar_extension, 0x1f202122u);
}

TEST(ItchGolden, OperationalHalt) {
  const auto m = decode_exact<OperationalHalt>(
      "68"                // 0  'h'
      "0102"              // 1  Stock Locate
      "0304"              // 3  Tracking Number
      "05060708090a"      // 5  Timestamp
      "5151512020202020"  // 11 stock
      "51"                // 19 market_code
      "48");              // 20 operational_halt_action
  expect_header(m.header);
  EXPECT_EQ(trim(m.stock), "QQQ");
  EXPECT_EQ(m.market_code, 'Q');
  EXPECT_EQ(m.operational_halt_action, 'H');
}

TEST(ItchGolden, AddOrder) {
  const auto m = decode_exact<AddOrder>(
      "41"                // 0  'A'
      "0102"              // 1  Stock Locate
      "0304"              // 3  Tracking Number
      "05060708090a"      // 5  Timestamp
      "0b0c0d0e0f101112"  // 11 order_ref
      "42"                // 19 side
      "14151617"          // 20 shares
      "4141504c20202020"  // 24 stock
      "20212223");        // 32 price
  expect_header(m.header);
  EXPECT_EQ(m.order_ref, 0x0b0c0d0e0f101112ULL);
  EXPECT_EQ(m.side, 'B');
  EXPECT_EQ(m.shares, 0x14151617u);
  EXPECT_EQ(trim(m.stock), "AAPL");
  EXPECT_EQ(m.price, 0x20212223u);
}

TEST(ItchGolden, AddOrderMpid) {
  const auto m = decode_exact<AddOrderMpid>(
      "46"                // 0  'F'
      "0102"              // 1  Stock Locate
      "0304"              // 3  Tracking Number
      "05060708090a"      // 5  Timestamp
      "0b0c0d0e0f101112"  // 11 order_ref
      "53"                // 19 side
      "14151617"          // 20 shares
      "4141504c20202020"  // 24 stock
      "20212223"          // 32 price
      "4753434f");        // 36 attribution
  expect_header(m.header);
  EXPECT_EQ(m.order_ref, 0x0b0c0d0e0f101112ULL);
  EXPECT_EQ(m.side, 'S');
  EXPECT_EQ(m.shares, 0x14151617u);
  EXPECT_EQ(trim(m.stock), "AAPL");
  EXPECT_EQ(m.price, 0x20212223u);
  EXPECT_EQ(trim(m.attribution), "GSCO");
}

TEST(ItchGolden, OrderExecuted) {
  const auto m = decode_exact<OrderExecuted>(
      "45"                  // 0  'E'
      "0102"                // 1  Stock Locate
      "0304"                // 3  Tracking Number
      "05060708090a"        // 5  Timestamp
      "0b0c0d0e0f101112"    // 11 order_ref
      "13141516"            // 19 executed_shares
      "1718191a1b1c1d1e");  // 23 match_number
  expect_header(m.header);
  EXPECT_EQ(m.order_ref, 0x0b0c0d0e0f101112ULL);
  EXPECT_EQ(m.executed_shares, 0x13141516u);
  EXPECT_EQ(m.match_number, 0x1718191a1b1c1d1eULL);
}

TEST(ItchGolden, OrderExecutedWithPrice) {
  const auto m = decode_exact<OrderExecutedWithPrice>(
      "43"                // 0  'C'
      "0102"              // 1  Stock Locate
      "0304"              // 3  Tracking Number
      "05060708090a"      // 5  Timestamp
      "0b0c0d0e0f101112"  // 11 order_ref
      "13141516"          // 19 executed_shares
      "1718191a1b1c1d1e"  // 23 match_number
      "59"                // 31 printable
      "20212223");        // 32 execution_price
  expect_header(m.header);
  EXPECT_EQ(m.order_ref, 0x0b0c0d0e0f101112ULL);
  EXPECT_EQ(m.executed_shares, 0x13141516u);
  EXPECT_EQ(m.match_number, 0x1718191a1b1c1d1eULL);
  EXPECT_EQ(m.printable, 'Y');
  EXPECT_EQ(m.execution_price, 0x20212223u);
}

TEST(ItchGolden, OrderCancel) {
  const auto m = decode_exact<OrderCancel>(
      "58"                // 0  'X'
      "0102"              // 1  Stock Locate
      "0304"              // 3  Tracking Number
      "05060708090a"      // 5  Timestamp
      "0b0c0d0e0f101112"  // 11 order_ref
      "13141516");        // 19 cancelled_shares
  expect_header(m.header);
  EXPECT_EQ(m.order_ref, 0x0b0c0d0e0f101112ULL);
  EXPECT_EQ(m.cancelled_shares, 0x13141516u);
}

TEST(ItchGolden, OrderDelete) {
  const auto m = decode_exact<OrderDelete>(
      "44"                  // 0  'D'
      "0102"                // 1  Stock Locate
      "0304"                // 3  Tracking Number
      "05060708090a"        // 5  Timestamp
      "0b0c0d0e0f101112");  // 11 order_ref
  expect_header(m.header);
  EXPECT_EQ(m.order_ref, 0x0b0c0d0e0f101112ULL);
}

TEST(ItchGolden, OrderReplace) {
  const auto m = decode_exact<OrderReplace>(
      "55"                // 0  'U'
      "0102"              // 1  Stock Locate
      "0304"              // 3  Tracking Number
      "05060708090a"      // 5  Timestamp
      "0b0c0d0e0f101112"  // 11 original_order_ref
      "131415161718191a"  // 19 new_order_ref
      "1b1c1d1e"          // 27 shares
      "1f202122");        // 31 price
  expect_header(m.header);
  EXPECT_EQ(m.original_order_ref, 0x0b0c0d0e0f101112ULL);
  EXPECT_EQ(m.new_order_ref, 0x131415161718191aULL);
  EXPECT_EQ(m.shares, 0x1b1c1d1eu);
  EXPECT_EQ(m.price, 0x1f202122u);
}

TEST(ItchGolden, Trade) {
  const auto m = decode_exact<Trade>(
      "50"                  // 0  'P'
      "0102"                // 1  Stock Locate
      "0304"                // 3  Tracking Number
      "05060708090a"        // 5  Timestamp
      "0b0c0d0e0f101112"    // 11 order_ref
      "42"                  // 19 side
      "14151617"            // 20 shares
      "4e56444120202020"    // 24 stock
      "20212223"            // 32 price
      "2425262728292a2b");  // 36 match_number
  expect_header(m.header);
  EXPECT_EQ(m.order_ref, 0x0b0c0d0e0f101112ULL);
  EXPECT_EQ(m.side, 'B');
  EXPECT_EQ(m.shares, 0x14151617u);
  EXPECT_EQ(trim(m.stock), "NVDA");
  EXPECT_EQ(m.price, 0x20212223u);
  EXPECT_EQ(m.match_number, 0x2425262728292a2bULL);
}

TEST(ItchGolden, CrossTrade) {
  const auto m = decode_exact<CrossTrade>(
      "51"                // 0  'Q'
      "0102"              // 1  Stock Locate
      "0304"              // 3  Tracking Number
      "05060708090a"      // 5  Timestamp
      "0b0c0d0e0f101112"  // 11 shares
      "5350592020202020"  // 19 stock
      "1b1c1d1e"          // 27 cross_price
      "1f20212223242526"  // 31 match_number
      "4f");              // 39 cross_type
  expect_header(m.header);
  EXPECT_EQ(m.shares, 0x0b0c0d0e0f101112ULL);
  EXPECT_EQ(trim(m.stock), "SPY");
  EXPECT_EQ(m.cross_price, 0x1b1c1d1eu);
  EXPECT_EQ(m.match_number, 0x1f20212223242526ULL);
  EXPECT_EQ(m.cross_type, 'O');
}

TEST(ItchGolden, BrokenTrade) {
  const auto m = decode_exact<BrokenTrade>(
      "42"                  // 0  'B'
      "0102"                // 1  Stock Locate
      "0304"                // 3  Tracking Number
      "05060708090a"        // 5  Timestamp
      "0b0c0d0e0f101112");  // 11 match_number
  expect_header(m.header);
  EXPECT_EQ(m.match_number, 0x0b0c0d0e0f101112ULL);
}

TEST(ItchGolden, Noii) {
  const auto m = decode_exact<Noii>(
      "49"                // 0  'I'
      "0102"              // 1  Stock Locate
      "0304"              // 3  Tracking Number
      "05060708090a"      // 5  Timestamp
      "0b0c0d0e0f101112"  // 11 paired_shares
      "131415161718191a"  // 19 imbalance_shares
      "42"                // 27 imbalance_direction
      "4d45544120202020"  // 28 stock
      "24252627"          // 36 far_price
      "28292a2b"          // 40 near_price
      "2c2d2e2f"          // 44 current_reference_price
      "43"                // 48 cross_type
      "4c");              // 49 price_variation_indicator
  expect_header(m.header);
  EXPECT_EQ(m.paired_shares, 0x0b0c0d0e0f101112ULL);
  EXPECT_EQ(m.imbalance_shares, 0x131415161718191aULL);
  EXPECT_EQ(m.imbalance_direction, 'B');
  EXPECT_EQ(trim(m.stock), "META");
  EXPECT_EQ(m.far_price, 0x24252627u);
  EXPECT_EQ(m.near_price, 0x28292a2bu);
  EXPECT_EQ(m.current_reference_price, 0x2c2d2e2fu);
  EXPECT_EQ(m.cross_type, 'C');
  EXPECT_EQ(m.price_variation_indicator, 'L');
}

TEST(ItchGolden, RetailPriceImprovement) {
  const auto m = decode_exact<RetailPriceImprovement>(
      "4e"                // 0  'N'
      "0102"              // 1  Stock Locate
      "0304"              // 3  Tracking Number
      "05060708090a"      // 5  Timestamp
      "49424d2020202020"  // 11 stock
      "41");              // 19 interest_flag
  expect_header(m.header);
  EXPECT_EQ(trim(m.stock), "IBM");
  EXPECT_EQ(m.interest_flag, 'A');
}

TEST(ItchGolden, DlcrPriceDiscovery) {
  const auto m = decode_exact<DlcrPriceDiscovery>(
      "4f"                // 0  'O'
      "0102"              // 1  Stock Locate
      "0304"              // 3  Tracking Number
      "05060708090a"      // 5  Timestamp
      "444c435220202020"  // 11 stock
      "59"                // 19 open_eligibility_status
      "14151617"          // 20 minimum_allowable_price
      "18191a1b"          // 24 maximum_allowable_price
      "1c1d1e1f"          // 28 near_execution_price
      "2021222324252627"  // 32 near_execution_time
      "28292a2b"          // 40 lower_price_range_collar
      "2c2d2e2f");        // 44 upper_price_range_collar
  expect_header(m.header);
  EXPECT_EQ(trim(m.stock), "DLCR");
  EXPECT_EQ(m.open_eligibility_status, 'Y');
  EXPECT_EQ(m.minimum_allowable_price, 0x14151617u);
  EXPECT_EQ(m.maximum_allowable_price, 0x18191a1bu);
  EXPECT_EQ(m.near_execution_price, 0x1c1d1e1fu);
  EXPECT_EQ(m.near_execution_time, 0x2021222324252627ULL);
  EXPECT_EQ(m.lower_price_range_collar, 0x28292a2bu);
  EXPECT_EQ(m.upper_price_range_collar, 0x2c2d2e2fu);
}

}  // namespace
}  // namespace mde::itch
