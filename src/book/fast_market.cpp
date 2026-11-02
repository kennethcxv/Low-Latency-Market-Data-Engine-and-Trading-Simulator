#include "mde/book/fast_market.hpp"

#include <algorithm>
#include <bit>
#include <iterator>

namespace mde::book {

FastMarket::FastMarket(std::size_t expected_orders) : books_(kMaxLocates) {
  pool_.reserve(expected_orders);
  free_.reserve(expected_orders);
  // At most 50% full at the expected peak, so probe sequences stay short.
  const std::size_t slots = std::bit_ceil(std::max<std::size_t>(16, 2 * expected_orders));
  table_.assign(slots, Slot{0, kNone});
  shift_ = 64 - static_cast<unsigned>(std::countr_zero(slots));
}

// ---- Order index: linear probing with backward-shift deletion ----------------------------------

std::uint32_t FastMarket::find(OrderId id) const {
  const std::size_t mask = table_.size() - 1;
  for (std::size_t i = home(id);; i = (i + 1) & mask) {
    const Slot& s = table_[i];
    if (s.value == kNone) return kNone;
    if (s.key == id) return s.value;
  }
}

void FastMarket::insert_index(OrderId id, std::uint32_t pool_index) {
  if (2 * (live_orders_ + 1) > table_.size()) grow_index();
  const std::size_t mask = table_.size() - 1;
  std::size_t i = home(id);
  while (table_[i].value != kNone) i = (i + 1) & mask;
  table_[i] = {id, pool_index};
}

// Deleting from a linear-probing table cannot just empty the slot: a later key whose probe
// sequence passed through it would become unreachable. Instead, walk the cluster after the hole
// and move back any entry whose home position is at or before the hole (cyclically).
void FastMarket::erase_index(OrderId id) {
  const std::size_t mask = table_.size() - 1;
  std::size_t hole = home(id);
  while (table_[hole].key != id || table_[hole].value == kNone) hole = (hole + 1) & mask;
  for (std::size_t i = (hole + 1) & mask; table_[i].value != kNone; i = (i + 1) & mask) {
    const std::size_t h = home(table_[i].key);
    // Movable if `h` is not cyclically within (hole, i].
    if (((i - h) & mask) >= ((i - hole) & mask)) {
      table_[hole] = table_[i];
      hole = i;
    }
  }
  table_[hole].value = kNone;
}

void FastMarket::grow_index() {
  std::vector<Slot> old = std::move(table_);
  table_.assign(old.size() * 2, Slot{0, kNone});
  --shift_;
  const std::size_t mask = table_.size() - 1;
  for (const Slot& s : old) {
    if (s.value == kNone) continue;
    std::size_t i = home(s.key);
    while (table_[i].value != kNone) i = (i + 1) & mask;
    table_[i] = s;
  }
}

// ---- Levels -------------------------------------------------------------------------------------

std::size_t FastMarket::find_level(const Levels& l, Side side, Price price) {
  for (std::size_t k = l.size(); k-- > 0;) {  // from the best price outward
    if (l[k].price == price) return k;
    if (better(side, price, l[k].price)) break;  // passed where it would be
  }
  return l.size();
}

FastMarket::PriceLevel& FastMarket::level_for_add(Levels& l, Side side, Price price) {
  auto it = l.end();
  while (it != l.begin()) {
    auto prev = std::prev(it);
    if (prev->price == price) return *prev;
    if (better(side, price, prev->price)) break;  // `price` is better than everything below `it`
    it = prev;
  }
  return *l.insert(it, PriceLevel{price, 0, 0, kNone, kNone});
}

// ---- Orders -------------------------------------------------------------------------------------

std::uint32_t FastMarket::allocate() {
  if (!free_.empty()) {
    const std::uint32_t i = free_.back();
    free_.pop_back();
    return i;
  }
  pool_.emplace_back();
  return static_cast<std::uint32_t>(pool_.size() - 1);
}

void FastMarket::insert(Locate locate, OrderId id, Side side, Price price, Qty quantity) {
  const std::uint32_t index = allocate();
  PriceLevel& level = level_for_add(levels(locate, side), side, price);
  pool_[index] = Order{id, price, quantity, level.tail, kNone, locate, side};
  if (level.tail != kNone) {
    pool_[level.tail].next = index;
  } else {
    level.head = index;
  }
  level.tail = index;
  ++level.count;
  level.quantity += quantity;
  insert_index(id, index);
  ++live_orders_;
}

void FastMarket::unlink(std::uint32_t index) {
  const Order& o = pool_[index];
  Levels& l = levels(o.locate, o.side);
  const std::size_t k = find_level(l, o.side, o.price);
  PriceLevel& level = l[k];
  if (o.prev != kNone) pool_[o.prev].next = o.next;
  else level.head = o.next;
  if (o.next != kNone) pool_[o.next].prev = o.prev;
  else level.tail = o.prev;
  level.quantity -= o.quantity;
  if (--level.count == 0) l.erase(l.begin() + static_cast<std::ptrdiff_t>(k));
  erase_index(o.id);
  free_.push_back(index);
  --live_orders_;
}

Result FastMarket::add(Locate locate, OrderId id, Side side, Price price, Qty quantity) {
  if (quantity == 0) return Result::kZeroQuantity;
  if (find(id) != kNone) return Result::kDuplicateOrder;
  insert(locate, id, side, price, quantity);
  return Result::kOk;
}

Result FastMarket::reduce(Locate locate, OrderId id, Qty quantity) {
  const std::uint32_t index = find(id);
  if (index == kNone) return Result::kUnknownOrder;
  Order& o = pool_[index];
  if (o.locate != locate) return Result::kLocateMismatch;
  if (quantity >= o.quantity) {
    const bool overfill = quantity > o.quantity;
    unlink(index);
    return overfill ? Result::kOverfill : Result::kOk;
  }
  o.quantity -= quantity;
  Levels& l = levels(locate, o.side);
  l[find_level(l, o.side, o.price)].quantity -= quantity;
  return Result::kOk;
}

Result FastMarket::remove(Locate locate, OrderId id) {
  const std::uint32_t index = find(id);
  if (index == kNone) return Result::kUnknownOrder;
  if (pool_[index].locate != locate) return Result::kLocateMismatch;
  unlink(index);
  return Result::kOk;
}

Result FastMarket::replace(Locate locate, OrderId old_id, OrderId new_id, Price price, Qty quantity) {
  const std::uint32_t index = find(old_id);
  if (index == kNone) return Result::kUnknownOrder;
  if (pool_[index].locate != locate) return Result::kLocateMismatch;
  if (quantity == 0) return Result::kZeroQuantity;
  if (new_id != old_id && find(new_id) != kNone) return Result::kDuplicateOrder;
  const Side side = pool_[index].side;
  unlink(index);
  insert(locate, new_id, side, price, quantity);
  return Result::kOk;
}

// ---- Queries ------------------------------------------------------------------------------------

std::optional<Level> FastMarket::best(Locate locate, Side side) const {
  const Levels& l = levels(locate, side);
  if (l.empty()) return std::nullopt;
  return Level{l.back().price, l.back().quantity, l.back().count};
}

std::vector<Level> FastMarket::depth(Locate locate, Side side, std::size_t max_levels) const {
  std::vector<Level> out;
  const Levels& l = levels(locate, side);
  for (auto it = l.rbegin(); it != l.rend() && out.size() < max_levels; ++it) out.push_back({it->price, it->quantity, it->count});
  return out;
}

std::vector<OrderEntry> FastMarket::queue(Locate locate, Side side, Price price) const {
  std::vector<OrderEntry> out;
  const Levels& l = levels(locate, side);
  const std::size_t k = find_level(l, side, price);
  if (k == l.size()) return out;
  for (std::uint32_t i = l[k].head; i != kNone; i = pool_[i].next) out.push_back({pool_[i].id, pool_[i].quantity});
  return out;
}

std::optional<OrderView> FastMarket::order(OrderId id) const {
  const std::uint32_t index = find(id);
  if (index == kNone) return std::nullopt;
  const Order& o = pool_[index];
  return OrderView{o.locate, o.side, o.price, o.quantity};
}

std::string FastMarket::check_invariants(Locate locate) const {
  for (Side side : {Side::kBuy, Side::kSell}) {
    const Levels& l = levels(locate, side);
    for (std::size_t k = 0; k < l.size(); ++k) {
      const PriceLevel& level = l[k];
      if (k > 0 && !better(side, level.price, l[k - 1].price)) return "levels not strictly sorted toward the best price";
      if (level.count == 0 || level.head == kNone || level.tail == kNone) return "empty level left in book";
      std::uint64_t sum = 0;
      std::uint32_t count = 0;
      std::uint32_t prev = kNone;
      for (std::uint32_t i = level.head; i != kNone; prev = i, i = pool_[i].next) {
        const Order& o = pool_[i];
        if (o.prev != prev) return "broken prev link in level queue";
        if (o.locate != locate || o.side != side || o.price != level.price) return "order filed under the wrong level";
        if (o.quantity == 0) return "zero-quantity order";
        if (find(o.id) != i) return "index does not point at queued order " + std::to_string(o.id);
        sum += o.quantity;
        if (++count > level.count) return "level queue longer than its count";
      }
      if (prev != level.tail) return "level tail does not match the last order";
      if (count != level.count || sum != level.quantity) return "level count or quantity disagrees with its orders";
    }
  }
  return {};
}

std::string FastMarket::check_all_invariants() const {
  std::size_t queued = 0;
  for (std::size_t locate = 0; locate < kMaxLocates; ++locate) {
    if (auto err = check_invariants(static_cast<Locate>(locate)); !err.empty()) return "locate " + std::to_string(locate) + ": " + err;
    for (Side side : {Side::kBuy, Side::kSell}) {
      for (const PriceLevel& level : levels(static_cast<Locate>(locate), side)) queued += level.count;
    }
  }
  std::size_t indexed = 0;
  for (const Slot& s : table_) indexed += s.value != kNone;
  if (queued != live_orders_ || indexed != live_orders_) return "order count, index size and queued orders disagree";
  if (pool_.size() - free_.size() != live_orders_) return "pool slots leaked";
  return {};
}

}  // namespace mde::book
