// ITCH-to-book semantics: which messages change the book and which only count toward volume.
#include <gtest/gtest.h>

#include "mde/book/book_builder.hpp"
#include "mde/book/reference_market.hpp"

namespace mde::book {
namespace {

constexpr Locate kLoc = 3;

class BookBuilderTest : public ::testing::Test {
 protected:
  template <class T>
  void send(T m) {
    m.header.stock_locate = kLoc;
    std::uint8_t buf[itch::kMaxMessageSize];
    const std::size_t n = itch::encode(m, buf);
    ASSERT_EQ(builder.apply(Bytes(buf, n)), itch::DecodeStatus::kOk);
  }

  void add(OrderId id, char side, Price price, Qty shares) {
    itch::AddOrder m{};
    m.order_ref = id; m.side = side; m.price = price; m.shares = shares;
    send(m);
  }

  ReferenceMarket market;
  BookBuilder<ReferenceMarket> builder{market, /*check_invariants=*/true};
};

TEST_F(BookBuilderTest, ExecutionReducesBookAndCountsVolumeAtRestingPrice) {
  add(1, 'S', 20'0000, 300);
  itch::OrderExecuted e{};
  e.order_ref = 1; e.executed_shares = 100; e.match_number = 1;
  send(e);
  EXPECT_EQ(market.best(kLoc, Side::kSell)->quantity, 200u);
  EXPECT_EQ(builder.stats(kLoc).volume, 100u);
  EXPECT_EQ(builder.stats(kLoc).last_price, 20'0000u);
}

TEST_F(BookBuilderTest, NonPrintableExecutionReducesBookButNotVolume) {
  add(1, 'B', 20'0000, 300);
  itch::OrderExecutedWithPrice c{};
  c.order_ref = 1; c.executed_shares = 100; c.printable = 'N'; c.execution_price = 20'0100;
  send(c);
  EXPECT_EQ(market.best(kLoc, Side::kBuy)->quantity, 200u);
  EXPECT_EQ(builder.stats(kLoc).volume, 0u);

  c.printable = 'Y';
  send(c);
  EXPECT_EQ(market.best(kLoc, Side::kBuy)->quantity, 100u);
  EXPECT_EQ(market.best(kLoc, Side::kBuy)->price, 20'0000u);  // the order's price, not the execution's
  EXPECT_EQ(builder.stats(kLoc).volume, 100u);
  EXPECT_EQ(builder.stats(kLoc).last_price, 20'0100u);
}

TEST_F(BookBuilderTest, TradeAndCrossMessagesNeverTouchTheBook) {
  add(1, 'B', 20'0000, 300);
  itch::Trade p{};
  p.side = 'B'; p.shares = 500; p.price = 20'0050;
  send(p);
  itch::CrossTrade q{};
  q.shares = 10'000; q.cross_price = 20'0025; q.cross_type = 'O';
  send(q);
  q.shares = 0;  // insufficient interest: the spec allows a zero-share cross
  send(q);

  EXPECT_EQ(market.order_count(), 1u);
  EXPECT_EQ(market.best(kLoc, Side::kBuy), (Level{20'0000, 300, 1}));
  EXPECT_EQ(builder.stats(kLoc).volume, 10'500u);
  EXPECT_EQ(builder.stats(kLoc).trades, 2u);
  EXPECT_EQ(builder.stats(kLoc).last_price, 20'0025u);
}

TEST_F(BookBuilderTest, ReplaceCarriesSideAndCancelDeleteRemoveShares) {
  add(1, 'S', 30'0000, 300);
  itch::OrderReplace u{};
  u.original_order_ref = 1; u.new_order_ref = 2; u.shares = 250; u.price = 30'0100;
  send(u);
  EXPECT_EQ(market.best(kLoc, Side::kSell), (Level{30'0100, 250, 1}));

  itch::OrderCancel x{};
  x.order_ref = 2; x.cancelled_shares = 50;
  send(x);
  EXPECT_EQ(market.best(kLoc, Side::kSell)->quantity, 200u);

  itch::OrderDelete d{};
  d.order_ref = 2;
  send(d);
  EXPECT_FALSE(market.best(kLoc, Side::kSell));
  EXPECT_EQ(builder.stats(kLoc).volume, 0u);
}

TEST_F(BookBuilderTest, UnknownOrdersAreCountedNotFatal) {
  itch::OrderExecuted e{};
  e.order_ref = 42; e.executed_shares = 100;
  send(e);
  itch::OrderDelete d{};
  d.order_ref = 42;
  send(d);
  EXPECT_EQ(builder.counters().book[static_cast<std::size_t>(Result::kUnknownOrder)], 2u);
  // The trade happened even though its order predates our view; its price is unknown.
  EXPECT_EQ(builder.stats(kLoc).volume, 100u);
  EXPECT_EQ(builder.stats(kLoc).last_price, 0u);
  EXPECT_EQ(builder.counters().invariant_violations, 0u);
}

TEST_F(BookBuilderTest, TracksDirectoryTradingStateAndBrokenTrades) {
  itch::StockDirectory r{};
  r.stock = itch::alpha<8>("ABCD");
  send(r);
  itch::StockTradingAction h{};
  h.trading_state = 'H';
  send(h);
  itch::BrokenTrade b{};
  b.match_number = 9;
  send(b);
  EXPECT_EQ(itch::trim(builder.symbol(kLoc)), "ABCD");
  EXPECT_EQ(builder.stats(kLoc).trading_state, 'H');
  EXPECT_EQ(builder.counters().broken_trades, 1u);
}

}  // namespace
}  // namespace mde::book
