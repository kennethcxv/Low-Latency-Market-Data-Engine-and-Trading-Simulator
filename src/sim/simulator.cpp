#include "mde/sim/simulator.hpp"

#include <algorithm>

#include "mde/common.hpp"

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

SimSummary simulate(const SimConfig& config, Exchange& exchange, std::vector<ExecutionReport>* reports) {
  Rng rng(config.seed);
  SimSummary summary;
  Digest digest;
  std::vector<Price> reference(config.instruments, 10'000);
  std::vector<ClientOrderId> next_client_id(config.accounts, 1);
  std::vector<std::vector<ClientOrderId>> live(config.accounts);  // client ids believed live, per account
  std::vector<ExecutionReport> batch;

  auto forget = [&](AccountId account, ClientOrderId id) {
    auto& ids = live[account];
    if (auto it = std::find(ids.begin(), ids.end(), id); it != ids.end()) {
      *it = ids.back();
      ids.pop_back();
    }
  };

  for (std::size_t step = 0; step < config.steps; ++step) {
    const auto account = static_cast<AccountId>(rng.below(config.accounts));
    const auto instrument = static_cast<InstrumentId>(rng.below(config.instruments));
    if (rng.percent(3)) reference[instrument] = std::max<Price>(100, reference[instrument] + (rng.percent(50) ? 1 : -1));

    batch.clear();
    const auto roll = rng.below(100);
    if (roll < 60 || live[account].empty()) {
      NewOrder o;
      o.account = account;
      o.instrument = instrument;
      o.side = rng.percent(50) ? Side::kBuy : Side::kSell;
      const auto offset = static_cast<Price>(rng.below(10));
      const bool aggressive = rng.percent(20);
      const bool buy = o.side == Side::kBuy;
      o.price = reference[instrument] + ((buy == aggressive) ? offset : -offset);
      o.quantity = static_cast<Qty>(1 + rng.below(200));
      o.client_id = next_client_id[account]++;
      // Deliberately bad requests, to exercise every pre-trade check.
      const auto mischief = rng.below(1000);
      if (mischief < 5) o.client_id = std::max<ClientOrderId>(1, o.client_id - 1 - rng.below(o.client_id));  // reuse
      else if (mischief < 10) o.quantity = config.limits.max_order_quantity + 1;
      else if (mischief < 15) o.price = config.limits.max_order_notional / o.quantity + 1;  // notional just over
      else if (mischief < 18) o.quantity = 0;
      else if (mischief < 21) o.price = 0;
      else if (mischief < 23) o.instrument = static_cast<InstrumentId>(config.instruments);
      exchange.submit(o, batch);
      ++summary.new_orders;
    } else {
      CancelOrder c;
      c.account = account;
      c.client_id = rng.percent(5) ? next_client_id[account] + 1000 : live[account][rng.below(live[account].size())];
      exchange.cancel(c, batch);
      ++summary.cancels;
    }

    for (const ExecutionReport& r : batch) {
      hash_report(digest, r);
      ++summary.reports;
      switch (r.type) {
        case ExecType::kNew: live[r.account].push_back(r.client_id); break;
        case ExecType::kRejected:
        case ExecType::kCancelRejected: ++summary.rejects[static_cast<std::size_t>(r.reason)]; break;
        case ExecType::kPartialFill: ++summary.fills; summary.traded_quantity += r.last_quantity; break;
        case ExecType::kFill:
          ++summary.fills;
          summary.traded_quantity += r.last_quantity;
          forget(r.account, r.client_id);
          break;
        case ExecType::kCanceled: forget(r.account, r.client_id); break;
      }
    }
    if (reports) reports->insert(reports->end(), batch.begin(), batch.end());
  }
  summary.traded_quantity /= 2;  // each trade is reported to both sides
  summary.digest = digest.value();
  return summary;
}

}  // namespace mde::sim
