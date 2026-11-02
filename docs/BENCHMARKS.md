# Benchmarks

Every number here was measured by `bench/mde_bench.cpp`. The raw reports are in
[`bench/results/`](../bench/results). Nothing is extrapolated, and no latency target is claimed.

## Reproducing

```sh
./scripts/bench.sh            # builds the release preset, runs everything, writes bench/results/<date>-<os>-<arch>.md
./scripts/bench.sh --pin      # Linux: pin feed/book threads to CPUs 2/4. macOS: QoS USER_INTERACTIVE
./build/release/bench/mde_bench --only book --messages 5000000 --reserve 4194304
```

A full run takes a few minutes. Run it on an otherwise idle machine. The report prints the load
average at start, so you can tell whether that held.

## Methodology

**Workload.** `mde gen` with 2,000,000 messages, 64 symbols and seed 1: 66.5 MB of synthetic
ITCH 5.0, producing 1,919,224 book operations. Weighted by message count, the mix is about:

| Messages | Share |
|---|---:|
| Adds | 45% |
| Deletes | 22% |
| Replaces | 10% |
| Executions | 11% |
| Partial cancels | 8% |
| Non-displayed trades (`P`) | 4% |

Order prices cluster within a few ticks of the touch, and a few symbols carry most of the
traffic. The distributions are invented. The results describe this workload, not a real Nasdaq
day.

**Clock.** `std::chrono::steady_clock`. The harness measures its resolution and per-read cost at
startup and prints both. On the development machine the resolution is **41 ns**: Apple Silicon's
system counter runs at 24 MHz. A read costs about 15–18 ns. Consequences:

- **Stages costing tens of nanoseconds** (decode, book updates, decode+book) are timed per batch
  of 64 operations. Their percentiles are percentiles of 64-operation averages. That understates
  single-operation tails, because one slow operation is diluted by 63 fast ones. The max column is
  the slowest batch average.
- **Replay** is timed per packet and divided by the messages in that packet.
- **Order entry** is timed per request (hundreds of ns, a few clock ticks).
- **Cross-thread latencies** (queue handoff, end-to-end) are timed per message, by subtracting a
  timestamp taken on one core from one taken on another. Both read the same system counter. These
  values are quantized to 41 ns, so "84 ns" means two ticks.

**Repetitions.** Each benchmark runs once untimed (warm-up, page faults, container growth) and
then 5 timed repetitions. Throughput is the median repetition. Percentiles pool every timed
sample.

**Build.** The `release` preset: `-O3 -DNDEBUG`. Sanitizers are never enabled for benchmarks.

**Measurement boundaries:**

| Benchmark | Timed region | Excluded |
|---|---|---|
| `decode` | `FramedReader::next` + `itch::decode` into a stack struct, trivial handler | file I/O |
| `book/*` | `add`/`reduce`/`remove`/`replace` on pre-decoded operations | decoding |
| `decode+book/*` | `BookBuilder::apply`: decode, book update, volume statistics | — |
| `replay/...` | `Sequencer::on_packet` + decode + `FastMarket` for in-memory packets | socket receive |
| `queue/*` | producer `try_push` of a 64-byte slot → consumer `try_pop`, both spinning | — |
| `queue-latency/*` | one message per 1 µs; time from push to pop on the other thread | — |
| `e2e/*` | feed thread: sequencer + push; book thread: decode + `FastMarket`. Latency = packet arrival on the feed thread → that message's book update finished | socket receive, kernel |
| `orders/exchange` | `Exchange::submit` / `cancel`: risk checks, matching, execution reports | request generation |

The end-to-end benchmarks start from datagrams already in memory. The kernel UDP path,
interrupt coalescing and NIC timestamps are not part of any number here.

## Environment for the recorded runs

| | |
|---|---|
| CPU | Apple M4 Max (12 performance + 4 efficiency cores, 16 hardware threads) |
| OS | macOS 27.0.1 (Darwin 27.0.0, arm64) |
| Compiler | Apple clang 21.0.0, `-O3 -DNDEBUG` (`release` preset) |
| Clock | `steady_clock`, 41 ns resolution, 15–18 ns per read |
| Load | **Not idle.** Unrelated user processes held the 1-minute load average at 17–25 throughout. |

The machine load is the most important caveat on this page. Single-threaded throughput was stable
to within about 10% across runs. Anything involving two threads waiting on each other was not
(see *Run-to-run variation*).

## Results

Run 1 (`bench/results/2026-11-02-darwin-arm64.md`), default thread placement. A dash means the
benchmark measures throughput only. The raw report from that run prints `0.0` in those columns;
the harness now prints `-`.

