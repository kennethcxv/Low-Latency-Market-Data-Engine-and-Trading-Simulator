#include "mde/synth/generator.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include "mde/book/reference_market.hpp"
#include "mde/itch/codec.hpp"

namespace mde::synth {
namespace {

using book::Locate;
using book::OrderId;
using book::Price;
using book::Qty;
using book::Result;
using book::Side;

constexpr Price kTick = 100;  // $0.01 in Price(4) units
constexpr std::uint64_t kNsPerHour = 3'600'000'000'000ULL;

class Generator {
 public:
  explicit Generator(const Config& c) : config_(c), rng_(c.seed) {
    if (c.symbols == 0 || c.symbols > 10'000) throw std::invalid_argument("symbols must be 1..10000");
    // open_session() emits 11 + 9*symbols messages and close_session() 4 + 2*symbols.
    const std::size_t fixed = 15 + 11 * c.symbols;
    if (c.messages < fixed) throw std::invalid_argument("messages must be at least " + std::to_string(fixed));
    continuous_target_ = c.messages - (4 + 2 * c.symbols);
  }

  std::vector<std::uint8_t> run() {
    open_session();
    while (emitted_ < continuous_target_) step();
    close_session();
    return std::move(out_);
  }

 private:
  struct Instrument {
    Locate locate;
    itch::Symbol symbol;
    Price mid;
    std::vector<OrderId> live;
  };

  template <class T>
  void emit(T m, Locate locate) {
    now_ns_ += 1 + rng_.below(4000);
    m.header.stock_locate = locate;
    m.header.timestamp.ns = now_ns_;
    itch::append_framed(out_, m);
    ++emitted_;
  }

  void expect_ok(Result r) {
    if (r != Result::kOk) throw std::logic_error("generator produced an inconsistent event");
  }

  // Skewed so a few instruments carry most of the traffic.
  Instrument& pick_instrument() { return instruments_[rng_.below(rng_.below(instruments_.size()) + 1)]; }

  std::size_t pick_live(const Instrument& inst) {
    const std::size_t n = inst.live.size();
    if (n > 8 && rng_.percent(70)) return n - 1 - rng_.below(8);  // recent orders churn most
    return rng_.below(n);
  }

  Qty random_shares() { return rng_.percent(10) ? static_cast<Qty>(1 + rng_.below(99)) : static_cast<Qty>(100 * (1 + rng_.below(10))); }

  // A price on `side` that never crosses the opposite best. Distance from the touch is roughly
  // geometric, so most orders rest within a few ticks.
  Price passive_price(const Instrument& inst, Side side) {
    Price ticks = 1;
    while (ticks < 40 && rng_.percent(60)) ++ticks;
    if (side == Side::kBuy) {
      Price p = inst.mid > ticks * kTick ? inst.mid - ticks * kTick : kTick;
      if (auto ask = market_.best(inst.locate, Side::kSell)) p = std::min(p, ask->price - kTick);
      return std::max(p, kTick);
    }
    Price p = inst.mid + ticks * kTick;
    if (auto bid = market_.best(inst.locate, Side::kBuy)) p = std::max(p, bid->price + kTick);
    return p;
  }

  void track_add(Instrument& inst, OrderId id) {
    live_index_[id] = inst.live.size();
    inst.live.push_back(id);
  }

  void track_remove(Instrument& inst, OrderId id) {
    const std::size_t i = live_index_.at(id);
    live_index_.erase(id);
    inst.live[i] = inst.live.back();
    inst.live.pop_back();
    if (i < inst.live.size()) live_index_[inst.live[i]] = i;
  }

  void add(Instrument& inst) {
    if (rng_.percent(5)) inst.mid = std::max<Price>(100 * kTick, rng_.percent(50) ? inst.mid + kTick : inst.mid - kTick);
    const Side side = rng_.percent(50) ? Side::kBuy : Side::kSell;
    const Price price = passive_price(inst, side);
    const Qty shares = random_shares();
    const OrderId id = next_order_++;
    const char side_char = side == Side::kBuy ? 'B' : 'S';
    if (rng_.percent(10)) {
      itch::AddOrderMpid m{};
      m.order_ref = id; m.side = side_char; m.shares = shares; m.stock = inst.symbol; m.price = price;
      m.attribution = itch::alpha<4>(rng_.percent(50) ? "SYNA" : "SYNB");
      emit(m, inst.locate);
    } else {
      itch::AddOrder m{};
      m.order_ref = id; m.side = side_char; m.shares = shares; m.stock = inst.symbol; m.price = price;
      emit(m, inst.locate);
    }
    expect_ok(market_.add(inst.locate, id, side, price, shares));
    track_add(inst, id);
  }

  void execute(Instrument& inst, bool with_price) {
    const Side side = rng_.percent(50) ? Side::kBuy : Side::kSell;
    auto best = market_.best(inst.locate, side);
    if (!best) return add(inst);
    const book::OrderEntry front = market_.queue(inst.locate, side, best->price).front();
    const Qty shares = rng_.percent(50) ? front.quantity : static_cast<Qty>(1 + rng_.below(front.quantity));
    if (with_price) {
      itch::OrderExecutedWithPrice m{};
      m.order_ref = front.id; m.executed_shares = shares; m.match_number = next_match_++;
      m.printable = rng_.percent(50) ? 'Y' : 'N';
      m.execution_price = inst.mid;
      emit(m, inst.locate);
    } else {
      itch::OrderExecuted m{};
      m.order_ref = front.id; m.executed_shares = shares; m.match_number = next_match_++;
      emit(m, inst.locate);
    }
    expect_ok(market_.reduce(inst.locate, front.id, shares));
    if (shares == front.quantity) track_remove(inst, front.id);
  }

  void cancel(Instrument& inst) {
    const OrderId id = inst.live[pick_live(inst)];
    const Qty remaining = market_.order(id)->quantity;
    if (remaining == 1) return remove(inst);
    itch::OrderCancel m{};
    m.order_ref = id;
    m.cancelled_shares = static_cast<Qty>(1 + rng_.below(remaining - 1));
    emit(m, inst.locate);
    expect_ok(market_.reduce(inst.locate, id, m.cancelled_shares));
  }

  void remove(Instrument& inst) {
    const OrderId id = inst.live[pick_live(inst)];
    itch::OrderDelete m{};
    m.order_ref = id;
    emit(m, inst.locate);
    expect_ok(market_.remove(inst.locate, id));
    track_remove(inst, id);
  }

  void replace(Instrument& inst) {
    const OrderId old_id = inst.live[pick_live(inst)];
    const auto old = *market_.order(old_id);
    itch::OrderReplace m{};
    m.original_order_ref = old_id;
    m.new_order_ref = next_order_++;
    m.shares = random_shares();
    m.price = passive_price(inst, old.side);
    emit(m, inst.locate);
    expect_ok(market_.replace(inst.locate, old_id, m.new_order_ref, m.price, m.shares));
    track_remove(inst, old_id);
    track_add(inst, m.new_order_ref);
  }

  void hidden_trade(Instrument& inst) {
    itch::Trade m{};
    m.side = 'B';  // the spec fixes this to 'B' since 2014; order_ref is zero since 2010
    m.shares = random_shares();
    m.stock = inst.symbol;
    m.price = inst.mid;
    m.match_number = next_match_++;
    emit(m, inst.locate);
  }

  void step() {
    Instrument& inst = pick_instrument();
    const auto roll = rng_.below(100);
    const bool has_orders = !inst.live.empty();
    if (roll < 45 || !has_orders) return add(inst);
    if (roll < 67) return remove(inst);
    if (roll < 75) return cancel(inst);
    if (roll < 85) return replace(inst);
    if (roll < 93) return execute(inst, false);
    if (roll < 96) return execute(inst, true);
    hidden_trade(inst);
  }

  void system_event(char code, std::uint64_t at_ns) {
    now_ns_ = std::max(now_ns_, at_ns);
    itch::SystemEvent m{};
    m.event_code = code;
    emit(m, 0);
  }

  void cross(Instrument& inst, char type) {
    itch::CrossTrade m{};
    m.shares = rng_.percent(10) ? 0 : 100 * (1 + rng_.below(500));  // the spec allows zero-share crosses
    m.stock = inst.symbol;
    m.cross_price = inst.mid;
    m.match_number = next_match_++;
    m.cross_type = type;
    emit(m, inst.locate);
  }

  void noii(Instrument& inst, char cross_type) {
    itch::Noii m{};
    m.paired_shares = 100 * rng_.below(1000);
    m.imbalance_shares = 100 * rng_.below(100);
    m.imbalance_direction = "BSNO"[rng_.below(4)];
    m.stock = inst.symbol;
    m.far_price = m.near_price = m.current_reference_price = inst.mid;
    m.cross_type = cross_type;
    m.price_variation_indicator = 'L';
    emit(m, inst.locate);
  }

  void open_session() {
    system_event('O', 3 * kNsPerHour);
    for (std::size_t i = 0; i < config_.symbols; ++i) {
      const std::string number = std::to_string(i + 1);  // at most 5 digits (symbols <= 10000)
      const std::string name = "SYN" + std::string(5 - number.size(), '0') + number;
      Instrument inst{static_cast<Locate>(i + 1), itch::alpha<8>(name),
                      static_cast<Price>((10 + rng_.below(490)) * 10'000), {}};
      itch::StockDirectory d{};
      d.stock = inst.symbol;
      d.market_category = 'Q';
      d.financial_status_indicator = 'N';
      d.round_lot_size = 100;
      d.round_lots_only = 'N';
      d.issue_classification = 'C';
      d.issue_sub_type = itch::alpha<2>("Z");
      d.authenticity = 'T';  // test, not live/production
      d.short_sale_threshold_indicator = 'N';
      d.ipo_flag = 'N';
      d.luld_reference_price_tier = '1';
      d.etp_flag = 'N';
      d.inverse_indicator = 'N';
      emit(d, inst.locate);
      itch::RegShoRestriction y{};
      y.stock = inst.symbol;
      y.reg_sho_action = '0';
      emit(y, inst.locate);
      instruments_.push_back(std::move(inst));
    }
    Instrument& first = instruments_.front();

    itch::MarketParticipantPosition l{};
    l.mpid = itch::alpha<4>("SYNA"); l.stock = first.symbol;
    l.primary_market_maker = 'N'; l.market_maker_mode = 'N'; l.market_participant_state = 'A';
    emit(l, first.locate);
    itch::MwcbDeclineLevel v{};
    v.level1 = 5'000'00000000ULL; v.level2 = 4'700'00000000ULL; v.level3 = 4'200'00000000ULL;
    emit(v, 0);
    itch::IpoQuotingPeriodUpdate k{};
    k.stock = first.symbol; k.ipo_quotation_release_time = 11 * 3600; k.ipo_quotation_release_qualifier = 'A'; k.ipo_price = first.mid;
    emit(k, 0);  // the spec documents Stock Locate as always 0 for this message
    itch::RetailPriceImprovement n{};
    n.stock = first.symbol; n.interest_flag = 'N';
    emit(n, first.locate);
    itch::DlcrPriceDiscovery o{};
    o.stock = first.symbol; o.open_eligibility_status = 'N';
    o.minimum_allowable_price = first.mid - 10 * kTick; o.maximum_allowable_price = first.mid + 10 * kTick;
    o.near_execution_price = first.mid; o.near_execution_time = now_ns_;
    o.lower_price_range_collar = first.mid - 5 * kTick; o.upper_price_range_collar = first.mid + 5 * kTick;
    emit(o, first.locate);

    system_event('S', 4 * kNsPerHour);
    for (Instrument& inst : instruments_) {
      itch::StockTradingAction h{};
      h.stock = inst.symbol; h.trading_state = 'T'; h.reserved = ' '; h.reason = itch::alpha<4>("");
      emit(h, inst.locate);
    }
    for (std::size_t i = 0; i < 4 * config_.symbols; ++i) add(pick_instrument());
    for (Instrument& inst : instruments_) noii(inst, 'O');
    system_event('Q', 9 * kNsPerHour + kNsPerHour / 2);
    for (Instrument& inst : instruments_) cross(inst, 'O');

    // One pause/resume cycle so collar, halt and MWCB status messages appear in every fixture.
    itch::LuldAuctionCollar j{};
    j.stock = first.symbol; j.auction_collar_reference_price = first.mid;
    j.upper_auction_collar_price = first.mid + 50 * kTick; j.lower_auction_collar_price = first.mid - 50 * kTick;
    emit(j, first.locate);
    itch::OperationalHalt halt{};
    halt.stock = first.symbol; halt.market_code = 'Q'; halt.operational_halt_action = 'T';
    emit(halt, first.locate);
    itch::MwcbStatus w{};
    w.breached_level = '1';
    emit(w, 0);
  }

  void close_session() {
    system_event('M', 16 * kNsPerHour);
    for (Instrument& inst : instruments_) noii(inst, 'C');
    for (Instrument& inst : instruments_) cross(inst, 'C');
    itch::BrokenTrade b{};
    b.match_number = 1;
    emit(b, instruments_.front().locate);
    system_event('E', 20 * kNsPerHour);
    system_event('C', 20 * kNsPerHour + 5'000'000'000ULL);
  }

  Config config_;
  Rng rng_;
  book::ReferenceMarket market_;
  std::vector<Instrument> instruments_;
  std::unordered_map<OrderId, std::size_t> live_index_;
  std::vector<std::uint8_t> out_;
  std::size_t emitted_ = 0;
  std::size_t continuous_target_ = 0;
  std::uint64_t now_ns_ = 0;
  OrderId next_order_ = 1;
  std::uint64_t next_match_ = 1;
};

}  // namespace

std::vector<std::uint8_t> generate(const Config& config) { return Generator(config).run(); }

}  // namespace mde::synth
