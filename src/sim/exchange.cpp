#include "mde/sim/exchange.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>

namespace mde::sim {

std::string_view to_string(ExecType t) {
  switch (t) {
    case ExecType::kNew: return "new";
    case ExecType::kRejected: return "rejected";
    case ExecType::kPartialFill: return "partial_fill";
    case ExecType::kFill: return "fill";
    case ExecType::kCanceled: return "canceled";
    case ExecType::kCancelRejected: return "cancel_rejected";
  }
  return "?";
}

std::string_view to_string(RejectReason r) {
  switch (r) {
    case RejectReason::kNone: return "none";
    case RejectReason::kUnknownInstrument: return "unknown_instrument";
    case RejectReason::kInvalidPrice: return "invalid_price";
    case RejectReason::kInvalidQuantity: return "invalid_quantity";
    case RejectReason::kDuplicateClientId: return "duplicate_client_id";
    case RejectReason::kMaxOrderQuantity: return "max_order_quantity";
    case RejectReason::kMaxOrderNotional: return "max_order_notional";
    case RejectReason::kPositionLimit: return "position_limit";
    case RejectReason::kUnknownOrder: return "unknown_order";
  }
  return "?";
}

// ---- OrderBook ----------------------------------------------------------------------------------

Qty OrderBook::match(Side side, Price limit, Qty quantity, std::vector<Fill>& fills) {
  auto& opposite = side == Side::kBuy ? asks_ : bids_;
  while (quantity > 0 && !opposite.empty()) {
    auto level = side == Side::kBuy ? opposite.begin() : std::prev(opposite.end());
    const Price price = level->first;
    if (side == Side::kBuy ? price > limit : price < limit) break;  // would trade through the limit
    Queue& queue = level->second;
    while (quantity > 0 && !queue.empty()) {
      Resting& front = queue.front();
      const Qty traded = std::min(quantity, front.quantity);
      fills.push_back({front.id, price, traded});
      quantity -= traded;
      front.quantity -= traded;
      if (front.quantity == 0) {
        index_.erase(front.id);
        queue.pop_front();
      }
    }
    if (queue.empty()) opposite.erase(level);
  }
  return quantity;
}

void OrderBook::rest(OrderId id, Side side, Price price, Qty quantity) {
  Queue& queue = (side == Side::kBuy ? bids_ : asks_)[price];
  queue.push_back({id, quantity});
  index_.emplace(id, Location{side, price, std::prev(queue.end())});
}

std::optional<Qty> OrderBook::cancel(OrderId id) {
  auto it = index_.find(id);
  if (it == index_.end()) return std::nullopt;
  const Location loc = it->second;
  auto& levels = loc.side == Side::kBuy ? bids_ : asks_;
  auto level = levels.find(loc.price);
  const Qty leaves = loc.position->quantity;
  level->second.erase(loc.position);
  if (level->second.empty()) levels.erase(level);
  index_.erase(it);
  return leaves;
}

std::optional<Price> OrderBook::best(Side side) const {
  const auto& levels = side == Side::kBuy ? bids_ : asks_;
  if (levels.empty()) return std::nullopt;
  return side == Side::kBuy ? levels.rbegin()->first : levels.begin()->first;
}

Qty OrderBook::quantity_at(Side side, Price price) const {
  const auto& levels = side == Side::kBuy ? bids_ : asks_;
  auto it = levels.find(price);
  if (it == levels.end()) return 0;
  Qty total = 0;
  for (const Resting& r : it->second) total += r.quantity;
  return total;
}

// ---- Exchange -----------------------------------------------------------------------------------

Exchange::Exchange(std::size_t instruments, RiskLimits limits) : limits_(limits), books_(instruments) {
  if (limits.max_order_quantity <= 0 || limits.max_order_notional <= 0 || limits.max_position < 0) {
    throw std::invalid_argument("max order quantity and notional must be positive, max position non-negative");
  }
}

const Position& Exchange::position(AccountId account, InstrumentId instrument) const {
  static const Position kFlat;
  auto it = positions_.find({account, instrument});
  return it == positions_.end() ? kFlat : it->second;
}

Position& Exchange::position_of(AccountId account, InstrumentId instrument) {
  return positions_[{account, instrument}];
}

RejectReason Exchange::check(const NewOrder& o) const {
  if (o.instrument >= books_.size()) return RejectReason::kUnknownInstrument;
  if (o.price <= 0) return RejectReason::kInvalidPrice;
  if (o.quantity <= 0) return RejectReason::kInvalidQuantity;
  if (used_client_ids_.contains({o.account, o.client_id})) return RejectReason::kDuplicateClientId;
  if (o.quantity > limits_.max_order_quantity) return RejectReason::kMaxOrderQuantity;
  // price * quantity > max, written so the product cannot overflow.
  if (o.price > limits_.max_order_notional / o.quantity) return RejectReason::kMaxOrderNotional;
  const Position& p = position(o.account, o.instrument);
  if (o.side == Side::kBuy ? p.net + p.open_buy + o.quantity > limits_.max_position
                           : p.net - p.open_sell - o.quantity < -limits_.max_position) {
    return RejectReason::kPositionLimit;
  }
  return RejectReason::kNone;
}

ExecutionReport Exchange::report(const LiveOrder& o, OrderId id, ExecType type) {
  ExecutionReport r;
  r.exec_id = next_exec_id_++;
  r.account = o.account;
  r.client_id = o.client_id;
  r.order_id = id;
  r.instrument = o.instrument;
  r.side = o.side;
  r.type = type;
  r.leaves_quantity = o.quantity - o.cumulative;
  r.cumulative_quantity = o.cumulative;
  return r;
}

void Exchange::apply_fill(LiveOrder& o, OrderId id, Price price, Qty quantity, std::vector<ExecutionReport>& out) {
  o.cumulative += quantity;
  Position& p = position_of(o.account, o.instrument);
  if (o.side == Side::kBuy) {
    p.open_buy -= quantity;
    p.net += quantity;
  } else {
    p.open_sell -= quantity;
    p.net -= quantity;
  }
  ExecutionReport r = report(o, id, o.cumulative == o.quantity ? ExecType::kFill : ExecType::kPartialFill);
  r.last_price = price;
  r.last_quantity = quantity;
  out.push_back(r);
}

void Exchange::submit(const NewOrder& order, std::vector<ExecutionReport>& out) {
  if (const RejectReason reason = check(order); reason != RejectReason::kNone) {
    ExecutionReport r;
    r.exec_id = next_exec_id_++;
    r.account = order.account;
    r.client_id = order.client_id;
    r.instrument = order.instrument;
    r.side = order.side;
    r.type = ExecType::kRejected;
    r.reason = reason;
    out.push_back(r);
    return;
  }
  used_client_ids_.insert({order.account, order.client_id});
  const OrderId id = next_order_id_++;
  LiveOrder incoming{order.account, order.client_id, order.instrument, order.side, order.price, order.quantity};
  Position& p = position_of(order.account, order.instrument);
  (order.side == Side::kBuy ? p.open_buy : p.open_sell) += order.quantity;
  out.push_back(report(incoming, id, ExecType::kNew));

  fills_.clear();
  const Qty remaining = books_[order.instrument].match(order.side, order.price, order.quantity, fills_);
  for (const OrderBook::Fill& f : fills_) {
    apply_fill(incoming, id, f.price, f.quantity, out);
    auto resting = live_.find(f.resting_id);
    apply_fill(resting->second, f.resting_id, f.price, f.quantity, out);
    if (resting->second.cumulative == resting->second.quantity) {
      by_client_id_.erase({resting->second.account, resting->second.client_id});
      live_.erase(resting);
    }
  }
  if (remaining > 0) {
    books_[order.instrument].rest(id, order.side, order.price, remaining);
    by_client_id_[{order.account, order.client_id}] = id;
    live_.emplace(id, incoming);
  }
}

void Exchange::cancel(const CancelOrder& request, std::vector<ExecutionReport>& out) {
  auto it = by_client_id_.find({request.account, request.client_id});
  if (it == by_client_id_.end()) {
    ExecutionReport r;
    r.exec_id = next_exec_id_++;
    r.account = request.account;
    r.client_id = request.client_id;
    r.type = ExecType::kCancelRejected;
    r.reason = RejectReason::kUnknownOrder;
    out.push_back(r);
    return;
  }
  const OrderId id = it->second;
  auto live = live_.find(id);
  LiveOrder& o = live->second;
  const Qty leaves = *books_[o.instrument].cancel(id);
  Position& p = position_of(o.account, o.instrument);
  (o.side == Side::kBuy ? p.open_buy : p.open_sell) -= leaves;
  ExecutionReport r = report(o, id, ExecType::kCanceled);
  r.leaves_quantity = 0;
  out.push_back(r);
  by_client_id_.erase(it);
  live_.erase(live);
}

}  // namespace mde::sim