| Benchmark | Throughput | p50 | p95 | p99 | max |
|---|---:|---:|---:|---:|---:|
| `decode` | 117.3 M msg/s | 7.8 ns | 9.8 ns | 11.7 ns | 25.7 µs |
| `book/reference` | 7.14 M ops/s | 127.6 ns | 196.6 ns | 470.0 ns | 187.9 µs |
| `book/fast` | 17.90 M ops/s | 50.8 ns | 78.8 ns | 120.4 ns | 35.8 µs |
| `decode+book/reference` | 7.00 M msg/s | 127.6 ns | 194.7 ns | 430.3 ns | 171.2 µs |
| `decode+book/fast` | 15.09 M msg/s | 62.5 ns | 93.1 ns | 153.0 ns | 38.9 µs |
| `replay/sequencer+book(fast)` | 14.19 M msg/s | 64.0 ns | 97.6 ns | 125.0 ns | 58.8 µs |
| `queue/mutex` | 46.3 M/s | – | – | – | – |
| `queue/spsc-naive` | 23.1 M/s | – | – | – | – |
| `queue/spsc` | 112.9 M/s | – | – | – | – |
| `queue-latency/mutex` (1M msg/s) | – | 7.67 µs | 74.8 µs | 395.9 µs | 7.5 ms |
| `queue-latency/spsc-naive` (1M msg/s) | – | 125 ns | 375 ns | 19.6 µs | 1.4 ms |
| `queue-latency/spsc` (1M msg/s) | – | 84 ns | 292 ns | 9.4 µs | 630 µs |
| `e2e/mutex` (unpaced) | 1.35 M msg/s | 12.2 ms | 15.0 ms | 18.2 ms | 27.8 ms |
| `e2e/spsc` (unpaced) | 7.69 M msg/s | 2.07 ms | 2.92 ms | 4.13 ms | 8.8 ms |
| `e2e-paced/mutex` (1M msg/s) | – | 19.7 µs | 147.2 µs | 2.65 ms | 17.2 ms |
| `e2e-paced/spsc` (1M msg/s) | – | 3.33 µs | 15.5 µs | 216.9 µs | 6.6 ms |
| `orders/exchange` | 4.13 M req/s | 208 ns | 417 ns | 750 ns | 2.1 ms |

Unpaced end-to-end latencies are queueing delay. The producer outruns the book thread, so they
measure how full the ring gets, not how fast a message moves.

### Run-to-run variation

Run 2 (`bench/results/2026-11-02-darwin-arm64-qos.md`) was the same build and workload with
`--pin`, which on macOS sets QoS `USER_INTERACTIVE` on both threads.

| Benchmark | Run 1 | Run 2 (`--pin`) |
|---|---:|---:|
| `book/reference` throughput | 7.14 M/s | 6.66 M/s |
| `book/fast` throughput | 17.90 M/s | 18.35 M/s |
| `replay/...` throughput | 14.19 M/s | 13.49 M/s |
| `queue/spsc` throughput | 112.9 M/s | 120.8 M/s |
| `queue/mutex` throughput | 46.3 M/s | 18.8 M/s |
| `queue-latency/spsc` p50 / p99 | 84 ns / 9.4 µs | 125 ns / 115.0 µs |
| `queue-latency/spsc-naive` p95 | 375 ns | 7.85 ms |
| `e2e-paced/spsc` p50 / p99 | 3.33 µs / 216.9 µs | 3.71 µs / 14.2 ms |
| `orders/exchange` throughput | 4.13 M/s | 4.69 M/s |

What this shows:

- **Single-threaded stages and the SPSC ring's throughput are consistent.** These figures can be
  quoted.
- **The mutex queue's throughput and every cross-thread tail are not.** They moved by 2.5× and by
  up to three orders of magnitude between back-to-back runs, which is consistent with threads
  being preempted or migrated by the other load. Treat cross-thread p99 and max on this page as
  "what a loaded laptop does", not as properties of the code.
- **QoS `USER_INTERACTIVE` showed no measurable benefit.** Run 2's tails were worse, but the
  variation between runs is larger than any QoS effect could be shown to be.

## Analysis

### Order book: profile first, then redesign — helped

The reference book was profiled before the optimized one existed (`sample`, macOS, 8 s on
`book/reference`):

- About 36% of samples were in `malloc`/`free`. Each order costs one `unordered_map` node and one
  `std::list` node, and a new price level costs a `std::map` node; all are allocated on add and
  freed on delete.
- Most of the rest was in `erase`, `reduce` and `insert`, which follow pointers between those
  nodes.

`FastMarket` targets exactly that: a pooled order array, an open-addressing index and
contiguous level vectors.

| | reference | fast | change |
|---|---:|---:|---:|
| throughput | 7.14 M ops/s | 17.90 M ops/s | 2.5× |
| p50 (64-op average) | 127.6 ns | 50.8 ns | 2.5× |
| p99 (64-op average) | 470.0 ns | 120.4 ns | 3.9× |
| max (64-op average) | 187.9 µs | 35.8 µs | 5.2× |

The same profile of `FastMarket` shows no allocator symbols in the steady state; the index grows
only a few times early in a run. Its samples sit in `unlink` (about 38%: find the level, relink
the neighbours, erase an emptied level, backward-shift the index), then `insert`, `reduce` and
`add`. I did not profile at line level, so I can't say which of those dominates inside `unlink`.

With decoding included, the gap narrows to 2.2× (7.00 → 15.09 M msg/s). Decoding costs about
8 ns per message, and the volume statistics add a little. Sequencing on top of that costs about
6% more (15.09 → 14.19 M msg/s).

