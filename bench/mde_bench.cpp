// Benchmarks for each stage of the system. Methodology, boundaries and caveats: docs/BENCHMARKS.md.
//
// Timing: std::chrono::steady_clock. Its resolution is measured and printed at startup (41.7 ns
// on Apple Silicon, where it is a 24 MHz counter). Operations far below that cannot be timed one
// at a time, so stages costing tens of nanoseconds are timed per batch of kBatch operations and
// the reported percentiles are of per-operation cost averaged over each batch. Cross-thread
// latencies (queue handoff, end-to-end) are timed per message with the same clock on both threads.
#include <sys/utsname.h>
#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#include <sys/sysctl.h>
#endif
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#include "mde/book/book_builder.hpp"
#include "mde/book/fast_market.hpp"
#include "mde/book/reference_market.hpp"
#include "mde/itch/codec.hpp"
#include "mde/moldudp64/replay.hpp"
#include "mde/moldudp64/sequencer.hpp"
#include "mde/pipeline/pipeline.hpp"
#include "mde/sim/simulator.hpp"
#include "mde/synth/generator.hpp"

namespace {

using namespace mde;
using Clock = std::chrono::steady_clock;

constexpr std::size_t kBatch = 64;

std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}

template <class T>
void keep(const T& value) {
  asm volatile("" : : "r,m"(value) : "memory");
}

struct Options {
  std::size_t messages = 2'000'000;
  std::size_t symbols = 64;
  std::uint64_t seed = 1;
  int repetitions = 5;
  std::string only;
  bool pin = false;
  std::uint64_t paced_rate = 1'000'000;  // messages per second for latency runs
  std::size_t reserve = 1 << 18;          // FastMarket up-front order capacity (its default)
};

// ---- Reporting --------------------------------------------------------------------------------

struct Result {
  std::string name;
  std::uint64_t items = 0;     // per repetition
  double throughput = 0;       // items per second, median over repetitions
  double p50 = 0, p95 = 0, p99 = 0, max = 0;  // ns per item
  std::string note;
};

std::vector<Result> g_results;

double percentile(const std::vector<double>& sorted, double q) {
  if (sorted.empty()) return 0;
  const std::size_t i = std::min(sorted.size() - 1, static_cast<std::size_t>(q * static_cast<double>(sorted.size())));
  return sorted[i];
}

void record(std::string name, std::uint64_t items, std::vector<double> rates, std::vector<double> samples, std::string note) {
  std::sort(rates.begin(), rates.end());
  std::sort(samples.begin(), samples.end());
  Result r;
  r.name = std::move(name);
  r.items = items;
  r.throughput = rates.empty() ? 0 : rates[rates.size() / 2];
  r.p50 = percentile(samples, 0.50);
  r.p95 = percentile(samples, 0.95);
  r.p99 = percentile(samples, 0.99);
  r.max = samples.empty() ? 0 : samples.back();
  r.note = std::move(note);
  std::fprintf(stderr, "  %-34s %8.2f M/s  p50 %7.1f  p99 %8.1f ns\n", r.name.c_str(), r.throughput / 1e6, r.p50, r.p99);
  g_results.push_back(std::move(r));
}

// ---- Environment ------------------------------------------------------------------------------

std::string cpu_model() {
#if defined(__APPLE__)
  char buf[256] = {};
  std::size_t len = sizeof buf;
  if (sysctlbyname("machdep.cpu.brand_string", buf, &len, nullptr, 0) == 0) return buf;
#else
  std::ifstream in("/proc/cpuinfo");
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("model name", 0) == 0) return line.substr(line.find(':') + 2);
  }
#endif
  return "unknown";
}

struct ClockInfo {
  double resolution_ns;
  double overhead_ns;
};

ClockInfo measure_clock() {
  std::uint64_t smallest = UINT64_MAX;
  const std::uint64_t start = now_ns();
  constexpr int kReads = 1'000'000;
  for (int i = 0; i < kReads; ++i) {
    const std::uint64_t a = now_ns(), b = now_ns();
    if (b > a) smallest = std::min(smallest, b - a);
  }
  return {static_cast<double>(smallest), static_cast<double>(now_ns() - start) / (2.0 * kReads)};
}

