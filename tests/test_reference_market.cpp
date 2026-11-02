#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <tuple>

#include "mde/book/reference_market.hpp"

namespace mde::book {
namespace {

constexpr Locate kLoc = 7;

std::vector<OrderId> ids(const std::vector<OrderEntry>& q) {
  std::vector<OrderId> out;
  for (const auto& e : q) out.push_back(e.id);
  return out;
}

TEST(ReferenceMarket, AggregatesLevelsBestFirst) {
  ReferenceMarket m;
  EXPECT_EQ(m.add(kLoc, 1, Side::kBuy, 100'0000, 100), Result::kOk);
  EXPECT_EQ(m.add(kLoc, 2, Side::kBuy, 101'0000, 200), Result::kOk);
  EXPECT_EQ(m.add(kLoc, 3, Side::kBuy, 101'0000, 50), Result::kOk);
  EXPECT_EQ(m.add(kLoc, 4, Side::kSell, 103'0000, 10), Result::kOk);
  EXPECT_EQ(m.add(kLoc, 5, Side::kSell, 102'0000, 20), Result::kOk);

  EXPECT_EQ(m.best(kLoc, Side::kBuy), (Level{101'0000, 250, 2}));
  EXPECT_EQ(m.best(kLoc, Side::kSell), (Level{102'0000, 20, 1}));
  EXPECT_EQ(m.depth(kLoc, Side::kBuy, 10), (std::vector<Level>{{101'0000, 250, 2}, {100'0000, 100, 1}}));
  EXPECT_EQ(m.depth(kLoc, Side::kSell, 1), (std::vector<Level>{{102'0000, 20, 1}}));
  EXPECT_FALSE(m.best(kLoc + 1, Side::kBuy));
  EXPECT_EQ(m.order_count(), 5u);
  EXPECT_EQ(m.check_all_invariants(), "");
}

TEST(ReferenceMarket, PartialReductionKeepsPriorityAndReplaceLosesIt) {
  ReferenceMarket m;
  for (OrderId id = 1; id <= 3; ++id) m.add(kLoc, id, Side::kSell, 50'0000, 100);
  EXPECT_EQ(m.reduce(kLoc, 1, 40), Result::kOk);
  EXPECT_EQ(ids(m.queue(kLoc, Side::kSell, 50'0000)), (std::vector<OrderId>{1, 2, 3}));
  EXPECT_EQ(m.best(kLoc, Side::kSell)->quantity, 260u);

  // Same price, same size: still goes to the back with its new reference number.
  EXPECT_EQ(m.replace(kLoc, 1, 10, 50'0000, 60), Result::kOk);
  EXPECT_EQ(ids(m.queue(kLoc, Side::kSell, 50'0000)), (std::vector<OrderId>{2, 3, 10}));
  EXPECT_FALSE(m.order(1));
  EXPECT_EQ(m.order(10)->side, Side::kSell);  // side carried over from the original order
  EXPECT_EQ(m.check_all_invariants(), "");
}

TEST(ReferenceMarket, EmptyLevelsDisappear) {
  ReferenceMarket m;
  m.add(kLoc, 1, Side::kBuy, 10'0000, 100);
  m.add(kLoc, 2, Side::kBuy, 9'0000, 100);
  EXPECT_EQ(m.reduce(kLoc, 1, 100), Result::kOk);
  EXPECT_EQ(m.best(kLoc, Side::kBuy)->price, 9'0000u);
  EXPECT_EQ(m.remove(kLoc, 2), Result::kOk);
  EXPECT_FALSE(m.best(kLoc, Side::kBuy));
  EXPECT_TRUE(m.depth(kLoc, Side::kBuy, 10).empty());
  EXPECT_EQ(m.order_count(), 0u);
  EXPECT_EQ(m.check_all_invariants(), "");
}

TEST(ReferenceMarket, InconsistentInputIsRejectedWithoutSideEffects) {
  ReferenceMarket m;
  m.add(kLoc, 1, Side::kBuy, 10'0000, 100);
  m.add(kLoc, 2, Side::kSell, 11'0000, 100);

  EXPECT_EQ(m.add(kLoc, 1, Side::kSell, 12'0000, 5), Result::kDuplicateOrder);
  EXPECT_EQ(m.add(kLoc, 3, Side::kBuy, 10'0000, 0), Result::kZeroQuantity);
  EXPECT_EQ(m.reduce(kLoc, 99, 1), Result::kUnknownOrder);
  EXPECT_EQ(m.remove(kLoc, 99), Result::kUnknownOrder);
  EXPECT_EQ(m.replace(kLoc, 99, 100, 1, 1), Result::kUnknownOrder);
  EXPECT_EQ(m.reduce(kLoc + 1, 1, 1), Result::kLocateMismatch);
  EXPECT_EQ(m.remove(kLoc + 1, 1), Result::kLocateMismatch);
  EXPECT_EQ(m.replace(kLoc + 1, 1, 5, 1, 1), Result::kLocateMismatch);
  EXPECT_EQ(m.replace(kLoc, 1, 2, 10'0000, 10), Result::kDuplicateOrder);  // new id already live
  EXPECT_EQ(m.replace(kLoc, 1, 5, 10'0000, 0), Result::kZeroQuantity);

  EXPECT_EQ(m.best(kLoc, Side::kBuy), (Level{10'0000, 100, 1}));
  EXPECT_EQ(m.best(kLoc, Side::kSell), (Level{11'0000, 100, 1}));
  EXPECT_EQ(m.order_count(), 2u);
  EXPECT_EQ(m.check_all_invariants(), "");
}

TEST(ReferenceMarket, OverfillRemovesTheOrder) {
  ReferenceMarket m;
  m.add(kLoc, 1, Side::kBuy, 10'0000, 100);
  EXPECT_EQ(m.reduce(kLoc, 1, 150), Result::kOverfill);
  EXPECT_FALSE(m.order(1));
  EXPECT_FALSE(m.best(kLoc, Side::kBuy));
  EXPECT_EQ(m.check_all_invariants(), "");
}

// Before the opening cross a Nasdaq book can be locked or crossed; the book must represent it.
TEST(ReferenceMarket, CrossedBookIsRepresentedNotRejected) {
  ReferenceMarket m;
  m.add(kLoc, 1, Side::kSell, 10'0000, 100);
  EXPECT_EQ(m.add(kLoc, 2, Side::kBuy, 10'0500, 100), Result::kOk);
  EXPECT_GT(m.best(kLoc, Side::kBuy)->price, m.best(kLoc, Side::kSell)->price);
  EXPECT_EQ(m.check_all_invariants(), "");
}

// Property test against a second, even simpler oracle: a flat list of live orders with an
// arrival counter, from which levels and queues are recomputed by sorting. Random operations,
// a third of them deliberately inconsistent, across a few instruments and a narrow price band so
// levels are shared, emptied and recreated constantly.
TEST(ReferenceMarket, MatchesFlatOracleUnderRandomOperations) {
  struct OracleOrder {
    Locate locate;
    Side side;
    Price price;
    Qty quantity;
    std::uint64_t arrival;
  };
  std::map<OrderId, OracleOrder> oracle;
  std::uint64_t arrival = 0;

  auto oracle_queue = [&](Locate loc, Side side, Price price) {
    std::vector<std::pair<std::uint64_t, OrderEntry>> rows;
    for (const auto& [id, o] : oracle) {
      if (o.locate == loc && o.side == side && o.price == price) rows.push_back({o.arrival, {id, o.quantity}});
    }
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<OrderEntry> out;
    for (const auto& r : rows) out.push_back(r.second);
    return out;
  };
  auto oracle_depth = [&](Locate loc, Side side) {
    std::map<Price, Level> levels;
    for (const auto& [id, o] : oracle) {
      if (o.locate != loc || o.side != side) continue;
      Level& l = levels[o.price];
      l.price = o.price;
      l.quantity += o.quantity;
      ++l.orders;
    }
    std::vector<Level> out;
    for (const auto& [p, l] : levels) out.push_back(l);
    if (side == Side::kBuy) std::reverse(out.begin(), out.end());
    return out;
  };

  ReferenceMarket m;
  Rng rng{7};
  OrderId next_id = 1;
  for (int step = 0; step < 20'000; ++step) {
    const Locate loc = static_cast<Locate>(1 + rng.below(3));
    const Side side = rng.below(2) ? Side::kBuy : Side::kSell;
    const Price price = static_cast<Price>(100 + rng.below(8));
    const Qty qty = static_cast<Qty>(rng.below(6));  // includes zero
    // Mostly live ids; sometimes stale or never-seen ones.
    OrderId target = next_id + 5;
    if (!oracle.empty() && rng.percent(80)) {
      auto it = oracle.begin();
      std::advance(it, static_cast<std::ptrdiff_t>(rng.below(oracle.size())));
      target = it->first;
    }
    const Locate target_loc = rng.percent(95) && oracle.contains(target) ? oracle.at(target).locate : loc;

    Result expected = Result::kOk;
    Result actual = Result::kOk;
    switch (rng.below(4)) {
      case 0: {
        const OrderId id = rng.percent(5) && !oracle.empty() ? oracle.begin()->first : next_id++;
        if (qty == 0) expected = Result::kZeroQuantity;
        else if (oracle.contains(id)) expected = Result::kDuplicateOrder;
        else oracle[id] = {loc, side, price, qty, arrival++};
        actual = m.add(loc, id, side, price, qty);
        break;
      }
      case 1: {
        if (!oracle.contains(target)) expected = Result::kUnknownOrder;
        else if (oracle.at(target).locate != target_loc) expected = Result::kLocateMismatch;
        else if (qty >= oracle.at(target).quantity) {
          expected = qty > oracle.at(target).quantity ? Result::kOverfill : Result::kOk;
          oracle.erase(target);
        } else {
          oracle.at(target).quantity -= qty;
        }
        actual = m.reduce(target_loc, target, qty);
        break;
      }
      case 2: {
        if (!oracle.contains(target)) expected = Result::kUnknownOrder;
        else if (oracle.at(target).locate != target_loc) expected = Result::kLocateMismatch;
        else oracle.erase(target);
        actual = m.remove(target_loc, target);
        break;
      }
      case 3: {
        const OrderId new_id = next_id++;
        if (!oracle.contains(target)) expected = Result::kUnknownOrder;
        else if (oracle.at(target).locate != target_loc) expected = Result::kLocateMismatch;
        else if (qty == 0) expected = Result::kZeroQuantity;
        else {
          OracleOrder o = oracle.at(target);
          oracle.erase(target);
          oracle[new_id] = {o.locate, o.side, price, qty, arrival++};
        }
        actual = m.replace(target_loc, target, new_id, price, qty);
        break;
      }
    }
    ASSERT_EQ(actual, expected) << "step " << step;
    ASSERT_EQ(m.order_count(), oracle.size()) << "step " << step;
    ASSERT_EQ(m.check_invariants(loc), "") << "step " << step;
    ASSERT_EQ(m.check_invariants(target_loc), "") << "step " << step;
    for (Side s : {Side::kBuy, Side::kSell}) {
      const auto depth = m.depth(target_loc, s, SIZE_MAX);
      ASSERT_EQ(depth, oracle_depth(target_loc, s)) << "step " << step;
      for (const Level& l : depth) ASSERT_EQ(m.queue(target_loc, s, l.price), oracle_queue(target_loc, s, l.price));
    }
  }
  EXPECT_EQ(m.check_all_invariants(), "");
}

}  // namespace
}  // namespace mde::book
