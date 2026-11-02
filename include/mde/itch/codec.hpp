// ITCH 5.0 decoding and encoding.
//
// decode(msg, handler) validates the length and the fields this engine acts on, then calls
// handler(const T&) with a host-order struct built on the stack. Nothing is allocated and no
// input byte is read outside `msg`. Every rejection is reported as a DecodeStatus; nothing throws.
//
// Policy (not dictated by the spec):
//  * Lengths must match the spec exactly. A longer message is rejected rather than partially
//    parsed, because silently ignoring trailing bytes would hide framing errors.
//  * Fields that change book or volume semantics are validated: Buy/Sell indicators ('B'/'S')
//    and the Printable flag ('Y'/'N'). Informational alpha fields are passed through unchecked.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "mde/common.hpp"
#include "mde/itch/messages.hpp"

namespace mde::itch {

enum class DecodeStatus : std::uint8_t { kOk, kEmpty, kUnknownType, kBadLength, kBadField };

[[nodiscard]] constexpr std::string_view to_string(DecodeStatus s) noexcept {
  switch (s) {
    case DecodeStatus::kOk: return "ok";
    case DecodeStatus::kEmpty: return "empty";
    case DecodeStatus::kUnknownType: return "unknown_type";
    case DecodeStatus::kBadLength: return "bad_length";
    case DecodeStatus::kBadField: return "bad_field";
  }
  return "?";
}

namespace detail {

template <class T>
concept Composite = requires(T& t) { t.fields(); };

template <class T>
struct is_alpha : std::false_type {};
template <std::size_t N>
struct is_alpha<Alpha<N>> : std::true_type {};

template <class T>
consteval std::size_t width();

template <class Tuple>
struct tuple_width;
template <class... F>
struct tuple_width<std::tuple<F&...>> {
  static constexpr std::size_t value = (width<std::remove_const_t<F>>() + ...);
};

// Bytes occupied on the wire by a field of type T.
template <class T>
consteval std::size_t width() {
  if constexpr (std::is_same_v<T, Timestamp>) return 6;
  else if constexpr (is_alpha<T>::value) return std::tuple_size_v<T>;
  else if constexpr (Composite<T>) return tuple_width<decltype(std::declval<T&>().fields())>::value;
  else {
    static_assert(std::is_integral_v<T>);
    return sizeof(T);
  }
}

template <class T>
inline void get(const std::uint8_t*& p, T& v) noexcept {
  if constexpr (std::is_same_v<T, Timestamp>) {
    v.ns = load_be48(p);
  } else if constexpr (is_alpha<T>::value) {
    std::memcpy(v.data(), p, v.size());
  } else if constexpr (Composite<T>) {
    std::apply([&p](auto&... f) { (get(p, f), ...); }, v.fields());
    return;  // p already advanced by the nested fields
  } else if constexpr (sizeof(T) == 1) {
    v = static_cast<T>(*p);
  } else if constexpr (sizeof(T) == 2) {
    v = load_be16(p);
  } else if constexpr (sizeof(T) == 4) {
    v = load_be32(p);
  } else {
    v = load_be64(p);
  }
  p += width<T>();
}

template <class T>
inline void put(std::uint8_t*& p, const T& v) noexcept {
  if constexpr (std::is_same_v<T, Timestamp>) {
    store_be48(p, v.ns);
  } else if constexpr (is_alpha<T>::value) {
    std::memcpy(p, v.data(), v.size());
  } else if constexpr (Composite<T>) {
    std::apply([&p](const auto&... f) { (put(p, f), ...); }, v.fields());
    return;
  } else if constexpr (sizeof(T) == 1) {
    *p = static_cast<std::uint8_t>(v);
  } else if constexpr (sizeof(T) == 2) {
    store_be16(p, v);
  } else if constexpr (sizeof(T) == 4) {
    store_be32(p, v);
  } else {
    store_be64(p, v);
  }
  p += width<T>();
}

}  // namespace detail

// Bytes on the wire including the type byte.
template <class T>
inline constexpr std::size_t kWireSize = 1 + detail::width<T>();

// The spec-derived lengths in messages.hpp must equal the sum of the declared field widths.
#define MDE_CHECK_SIZE(T) static_assert(kWireSize<T> == T::kSize, #T " layout disagrees with spec length")
MDE_CHECK_SIZE(SystemEvent);
MDE_CHECK_SIZE(StockDirectory);
MDE_CHECK_SIZE(StockTradingAction);
MDE_CHECK_SIZE(RegShoRestriction);
MDE_CHECK_SIZE(MarketParticipantPosition);
MDE_CHECK_SIZE(MwcbDeclineLevel);
MDE_CHECK_SIZE(MwcbStatus);
MDE_CHECK_SIZE(IpoQuotingPeriodUpdate);
MDE_CHECK_SIZE(LuldAuctionCollar);
MDE_CHECK_SIZE(OperationalHalt);
MDE_CHECK_SIZE(AddOrder);
MDE_CHECK_SIZE(AddOrderMpid);
MDE_CHECK_SIZE(OrderExecuted);
MDE_CHECK_SIZE(OrderExecutedWithPrice);
MDE_CHECK_SIZE(OrderCancel);
MDE_CHECK_SIZE(OrderDelete);
MDE_CHECK_SIZE(OrderReplace);
MDE_CHECK_SIZE(Trade);
MDE_CHECK_SIZE(CrossTrade);
MDE_CHECK_SIZE(BrokenTrade);
MDE_CHECK_SIZE(Noii);
MDE_CHECK_SIZE(RetailPriceImprovement);
MDE_CHECK_SIZE(DlcrPriceDiscovery);
#undef MDE_CHECK_SIZE

inline constexpr std::size_t kMaxMessageSize = Noii::kSize;  // largest ITCH 5.0 message

// ---- Field validation -------------------------------------------------------------------------

constexpr bool is_side(char c) noexcept { return c == 'B' || c == 'S'; }

template <class T>
constexpr bool valid(const T&) noexcept { return true; }
constexpr bool valid(const AddOrder& m) noexcept { return is_side(m.side); }
constexpr bool valid(const AddOrderMpid& m) noexcept { return is_side(m.side); }
constexpr bool valid(const Trade& m) noexcept { return is_side(m.side); }
constexpr bool valid(const OrderExecutedWithPrice& m) noexcept {
  return m.printable == 'Y' || m.printable == 'N';
}

// ---- Decode -----------------------------------------------------------------------------------

// Reads T from `msg`, which must be exactly T::kSize bytes starting with the type byte.
template <class T>
[[nodiscard]] inline T read(const std::uint8_t* msg) noexcept {
  T m{};
  const std::uint8_t* p = msg + 1;
  detail::get(p, m);
  return m;
}

template <class T, class Handler>
inline DecodeStatus decode_as(Bytes msg, Handler& handler) {
  if (msg.size() != T::kSize) return DecodeStatus::kBadLength;
  const T m = read<T>(msg.data());
  if (!valid(m)) return DecodeStatus::kBadField;
  handler(m);
  return DecodeStatus::kOk;
}

// Decodes one ITCH message and passes it to `handler`, which must accept every message type
// (a generic `[](const auto&) {}` overload is the usual catch-all).
template <class Handler>
inline DecodeStatus decode(Bytes msg, Handler&& handler) {
  if (msg.empty()) return DecodeStatus::kEmpty;
  switch (static_cast<char>(msg[0])) {
    case SystemEvent::kType: return decode_as<SystemEvent>(msg, handler);
    case StockDirectory::kType: return decode_as<StockDirectory>(msg, handler);
    case StockTradingAction::kType: return decode_as<StockTradingAction>(msg, handler);
    case RegShoRestriction::kType: return decode_as<RegShoRestriction>(msg, handler);
    case MarketParticipantPosition::kType: return decode_as<MarketParticipantPosition>(msg, handler);
    case MwcbDeclineLevel::kType: return decode_as<MwcbDeclineLevel>(msg, handler);
    case MwcbStatus::kType: return decode_as<MwcbStatus>(msg, handler);
    case IpoQuotingPeriodUpdate::kType: return decode_as<IpoQuotingPeriodUpdate>(msg, handler);
    case LuldAuctionCollar::kType: return decode_as<LuldAuctionCollar>(msg, handler);
    case OperationalHalt::kType: return decode_as<OperationalHalt>(msg, handler);
    case AddOrder::kType: return decode_as<AddOrder>(msg, handler);
    case AddOrderMpid::kType: return decode_as<AddOrderMpid>(msg, handler);
    case OrderExecuted::kType: return decode_as<OrderExecuted>(msg, handler);
    case OrderExecutedWithPrice::kType: return decode_as<OrderExecutedWithPrice>(msg, handler);
    case OrderCancel::kType: return decode_as<OrderCancel>(msg, handler);
    case OrderDelete::kType: return decode_as<OrderDelete>(msg, handler);
    case OrderReplace::kType: return decode_as<OrderReplace>(msg, handler);
    case Trade::kType: return decode_as<Trade>(msg, handler);
    case CrossTrade::kType: return decode_as<CrossTrade>(msg, handler);
    case BrokenTrade::kType: return decode_as<BrokenTrade>(msg, handler);
    case Noii::kType: return decode_as<Noii>(msg, handler);
    case RetailPriceImprovement::kType: return decode_as<RetailPriceImprovement>(msg, handler);
    case DlcrPriceDiscovery::kType: return decode_as<DlcrPriceDiscovery>(msg, handler);
    default: return DecodeStatus::kUnknownType;
  }
}

// ---- Encode -----------------------------------------------------------------------------------

// Writes T::kSize bytes to `out`. Used by the synthetic generator and tests.
template <class T>
inline std::size_t encode(const T& m, std::uint8_t* out) noexcept {
  out[0] = static_cast<std::uint8_t>(T::kType);
  std::uint8_t* p = out + 1;
  detail::put(p, m);
  return T::kSize;
}

// Appends [u16 length][message] — the framing of ITCH files and MoldUDP64 message blocks.
template <class T>
inline void append_framed(std::vector<std::uint8_t>& out, const T& m) {
  const std::size_t at = out.size();
  out.resize(at + 2 + T::kSize);
  store_be16(out.data() + at, static_cast<std::uint16_t>(T::kSize));
  encode(m, out.data() + at + 2);
}

// Overload-set helper for building decode handlers from lambdas.
template <class... F>
struct Overloaded : F... {
  using F::operator()...;
};
template <class... F>
Overloaded(F...) -> Overloaded<F...>;

}  // namespace mde::itch
