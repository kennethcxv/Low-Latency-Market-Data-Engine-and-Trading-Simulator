// Deterministic order-flow simulation against the Exchange: seeded agents submit passive and
// aggressive limit orders, cancel their own live orders, and occasionally send invalid or
// oversized requests so every risk check is exercised.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

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

// Runs the simulation. If `reports` is given, every execution report is appended to it.
SimSummary simulate(const SimConfig& config, Exchange& exchange, std::vector<ExecutionReport>* reports = nullptr);

}  // namespace mde::sim
