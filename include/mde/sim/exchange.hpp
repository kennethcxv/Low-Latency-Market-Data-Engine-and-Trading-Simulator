// Simulated order entry: pre-trade risk, a price-time-priority matching engine and execution
// reports. Self-contained and deterministic (no clocks, no randomness); it shares nothing with
// the ITCH side, which is a market-data feed, not an order-entry protocol.
#pragma once

#include <cstdint>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mde::sim {

using AccountId = std::uint32_t;
using InstrumentId = std::uint32_t;
using OrderId = std::uint64_t;        // assigned by the exchange
using ClientOrderId = std::uint64_t;  // chosen by the client, unique per account
using Price = std::int64_t;           // integer ticks
using Qty = std::int64_t;

enum class Side : std::uint8_t { kBuy, kSell };

struct NewOrder {
  AccountId account = 0;
  ClientOrderId client_id = 0;
  InstrumentId instrument = 0;
  Side side = Side::kBuy;
  Price price = 0;  // limit price
  Qty quantity = 0;
};

struct CancelOrder {
  AccountId account = 0;
  ClientOrderId client_id = 0;  // of the order to cancel
};

enum class ExecType : std::uint8_t { kNew, kRejected, kPartialFill, kFill, kCanceled, kCancelRejected };

enum class RejectReason : std::uint8_t {
  kNone,
  kUnknownInstrument,
  kInvalidPrice,
  kInvalidQuantity,
  kDuplicateClientId,
  kMaxOrderQuantity,
  kMaxOrderNotional,
  kPositionLimit,
  kUnknownOrder,  // cancel of an order that is not live
};
inline constexpr std::size_t kRejectReasonCount = 9;

std::string_view to_string(ExecType t);
std::string_view to_string(RejectReason r);

struct ExecutionReport {
  std::uint64_t exec_id = 0;  // exchange-wide, strictly increasing
  AccountId account = 0;
  ClientOrderId client_id = 0;
  OrderId order_id = 0;  // 0 for rejected new orders
  InstrumentId instrument = 0;
  Side side = Side::kBuy;
  ExecType type = ExecType::kNew;
  RejectReason reason = RejectReason::kNone;
  Price last_price = 0;
  Qty last_quantity = 0;
  Qty leaves_quantity = 0;
  Qty cumulative_quantity = 0;
  bool operator==(const ExecutionReport&) const = default;
};

struct RiskLimits {
  Qty max_order_quantity = 10'000;
  std::int64_t max_order_notional = 5'000'000;  // price * quantity
  // Bound on |position| per account and instrument in the worst case where every open order on
  // the order's side fills: position + open buys + new buy <= max, position - open sells - new sell >= -max.
  std::int64_t max_position = 50'000;
};

struct Position {
  std::int64_t net = 0;
  Qty open_buy = 0;
  Qty open_sell = 0;
};

// One instrument's limit order book with price-time priority.
class OrderBook {
 public:
  struct Fill {
    OrderId resting_id;
    Price price;  // always the resting order's price
    Qty quantity;
  };

  // Executes up to `quantity` against the opposite side at prices no worse than `limit`, oldest
  // order first within a level. Appends fills in execution order; returns the unfilled remainder.
  Qty match(Side side, Price limit, Qty quantity, std::vector<Fill>& fills);
  void rest(OrderId id, Side side, Price price, Qty quantity);
  // Removes a resting order; returns its remaining quantity, or nothing if it is not resting.
  std::optional<Qty> cancel(OrderId id);

  std::optional<Price> best(Side side) const;
  Qty quantity_at(Side side, Price price) const;
  std::size_t order_count() const { return index_.size(); }

 private:
  struct Resting {
    OrderId id;
    Qty quantity;
  };
  using Queue = std::list<Resting>;
  struct Location {
    Side side;
    Price price;
    Queue::iterator position;
  };
  std::map<Price, Queue> bids_;  // best = highest = last key
  std::map<Price, Queue> asks_;  // best = lowest = first key
  std::unordered_map<OrderId, Location> index_;
};

// Gateway + risk + matching for a fixed set of instruments. Every request produces one or more
// execution reports, appended to `out` in the order they occur.
class Exchange {
 public:
  Exchange(std::size_t instruments, RiskLimits limits);

  void submit(const NewOrder& order, std::vector<ExecutionReport>& out);
  void cancel(const CancelOrder& request, std::vector<ExecutionReport>& out);

  const Position& position(AccountId account, InstrumentId instrument) const;
  const OrderBook& book(InstrumentId instrument) const { return books_.at(instrument); }
  const RiskLimits& limits() const { return limits_; }

 private:
  struct LiveOrder {
    AccountId account;
    ClientOrderId client_id;
    InstrumentId instrument;
    Side side;
    Price price;
    Qty quantity;
    Qty cumulative = 0;
  };

  RejectReason check(const NewOrder& order) const;
  ExecutionReport report(const LiveOrder& o, OrderId id, ExecType type);
  void apply_fill(LiveOrder& o, OrderId id, Price price, Qty quantity, std::vector<ExecutionReport>& out);
  Position& position_of(AccountId account, InstrumentId instrument);

  RiskLimits limits_;
  std::vector<OrderBook> books_;
  std::unordered_map<OrderId, LiveOrder> live_;
  std::map<std::pair<AccountId, ClientOrderId>, OrderId> by_client_id_;  // live orders only
  std::set<std::pair<AccountId, ClientOrderId>> used_client_ids_;
  std::map<std::pair<AccountId, InstrumentId>, Position> positions_;
  std::vector<OrderBook::Fill> fills_;  // scratch, reused across submits
  OrderId next_order_id_ = 1;
  std::uint64_t next_exec_id_ = 1;
};

}  // namespace mde::sim
