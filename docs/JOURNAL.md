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