// Linux: hard affinity to `cpu`. macOS has no affinity API; the closest control is the
// USER_INTERACTIVE QoS class, which steers the thread to performance cores.
void pin_current_thread([[maybe_unused]] int cpu) {
#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  pthread_setaffinity_np(pthread_self(), sizeof set, &set);
#elif defined(__APPLE__)
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}

// ---- Workload ---------------------------------------------------------------------------------

struct Workload {
  std::vector<std::uint8_t> framed;  // synthetic ITCH, [u16 length][message]...
  std::vector<Bytes> messages;
};

Workload make_workload(const Options& o) {
  synth::Config c;
  c.seed = o.seed;
  c.messages = o.messages;
  c.symbols = o.symbols;
  Workload w;
  w.framed = synth::generate(c);
  FramedReader reader{w.framed};
  Bytes msg;
  while (reader.next(msg) == FramedReader::Status::kOk) w.messages.push_back(msg);
  return w;
}

// Book operations extracted from the stream, so the book can be timed without decoding.
struct BookOp {
  enum Kind : std::uint8_t { kAdd, kReduce, kRemove, kReplace } kind;
  book::Side side;
  book::Locate locate;
  book::Qty quantity;
  book::OrderId id;
  book::OrderId new_id;
  book::Price price;
};

std::vector<BookOp> extract_book_ops(const Workload& w) {
  std::vector<BookOp> ops;
  for (Bytes msg : w.messages) {
    itch::decode(msg, itch::Overloaded{
                          [&](const itch::AddOrder& m) {
                            ops.push_back({BookOp::kAdd, m.side == 'B' ? book::Side::kBuy : book::Side::kSell,
                                           m.header.stock_locate, m.shares, m.order_ref, 0, m.price});
                          },
                          [&](const itch::AddOrderMpid& m) {
                            ops.push_back({BookOp::kAdd, m.side == 'B' ? book::Side::kBuy : book::Side::kSell,
                                           m.header.stock_locate, m.shares, m.order_ref, 0, m.price});
                          },
                          [&](const itch::OrderExecuted& m) {
                            ops.push_back({BookOp::kReduce, {}, m.header.stock_locate, m.executed_shares, m.order_ref, 0, 0});
                          },
                          [&](const itch::OrderExecutedWithPrice& m) {
                            ops.push_back({BookOp::kReduce, {}, m.header.stock_locate, m.executed_shares, m.order_ref, 0, 0});
                          },
                          [&](const itch::OrderCancel& m) {
                            ops.push_back({BookOp::kReduce, {}, m.header.stock_locate, m.cancelled_shares, m.order_ref, 0, 0});
                          },
                          [&](const itch::OrderDelete& m) {
                            ops.push_back({BookOp::kRemove, {}, m.header.stock_locate, 0, m.order_ref, 0, 0});
                          },
                          [&](const itch::OrderReplace& m) {
                            ops.push_back({BookOp::kReplace, {}, m.header.stock_locate, m.shares, m.original_order_ref,
                                           m.new_order_ref, m.price});
                          },
                          [](const auto&) {},
                      });
  }
  return ops;
}

template <class M>
std::unique_ptr<M> make_market(const Options& o) {
  if constexpr (std::is_same_v<M, book::FastMarket>) {
    return std::make_unique<M>(o.reserve);
  } else {
    return std::make_unique<M>();
  }
}

template <class M>
book::Result apply(M& market, const BookOp& op) {
  switch (op.kind) {
    case BookOp::kAdd: return market.add(op.locate, op.id, op.side, op.price, op.quantity);
    case BookOp::kReduce: return market.reduce(op.locate, op.id, op.quantity);
    case BookOp::kRemove: return market.remove(op.locate, op.id);
    case BookOp::kReplace: return market.replace(op.locate, op.id, op.new_id, op.price, op.quantity);
  }
  return book::Result::kOk;
}

