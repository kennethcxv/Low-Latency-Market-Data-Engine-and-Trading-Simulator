#include "mde/book/reference_market.hpp"

#include <iterator>

namespace mde::book {

ReferenceMarket::ReferenceMarket() : books_(kMaxLocates) {}

ReferenceMarket::Levels& ReferenceMarket::levels(Locate locate, Side side) {
  Book& b = books_[locate];
  return side == Side::kBuy ? b.bids : b.asks;
}

const ReferenceMarket::Levels& ReferenceMarket::levels(Locate locate, Side side) const {
  const Book& b = books_[locate];
  return side == Side::kBuy ? b.bids : b.asks;
}

void ReferenceMarket::insert(Locate locate, OrderId id, Side side, Price price, Qty quantity) {
  PriceLevel& level = levels(locate, side)[price];
  level.queue.push_back(id);
  level.quantity += quantity;
  ++books_[locate].orders;
  orders_.emplace(id, Order{locate, side, price, quantity, std::prev(level.queue.end())});
}

void ReferenceMarket::erase(OrderIndex::iterator it) {
  const Order& o = it->second;
  Levels& side_levels = levels(o.locate, o.side);
  auto level = side_levels.find(o.price);
  level->second.queue.erase(o.position);
  level->second.quantity -= o.quantity;
  if (level->second.queue.empty()) side_levels.erase(level);
  --books_[o.locate].orders;
  orders_.erase(it);
}

Result ReferenceMarket::add(Locate locate, OrderId id, Side side, Price price, Qty quantity) {
  if (quantity == 0) return Result::kZeroQuantity;
  if (orders_.contains(id)) return Result::kDuplicateOrder;
  insert(locate, id, side, price, quantity);
  return Result::kOk;
}

Result ReferenceMarket::reduce(Locate locate, OrderId id, Qty quantity) {
  auto it = orders_.find(id);
  if (it == orders_.end()) return Result::kUnknownOrder;
  Order& o = it->second;
  if (o.locate != locate) return Result::kLocateMismatch;
  if (quantity >= o.quantity) {
    const bool overfill = quantity > o.quantity;
    erase(it);
    return overfill ? Result::kOverfill : Result::kOk;
  }
  o.quantity -= quantity;
  levels(locate, o.side).find(o.price)->second.quantity -= quantity;
  return Result::kOk;
}

Result ReferenceMarket::remove(Locate locate, OrderId id) {
  auto it = orders_.find(id);
  if (it == orders_.end()) return Result::kUnknownOrder;
  if (it->second.locate != locate) return Result::kLocateMismatch;
  erase(it);
  return Result::kOk;
}

Result ReferenceMarket::replace(Locate locate, OrderId old_id, OrderId new_id, Price price,
                                Qty quantity) {
  auto it = orders_.find(old_id);
  if (it == orders_.end()) return Result::kUnknownOrder;
  if (it->second.locate != locate) return Result::kLocateMismatch;
  if (quantity == 0) return Result::kZeroQuantity;
  if (new_id != old_id && orders_.contains(new_id)) return Result::kDuplicateOrder;
  const Side side = it->second.side;
  erase(it);
  insert(locate, new_id, side, price, quantity);
  return Result::kOk;
}

std::optional<Level> ReferenceMarket::best(Locate locate, Side side) const {
  const Levels& l = levels(locate, side);
  if (l.empty()) return std::nullopt;
  const auto& [price, level] = side == Side::kBuy ? *l.rbegin() : *l.begin();
  return Level{price, level.quantity, static_cast<std::uint32_t>(level.queue.size())};
}

std::vector<Level> ReferenceMarket::depth(Locate locate, Side side, std::size_t max_levels) const {
  std::vector<Level> out;
  auto emit = [&](const auto& entry) {
    out.push_back({entry.first, entry.second.quantity,
                   static_cast<std::uint32_t>(entry.second.queue.size())});
    return out.size() < max_levels;
  };
  const Levels& l = levels(locate, side);
  if (side == Side::kBuy) {
    for (auto it = l.rbegin(); it != l.rend() && emit(*it); ++it) {}
  } else {
    for (auto it = l.begin(); it != l.end() && emit(*it); ++it) {}
  }
  return out;
}

std::vector<OrderEntry> ReferenceMarket::queue(Locate locate, Side side, Price price) const {
  std::vector<OrderEntry> out;
  const Levels& l = levels(locate, side);
  auto level = l.find(price);
  if (level == l.end()) return out;
  for (OrderId id : level->second.queue) out.push_back({id, orders_.at(id).quantity});
  return out;
}

std::optional<OrderView> ReferenceMarket::order(OrderId id) const {
  auto it = orders_.find(id);
  if (it == orders_.end()) return std::nullopt;
  const Order& o = it->second;
  return OrderView{o.locate, o.side, o.price, o.quantity};
}

std::string ReferenceMarket::check_invariants(Locate locate) const {
  std::size_t queued = 0;
  for (Side side : {Side::kBuy, Side::kSell}) {
    for (const auto& [price, level] : levels(locate, side)) {
      if (level.queue.empty()) return "empty level left in book at price " + std::to_string(price);
      std::uint64_t sum = 0;
      for (auto pos = level.queue.begin(); pos != level.queue.end(); ++pos) {
        auto it = orders_.find(*pos);
        if (it == orders_.end()) return "queued order " + std::to_string(*pos) + " missing from index";
        const Order& o = it->second;
        if (o.locate != locate || o.side != side || o.price != price || o.position != pos) {
          return "index entry for order " + std::to_string(*pos) + " disagrees with its queue slot";
        }
        if (o.quantity == 0) return "zero-quantity order " + std::to_string(*pos);
        sum += o.quantity;
        ++queued;
      }
      if (sum != level.quantity) return "level quantity != sum of orders at price " + std::to_string(price);
    }
  }
  if (queued != books_[locate].orders) return "book order count != queued orders";
  return {};
}

std::string ReferenceMarket::check_all_invariants() const {
  std::size_t total = 0;
  for (std::size_t locate = 0; locate < kMaxLocates; ++locate) {
    if (auto err = check_invariants(static_cast<Locate>(locate)); !err.empty()) {
      return "locate " + std::to_string(locate) + ": " + err;
    }
    total += books_[locate].orders;
  }
  if (total != orders_.size()) return "index holds orders that are in no book";
  return {};
}

}  // namespace mde::book
