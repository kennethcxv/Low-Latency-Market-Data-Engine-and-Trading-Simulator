#pragma once

#include <cstdint>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

#include "mde/common.hpp"
#include "mde/itch/codec.hpp"

namespace mde::test {

// "41 00 01 ..." -> bytes. Whitespace is ignored so vectors can be laid out by field.
inline std::vector<std::uint8_t> hex(std::string_view s) {
  auto nibble = [](char c) -> std::uint8_t {
    if (c >= '0' && c <= '9') return static_cast<std::uint8_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<std::uint8_t>(c - 'a' + 10);
    return static_cast<std::uint8_t>(c - 'A' + 10);
  };
  std::vector<std::uint8_t> out;
  int hi = -1;
  for (char c : s) {
    if (c == ' ' || c == '\n') continue;
    if (hi < 0) {
      hi = nibble(c);
    } else {
      out.push_back(static_cast<std::uint8_t>((hi << 4) | nibble(c)));
      hi = -1;
    }
  }
  return out;
}

// Fills every field with random wire-representable values (48-bit timestamps, printable ASCII).
template <class T>
void fill_random(T& v, Rng& rng) {
  using itch::detail::Composite;
  using itch::detail::is_alpha;
  if constexpr (std::is_same_v<T, itch::Timestamp>) {
    v.ns = rng.next() & 0xFFFF'FFFF'FFFFULL;
  } else if constexpr (is_alpha<T>::value) {
    for (char& c : v) c = static_cast<char>(' ' + rng.below(95));
  } else if constexpr (Composite<T>) {
    std::apply([&rng](auto&... f) { (fill_random(f, rng), ...); }, v.fields());
  } else if constexpr (std::is_same_v<T, char>) {
    v = static_cast<char>(' ' + rng.below(95));
  } else {
    v = static_cast<T>(rng.next());
  }
}

using AllMessages =
    std::tuple<itch::SystemEvent, itch::StockDirectory, itch::StockTradingAction,
               itch::RegShoRestriction, itch::MarketParticipantPosition, itch::MwcbDeclineLevel,
               itch::MwcbStatus, itch::IpoQuotingPeriodUpdate, itch::LuldAuctionCollar,
               itch::OperationalHalt, itch::AddOrder, itch::AddOrderMpid, itch::OrderExecuted,
               itch::OrderExecutedWithPrice, itch::OrderCancel, itch::OrderDelete,
               itch::OrderReplace, itch::Trade, itch::CrossTrade, itch::BrokenTrade, itch::Noii,
               itch::RetailPriceImprovement, itch::DlcrPriceDiscovery>;

}  // namespace mde::test
