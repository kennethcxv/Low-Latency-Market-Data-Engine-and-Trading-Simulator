# Engineering Journal

A dated record of decisions, problems and measurements as they happened. Entries are not edited
after the fact except to fix typos; later corrections are new entries.

This project is developed by Kenneth Camacho with AI assistance (Claude, via Claude Code), which
wrote much of the code and documentation under review. Commits carry a `Co-Authored-By` trailer.

---

## 2026-11-02 — Milestone 1: environment and protocol analysis

**Environment.** Apple M4 Max (12 performance + 4 efficiency cores), macOS 27.0.1, Apple clang
21.0.0, CMake 4.4.3, Ninja. Docker 29.8 is available, so Linux (GCC, ThreadSanitizer, libFuzzer)
can be exercised locally before pushing.

**Specifications.** Downloaded both official PDFs from nasdaqtrader.com. The ITCH PDF is 36
pages; the local PDF renderer was missing, so text was extracted with `pypdf` in a throwaway
virtualenv. Every message length in the design doc's table was computed by summing the field
lengths in the spec's tables, not copied from memory. These sums are also enforced in the code
with `static_assert`s.

Spec details that changed the design:

- Order reference numbers lost their "increasing" guarantee in 2009, so they can't be used as
  direct array indices. The order index has to be a hash table.
- The `P` trade message is for non-displayed orders and must never touch the book.
- A `C` execution with Printable=`N` must be excluded from volume.
- The book can legitimately be crossed before the opening cross. "Not crossed" is therefore not
  an invariant.

**Hardware facts measured, not assumed.**

- `sysctl hw.cachelinesize` reports **128 bytes** on this machine, not 64. Padding for
  false-sharing avoidance must be at least 128 bytes on Apple Silicon.
- `steady_clock` on macOS/arm64 is backed by a 24 MHz counter (`mach_timebase_info` = 125/3).
  The smallest non-zero difference between two reads is **41 ns**. A pair of reads costs about
  29 ns on average. Timing a single book update, which is expected to take tens of nanoseconds,
  is therefore meaningless on this machine. Sub-100 ns operations will be timed in batches, and
  the benchmark will print the measured clock resolution so that readers can judge.
- Apple clang ships no libFuzzer runtime (`-fsanitize=fuzzer`). The fuzz targets will be built and
  run on Linux with upstream clang, both in CI and in Docker locally.

**Repository.** The user supplied the target repository
(`kennethcxv/Low-Latency-Market-Data-Engine-and-Trading-Simulator`, public) and asked for commits
and pushes there. That differs from the name in the original brief (`market-data-engine`). The
user's explicit instruction was followed.

**Process change (requested mid-milestone).** From milestone 2 onward, each milestone ends
with a review by the user. Pushes to GitHub happen only after the user authorizes them.
Milestone 1 was committed locally and had not been pushed when this was agreed.

## 2026-11-02 — Milestone 2: build system and ITCH decoder

**Decoder design.** Each message struct declares its fields in wire order with a
`MDE_WIRE_FIELDS(...)` macro. The macro yields a `std::tie` of the members. One generic
routine walks that tuple to decode, and another walks it to encode. The alternative
considered was 23 hand-written decode functions and 23 encode functions. The generic version
has two concrete advantages:

- The wire size of each struct is computed at compile time and `static_assert`ed against the
  length taken from the spec table. A wrong field width or a missing field fails the build.
- There is one decode path to review instead of 46 near-identical functions.

The cost is a little template code (`detail::width`, `get`, `put`) that a reader has to
understand once.

**Checked, not assumed:** at `-O2` (Apple clang 21, arm64), `read<AddOrder>` compiles to
straight-line code. Each field becomes a constant-offset load, a `rev` byte swap and a store,
with no loop and no call. That is the same code a hand-written decoder would produce.

**Validation policy.**

- Lengths must match exactly. A message that is too long is rejected, not truncated.
- Only fields that change book or volume semantics are validated: Buy/Sell must be `B` or
  `S`, and Printable must be `Y` or `N`.
- Every rejection is a `DecodeStatus` value. Nothing throws on the data path.

**Problems hit.**