bool selected(const Options& o, std::string_view name) { return o.only.empty() || name.find(o.only) != std::string_view::npos; }

// Runs `body(samples)` once untimed (warm-up) and then o.repetitions times; body returns elapsed ns
// for `items` items and appends per-item samples.
template <class Body>
void bench(const Options& o, const std::string& name, std::uint64_t items, const std::string& note, Body&& body) {
  if (!selected(o, name)) return;
  std::vector<double> warmup;
  body(warmup);
  std::vector<double> rates, samples;
  for (int r = 0; r < o.repetitions; ++r) {
    const std::uint64_t elapsed = body(samples);
    rates.push_back(static_cast<double>(items) * 1e9 / static_cast<double>(elapsed));
  }
  record(name, items, std::move(rates), std::move(samples), note);
}

// ---- Benchmarks -------------------------------------------------------------------------------

void bench_decode(const Options& o, const Workload& w) {
  bench(o, "decode", w.messages.size(), "framing + ITCH decode, trivial handler; batch-averaged", [&](std::vector<double>& samples) {
    std::uint64_t sink = 0;
    FramedReader reader{w.framed};
    Bytes msg;
    const std::uint64_t start = now_ns();
    std::uint64_t batch_start = start;
    std::size_t n = 0;
    while (reader.next(msg) == FramedReader::Status::kOk) {
      itch::decode(msg, [&](const auto& m) { sink += m.header.timestamp.ns; });
      if (++n % kBatch == 0) {
        const std::uint64_t t = now_ns();
        samples.push_back(static_cast<double>(t - batch_start) / kBatch);
        batch_start = t;
      }
    }
    keep(sink);
    return now_ns() - start;
  });
}

template <class M>
void bench_book(const Options& o, const std::vector<BookOp>& ops, const std::string& name) {
  bench(o, name, ops.size(), "pre-decoded add/reduce/remove/replace only; batch-averaged", [&](std::vector<double>& samples) {
    auto market = make_market<M>(o);
    const std::uint64_t start = now_ns();
    std::uint64_t batch_start = start;
    for (std::size_t i = 0; i < ops.size(); ++i) {
      keep(apply(*market, ops[i]));
      if ((i + 1) % kBatch == 0) {
        const std::uint64_t t = now_ns();
        samples.push_back(static_cast<double>(t - batch_start) / kBatch);
        batch_start = t;
      }
    }
    return now_ns() - start;
  });
}

template <class M>
void bench_builder(const Options& o, const Workload& w, const std::string& name) {
  bench(o, name, w.messages.size(), "decode + book + trade stats, one thread; batch-averaged", [&](std::vector<double>& samples) {
    auto market = make_market<M>(o);
    auto builder = std::make_unique<book::BookBuilder<M>>(*market);
    const std::uint64_t start = now_ns();
    std::uint64_t batch_start = start;
    for (std::size_t i = 0; i < w.messages.size(); ++i) {
      builder->apply(w.messages[i]);
      if ((i + 1) % kBatch == 0) {
        const std::uint64_t t = now_ns();
        samples.push_back(static_cast<double>(t - batch_start) / kBatch);
        batch_start = t;
      }
    }
    return now_ns() - start;
  });
}

void bench_replay(const Options& o, const Workload& w, const std::vector<std::vector<std::uint8_t>>& packets) {
  bench(o, "replay/sequencer+book(fast)", w.messages.size(), "MoldUDP64 sequencing + decode + book, one thread; per packet / messages in it",
        [&](std::vector<double>& samples) {
          auto market = make_market<book::FastMarket>(o);
          auto builder = std::make_unique<book::BookBuilder<book::FastMarket>>(*market);
          moldudp64::Sequencer seq;
          const std::uint64_t start = now_ns();
          for (const auto& p : packets) {
            const std::uint64_t t0 = now_ns();
            std::size_t n = 0;
            seq.on_packet(p, t0, [&](std::uint64_t, Bytes m) {
              builder->apply(m);
              ++n;
            });
            if (n) samples.push_back(static_cast<double>(now_ns() - t0) / static_cast<double>(n));
          }
          return now_ns() - start;
        });
}

