// Nasdaq TotalView-ITCH 5.0 message definitions.
//
// Each struct lists its fields in wire order, exactly as in the specification's field tables.
// The leading message-type byte is implied by the C++ type (kType) and handled by the codec.
// `MDE_WIRE_FIELDS` gives the codec a tuple of references to walk, so one generic routine
// decodes and encodes every message. codec.hpp static_asserts that the summed field widths
// equal kSize, the length derived from the spec, for every type.
//
// Host representation: integers are host-order, alpha fields are raw ASCII (space padded,
// untrimmed), prices are the raw fixed-point integers (Price4: 4 implied decimals,
// Price8: 8 implied decimals).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>

#define MDE_WIRE_FIELDS(...)                                                    \
  static constexpr std::string_view kFieldNames = #__VA_ARGS__;                 \
  constexpr auto fields() noexcept { return std::tie(__VA_ARGS__); }            \
  constexpr auto fields() const noexcept { return std::tie(__VA_ARGS__); }

namespace mde::itch {

using Price4 = std::uint32_t;
using Price8 = std::uint64_t;
template <std::size_t N>
using Alpha = std::array<char, N>;
using Symbol = Alpha<8>;

// Nanoseconds since midnight; 6 bytes on the wire.
struct Timestamp {
  std::uint64_t ns = 0;
  bool operator==(const Timestamp&) const = default;
};

// Common to every message after the type byte.
struct Header {
  std::uint16_t stock_locate = 0;
  std::uint16_t tracking_number = 0;
  Timestamp timestamp;
  MDE_WIRE_FIELDS(stock_locate, tracking_number, timestamp)
  bool operator==(const Header&) const = default;
};

// 1.1
struct SystemEvent {
  static constexpr char kType = 'S';
  static constexpr std::size_t kSize = 12;
  static constexpr std::string_view kName = "SystemEvent";
  Header header;
  char event_code = 0;
  MDE_WIRE_FIELDS(header, event_code)
  bool operator==(const SystemEvent&) const = default;
};

// 1.2.1
struct StockDirectory {
  static constexpr char kType = 'R';
  static constexpr std::size_t kSize = 39;
  static constexpr std::string_view kName = "StockDirectory";
  Header header;
  Symbol stock{};
  char market_category = 0;
  char financial_status_indicator = 0;
  std::uint32_t round_lot_size = 0;
  char round_lots_only = 0;
  char issue_classification = 0;
  Alpha<2> issue_sub_type{};
  char authenticity = 0;
  char short_sale_threshold_indicator = 0;
  char ipo_flag = 0;
  char luld_reference_price_tier = 0;
  char etp_flag = 0;
  std::uint32_t etp_leverage_factor = 0;
  char inverse_indicator = 0;
  MDE_WIRE_FIELDS(header, stock, market_category, financial_status_indicator, round_lot_size,
                  round_lots_only, issue_classification, issue_sub_type, authenticity,
                  short_sale_threshold_indicator, ipo_flag, luld_reference_price_tier, etp_flag,
                  etp_leverage_factor, inverse_indicator)
  bool operator==(const StockDirectory&) const = default;
};

// 1.2.2
struct StockTradingAction {
  static constexpr char kType = 'H';
  static constexpr std::size_t kSize = 25;
  static constexpr std::string_view kName = "StockTradingAction";
  Header header;
  Symbol stock{};
  char trading_state = 0;
  char reserved = 0;
  Alpha<4> reason{};
  MDE_WIRE_FIELDS(header, stock, trading_state, reserved, reason)
  bool operator==(const StockTradingAction&) const = default;
};

// 1.2.3
struct RegShoRestriction {
  static constexpr char kType = 'Y';
  static constexpr std::size_t kSize = 20;
  static constexpr std::string_view kName = "RegShoRestriction";
  Header header;
  Symbol stock{};
  char reg_sho_action = 0;
  MDE_WIRE_FIELDS(header, stock, reg_sho_action)
  bool operator==(const RegShoRestriction&) const = default;
};

// 1.2.4
struct MarketParticipantPosition {
  static constexpr char kType = 'L';
  static constexpr std::size_t kSize = 26;
  static constexpr std::string_view kName = "MarketParticipantPosition";
  Header header;
  Alpha<4> mpid{};
  Symbol stock{};
  char primary_market_maker = 0;
  char market_maker_mode = 0;
  char market_participant_state = 0;
  MDE_WIRE_FIELDS(header, mpid, stock, primary_market_maker, market_maker_mode,
                  market_participant_state)
  bool operator==(const MarketParticipantPosition&) const = default;
};

// 1.2.5.1
struct MwcbDeclineLevel {
  static constexpr char kType = 'V';
  static constexpr std::size_t kSize = 35;
  static constexpr std::string_view kName = "MwcbDeclineLevel";
  Header header;
  Price8 level1 = 0;
  Price8 level2 = 0;
  Price8 level3 = 0;
  MDE_WIRE_FIELDS(header, level1, level2, level3)
  bool operator==(const MwcbDeclineLevel&) const = default;
};

// 1.2.5.2
struct MwcbStatus {
  static constexpr char kType = 'W';
  static constexpr std::size_t kSize = 12;
  static constexpr std::string_view kName = "MwcbStatus";
  Header header;
  char breached_level = 0;
  MDE_WIRE_FIELDS(header, breached_level)
  bool operator==(const MwcbStatus&) const = default;
};

// 1.2.6
struct IpoQuotingPeriodUpdate {
  static constexpr char kType = 'K';
  static constexpr std::size_t kSize = 28;
  static constexpr std::string_view kName = "IpoQuotingPeriodUpdate";
  Header header;
  Symbol stock{};
  std::uint32_t ipo_quotation_release_time = 0;  // seconds since midnight
  char ipo_quotation_release_qualifier = 0;
  Price4 ipo_price = 0;
  MDE_WIRE_FIELDS(header, stock, ipo_quotation_release_time, ipo_quotation_release_qualifier,
                  ipo_price)
  bool operator==(const IpoQuotingPeriodUpdate&) const = default;
};

// 1.2.7
struct LuldAuctionCollar {
  static constexpr char kType = 'J';
  static constexpr std::size_t kSize = 35;
  static constexpr std::string_view kName = "LuldAuctionCollar";
  Header header;
  Symbol stock{};
  Price4 auction_collar_reference_price = 0;
  Price4 upper_auction_collar_price = 0;
  Price4 lower_auction_collar_price = 0;
  std::uint32_t auction_collar_extension = 0;
  MDE_WIRE_FIELDS(header, stock, auction_collar_reference_price, upper_auction_collar_price,
                  lower_auction_collar_price, auction_collar_extension)
  bool operator==(const LuldAuctionCollar&) const = default;
};

// 1.2.8
struct OperationalHalt {
  static constexpr char kType = 'h';
  static constexpr std::size_t kSize = 21;
  static constexpr std::string_view kName = "OperationalHalt";
  Header header;
  Symbol stock{};
  char market_code = 0;
  char operational_halt_action = 0;
  MDE_WIRE_FIELDS(header, stock, market_code, operational_halt_action)
  bool operator==(const OperationalHalt&) const = default;
};

// 1.3.1
struct AddOrder {
  static constexpr char kType = 'A';
  static constexpr std::size_t kSize = 36;
  static constexpr std::string_view kName = "AddOrder";
  Header header;
  std::uint64_t order_ref = 0;
  char side = 0;  // 'B' or 'S'
  std::uint32_t shares = 0;
  Symbol stock{};
  Price4 price = 0;
  MDE_WIRE_FIELDS(header, order_ref, side, shares, stock, price)
  bool operator==(const AddOrder&) const = default;
};

// 1.3.2
struct AddOrderMpid {
  static constexpr char kType = 'F';
  static constexpr std::size_t kSize = 40;
  static constexpr std::string_view kName = "AddOrderMpid";
  Header header;
  std::uint64_t order_ref = 0;
  char side = 0;
  std::uint32_t shares = 0;
  Symbol stock{};
  Price4 price = 0;
  Alpha<4> attribution{};
  MDE_WIRE_FIELDS(header, order_ref, side, shares, stock, price, attribution)
  bool operator==(const AddOrderMpid&) const = default;
};

// 1.4.1
struct OrderExecuted {
  static constexpr char kType = 'E';
  static constexpr std::size_t kSize = 31;
  static constexpr std::string_view kName = "OrderExecuted";
  Header header;
  std::uint64_t order_ref = 0;
  std::uint32_t executed_shares = 0;
  std::uint64_t match_number = 0;
  MDE_WIRE_FIELDS(header, order_ref, executed_shares, match_number)
  bool operator==(const OrderExecuted&) const = default;
};

// 1.4.2
struct OrderExecutedWithPrice {
  static constexpr char kType = 'C';
  static constexpr std::size_t kSize = 36;
  static constexpr std::string_view kName = "OrderExecutedWithPrice";
  Header header;
  std::uint64_t order_ref = 0;
  std::uint32_t executed_shares = 0;
  std::uint64_t match_number = 0;
  char printable = 0;  // 'Y' or 'N'
  Price4 execution_price = 0;
  MDE_WIRE_FIELDS(header, order_ref, executed_shares, match_number, printable, execution_price)
  bool operator==(const OrderExecutedWithPrice&) const = default;
};

// 1.4.3
struct OrderCancel {
  static constexpr char kType = 'X';
  static constexpr std::size_t kSize = 23;
  static constexpr std::string_view kName = "OrderCancel";
  Header header;
  std::uint64_t order_ref = 0;
  std::uint32_t cancelled_shares = 0;
  MDE_WIRE_FIELDS(header, order_ref, cancelled_shares)
  bool operator==(const OrderCancel&) const = default;
};

// 1.4.4
struct OrderDelete {
  static constexpr char kType = 'D';
  static constexpr std::size_t kSize = 19;
  static constexpr std::string_view kName = "OrderDelete";
  Header header;
  std::uint64_t order_ref = 0;
  MDE_WIRE_FIELDS(header, order_ref)
  bool operator==(const OrderDelete&) const = default;
};

// 1.4.5
struct OrderReplace {
  static constexpr char kType = 'U';
  static constexpr std::size_t kSize = 35;
  static constexpr std::string_view kName = "OrderReplace";
  Header header;
  std::uint64_t original_order_ref = 0;
  std::uint64_t new_order_ref = 0;
  std::uint32_t shares = 0;
  Price4 price = 0;
  MDE_WIRE_FIELDS(header, original_order_ref, new_order_ref, shares, price)
  bool operator==(const OrderReplace&) const = default;
};

// 1.5.1 Non-displayed executions. Never affects the book.
struct Trade {
  static constexpr char kType = 'P';
  static constexpr std::size_t kSize = 44;
  static constexpr std::string_view kName = "Trade";
  Header header;
  std::uint64_t order_ref = 0;  // zero since 2010
  char side = 0;                // always 'B' since 2014
  std::uint32_t shares = 0;
  Symbol stock{};
  Price4 price = 0;
  std::uint64_t match_number = 0;
  MDE_WIRE_FIELDS(header, order_ref, side, shares, stock, price, match_number)
  bool operator==(const Trade&) const = default;
};

// 1.5.2
struct CrossTrade {
  static constexpr char kType = 'Q';
  static constexpr std::size_t kSize = 40;
  static constexpr std::string_view kName = "CrossTrade";
  Header header;
  std::uint64_t shares = 0;  // 8 bytes, unlike every other share field
  Symbol stock{};
  Price4 cross_price = 0;
  std::uint64_t match_number = 0;
  char cross_type = 0;
  MDE_WIRE_FIELDS(header, shares, stock, cross_price, match_number, cross_type)
  bool operator==(const CrossTrade&) const = default;
};

// 1.5.3
struct BrokenTrade {
  static constexpr char kType = 'B';
  static constexpr std::size_t kSize = 19;
  static constexpr std::string_view kName = "BrokenTrade";
  Header header;
  std::uint64_t match_number = 0;
  MDE_WIRE_FIELDS(header, match_number)
  bool operator==(const BrokenTrade&) const = default;
};

// 1.6
struct Noii {
  static constexpr char kType = 'I';
  static constexpr std::size_t kSize = 50;
  static constexpr std::string_view kName = "Noii";
  Header header;
  std::uint64_t paired_shares = 0;
  std::uint64_t imbalance_shares = 0;
  char imbalance_direction = 0;
  Symbol stock{};
  Price4 far_price = 0;
  Price4 near_price = 0;
  Price4 current_reference_price = 0;
  char cross_type = 0;
  char price_variation_indicator = 0;
  MDE_WIRE_FIELDS(header, paired_shares, imbalance_shares, imbalance_direction, stock, far_price,
                  near_price, current_reference_price, cross_type, price_variation_indicator)
  bool operator==(const Noii&) const = default;
};

// 1.7
struct RetailPriceImprovement {
  static constexpr char kType = 'N';
  static constexpr std::size_t kSize = 20;
  static constexpr std::string_view kName = "RetailPriceImprovement";
  Header header;
  Symbol stock{};
  char interest_flag = 0;
  MDE_WIRE_FIELDS(header, stock, interest_flag)
  bool operator==(const RetailPriceImprovement&) const = default;
};

// 1.8
struct DlcrPriceDiscovery {
  static constexpr char kType = 'O';
  static constexpr std::size_t kSize = 48;
  static constexpr std::string_view kName = "DlcrPriceDiscovery";
  Header header;
  Symbol stock{};
  char open_eligibility_status = 0;
  Price4 minimum_allowable_price = 0;
  Price4 maximum_allowable_price = 0;
  Price4 near_execution_price = 0;
  std::uint64_t near_execution_time = 0;
  Price4 lower_price_range_collar = 0;
  Price4 upper_price_range_collar = 0;
  MDE_WIRE_FIELDS(header, stock, open_eligibility_status, minimum_allowable_price,
                  maximum_allowable_price, near_execution_price, near_execution_time,
                  lower_price_range_collar, upper_price_range_collar)
  bool operator==(const DlcrPriceDiscovery&) const = default;
};

// Trims the right-hand space padding of an alpha field.
template <std::size_t N>
[[nodiscard]] constexpr std::string_view trim(const Alpha<N>& a) noexcept {
  std::size_t n = N;
  while (n > 0 && (a[n - 1] == ' ' || a[n - 1] == '\0')) --n;
  return {a.data(), n};
}

// Right-pads `s` with spaces (truncating if longer), as the spec requires for alpha fields.
template <std::size_t N>
[[nodiscard]] constexpr Alpha<N> alpha(std::string_view s) noexcept {
  Alpha<N> a{};
  for (std::size_t i = 0; i < N; ++i) a[i] = i < s.size() ? s[i] : ' ';
  return a;
}

}  // namespace mde::itch
