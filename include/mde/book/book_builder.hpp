// Applies decoded ITCH 5.0 messages to a Market and keeps per-instrument trade statistics.
//
// Book state and trade statistics are deliberately separate:
//  * A/F/E/C/X/D/U change the displayed book.
//  * E, printable C, P and Q contribute to volume. P (non-displayed executions) and Q (cross
//    prints) never touch the book; non-printable C reduces the order but is excluded from volume
//    because the shares are reported again in a later bulk print.
//  * B (broken trade) is counted. Volume is not reversed: that would need a match-number index
//    over every execution of the day. See docs/DESIGN.md.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "mde/book/types.hpp"
#include "mde/itch/codec.hpp"

namespace mde::book {

struct InstrumentStats {
  std::uint64_t volume = 0;
  std::uint64_t trades = 0;
  Price last_price = 0;
  char trading_state = ' ';  // last 'H' state: 'H' halted, 'P' paused, 'Q' quote-only, 'T' trading
};

struct BuilderCounters {
  std::uint64_t messages = 0;
  std::array<std::uint64_t, 5> decode{};             // indexed by itch::DecodeStatus
  std::array<std::uint64_t, kResultCount> book{};    // indexed by Result
  std::uint64_t broken_trades = 0;
  std::uint64_t invariant_violations = 0;

  [[nodiscard]] std::uint64_t book_anomalies() const {
    std::uint64_t n = 0;
    for (std::size_t i = 1; i < book.size(); ++i) n += book[i];
    return n;
  }
};

template <Market M>
class BookBuilder {
 public:
  // With check_invariants set, the touched instrument's book is fully validated after every
  // book-changing message (correctness mode; far too slow for benchmarking).
  explicit BookBuilder(M& market, bool check_invariants = false)
      : market_(market), check_(check_invariants), stats_(kMaxLocates), symbols_(kMaxLocates) {}

  itch::DecodeStatus apply(Bytes message) {
    ++counters_.messages;
    const auto status = itch::decode(message, *this);
    ++counters_.decode[static_cast<std::size_t>(status)];
    return status;
  }

  const M& market() const { return market_; }
  const BuilderCounters& counters() const { return counters_; }
  const InstrumentStats& stats(Locate locate) const { return stats_[locate]; }
  const itch::Symbol& symbol(Locate locate) const { return symbols_[locate]; }
  const std::string& first_violation() const { return first_violation_; }
  char system_event() const { return system_event_; }

  // Message handlers, public so the builder can be passed straight to itch::decode.
  void operator()(const itch::AddOrder& m) {
    book(m.header.stock_locate, market_.add(m.header.stock_locate, m.order_ref, side(m.side), m.price, m.shares));
  }
  void operator()(const itch::AddOrderMpid& m) {
    book(m.header.stock_locate, market_.add(m.header.stock_locate, m.order_ref, side(m.side), m.price, m.shares));
  }
  void operator()(const itch::OrderExecuted& m) {
    const Locate loc = m.header.stock_locate;
    const auto resting = market_.order(m.order_ref);
    book(loc, market_.reduce(loc, m.order_ref, m.executed_shares));
    // The shares traded whether or not we know the order (it may predate a gap), but the
    // price is only known from the resting order.
    trade(loc, m.executed_shares, resting && resting->locate == loc ? resting->price : 0);
  }
  void operator()(const itch::OrderExecutedWithPrice& m) {
    const Locate loc = m.header.stock_locate;
    book(loc, market_.reduce(loc, m.order_ref, m.executed_shares));
    if (m.printable == 'Y') trade(loc, m.executed_shares, m.execution_price);
  }
  void operator()(const itch::OrderCancel& m) {
    book(m.header.stock_locate, market_.reduce(m.header.stock_locate, m.order_ref, m.cancelled_shares));
  }
  void operator()(const itch::OrderDelete& m) {
    book(m.header.stock_locate, market_.remove(m.header.stock_locate, m.order_ref));
  }
  void operator()(const itch::OrderReplace& m) {
    book(m.header.stock_locate, market_.replace(m.header.stock_locate, m.original_order_ref,
                                                m.new_order_ref, m.price, m.shares));
  }
  void operator()(const itch::Trade& m) { trade(m.header.stock_locate, m.shares, m.price); }
  void operator()(const itch::CrossTrade& m) {
    if (m.shares > 0) trade(m.header.stock_locate, m.shares, m.cross_price);
  }
  void operator()(const itch::BrokenTrade&) { ++counters_.broken_trades; }
  void operator()(const itch::StockDirectory& m) { symbols_[m.header.stock_locate] = m.stock; }
  void operator()(const itch::StockTradingAction& m) {
    stats_[m.header.stock_locate].trading_state = m.trading_state;
  }
  void operator()(const itch::SystemEvent& m) { system_event_ = m.event_code; }
  template <class Other>
  void operator()(const Other&) {}  // informational messages with no book or volume effect

 private:
  static Side side(char c) { return c == 'B' ? Side::kBuy : Side::kSell; }

  void book(Locate locate, Result r) {
    ++counters_.book[static_cast<std::size_t>(r)];
    if constexpr (requires { market_.check_invariants(locate); }) {
      if (!check_) return;
      if (std::string err = market_.check_invariants(locate); !err.empty()) {
        if (counters_.invariant_violations++ == 0) {
          first_violation_ = "message " + std::to_string(counters_.messages) + ": " + err;
        }
      }
    }
  }

  void trade(Locate locate, std::uint64_t shares, Price price) {
    InstrumentStats& s = stats_[locate];
    s.volume += shares;
    ++s.trades;
    if (price != 0) s.last_price = price;
  }

  M& market_;
  bool check_;
  BuilderCounters counters_;
  std::vector<InstrumentStats> stats_;
  std::vector<itch::Symbol> symbols_;
  std::string first_violation_;
  char system_event_ = ' ';
};

}  // namespace mde::book
