# Status

Last updated: 2026-11-02

## Done

- Protocol analysis of ITCH 5.0 and MoldUDP64 (`docs/DESIGN.md`).
- CMake build with presets (`dev`, `release`, `asan`, `tsan`), plus Linux CI with GCC 14 and
  clang 18, each in Release and ASan+UBSan.
- ITCH 5.0 decoder and encoder for all 23 message types. Wire sizes are checked against the
  spec at compile time.
- `mde decode` CLI.
- Conformance: golden vectors for all 23 types; exhaustive length, type-byte and enum rejection
  tests; seeded mutation fuzz in ctest; libFuzzer target in CI (11.4M execs / 60 s locally, no
  crashes).
- Reference order book (map/list/hash), ITCH book builder with separate volume statistics,
  invariant checker, synthetic generator with cross-platform byte-identical output, `mde gen`,
  `mde book --check`.

## Remaining (acceptance criteria)

| # | Milestone | Done when |
|---|---|---|
| 5 | Sequencing and replay | MoldUDP64 parser with golden vectors; sequencer handles gap/dup/reorder/heartbeat/end-of-session; lossy replay recovers to the same book digest as lossless; UDP loopback test with re-requests |
| 6 | Multithreaded pipeline | SPSC ring and mutex baseline; threaded replay digest equals single-threaded; TSan job in CI |
| 7 | Simulated trading | matching engine (price-time), cancels, execution reports, positions, max qty/notional/position checks; deterministic digest test |
| 8 | Performance | optimized book matches reference under differential tests; benchmarks for decode, book, queue, replay, e2e, order entry with p50/p95/p99/max |
| 9 | Stress and failure | high/burst loss, reorder beyond buffer, re-request server down, malformed packets mixed in, sustained backpressure |
| 10 | Docs and release | README, design, benchmark report; fresh-clone build verified; CI green |

## Latest test results

76/76 pass on macOS (Apple clang 21: Debug, ASan+UBSan) and Linux in Docker (GCC 14, clang 18:
Release and Debug ASan+UBSan, `-Werror`).

## Known issues

None open.

## Next step

Milestone 5: MoldUDP64 packets, sequencer, lossy replay harness, UDP transport.
