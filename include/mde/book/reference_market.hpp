// Reference order book: the simplest structure that is obviously correct. It is the oracle that
// the optimized market is differentially tested against, and the baseline it is benchmarked
// against. Nothing here is tuned.
#pragma once

#include <list>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "mde/book/types.hpp"

namespace mde::book {

class ReferenceMarket {
 public:
  ReferenceMarket();

  Result add(Locate locate, OrderId id, Side side, Price price, Qty quantity);
  Result reduce(Locate locate, OrderId id, Qty quantity);  // execution or partial cancel
  Result remove(Locate locate, OrderId id);
  // Removes `old_id` and adds `new_id` on the same side at the back of its new level.
  Result replace(Locate locate, OrderId old_id, OrderId new_id, Price price, Qty quantity);

  std::optional<Level> best(Locate locate, Side side) const;
  std::vector<Level> depth(Locate locate, Side side, std::size_t max_levels) const;
  std::vector<OrderEntry> queue(Locate locate, Side side, Price price) const;
  std::optional<OrderView> order(OrderId id) const;
  std::size_t order_count() const { return orders_.size(); }

  // Empty if every structural invariant of this instrument's book holds, otherwise a description
  // of the first violation. check_all_invariants() also checks the global order count.
  std::string check_invariants(Locate locate) const;
  std::string check_all_invariants() const;

 private:
  struct PriceLevel {
    std::list<OrderId> queue;  // front = oldest = highest time priority
    std::uint64_t quantity = 0;
  };
  using Levels = std::map<Price, PriceLevel>;  // best bid is the last key, best ask the first

  struct Book {
    Levels bids;
    Levels asks;
    std::size_t orders = 0;
  };

  struct Order {
    Locate locate;
    Side side;
    Price price;
    Qty quantity;
    std::list<OrderId>::iterator position;
  };
  using OrderIndex = std::unordered_map<OrderId, Order>;

  Levels& levels(Locate locate, Side side);
  const Levels& levels(Locate locate, Side side) const;
  void insert(Locate locate, OrderId id, Side side, Price price, Qty quantity);
  void erase(OrderIndex::iterator it);

  OrderIndex orders_;
  std::vector<Book> books_;
};

static_assert(Market<ReferenceMarket>);

}  // namespace mde::book