- GoogleTest's CMake project enables C, so Linux builds need `CMAKE_C_COMPILER` set alongside
  the C++ compiler. CI passes both.
- Apple clang 21 emits `-Wcharacter-conversion` inside GoogleTest's own `gtest-printers.h`.
  The warning comes from googletest's translation units, not ours. `-Werror` applies only to
  project targets, so it doesn't break the build.

**Tests run.** 32 tests: byte order, framing, a hand-checked AddOrder vector with distinct
bytes per field, 2,000 random encode/read round trips per message type (23 types), and basic
rejections. All pass in these configurations:

- macOS (Apple clang 21): Debug, and ASan+UBSan.
- Ubuntu 24.04 in Docker: GCC 14 and clang 18, each plain and with ASan+UBSan, all with
  `-Werror`.

## 2026-11-02 — Milestone 3: protocol conformance

**Golden vectors.** There is one vector per message type, all 23, laid out field by field from
the spec's offset tables. A small script outside the repo derived them. It asserts that each
field's offset equals the running sum of the previous widths and that the total matches the
spec length. In every vector, each integer byte holds its own offset. A field read from the
wrong place or with the wrong width therefore decodes to a visibly different number.

To check that the tests can actually fail, I temporarily swapped the two same-width LULD collar
fields in the message definition. The size `static_assert` cannot catch that. The golden test
failed on exactly those two fields, as intended, and the change was reverted.

**Malformed input.** Buffers are allocated at their exact size, so ASan turns any over-read into
a crash. Coverage:

- Every wrong length from 0 to `kSize+8` for every type.
- All 233 unassigned type bytes.
- All 254 invalid values of each validated enumerated field.
- A seeded mutation test over a 400-message framed stream, with 3,000 mutated copies. Mutations
  are byte replacement, bit flips, insertions and truncation.

The property checked is that an accepted message re-encodes to exactly its input bytes. The
test also asserts that every decode outcome was actually produced at least once, so it can't
pass vacuously.

**libFuzzer** (`fuzz/fuzz_itch.cpp`, clang 18, ASan+UBSan, Linux in Docker): 11,465,468
executions in 60 s, 219 coverage edges, no crashes. CI now runs each fuzz target for 60 s.

**Results.** 59/59 tests pass on macOS in Debug and in ASan+UBSan.

## 2026-11-02 — Milestone 4: reference order book

**Reference market.** Built from `std::map<Price, Level>` per side, a `std::list` FIFO queue
per level, and an `unordered_map` from order id to a record that holds the list iterator.
It is intentionally untuned and serves as the oracle for the later optimized book.

All markets share one policy for input that contradicts the book state. The policy lives in
`book::Result`:

- duplicate add, unknown order, locate mismatch and zero quantity: rejected, with no side
  effects;
- reduce by more than the order's remaining size: the order is removed and the event is
  counted as `kOverfill`.

Crossed books are represented, not rejected (see milestone 1).

**Book builder.** It is a template over a `Market` concept. Book effects and volume are
handled separately: `P` and `Q` never touch the book, and a non-printable `C` reduces the
order but adds nothing to volume. When an `E` names an unknown order, its volume still counts,
because the trade happened, but the price is unknown. Broken trades are counted but don't
reverse volume. Reversing would need a match-number index over every execution of the day,
which I judged not worth the memory for this project.

**Testing against a second oracle.** The reference market is checked against an even simpler
model: a flat `std::map` of orders with arrival counters, from which levels and queues are
recomputed by sorting. The test runs 20,000 random operations (a third deliberately invalid)
in a narrow price band, comparing results, depth, queues and invariants after every step.

**Synthetic generator.** It tracks the state it generates using the reference market. Every
modify therefore targets a live order, executions hit the front of the best level, and books
never cross. Each fixture contains all 23 message types. The generator uses SplitMix64 rather
than `<random>` distributions. A test pins the generated bytes and the resulting book digest.
That test passes on macOS with libc++ and on Linux with libstdc++ (GCC 14 and clang 18), so
fixtures are byte-identical across platforms.

**Bug found by GCC.** GCC 14 `-Werror=format-truncation` rejected `snprintf("SYN%05zu")` into
a 9-byte buffer, because it cannot prove the symbol index stays below 100,000. Replaced with
explicit zero padding.