// Naive SPSC ring for comparison: same algorithm, but both indices share one cache line and each
// operation reloads the other side's index (no caching).
template <class T, std::size_t Capacity>
class NaiveSpscQueue {
 public:
  bool try_push(const T& v) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - head_.load(std::memory_order_acquire) == Capacity) return false;
    slots_[tail % Capacity] = v;
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }
  bool try_pop(T& out) {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_.load(std::memory_order_acquire)) return false;
    out = slots_[head % Capacity];
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

 private:
  std::atomic<std::size_t> head_{0};
  std::atomic<std::size_t> tail_{0};
  std::array<T, Capacity> slots_{};
};

constexpr std::size_t kQueueCapacity = 1 << 14;

// Handoff throughput: the producer pushes as fast as it can, the consumer pops as fast as it can.
template <class Q>
void bench_queue_throughput(const Options& o, const std::string& name, std::uint64_t items) {
  bench(o, name, items, "64-byte slots, both threads spinning", [&](std::vector<double>&) {
    auto q = std::make_unique<Q>();
    std::thread consumer([&] {
      if (o.pin) pin_current_thread(4);
      pipeline::MessageSlot s;
      std::uint64_t sum = 0;
      for (std::uint64_t got = 0; got < items;) {
        if (q->try_pop(s)) {
          sum += s.ingress_ns;
          ++got;
        }
      }
      keep(sum);
    });
    if (o.pin) pin_current_thread(2);
    const std::uint64_t start = now_ns();
    pipeline::MessageSlot s;
    for (std::uint64_t i = 0; i < items; ++i) {
      s.ingress_ns = i;
      while (!q->try_push(s)) pipeline::cpu_relax();
    }
    consumer.join();
    return now_ns() - start;
  });
}

// Handoff latency: one message every 1e9/rate ns, so the queue is normally empty and the
// measurement is the one-way handoff, not queueing delay.
template <class Q>
void bench_queue_latency(const Options& o, const std::string& name, std::uint64_t items) {
  const std::uint64_t interval = 1'000'000'000ULL / o.paced_rate;
  bench(o, name, items, "paced at " + std::to_string(o.paced_rate) + " msg/s; one-way, per message", [&](std::vector<double>& samples) {
    auto q = std::make_unique<Q>();
    std::vector<double> local(items);
    std::thread consumer([&] {
      if (o.pin) pin_current_thread(4);
      pipeline::MessageSlot s;
      for (std::uint64_t got = 0; got < items;) {
        if (q->try_pop(s)) local[got++] = static_cast<double>(now_ns() - s.ingress_ns);
      }
    });
    if (o.pin) pin_current_thread(2);
    const std::uint64_t start = now_ns();
    pipeline::MessageSlot s;
    for (std::uint64_t i = 0; i < items; ++i) {
      while (now_ns() < start + i * interval) {
      }
      s.ingress_ns = now_ns();
      while (!q->try_push(s)) pipeline::cpu_relax();
    }
    consumer.join();
    samples.insert(samples.end(), local.begin(), local.end());
    return now_ns() - start;
  });
}

