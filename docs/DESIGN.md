# Technical Design

Status: living document. Sections marked *(planned)* describe intent that is not yet implemented;
everything else describes code that exists in this repository.

## 1. Scope

The system ingests Nasdaq TotalView-ITCH 5.0 market data carried over MoldUDP64, reconstructs a
full-depth limit order book per instrument, and exposes top-of-book, depth, spread and volume.
A separate, self-contained component simulates order entry, pre-trade risk and matching.

Non-goals: connecting to a real exchange or broker, redistributing exchange data, SoupBinTCP,
GLIMPSE snapshot recovery, and kernel-bypass networking. These are listed again under
limitations with the reason they are out of scope.

## 2. Sources

All protocol behaviour is taken from the two official Nasdaq documents, read in full before
any code was written:

| Document | Version read | Where |
|---|---|---|
| Nasdaq TotalView-ITCH 5.0 | revision log ends April 28, 2023 (DLCR message added) | nasdaqtrader.com `NQTVITCHspecification.pdf` |
| MoldUDP64 Protocol Specification | V1.00, formatting update 8/2/24 | nasdaqtrader.com `moldudp64.pdf` |

The specifications are not redistributed here. Where this project takes a position the
specifications do not dictate (for example, what to do with a malformed packet), the decision
and its reason are recorded in this document and labelled **policy**, not protocol.

## 3. Protocol analysis

### 3.1 MoldUDP64

Downstream packet = 20-byte header followed by `Message Count` message blocks.

| Field | Offset | Length | Notes |
|---|---|---|---|
| Session | 0 | 10 | ASCII |
| Sequence Number | 10 | 8 | sequence number of the *first* message in the packet; later messages are numbered implicitly |
| Message Count | 18 | 2 | `0` = heartbeat, `0xFFFF` = end of session |

Each message block is a 2-byte big-endian length (excluding itself) and that many data bytes.
Zero-length messages are legal. All MoldUDP64 integers are big-endian.

Heartbeats and end-of-session packets carry the **next expected** sequence number. That is what
lets a receiver detect loss at the tail of a burst during quiet periods: if a heartbeat says
"next is 1001" and we have only seen up to 990, ten messages are missing.

Request packet (to a re-request server, unicast): Session (10), Sequence Number (8), Requested
Message Count (2). The response is an ordinary downstream packet unicast back to the requester's
socket. A server returns only as many messages as fit in one UDP payload, so a receiver must be
prepared to issue follow-up requests for the remainder.

The spec's reference receiver aborts on a session mismatch and re-requests from the next
expected number on any gap.

### 3.2 ITCH 5.0

Every message starts with the same 11-byte header: type (1), stock locate (2), tracking number
(2), timestamp (6 bytes, nanoseconds since midnight). Integers are big-endian and unsigned. Alpha
fields are left-justified ASCII padded with spaces. `Price(4)` is a 32-bit integer with four
implied decimals (max 200,000.0000 = `0x77359400`); `Price(8)` (MWCB levels only) has eight.

Message types and fixed lengths, computed from the field tables:

| Type | Message | Length | Book impact |
|---|---|---|---|
| `S` | System Event | 12 | none (session state) |
| `R` | Stock Directory | 39 | binds locate to symbol |
| `H` | Stock Trading Action | 25 | none (trading state) |
| `Y` | Reg SHO Restriction | 20 | none |
| `L` | Market Participant Position | 26 | none |
| `V` | MWCB Decline Level | 35 | none |
| `W` | MWCB Status | 12 | none |
| `K` | IPO Quoting Period Update | 28 | none |
| `J` | LULD Auction Collar | 35 | none |
| `h` | Operational Halt | 21 | none |
| `A` | Add Order (no MPID) | 36 | add |
| `F` | Add Order (MPID) | 40 | add |
| `E` | Order Executed | 31 | reduce; counts toward volume |
| `C` | Order Executed With Price | 36 | reduce; volume only if Printable = `Y` |
| `X` | Order Cancel | 23 | reduce |
| `D` | Order Delete | 19 | remove |
| `U` | Order Replace | 35 | remove old, add new (loses priority) |
| `P` | Trade (non-cross) | 44 | **none** — non-displayed orders; volume only |
| `Q` | Cross Trade | 40 | none; bulk cross volume (8-byte shares) |
| `B` | Broken Trade | 19 | none; affects trade statistics only |
| `I` | NOII | 50 | none |
| `N` | Retail Price Improvement Indicator | 20 | none |
| `O` | DLCR Price Discovery | 48 | none |

Points from the specification that drive the design:

* **Full-depth vs. trade reporting.** `A/F/E/C/X/D/U` describe displayed orders and are the only
  messages that change the book. `P` reports executions of *non-displayed* orders: there was
  never an Add for them, so applying `P` to the book would be wrong. `Q` is one bulk print per
  cross. The engine therefore keeps two separate accumulators: book state and trade statistics.
