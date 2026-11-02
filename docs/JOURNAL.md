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
