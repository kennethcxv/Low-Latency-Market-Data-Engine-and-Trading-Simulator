// Synthetic ITCH 5.0 data. No exchange data ships with this project; every fixture and benchmark
// workload comes from here and is labelled synthetic.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mde::synth {

struct Config {
  std::uint64_t seed = 1;
  std::size_t messages = 1'000'000;  // total messages, including session and directory messages
  std::size_t symbols = 32;
};

// Returns a framed ([u16 length][message]...) stream — the layout of Nasdaq's historical ITCH
// files. Output is a pure function of Config on every platform.
//
// The stream is internally consistent: every modify references a live order on the right
// instrument, executions hit the front of the best level, replaces keep their side, and books
// never cross. Every one of the 23 message types appears at least once. Resemblance to real flow
// is coarse: activity is concentrated near the touch, most orders are cancelled rather than
// filled, and a few instruments carry most of the traffic.
std::vector<std::uint8_t> generate(const Config& config);

}  // namespace mde::synth