### SPSC ring vs. mutex queue vs. naive SPSC — helped, with a surprise

Throughput (both threads spinning, 64-byte slots): the SPSC ring reached 113–121 M/s. The mutex
queue reached 19–46 M/s. The naive SPSC ring, the same algorithm with both indices on one cache
line and no index caching, reached 23 M/s, sometimes slower than the mutex queue.

Every operation of the naive ring reads the other thread's index. Head and tail share a line, so
each side's writes invalidate the line the other side is reading. The line moves between cores on
nearly every operation. Caching the other index (reloading it only when the ring looks full or
empty) and splitting the fields onto separate 128-byte lines removes almost all of that traffic.
**The lock-free algorithm alone wasn't the win; avoiding cache-line sharing was.**

Paced at 1M msg/s, where the ring is normally empty, the one-way handoff median was 84–125 ns for
the SPSC ring, two to three clock ticks. For the mutex queue it was 7.2–7.7 µs. I did not
instrument the mutex case. A plausible explanation is that the consumer spinning on `try_pop`
keeps re-taking the lock, so the producer's `try_push` has to wait for it. That is a hypothesis,
not a measurement.

### The second thread lowers throughput on this workload

The unpaced pipeline (feed thread → SPSC ring → book thread) reached 7.4–7.7 M msg/s. The same
work on one thread (`replay/...`) reached 13.5–14.2 M msg/s.

The two-thread benchmark also timestamps every message on the book thread to measure latency. A
variant without those timestamps, measured in a separate session, reached 8.9–10.3 M msg/s
against 11.3–11.6 M msg/s single-threaded in the same window. So the timestamps cost roughly a
quarter of the gap. The rest is moving every 64-byte slot from the feed core to the book core.

Sequencing is only about 6% of per-message work. Running it in parallel with the book therefore
cannot pay for a per-message cross-core transfer. The value of the split is isolation: the
socket is drained and re-requests go out while the book is busy, so a burst lands in an 8 MB
socket buffer and a 1 MB ring instead of stalling the receive loop.

**Next step, not implemented:** hand off one batch per packet (about 35 messages) instead of one
slot per message, so index updates and line transfers are amortized.

### Where end-to-end latency goes

`e2e-paced/spsc` measures from a packet's arrival on the feed thread to the end of each of its
messages' book updates. At 1M msg/s the median was 3.3–3.7 µs. The ring itself takes about 0.1 µs
of that (above). A 1,400-byte packet carries about 35 messages, and the book thread applies them
one after another at roughly 65–70 ns each. So the *n*-th message of a packet waits for the *n*−1
before it, and the median message sits mid-packet.

This latency is dominated by message batching in the transport, not by the handoff.

### Up-front capacity (`--reserve`) — a trade-off, not a free win

`FastMarket` grows its pool and index by doubling when the live order count passes its
reservation. This workload peaks above the default reservation of 262,144 orders. Three
interleaved runs of `book/fast` at each size:

| Reserved orders | Throughput | p50 | p99 | max (64-op average) |
|---|---:|---:|---:|---:|
| 65,536 | 19.6–20.1 M/s | 45.6–47.5 ns | 85–93 ns | 33–38 µs |
| 262,144 (default) | 18.8–21.7 M/s | 41.7–50.8 ns | 81–98 ns | 34–40 µs |
| 4,194,304 | 16.8–17.7 M/s | 55.3–57.9 ns | 93–111 ns | 1.7–3.4 µs |

The max column is a 64-operation average, so a 35 µs batch means one operation in it took about
2 ms: an index rehash or pool reallocation. Reserving for the peak removes those stalls. The
worst batch dropped by 10–20×.

It costs about 12% throughput and 15–25% at the median. The index's memory follows its
capacity, not the live order count: 4.2M reserved orders means an 8.4M-slot (128 MB) table, so
lookups land on more distinct cache lines and pages. That is my explanation, not a measurement;
I did not count TLB or cache misses.

For a feed handler, a 2 ms stall in the middle of the open is worse than a slower median.
Reserve for the expected peak (for example, the previous day's maximum live order count)
rather than the default.

An earlier single run at 4.2M reported 9.0 M/s. The repeated runs show that was an outlier on
the loaded machine; it is not used.

### Order entry

`Exchange` (risk checks, price-time matching, report generation) sustained 4.1–4.7 M requests/s,
with a p50 of 208 ns per request. It uses `std::map`, `std::list` and `std::unordered_map` and
has not been tuned. It is the slowest stage here, and would be the next candidate for the
treatment the market-data book received.

## What was not measured

- **Linux with real CPU pinning.** The only Linux available was a Docker VM on the same loaded
  Mac, which would not have been representative. The harness supports `--pin` (CPU affinity) on
  Linux.
- **Kernel receive cost, `recvmmsg`, busy-polling sockets, NIC timestamps.**
- **Binary versus linear search for price levels on deep books.** The synthetic flow keeps most
  activity near the touch, where a linear scan from the best price is short. A deep-book workload
  might favour binary search; I haven't tested it.
