#include "mde/sim/simulator.hpp"

#include <algorithm>

namespace mde::sim {
namespace {

void hash_report(Digest& d, const ExecutionReport& r) {
  d.add(r.exec_id);
  d.add(r.account);
  d.add(r.client_id);
  d.add(r.order_id);
  d.add(r.instrument);
  d.add(static_cast<std::uint64_t>(r.side));
  d.add(static_cast<std::uint64_t>(r.type));
  d.add(static_cast<std::uint64_t>(r.reason));
  d.add(static_cast<std::uint64_t>(r.last_price));
  d.add(static_cast<std::uint64_t>(r.last_quantity));
  d.add(static_cast<std::uint64_t>(r.leaves_quantity));
  d.add(static_cast<std::uint64_t>(r.cumulative_quantity));
}

}  // namespace

OrderFlow::OrderFlow(const SimConfig& config)
    : config_(config),
      rng_(config.seed),
      reference_(config.instruments, 10'000),
      next_client_id_(config.accounts, 1),
      live_(config.accounts) {}

OrderFlow::Request OrderFlow::next() {
  const auto account = static_cast<AccountId>(rng_.below(config_.accounts));
  const auto instrument = static_cast<InstrumentId>(rng_.below(config_.instruments));
  if (rng_.percent(3)) reference_[instrument] = std::max<Price>(100, reference_[instrument] + (rng_.percent(50) ? 1 : -1));

  if (rng_.below(100) >= 60 && !live_[account].empty()) {
    CancelOrder c;
    c.account = account;
    c.client_id = rng_.percent(5) ? next_client_id_[account] + 1000 : live_[account][rng_.below(live_[account].size())];
    return c;
  }

  NewOrder o;
  o.account = account;
  o.instrument = instrument;
  o.side = rng_.percent(50) ? Side::kBuy : Side::kSell;
  const auto offset = static_cast<Price>(rng_.below(10));
  const bool aggressive = rng_.percent(20);
  o.price = reference_[instrument] + ((o.side == Side::kBuy) == aggressive ? offset : -offset);
  o.quantity = static_cast<Qty>(1 + rng_.below(200));
  o.client_id = next_client_id_[account]++;
  // Deliberately bad requests, to exercise every pre-trade check.
  const auto mischief = rng_.below(1000);
  if (mischief < 5) o.client_id = std::max<ClientOrderId>(1, o.client_id - 1 - rng_.below(o.client_id));  // reuse
  else if (mischief < 10) o.quantity = config_.limits.max_order_quantity + 1;
  else if (mischief < 15) o.price = config_.limits.max_order_notional / o.quantity + 1;  // notional just over
  else if (mischief < 18) o.quantity = 0;
  else if (mischief < 21) o.price = 0;
  else if (mischief < 23) o.instrument = static_cast<InstrumentId>(config_.instruments);
  return o;
}

void OrderFlow::forget(AccountId account, ClientOrderId id) {
  auto& ids = live_[account];
  if (auto it = std::find(ids.begin(), ids.end(), id); it != ids.end()) {
    *it = ids.back();
    ids.pop_back();
  }
}

void OrderFlow::observe(const ExecutionReport& r) {
  if (r.type == ExecType::kNew) live_[r.account].push_back(r.client_id);
  if (r.type == ExecType::kFill || r.type == ExecType::kCanceled) forget(r.account, r.client_id);
}

SimSummary simulate(const SimConfig& config, Exchange& exchange, std::vector<ExecutionReport>* reports) {
  OrderFlow flow(config);
  SimSummary summary;
  Digest digest;
  std::vector<ExecutionReport> batch;
  for (std::size_t step = 0; step < config.steps; ++step) {
    batch.clear();
    const auto request = flow.next();
    if (const auto* order = std::get_if<NewOrder>(&request)) {
      exchange.submit(*order, batch);
      ++summary.new_orders;
    } else {
      exchange.cancel(std::get<CancelOrder>(request), batch);
      ++summary.cancels;
    }
    for (const ExecutionReport& r : batch) {
      flow.observe(r);
      hash_report(digest, r);
      ++summary.reports;
      if (r.type == ExecType::kRejected || r.type == ExecType::kCancelRejected) {
        ++summary.rejects[static_cast<std::size_t>(r.reason)];
      } else if (r.type == ExecType::kFill || r.type == ExecType::kPartialFill) {
        ++summary.fills;
        summary.traded_quantity += r.last_quantity;
      }
    }
    if (reports) reports->insert(reports->end(), batch.begin(), batch.end());
  }
  summary.traded_quantity /= 2;  // each trade is reported to both sides
  summary.digest = digest.value();
  return summary;
}

}  // namespace mde::sim
