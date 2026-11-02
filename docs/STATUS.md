# Status

Last updated: 2026-11-02

## Done

- Protocol analysis of ITCH 5.0 and MoldUDP64 (`docs/DESIGN.md`).
- CMake presets (`dev`, `release`, `asan`, `tsan`, `fuzz`).
- GitHub Actions CI:
  - GCC 14 and clang 18, each in Release and in ASan+UBSan;
  - clang 18 under TSan;
  - 60 s of fuzzing per target.
- ITCH 5.0 codec for all 23 message types, with sizes checked against the spec at compile time.
- Golden vectors, malformed-input tests, mutation fuzzing in ctest, and libFuzzer targets.
- Order books:
  - a reference book;
  - an optimized book: order pool, open-addressing index, contiguous levels;
  - `BookBuilder` with separate volume statistics;
  - invariant checkers;
  - typed, oracle and lockstep differential tests.
- MoldUDP64:
  - whole-packet validation;
  - sequencer covering gaps, duplicates, reordering, heartbeats, end of session, a bounded
    buffer, and retry-then-skip;
  - offline lossy replay;
  - UDP unicast and multicast publisher and receiver.
- Two-thread pipeline: SPSC ring or mutex queue, with backpressure.
- Simulated exchange: pre-trade risk checks, price-time matching, execution reports, positions,
  a deterministic simulator.
- Stress and failure tests, including real kernel drops on a tiny socket buffer.
- Benchmark harness and `scripts/bench.sh`. The reference book was profiled before the optimized
  book was written. Recorded reports are in `bench/results/`.
- CLI commands: `mde decode | gen | book | replay | serve | listen | sim`.
- Release checks: a fresh clone builds and passes the tests, every README example was run, and
  two independent code reviews were triaged; the fixes are in (see JOURNAL, milestone 10).

## Remaining

Nothing required by the brief is open. Possible next steps:

- batch the feed-to-book handoff per packet;
- a deep-book benchmark (binary versus linear level search);
- Linux benchmarks on dedicated hardware with real pinning;
- tune the simulated exchange.

## Latest test results

131/131 pass:

- macOS (Apple clang 21): Debug, ASan+UBSan, TSan, Release.
- Linux in Docker (GCC 14, clang 18): Release and Debug ASan+UBSan, plus clang 18 TSan.

All builds use `-Werror`. Both fuzzers ran 60 s with no findings.

## Known issues

- Benchmarks were recorded while unrelated processes kept the machine loaded (load average about
  17–25 on 16 hardware threads). The tails are noisier than on an idle host.
- On this workload, the two-thread pipeline has lower throughput than a single thread. Batching
  the handoff per packet is the identified next step; it is not implemented.
- Multicast through the default interface did not deliver locally on macOS. Loopback multicast
  works.

## Next step

None scheduled. See "Remaining" for candidates.