// Feed thread: sequencer over in-memory packets. Book thread: decode + FastMarket. Latency is from
// the packet's arrival at the feed thread to the end of its book update on the book thread.
template <class Q>
void bench_e2e(const Options& o, const std::string& name, const Workload& w,
               const std::vector<std::vector<std::uint8_t>>& packets, bool paced) {
  const double per_packet = static_cast<double>(w.messages.size()) / static_cast<double>(packets.size());
  const auto interval = static_cast<std::uint64_t>(per_packet * 1e9 / static_cast<double>(o.paced_rate));
  const std::string note = paced ? "paced at " + std::to_string(o.paced_rate) + " msg/s; packet arrival -> book updated"
                                 : "unpaced (includes queueing delay); packet arrival -> book updated";
  bench(o, name, w.messages.size(), note, [&](std::vector<double>& samples) {
    auto q = std::make_unique<Q>();
    auto market = make_market<book::FastMarket>(o);
    auto builder = std::make_unique<book::BookBuilder<book::FastMarket>>(*market);
    std::vector<double> local;
    local.reserve(w.messages.size());
    bool pinned = false;
    moldudp64::Sequencer seq;
    if (o.pin) pin_current_thread(2);
    const std::uint64_t start = now_ns();
    pipeline::run(
        *q,
        [&](auto& push) {
          for (std::size_t i = 0; i < packets.size(); ++i) {
            if (paced) {
              while (now_ns() < start + i * interval) {
              }
            }
            const std::uint64_t arrival = now_ns();
            seq.on_packet(packets[i], arrival, [&](std::uint64_t, Bytes m) { push(arrival, m); });
          }
        },
        [&](const pipeline::MessageSlot& slot) {
          if (o.pin && !pinned) {
            pin_current_thread(4);
            pinned = true;
          }
          builder->apply(Bytes(slot.data, slot.length));
          local.push_back(static_cast<double>(now_ns() - slot.ingress_ns));
        });
    const std::uint64_t elapsed = now_ns() - start;
    samples.insert(samples.end(), local.begin(), local.end());
    return elapsed;
  });
}

void bench_orders(const Options& o) {
  sim::SimConfig config;
  config.seed = o.seed;
  config.steps = 1'000'000;
  bench(o, "orders/exchange", config.steps, "risk + matching + reports per request; per request", [&](std::vector<double>& samples) {
    sim::Exchange exchange(config.instruments, config.limits);
    sim::OrderFlow flow(config);
    std::vector<sim::ExecutionReport> out;
    std::uint64_t busy = 0;
    for (std::size_t i = 0; i < config.steps; ++i) {
      const auto request = flow.next();
      out.clear();
      const std::uint64_t t0 = now_ns();
      if (const auto* order = std::get_if<sim::NewOrder>(&request)) {
        exchange.submit(*order, out);
      } else {
        exchange.cancel(std::get<sim::CancelOrder>(request), out);
      }
      const std::uint64_t dt = now_ns() - t0;
      busy += dt;
      samples.push_back(static_cast<double>(dt));
      for (const auto& r : out) flow.observe(r);
    }
    return busy;  // throughput over exchange time only; request generation is excluded
  });
}

