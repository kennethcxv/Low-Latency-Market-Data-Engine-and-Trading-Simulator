## Environment

| | |
|---|---|
| CPU | Apple M4 Max (16 hardware threads) |
| OS | Darwin 27.0.0 arm64 |
| Compiler | Apple LLVM 21.0.0 (clang-2100.3.34.2) |
| Flags | `Release:  -O3 -DNDEBUG` |
| Load average at start | 19.01 20.95 20.99 (1/5/15 min; nonzero means other work was running) |
| Clock | steady_clock: resolution 41.0 ns, cost per read 16.6 ns (measured) |
| Thread placement | --pin: Linux CPU affinity (feed cpu 2, book cpu 4); macOS QoS USER_INTERACTIVE |
| Workload | synthetic ITCH, 2000000 messages, 64 symbols, seed 1, 66.5 MB |
| FastMarket reserve | 262144 orders |
| Repetitions | 1 warm-up + 5 timed; throughput is the median, percentiles pool all timed samples |

## Results

| Benchmark | Items | Throughput (M/s) | p50 (ns) | p95 (ns) | p99 (ns) | max (ns) | Measures |
|---|---:|---:|---:|---:|---:|---:|---|
| decode | 2000000 | 107.59 | 9.1 | 9.8 | 10.4 | 2987 | framing + ITCH decode, trivial handler; batch-averaged |
| book/reference | 1919224 | 6.66 | 134.8 | 204.4 | 481.1 | 172393 | pre-decoded add/reduce/remove/replace only; batch-averaged |
| book/fast | 1919224 | 18.35 | 51.4 | 76.2 | 99.6 | 36813 | pre-decoded add/reduce/remove/replace only; batch-averaged |
| decode+book/reference | 2000000 | 6.61 | 133.5 | 216.2 | 527.3 | 491420 | decode + book + trade stats, one thread; batch-averaged |
| decode+book/fast | 2000000 | 15.84 | 59.2 | 87.2 | 106.8 | 95910 | decode + book + trade stats, one thread; batch-averaged |
| replay/sequencer+book(fast) | 2000000 | 13.49 | 68.4 | 100.0 | 173.6 | 656353 | MoldUDP64 sequencing + decode + book, one thread; per packet / messages in it |
| queue/mutex | 5000000 | 18.78 | 0.0 | 0.0 | 0.0 | 0 | 64-byte slots, both threads spinning |
| queue/spsc-naive | 5000000 | 23.55 | 0.0 | 0.0 | 0.0 | 0 | 64-byte slots, both threads spinning |
| queue/spsc | 5000000 | 120.80 | 0.0 | 0.0 | 0.0 | 0 | 64-byte slots, both threads spinning |
| queue-latency/mutex | 500000 | 1.00 | 7208.0 | 1493333.0 | 12084000.0 | 22660959 | paced at 1000000 msg/s; one-way, per message |
| queue-latency/spsc-naive | 500000 | 1.00 | 333.0 | 7850375.0 | 16433500.0 | 21163709 | paced at 1000000 msg/s; one-way, per message |
| queue-latency/spsc | 500000 | 1.00 | 125.0 | 8625.0 | 115041.0 | 2554625 | paced at 1000000 msg/s; one-way, per message |
| e2e/mutex | 2000000 | 1.76 | 9016583.0 | 13530333.0 | 20608458.0 | 77332500 | unpaced (includes queueing delay); packet arrival -> book updated |
| e2e/spsc | 2000000 | 7.37 | 2151166.0 | 3120833.0 | 4946750.0 | 6912041 | unpaced (includes queueing delay); packet arrival -> book updated |
| e2e-paced/mutex | 2000000 | 1.00 | 18583.0 | 15705500.0 | 30742583.0 | 77348916 | paced at 1000000 msg/s; packet arrival -> book updated |
| e2e-paced/spsc | 2000000 | 1.00 | 3709.0 | 3498375.0 | 14220125.0 | 76377625 | paced at 1000000 msg/s; packet arrival -> book updated |
| orders/exchange | 1000000 | 4.69 | 208.0 | 375.0 | 625.0 | 723000 | risk + matching + reports per request; per request |
