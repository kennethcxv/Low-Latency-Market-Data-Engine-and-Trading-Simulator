// Optimized order book. Same semantics and Result policy as ReferenceMarket, verified by
// differential tests; different data layout:
//
//  * Orders live in one preallocated pool (std::vector) and are linked into their level's FIFO
//    by 32-bit pool indices, so adding or removing an order allocates nothing once warm.
//  * The order-id index is an open-addressing hash table (linear probing, Fibonacci hashing,
//    backward-shift deletion: no tombstones, so probe lengths do not degrade over a day of churn).
//  * Each side of a book is a sorted contiguous vector of levels with the best price at the back.
//    Most activity is within a few ticks of the touch, so finding a level is a short scan from
//    the back over 24-byte entries, and inserting or erasing near the touch moves few elements.
//
// Growth: the pool and the index grow (amortized doubling) if a day exceeds the reserved
// capacity; reserve generously up front to keep growth out of the hot path.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "mde/book/types.hpp"

namespace mde::book {

class FastMarket {
 public:
  explicit FastMarket(std::size_t expected_orders = 1 << 18);

  Result add(Locate locate, OrderId id, Side side, Price price, Qty quantity);
  Result reduce(Locate locate, OrderId id, Qty quantity);
  Result remove(Locate locate, OrderId id);
  Result replace(Locate locate, OrderId old_id, OrderId new_id, Price price, Qty quantity);

  std::optional<Level> best(Locate locate, Side side) const;
  std::vector<Level> depth(Locate locate, Side side, std::size_t max_levels) const;
  std::vector<OrderEntry> queue(Locate locate, Side side, Price price) const;
  std::optional<OrderView> order(OrderId id) const;
  std::size_t order_count() const { return live_orders_; }

  std::string check_invariants(Locate locate) const;
  std::string check_all_invariants() const;

 private:
  static constexpr std::uint32_t kNone = 0xFFFF'FFFF;

  struct Order {
    OrderId id;
    Price price;
    Qty quantity;
    std::uint32_t prev;  // pool indices within the level's FIFO; kNone at the ends
    std::uint32_t next;
    Locate locate;
    Side side;
  };
  static_assert(sizeof(Order) == 32);

  struct PriceLevel {
    Price price;
    std::uint32_t count;
    std::uint64_t quantity;
    std::uint32_t head;  // oldest order
    std::uint32_t tail;
  };
  static_assert(sizeof(PriceLevel) == 24);

  // Sorted so the best price is at the back: bids ascending, asks descending.
  using Levels = std::vector<PriceLevel>;
  struct Book {
    Levels bids;
    Levels asks;
  };

  struct Slot {
    OrderId key;
    std::uint32_t value;  // pool index, or kNone if the slot is empty
  };

  // Order index.
  std::uint32_t find(OrderId id) const;
  void insert_index(OrderId id, std::uint32_t pool_index);
  void erase_index(OrderId id);
  void grow_index();
  std::size_t home(OrderId id) const { return static_cast<std::size_t>((id * 0x9E37'79B9'7F4A'7C15ULL) >> shift_); }

  Levels& levels(Locate locate, Side side) { return side == Side::kBuy ? books_[locate].bids : books_[locate].asks; }
  const Levels& levels(Locate locate, Side side) const {
    return side == Side::kBuy ? books_[locate].bids : books_[locate].asks;
  }
  static bool better(Side side, Price a, Price b) { return side == Side::kBuy ? a > b : a < b; }
  // Index of the level at `price`, or l.size() if there is none.
  static std::size_t find_level(const Levels& l, Side side, Price price);
  PriceLevel& level_for_add(Levels& l, Side side, Price price);

  std::uint32_t allocate();
  void insert(Locate locate, OrderId id, Side side, Price price, Qty quantity);
  void unlink(std::uint32_t index);  // removes from its level, frees the slot, erases from index

  std::vector<Order> pool_;
  std::vector<std::uint32_t> free_;
  std::vector<Slot> table_;
  unsigned shift_ = 0;  // 64 - log2(table size)
  std::size_t live_orders_ = 0;
  std::vector<Book> books_;
};

static_assert(Market<FastMarket>);

}  // namespace mde::book
