// Vocabulary shared by the order-book implementations, and the interface they both satisfy.
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "mde/common.hpp"

namespace mde::book {

using Locate = std::uint16_t;   // ITCH stock locate: a dense per-day instrument index
using OrderId = std::uint64_t;  // ITCH order reference number: day-unique, not dense
using Price = std::uint32_t;    // ITCH Price(4): four implied decimals
using Qty = std::uint32_t;

inline constexpr std::size_t kMaxLocates = 65536;

enum class Side : std::uint8_t { kBuy, kSell };

struct Level {
  Price price = 0;
  std::uint64_t quantity = 0;  // sum of resting shares; can exceed 2^32 across many orders
  std::uint32_t orders = 0;
  bool operator==(const Level&) const = default;
};

struct OrderEntry {
  OrderId id = 0;
  Qty quantity = 0;
  bool operator==(const OrderEntry&) const = default;
};

struct OrderView {
  Locate locate = 0;
  Side side = Side::kBuy;
  Price price = 0;
  Qty quantity = 0;
};

// Outcome of applying one book operation. Anything but kOk means the input contradicted the
// book's state; the operation is then either ignored or (kOverfill) clamped, never half-applied.
enum class Result : std::uint8_t {
  kOk,
  kDuplicateOrder,  // add/replace with an id that is already live
  kUnknownOrder,    // modify of an id that is not live (e.g. lost to a sequence gap)
  kLocateMismatch,  // modify whose locate differs from the order's
  kZeroQuantity,    // add/replace of zero shares
  kOverfill,        // reduce by more than remains: order removed, excess ignored
};
inline constexpr std::size_t kResultCount = 6;

[[nodiscard]] constexpr std::string_view to_string(Result r) noexcept {
  switch (r) {
    case Result::kOk: return "ok";
    case Result::kDuplicateOrder: return "duplicate_order";
    case Result::kUnknownOrder: return "unknown_order";
    case Result::kLocateMismatch: return "locate_mismatch";
    case Result::kZeroQuantity: return "zero_quantity";
    case Result::kOverfill: return "overfill";
  }
  return "?";
}

// What BookBuilder, the tests and the benchmarks require of a market (a set of per-locate books
// plus the order index). Query results are best-first; queue() is in time priority.
template <class M>
concept Market = requires(M m, const M cm, Locate loc, OrderId id, Side side, Price px, Qty qty,
                          std::size_t n) {
  { m.add(loc, id, side, px, qty) } -> std::same_as<Result>;
  { m.reduce(loc, id, qty) } -> std::same_as<Result>;
  { m.remove(loc, id) } -> std::same_as<Result>;
  { m.replace(loc, id, id, px, qty) } -> std::same_as<Result>;
  { cm.best(loc, side) } -> std::same_as<std::optional<Level>>;
  { cm.depth(loc, side, n) } -> std::same_as<std::vector<Level>>;
  { cm.queue(loc, side, px) } -> std::same_as<std::vector<OrderEntry>>;
  { cm.order(id) } -> std::same_as<std::optional<OrderView>>;
  { cm.order_count() } -> std::same_as<std::size_t>;
};

// Hash of the complete visible state: every level and every order in priority order, for every
// locate. Two markets with equal digests hold the same book.
template <Market M>
std::uint64_t state_digest(const M& market) {
  Digest d;
  for (std::size_t loc = 0; loc < kMaxLocates; ++loc) {
    for (Side side : {Side::kBuy, Side::kSell}) {
      for (const Level& level : market.depth(static_cast<Locate>(loc), side, SIZE_MAX)) {
        d.add(loc);
        d.add(static_cast<std::uint64_t>(side));
        d.add(level.price);
        d.add(level.quantity);
        d.add(level.orders);
        for (const OrderEntry& o : market.queue(static_cast<Locate>(loc), side, level.price)) {
          d.add(o.id);
          d.add(o.quantity);
        }
      }
    }
  }
  return d.value();
}

}  // namespace mde::book