* **Non-printable executions.** A `C` with Printable = `N` will be included in a later bulk print
  (typically the cross). Counting it would double count volume, so it reduces the order but is
  excluded from volume.
* **`C` price vs. order price.** The execution price on `C` may differ from the resting order's
  display price. The book reduction applies at the order's price, not the execution price.
* **Replace semantics.** Side, symbol and attribution cannot change on `U` and are not
  transmitted; they must be carried over from the original order. The new reference number is
  used for all later updates and the order loses time priority.
* **Order reference numbers.** Day-unique, but the 2009 revision deliberately removed "increasing".
  They cannot be used as dense array indices; an associative index is required.
* **Stock locate codes.** Assigned daily starting at 1 and documented as intended for use as an
  array index. A flat `65536`-entry table keyed by locate is the intended access pattern, and
  every message carries the locate at the same offset.
* **`P` Buy/Sell is always `B`** since 2014 and the order reference is zero since 2010, so no
  information about the resting side can be inferred from it.
* **Crossed books are not a protocol violation.** Before the opening cross, orders accumulate and
  the Nasdaq book can be locked or crossed until the cross executes. "Best bid < best ask" is
  therefore reported as an observation, never asserted as an invariant.

### 3.3 Framing of offline files

Nasdaq's historical TotalView-ITCH files use the same framing as a MoldUDP64 message block: a
2-byte big-endian length followed by the message. The same bounds-checked block reader serves
both. This project does **not** ship any Nasdaq data; all fixtures are produced by the synthetic
generator described below and are labelled as synthetic.

## 4. Architecture (proposed at milestone 1)

```
             +-----------------------------------------------------------+
  UDP / file |  feed thread                                              |
  --------->  |  MoldUDP64 parse --> Sequencer --(in-order msgs)--> queue |---+
             |      ^   (validate   (gap/dup/reorder,                    |   |
             |      |    whole       re-request timer)                   |   |
             |      |    packet)          |                              |   |
             |      +---- retransmits <---+ Request packets ----> re-request server
             +-----------------------------------------------------------+   |
                                                                             v
             +-----------------------------------------------------------+
             |  book thread (sole owner of all book state, no locks)     |
             |  ITCH decode --> BookBuilder --> Market (per-locate books) |
             |                       |                                   |
             |                       +--> trade statistics (volume, etc) |
             +-----------------------------------------------------------+

  Separate, independent of ITCH:
     order entry --> pre-trade risk --> matching engine --> execution reports
```

Libraries (one CMake target each, dependencies point downward only):

| Target | Contents | Depends on |
|---|---|---|
| `mde_common` | byte-order loads/stores, deterministic PRNG, hashing | — |
| `mde_itch` | ITCH 5.0 message types, decoder, encoder | common |
| `mde_moldudp64` | packet parser/builder, sequencer, replay harness, UDP transport | common |
| `mde_book` | reference and optimized markets, ITCH-to-book builder | itch |
| `mde_pipeline` | SPSC ring, baseline mutex queue, two-thread pipeline | moldudp64, book |
| `mde_synth` | synthetic ITCH generator | itch, book |
| `mde_sim` | order entry, risk checks, matching engine, simulator | common only |

Key decisions, each to be validated by tests or measurements:

1. **Validate a MoldUDP64 packet completely before delivering any message from it** (policy). A
   corrupt packet is dropped whole and treated as loss; recovery then goes through the same
   re-request path as a lost packet. This avoids ever applying half of a damaged packet.
2. **Exact-length ITCH decoding** (policy). Each type has one length in the spec; a message whose
   length differs is reported as malformed rather than partially parsed.
3. **Sequencer as a pure state machine.** It takes packets and a caller-supplied clock and returns
   in-order messages and re-request instructions. No sockets, no wall clock inside it, so every
   loss/duplicate/reorder case is unit-testable and replay is deterministic.
4. **One owner per piece of state.** The book thread owns every book. The only cross-thread
   structure is the queue between the feed and book threads.
5. **Reference first, optimized second.** A deliberately simple `std::map`/`std::list` market is
   the oracle. The optimized market must produce identical state after every event under
   randomized differential tests before any performance number is reported.
6. **Determinism.** No wall-clock reads in the data path of replay, simulation or generation.
   Randomness uses a fixed, self-implemented PRNG (not `<random>` distributions, whose output
   differs between libstdc++ and libc++) so fixtures and digests are identical across platforms.

## 5. Milestones

1. Architecture and protocol analysis
2. Build system and basic parser
3. Protocol conformance and malformed-input tests
4. Correct reference order book
5. Market-data sequencing and replay
6. Multithreaded pipeline
7. Simulated order entry and risk checks
8. Performance profiling and optimization
9. Stress testing and failure handling
10. Documentation, reproducibility, and release
