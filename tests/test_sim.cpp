#include <gtest/gtest.h>

#include <map>

#include "mde/sim/exchange.hpp"
#include "mde/sim/simulator.hpp"

namespace mde::sim {
namespace {

NewOrder order(AccountId account, ClientOrderId id, Side side, Price price, Qty qty, InstrumentId instrument = 0) {
  return {account, id, instrument, side, price, qty};
}

TEST(OrderBook, PriceThenTimePriorityAndNoTradeThrough) {
  OrderBook book;
  book.rest(1, Side::kSell, 101, 10);
  book.rest(2, Side::kSell, 100, 10);
  book.rest(3, Side::kSell, 100, 10);  // same price as 2, arrived later
  std::vector<OrderBook::Fill> fills;
  const Qty left = book.match(Side::kBuy, 100, 25, fills);  // must not touch 101
  ASSERT_EQ(fills.size(), 2u);
  EXPECT_EQ(fills[0].resting_id, 2u);
  EXPECT_EQ(fills[1].resting_id, 3u);
  EXPECT_EQ(left, 5);
  EXPECT_EQ(book.best(Side::kSell), 101);
  EXPECT_EQ(book.order_count(), 1u);
}

TEST(OrderBook, PartialFillKeepsPriorityAndCancelRemovesLeaves) {
  OrderBook book;
  book.rest(1, Side::kBuy, 50, 10);
  book.rest(2, Side::kBuy, 50, 10);
  std::vector<OrderBook::Fill> fills;
  EXPECT_EQ(book.match(Side::kSell, 50, 4, fills), 0);
  EXPECT_EQ(fills[0].resting_id, 1u);
  EXPECT_EQ(book.quantity_at(Side::kBuy, 50), 16);
  EXPECT_EQ(book.cancel(1), 6);
  EXPECT_EQ(book.cancel(1), std::nullopt);
  fills.clear();
  book.match(Side::kSell, 1, 100, fills);  // sells at any price: 2 is now first
  EXPECT_EQ(fills[0].resting_id, 2u);
  EXPECT_FALSE(book.best(Side::kBuy));
}

TEST(Exchange, TradeProducesReportsForBothSidesAndMovesPositions) {
  Exchange ex(1, RiskLimits{});
  std::vector<ExecutionReport> out;
  ex.submit(order(1, 10, Side::kSell, 100, 30), out);
  ex.submit(order(2, 20, Side::kBuy, 105, 50), out);  // crosses: trades 30 @ 100, rests 20 @ 105

  ASSERT_EQ(out.size(), 4u);
  EXPECT_EQ(out[0].type, ExecType::kNew);
  EXPECT_EQ(out[1].type, ExecType::kNew);
  EXPECT_EQ(out[2].account, 2u);  // aggressor
  EXPECT_EQ(out[2].type, ExecType::kPartialFill);
  EXPECT_EQ(out[2].last_price, 100);  // the resting order's price
  EXPECT_EQ(out[2].last_quantity, 30);
  EXPECT_EQ(out[2].leaves_quantity, 20);
  EXPECT_EQ(out[3].account, 1u);  // resting side
  EXPECT_EQ(out[3].type, ExecType::kFill);
  EXPECT_EQ(out[3].leaves_quantity, 0);
  EXPECT_EQ(out[3].cumulative_quantity, 30);
  for (std::size_t i = 1; i < out.size(); ++i) EXPECT_GT(out[i].exec_id, out[i - 1].exec_id);

  EXPECT_EQ(ex.position(1, 0).net, -30);
  EXPECT_EQ(ex.position(1, 0).open_sell, 0);
  EXPECT_EQ(ex.position(2, 0).net, 30);
  EXPECT_EQ(ex.position(2, 0).open_buy, 20);
  EXPECT_EQ(ex.book(0).best(Side::kBuy), 105);
}

TEST(Exchange, CancelReleasesOpenQuantityAndUnknownCancelIsRejected) {
  Exchange ex(1, RiskLimits{});
  std::vector<ExecutionReport> out;
  ex.submit(order(1, 7, Side::kBuy, 100, 40), out);
  ex.cancel({1, 7}, out);
  ex.cancel({1, 7}, out);  // already gone
  ASSERT_EQ(out.size(), 3u);
  EXPECT_EQ(out[1].type, ExecType::kCanceled);
  EXPECT_EQ(out[1].leaves_quantity, 0);
  EXPECT_EQ(out[2].type, ExecType::kCancelRejected);
  EXPECT_EQ(out[2].reason, RejectReason::kUnknownOrder);
  EXPECT_EQ(ex.position(1, 0).open_buy, 0);
  EXPECT_EQ(ex.book(0).order_count(), 0u);
}

TEST(Exchange, PreTradeRiskChecks) {
  RiskLimits limits;
  limits.max_order_quantity = 100;
  limits.max_order_notional = 10'000;
  limits.max_position = 150;
  Exchange ex(2, limits);
  std::vector<ExecutionReport> out;
  auto reject_reason = [&](const NewOrder& o) {
    out.clear();
    ex.submit(o, out);
    return out.at(0).type == ExecType::kRejected ? out[0].reason : RejectReason::kNone;
  };

  EXPECT_EQ(reject_reason(order(1, 1, Side::kBuy, 50, 10, 2)), RejectReason::kUnknownInstrument);
  EXPECT_EQ(reject_reason(order(1, 2, Side::kBuy, 0, 10)), RejectReason::kInvalidPrice);
  EXPECT_EQ(reject_reason(order(1, 3, Side::kBuy, 50, 0)), RejectReason::kInvalidQuantity);
  EXPECT_EQ(reject_reason(order(1, 4, Side::kBuy, 50, 101)), RejectReason::kMaxOrderQuantity);
  EXPECT_EQ(reject_reason(order(1, 5, Side::kBuy, 101, 100)), RejectReason::kMaxOrderNotional);
  EXPECT_EQ(reject_reason(order(1, 6, Side::kBuy, INT64_MAX, 2)), RejectReason::kMaxOrderNotional);  // no overflow
  EXPECT_EQ(reject_reason(order(1, 7, Side::kBuy, 100, 100)), RejectReason::kNone);  // exactly at the notional limit
  EXPECT_EQ(reject_reason(order(1, 7, Side::kBuy, 10, 1)), RejectReason::kDuplicateClientId);

  // Position limit counts open orders on the same side: 100 open + 60 > 150 even with no fills.
  EXPECT_EQ(reject_reason(order(1, 8, Side::kBuy, 10, 60)), RejectReason::kPositionLimit);
  EXPECT_EQ(reject_reason(order(1, 9, Side::kBuy, 10, 50)), RejectReason::kNone);
  // The short side is bounded the same way, per instrument.
  EXPECT_EQ(reject_reason(order(1, 10, Side::kSell, 100, 100, 1)), RejectReason::kNone);
  EXPECT_EQ(reject_reason(order(1, 11, Side::kSell, 100, 51, 1)), RejectReason::kPositionLimit);
  // Rejected orders change nothing.
  EXPECT_EQ(ex.position(1, 0).open_buy, 150);
  EXPECT_EQ(ex.position(1, 1).open_sell, 100);
}

// Global properties over a long random run: every trade has two equal and opposite sides, so net
// positions sum to zero per instrument; open quantities match a model rebuilt from the reports;
// no account ever exceeds its position limit.
TEST(Simulator, ConservesPositionsAndRespectsLimits) {
  SimConfig config;
  config.seed = 17;
  config.steps = 50'000;
  config.limits.max_position = 2'000;
  Exchange ex(config.instruments, config.limits);
  std::vector<ExecutionReport> reports;
  const SimSummary s = simulate(config, ex, &reports);

  EXPECT_GT(s.fills, 1000u);
  for (std::size_t r = 1; r < kRejectReasonCount; ++r) {
    EXPECT_GT(s.rejects[r], 0u) << to_string(static_cast<RejectReason>(r)) << " never exercised";
  }

  std::map<std::pair<AccountId, InstrumentId>, std::pair<Qty, Qty>> open;  // leaves of live orders
  std::map<std::pair<AccountId, ClientOrderId>, ExecutionReport> last;
  for (const auto& r : reports) {
    if (r.type == ExecType::kRejected || r.type == ExecType::kCancelRejected) continue;
    last[{r.account, r.client_id}] = r;
  }
  for (const auto& [key, r] : last) {
    auto& o = open[{r.account, r.instrument}];
    (r.side == Side::kBuy ? o.first : o.second) += r.leaves_quantity;
  }
  for (InstrumentId i = 0; i < config.instruments; ++i) {
    std::int64_t net = 0;
    for (AccountId a = 0; a < config.accounts; ++a) {
      const Position& p = ex.position(a, i);
      net += p.net;
      const auto& expected_open = open[std::make_pair(a, i)];
      EXPECT_EQ(p.open_buy, expected_open.first);
      EXPECT_EQ(p.open_sell, expected_open.second);
      EXPECT_LE(p.net + p.open_buy, config.limits.max_position);
      EXPECT_GE(p.net - p.open_sell, -config.limits.max_position);
    }
    EXPECT_EQ(net, 0) << "instrument " << i;
  }
}

TEST(Simulator, IsDeterministicAndPlatformIndependent) {
  SimConfig config;
  config.seed = 4242;
  config.steps = 20'000;
  Exchange a(config.instruments, config.limits), b(config.instruments, config.limits);
  const auto first = simulate(config, a);
  EXPECT_EQ(first.digest, simulate(config, b).digest);
  EXPECT_EQ(first.digest, 0x2dce05ac4b9c9c33ULL) << std::hex << first.digest;
}

}  // namespace
}  // namespace mde::sim