**Test slowdown diagnosed.** Under GCC 14 with ASan, the synthetic-stream test exceeded 10
minutes, against 1.1 s without ASan. I measured the property test with
`detect_stack_use_after_return` toggled: 119 ms when off, 36 s when on. That flag alone
caused the ~300x slowdown. Clang 18's ASan with the same check ran normally. The test build
now disables that one check for GCC+ASan only. Clang ASan in CI still covers stack use after
return.

**Correctness mode cost.** `mde book --check` validates the touched instrument's entire book
after every event. On a 200k-message fixture (16 symbols, ~2,000 resting orders per book) that
took 33 s in a Debug build. It is intended for correctness runs, not benchmarks.

**Results.** 76/76 tests pass on macOS (Debug, ASan+UBSan) and Linux (GCC 14 and clang 18,
Release and Debug ASan+UBSan, all with `-Werror`).

## 2026-11-02 — Milestone 5: MoldUDP64 sequencing and replay

**Packet validation.** `parse()` walks every message block before it reports success. A packet
with a truncated block, trailing bytes, a payload on a heartbeat, or a sequence+count that
overflows 64 bits is rejected whole and treated like a lost packet.

**Sequencer.** It is a pure state machine: datagrams and timestamps go in, in-order messages
and re-requests come out. Packets after a hole are buffered (bounded at 4,096 by default).
Requests cover only the hole up to the first buffered packet and are repeated after a timeout.
Any progress resets the retry budget. A hole that cannot be filled is skipped and counted in
`messages_lost`.

**Design bug found by exercising the CLI.** The first version had two related flaws, both
visible in `mde replay --drop 0.01 --no-retransmit` on a 200k-message fixture:

1. `gaps_detected` counted periods during which a hole was open, so it reported 1 for about
   48 lost packets. It now counts holes: each time the stream jumps past the highest sequence
   seen.
2. With re-requests disabled, a hole's timeout only started once it reached the head of the
   line. Consecutive holes were therefore skipped one timeout after another. At 100k packets
   per second with a 20 ms timeout, the 4,096-packet reorder buffer overflowed: 39 overflows
   and 3,670 messages lost from roughly 1,900 actually dropped.

The fix measures each hole's timeout from the moment it was revealed, i.e. the arrival of the
packet after it. That made `on_packet` take a receive timestamp. `on_timer` now skips every
expired hole in one call. After the fix, the same run lost 2,101 messages, about the number
dropped, with 0 overflows and 51 holes. A unit test pins the behaviour of holes ageing in
parallel.

**Results, all on the 200k-message synthetic fixture:**

- `mde replay`, 2% drop, 1% duplicate, 2% reorder: 173 holes, 111 requests, 0 lost. The final
  book digest equals direct replay (`55edd1085deb3610`).
- `mde serve` + `mde listen` over real loopback UDP with 2% deliberate drops: 82 dropped, 82
  re-requested and answered, 0 lost, same digest.
- Multicast `239.255.0.1` over `lo0` (`--interface 127.0.0.1` on both ends): 45 dropped, 45
  recovered, same digest. Multicast via the default interface (`en0`, Wi-Fi) delivered nothing
  on this Mac. That is consistent with macOS's Local Network privacy restrictions, but I did
  not confirm the cause. Local multicast tests on macOS therefore use the loopback interface.
- Without retransmission, at 1% drop, the book stays structurally valid: invariants held after
  all 189,891 book events, with 1,036 unknown-order anomalies from the lost adds.
- libFuzzer, 60 s each: MoldUDP64 target 13.5M executions and ITCH target 16.7M, no crashes
  and no ordering violations.

**Tests.** 97/97 pass on macOS (Debug, ASan+UBSan) and Linux (GCC 14 and clang 18, Release and
ASan+UBSan). This includes a 40-seed property test of exactly-once, in-order delivery at 5–10%
loss with duplication and reordering, and a real-socket loopback test.

## 2026-11-02 — Milestone 6: multithreaded pipeline