void print_markdown(const Options& o, const Workload& w, const ClockInfo& clock) {
  utsname u{};
  uname(&u);
  std::printf("## Environment\n\n");
  std::printf("| | |\n|---|---|\n");
  std::printf("| CPU | %s (%u hardware threads) |\n", cpu_model().c_str(), std::thread::hardware_concurrency());
  std::printf("| OS | %s %s %s |\n", u.sysname, u.release, u.machine);
  std::printf("| Compiler | %s |\n", __VERSION__);
  std::printf("| Flags | `%s` |\n", MDE_BENCH_FLAGS);
  double load[3] = {0, 0, 0};
  getloadavg(load, 3);
  std::printf("| Load average at start | %.2f %.2f %.2f (1/5/15 min; nonzero means other work was running) |\n", load[0], load[1], load[2]);
  std::printf("| Clock | steady_clock: resolution %.1f ns, cost per read %.1f ns (measured) |\n", clock.resolution_ns, clock.overhead_ns);
  std::printf("| Thread placement | %s |\n", o.pin ? "--pin: Linux CPU affinity (feed cpu 2, book cpu 4); macOS QoS USER_INTERACTIVE" : "OS default");
  std::printf("| Workload | synthetic ITCH, %zu messages, %zu symbols, seed %" PRIu64 ", %.1f MB |\n", w.messages.size(),
              o.symbols, o.seed, static_cast<double>(w.framed.size()) / 1e6);
  std::printf("| FastMarket reserve | %zu orders |\n", o.reserve);
  std::printf("| Repetitions | 1 warm-up + %d timed; throughput is the median, percentiles pool all timed samples |\n\n", o.repetitions);

  std::printf("## Results\n\n");
  std::printf("| Benchmark | Items | Throughput (M/s) | p50 (ns) | p95 (ns) | p99 (ns) | max (ns) | Measures |\n");
  std::printf("|---|---:|---:|---:|---:|---:|---:|---|\n");
  for (const auto& r : g_results) {
    if (r.max == 0) {  // throughput-only benchmark: no per-item samples
      std::printf("| %s | %" PRIu64 " | %.2f | - | - | - | - | %s |\n", r.name.c_str(), r.items, r.throughput / 1e6, r.note.c_str());
      continue;
    }
    std::printf("| %s | %" PRIu64 " | %.2f | %.1f | %.1f | %.1f | %.0f | %s |\n", r.name.c_str(), r.items, r.throughput / 1e6,
                r.p50, r.p95, r.p99, r.max, r.note.c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    auto value = [&]() -> std::string {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", argv[i]);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--messages") o.messages = std::stoull(value());
    else if (a == "--symbols") o.symbols = std::stoull(value());
    else if (a == "--seed") o.seed = std::stoull(value());
    else if (a == "--repetitions") o.repetitions = std::stoi(value());
    else if (a == "--only") o.only = value();
    else if (a == "--rate") o.paced_rate = std::stoull(value());
    else if (a == "--reserve") o.reserve = std::stoull(value());
    else if (a == "--pin") o.pin = true;
    else {
      std::fprintf(stderr,
                   "usage: mde_bench [--messages N] [--symbols N] [--seed N] [--repetitions N] [--only SUBSTRING]\n"
                   "                 [--rate MSG_PER_SEC] [--reserve ORDERS] [--pin]\n");
      return 2;
    }
  }

  const ClockInfo clock = measure_clock();
  std::fprintf(stderr, "generating workload...\n");
  const Workload w = make_workload(o);
  const auto ops = extract_book_ops(w);
  const moldudp64::MessageLog log(moldudp64::make_session("BENCH"), w.framed);
  const auto packets = log.packetize(1400);

  bench_decode(o, w);
  bench_book<book::ReferenceMarket>(o, ops, "book/reference");
  bench_book<book::FastMarket>(o, ops, "book/fast");
  bench_builder<book::ReferenceMarket>(o, w, "decode+book/reference");
  bench_builder<book::FastMarket>(o, w, "decode+book/fast");
  bench_replay(o, w, packets);

  using Slot = pipeline::MessageSlot;
  constexpr std::uint64_t kHandoffs = 5'000'000;
  constexpr std::uint64_t kPacedHandoffs = 500'000;
  bench_queue_throughput<pipeline::MutexQueue<Slot, kQueueCapacity>>(o, "queue/mutex", kHandoffs);
  bench_queue_throughput<NaiveSpscQueue<Slot, kQueueCapacity>>(o, "queue/spsc-naive", kHandoffs);
  bench_queue_throughput<pipeline::SpscQueue<Slot, kQueueCapacity>>(o, "queue/spsc", kHandoffs);
  bench_queue_latency<pipeline::MutexQueue<Slot, kQueueCapacity>>(o, "queue-latency/mutex", kPacedHandoffs);
  bench_queue_latency<NaiveSpscQueue<Slot, kQueueCapacity>>(o, "queue-latency/spsc-naive", kPacedHandoffs);
  bench_queue_latency<pipeline::SpscQueue<Slot, kQueueCapacity>>(o, "queue-latency/spsc", kPacedHandoffs);

  bench_e2e<pipeline::MutexQueue<Slot, kQueueCapacity>>(o, "e2e/mutex", w, packets, false);
  bench_e2e<pipeline::SpscQueue<Slot, kQueueCapacity>>(o, "e2e/spsc", w, packets, false);
  bench_e2e<pipeline::MutexQueue<Slot, kQueueCapacity>>(o, "e2e-paced/mutex", w, packets, true);
  bench_e2e<pipeline::SpscQueue<Slot, kQueueCapacity>>(o, "e2e-paced/spsc", w, packets, true);

  bench_orders(o);

  print_markdown(o, w, clock);
  return 0;
}
