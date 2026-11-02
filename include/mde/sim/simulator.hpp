// Deterministic order-flow simulation against the Exchange: seeded agents submit passive and
// aggressive limit orders, cancel their own live orders, and occasionally send invalid or
// oversized requests so every risk check is exercised.
#pragma once

#include <array>
#include <cstdint>
#include <variant>
#include <vector>

#include "mde/common.hpp"
#include "mde/sim/exchange.hpp"

namespace mde::sim {

struct SimConfig {
  std::uint64_t seed = 1;
  std::size_t steps = 100'000;  // one request (new order or cancel) per step
  std::size_t accounts = 8;
  std::size_t instruments = 4;
  RiskLimits limits;
};

struct SimSummary {
  std::uint64_t new_orders = 0;
  std::uint64_t cancels = 0;
  std::uint64_t reports = 0;
  std::uint64_t fills = 0;  // fill and partial-fill reports (two per trade)
  std::int64_t traded_quantity = 0;
  std::array<std::uint64_t, kRejectReasonCount> rejects{};
  std::uint64_t digest = 0;  // over every execution report, in order
};

// The simulator's request stream. Feed every execution report back through observe() so that
// cancels target orders the agent believes are live.
class OrderFlow {
 public:
  using Request = std::variant<NewOrder, CancelOrder>;
  explicit OrderFlow(const SimConfig& config);
  Request next();
  void observe(const ExecutionReport& report);

 private:
  void forget(AccountId account, ClientOrderId id);

  SimConfig config_;
  Rng rng_;
  std::vector<Price> reference_;
  std::vector<ClientOrderId> next_client_id_;
  std::vector<std::vector<ClientOrderId>> live_;  // client ids believed live, per account
};

// Runs the simulation. If `reports` is given, every execution report is appended to it.
SimSummary simulate(const SimConfig& config, Exchange& exchange, std::vector<ExecutionReport>* reports = nullptr);

}  // namespace mde::sim