**Shape.** The feed thread receives packets, runs the sequencer and copies each in-order
message into a 64-byte slot. A bounded queue carries the slots to the book thread, which
decodes them and updates the book. The book thread owns all book state while the pipeline
runs, and the queue is the only shared structure. When the queue is full, the producer spins
and counts it. Nothing is dropped in-process; the kernel socket buffer and MoldUDP64
re-requests handle overload upstream.

**SPSC ring.**

- Indices increase monotonically and are masked to address slots.
- The release store of `tail_` publishes a slot to the consumer. The release store of `head_`
  returns it to the producer.
- Each side caches the other's index.
- Producer-owned and consumer-owned fields sit on separate 128-byte lines.

The baseline is a bounded `std::deque` under a `std::mutex` with the same `try_push`/`try_pop`
interface. Whether the ring is worth having is a benchmark question for milestone 8, not
something assumed here.

**Checking the checker.** I temporarily weakened the publish/acquire pair on `tail_` to
`relaxed` and ran the suite under TSan (macOS, Apple clang 21).
`QueueTest/0.TwoThreadsPreserveOrderUnderContention` (8-byte payload) reported a data race in
`try_pop`. The test that moves 64-byte slots did **not** report one on its own, although a
standalone program with the same queue and a 64-byte payload did, at both `-O1` and `-O2`.
I didn't find out why, so I'm not claiming that test detects ordering bugs. The suite as a
whole, which CI runs, does catch this one.

**Results.**

- 105/105 tests pass under TSan on macOS (Apple clang 21) and Linux (clang 18 and GCC 14), and
  under Debug and ASan on macOS.
- `mde replay` at 2% drop and 2% reorder gives the same digest (`55edd1085deb3610`) inline,
  with the SPSC pipeline and with the mutex pipeline.
- `mde listen` with the SPSC pipeline over real UDP, 1% deliberate drop: 0 lost, same digest.

**CI.** Added a TSan job with `vm.mmap_rnd_bits=28`. GitHub's Ubuntu 24.04 runners use 32-bit
mmap randomisation, which is known to break TSan's shadow mapping. The Docker kernel here uses
18 bits, so I could not reproduce that issue locally.

## 2026-11-02 — Milestone 7: simulated order entry, risk and matching

`mde_sim` depends only on `mde_common`. It shares no types with the ITCH side: ITCH is an
outbound market-data feed, and nothing here pretends otherwise.

**Request flow.** `Exchange::submit` runs the pre-trade checks in this order:

1. instrument exists
2. price > 0
3. quantity > 0
4. client order id not used before by that account
5. quantity ≤ max order quantity
6. notional ≤ max notional, computed as `price > max / quantity` so the product cannot overflow
7. worst-case position: `net + open buys + new buy ≤ limit` (and the mirror for sells)

Accepted orders match in price-time priority at the resting order's price and never trade
through the limit. Every request produces execution reports for both sides of each fill, with
cumulative and leaves quantities, from one exchange-wide counter. Cancels release open
quantity; cancelling a non-live client id yields `CancelRejected`.

**Policy choices:**

- Self-trades are allowed. Real venues usually offer self-trade prevention; it isn't implemented.
- Only GTC limit orders are supported: no market, IOC or amend.
- Positions are per account and instrument, with no cross-instrument netting.

**Tests.**

- Unit tests cover priority and trade-through, report contents for both sides, and every reject
  reason. A notional of `INT64_MAX * 2` is rejected without overflowing, and an order exactly
  at the notional limit is accepted.
- The simulator property test (50,000 steps, position limit 2,000) checks four things:
  - net positions sum to zero for every instrument;
  - each account's open buy/sell matches the leaves quantities rebuilt independently from the
    reports;
  - the worst-case position bound holds for every account;
  - every reject reason was produced at least once.
- The report digest is pinned and reproduced on macOS (libc++) and Linux (libstdc++).

**Bug caught before running.** Simulator requests meant to exceed the notional limit used
`price = max_notional`. With quantity 1 that is exactly at the limit and is accepted. The value
became `max_notional / quantity + 1`.

**Results.** 112/112 tests pass on macOS (Debug) and on Linux in five configurations: GCC 14
and clang 18 in Release and ASan+UBSan, plus clang 18 TSan.
