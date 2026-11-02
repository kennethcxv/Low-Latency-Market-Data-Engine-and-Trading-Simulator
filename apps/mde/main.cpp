// mde: command-line front end. Each subcommand is a thin layer over the libraries.
#include <arpa/inet.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <array>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "mde/book/book_builder.hpp"
#include "mde/book/reference_market.hpp"
#include "mde/common.hpp"
#include "mde/itch/codec.hpp"
#include "mde/itch/format.hpp"
#include "mde/moldudp64/replay.hpp"
#include "mde/net/session.hpp"
#include "mde/pipeline/pipeline.hpp"
#include "mde/sim/simulator.hpp"
#include "mde/synth/generator.hpp"

namespace {

// Positional arguments plus `--name value` options and bare `--flag`s.
class Args {
 public:
  Args(const std::vector<std::string_view>& raw, std::initializer_list<std::string_view> flags) {
    for (std::size_t i = 0; i < raw.size(); ++i) {
      if (!raw[i].starts_with("--")) {
        positional_.emplace_back(raw[i]);
      } else if (std::find(flags.begin(), flags.end(), raw[i]) != flags.end()) {
        options_.emplace_back(raw[i], "");
      } else if (i + 1 < raw.size()) {
        options_.emplace_back(raw[i], raw[i + 1]);
        ++i;
      } else {
        throw std::invalid_argument("missing value for " + std::string(raw[i]));
      }
    }
  }

  const std::vector<std::string>& positional() const { return positional_; }
  bool has(std::string_view name) const { return find(name) != nullptr; }
  std::uint64_t number(std::string_view name, std::uint64_t fallback) const {
    const std::string* v = find(name);
    return v ? std::stoull(*v) : fallback;
  }
  std::string text(std::string_view name, std::string fallback) const {
    const std::string* v = find(name);
    return v ? *v : fallback;
  }

 private:
  const std::string* find(std::string_view name) const {
    for (const auto& [k, v] : options_) {
      if (k == name) return &v;
    }
    return nullptr;
  }
  std::vector<std::string> positional_;
  std::vector<std::pair<std::string, std::string>> options_;
};

// shortcut: reads the whole file into memory; switch to mmap once multi-GB captures matter.
std::vector<std::uint8_t> read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open " + path);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_file(const std::string& path, const std::vector<std::uint8_t>& data) {
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  if (!out) throw std::runtime_error("cannot write " + path);
}

std::string price_text(mde::book::Price p) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%u.%04u", p / 10'000, p % 10'000);
  return buf;
}

int cmd_decode(const std::vector<std::string_view>& raw) {
  const Args args(raw, {});
  if (args.positional().size() != 1) {
    std::fprintf(stderr, "usage: mde decode FILE [--print N]\n");
    return 2;
  }
  std::uint64_t print_limit = args.number("--print", 0);

  const std::vector<std::uint8_t> file = read_file(args.positional()[0]);
  mde::FramedReader reader{file};
  std::array<std::uint64_t, 256> count_by_type{};
  std::array<std::string_view, 256> name_by_type{};
  std::array<std::uint64_t, 5> count_by_status{};

  mde::Bytes msg;
  mde::FramedReader::Status framing;
  while ((framing = reader.next(msg)) == mde::FramedReader::Status::kOk) {
    const auto status = mde::itch::decode(msg, [&](const auto& m) {
      using T = std::remove_cvref_t<decltype(m)>;
      const auto type = static_cast<std::uint8_t>(T::kType);
      ++count_by_type[type];
      name_by_type[type] = T::kName;
      if (print_limit > 0) {
        --print_limit;
        std::printf("%s\n", mde::itch::to_string(m).c_str());
      }
    });
    ++count_by_status[static_cast<std::size_t>(status)];
  }

  std::printf("%-28s %12s\n", "message", "count");
  for (std::size_t t = 0; t < count_by_type.size(); ++t) {
    if (count_by_type[t] == 0) continue;
    std::printf("%c %-26.*s %12" PRIu64 "\n", static_cast<char>(t), static_cast<int>(name_by_type[t].size()),
                name_by_type[t].data(), count_by_type[t]);
  }
  for (std::size_t s = 1; s < count_by_status.size(); ++s) {
    if (count_by_status[s] == 0) continue;
    const auto name = mde::itch::to_string(static_cast<mde::itch::DecodeStatus>(s));
    std::printf("rejected %-19.*s %12" PRIu64 "\n", static_cast<int>(name.size()), name.data(),
                count_by_status[s]);
  }
  if (framing == mde::FramedReader::Status::kTruncated) {
    std::printf("truncated record at byte offset %zu\n", reader.offset());
    return 1;
  }
  return 0;
}

int cmd_gen(const std::vector<std::string_view>& raw) {
  const Args args(raw, {});
  if (!args.has("--out")) {
    std::fprintf(stderr, "usage: mde gen --out FILE [--seed N] [--messages N] [--symbols N]\n");
    return 2;
  }
  mde::synth::Config config;
  config.seed = args.number("--seed", config.seed);
  config.messages = args.number("--messages", config.messages);
  config.symbols = args.number("--symbols", config.symbols);
  const auto data = mde::synth::generate(config);
  write_file(args.text("--out", ""), data);
  std::printf("wrote %zu synthetic messages (%zu bytes) to %s\n", config.messages, data.size(),
              args.text("--out", "").c_str());
  return 0;
}

template <class Builder>
void print_book_report(const Builder& builder, std::size_t top) {
  using mde::book::Locate;
  using mde::book::Side;
  const auto& market = builder.market();
  std::vector<Locate> active;
  for (std::size_t loc = 1; loc < mde::book::kMaxLocates; ++loc) {
    const auto l = static_cast<Locate>(loc);
    if (builder.stats(l).volume > 0 || market.best(l, Side::kBuy) || market.best(l, Side::kSell)) active.push_back(l);
  }
  std::stable_sort(active.begin(), active.end(),
                   [&](Locate a, Locate b) { return builder.stats(a).volume > builder.stats(b).volume; });
  if (active.size() > top) active.resize(top);

  std::printf("%-8s %22s %22s %10s %14s %10s %12s\n", "symbol", "bid (qty @ px)", "ask (qty @ px)",
              "spread", "volume", "trades", "last");
  for (Locate l : active) {
    const auto bid = market.best(l, Side::kBuy);
    const auto ask = market.best(l, Side::kSell);
    auto side_text = [](const auto& level) {
      return level ? std::to_string(level->quantity) + " @ " + price_text(level->price) : std::string("-");
    };
    const std::string spread = bid && ask ? (ask->price >= bid->price ? price_text(ask->price - bid->price) : "crossed") : "-";
    const auto& s = builder.stats(l);
    std::printf("%-8.*s %22s %22s %10s %14" PRIu64 " %10" PRIu64 " %12s\n",
                static_cast<int>(mde::itch::trim(builder.symbol(l)).size()), mde::itch::trim(builder.symbol(l)).data(),
                side_text(bid).c_str(), side_text(ask).c_str(), spread.c_str(), s.volume, s.trades,
                price_text(s.last_price).c_str());
  }
}

template <class Builder>
int finish_report(const Builder& builder, bool check, std::size_t top) {
  const auto& market = builder.market();
  print_book_report(builder, top);
  const auto& c = builder.counters();
  std::printf("\nmessages %" PRIu64 ", resting orders %zu, broken trades %" PRIu64 "\n", c.messages,
              market.order_count(), c.broken_trades);
  for (std::size_t s = 1; s < c.decode.size(); ++s) {
    if (c.decode[s]) std::printf("decode rejected (%s): %" PRIu64 "\n", mde::itch::to_string(static_cast<mde::itch::DecodeStatus>(s)).data(), c.decode[s]);
  }
  for (std::size_t r = 1; r < c.book.size(); ++r) {
    if (c.book[r]) std::printf("book anomaly (%s): %" PRIu64 "\n", mde::book::to_string(static_cast<mde::book::Result>(r)).data(), c.book[r]);
  }
  std::printf("state digest %016" PRIx64 "\n", mde::book::state_digest(market));
  if (!check) return 0;
  const std::string final_check = market.check_all_invariants();
  if (c.invariant_violations || !final_check.empty()) {
    std::printf("INVARIANT VIOLATIONS: %" PRIu64 "; first: %s %s\n", c.invariant_violations,
                builder.first_violation().c_str(), final_check.c_str());
    return 1;
  }
  std::uint64_t events = 0;
  for (auto n : c.book) events += n;
  std::printf("invariants held after every book event (%" PRIu64 " checks)\n", events);
  return 0;
}

int cmd_book(const std::vector<std::string_view>& raw) {
  const Args args(raw, {"--check"});
  if (args.positional().size() != 1) {
    std::fprintf(stderr, "usage: mde book FILE [--check] [--top N]\n");
    return 2;
  }
  const bool check = args.has("--check");
  const std::vector<std::uint8_t> file = read_file(args.positional()[0]);

  mde::book::ReferenceMarket market;
  mde::book::BookBuilder builder(market, check);
  mde::FramedReader reader{file};
  mde::Bytes msg;
  mde::FramedReader::Status framing;
  while ((framing = reader.next(msg)) == mde::FramedReader::Status::kOk) builder.apply(msg);

  int rc = finish_report(builder, check, args.number("--top", 10));
  if (framing == mde::FramedReader::Status::kTruncated) {
    std::printf("truncated record at byte offset %zu\n", reader.offset());
    rc = 1;
  }
  return rc;
}

void print_sequencer_stats(const mde::moldudp64::SequencerStats& s) {
  std::printf("moldudp64: packets %" PRIu64 ", delivered %" PRIu64 ", duplicates %" PRIu64 ", out-of-order %" PRIu64
              ", gaps %" PRIu64 ", requests %" PRIu64 ", lost %" PRIu64 ", malformed %" PRIu64 ", session mismatches %" PRIu64
              ", buffer overflows %" PRIu64 "\n",
              s.packets, s.delivered, s.duplicates, s.out_of_order_packets, s.gaps_detected, s.requests_sent,
              s.messages_lost, s.malformed_packets, s.session_mismatches, s.buffer_overflows);
}

// Feeds every message `source` delivers to `builder`: inline on this thread ("none"), or on a
// separate book thread behind an SPSC ring or the mutex-queue baseline.
template <class Builder, class Source>
std::optional<mde::pipeline::PipelineStats> drive(const std::string& mode, Builder& builder, Source&& source) {
  using mde::pipeline::MessageSlot;
  if (mode == "none") {
    source([&](std::uint64_t, mde::Bytes m) { builder.apply(m); });
    return std::nullopt;
  }
  auto consume = [&](const MessageSlot& slot) { builder.apply(mde::Bytes(slot.data, slot.length)); };
  auto produce = [&](auto& push) { source([&](std::uint64_t, mde::Bytes m) { push(0, m); }); };
  if (mode == "spsc") {
    auto queue = std::make_unique<mde::pipeline::SpscQueue<MessageSlot, 1 << 16>>();
    return mde::pipeline::run(*queue, produce, consume);
  }
  if (mode == "mutex") {
    auto queue = std::make_unique<mde::pipeline::MutexQueue<MessageSlot, 1 << 16>>();
    return mde::pipeline::run(*queue, produce, consume);
  }
  throw std::invalid_argument("--pipeline must be none, spsc or mutex");
}

void print_pipeline_stats(const std::optional<mde::pipeline::PipelineStats>& p) {
  if (!p) return;
  std::printf("pipeline: %" PRIu64 " messages handed to the book thread, %" PRIu64 " full-queue waits, %" PRIu64 " oversized\n",
              p->messages, p->full_waits, p->oversized);
}

int cmd_replay(const std::vector<std::string_view>& raw) {
  const Args args(raw, {"--no-retransmit", "--check"});
  if (args.positional().size() != 1) {
    std::fprintf(stderr, "usage: mde replay FILE [--drop P] [--dup P] [--reorder P] [--seed N] [--no-retransmit]\n"
                         "                        [--packet-size BYTES] [--pipeline none|spsc|mutex] [--check] [--top N]\n");
    return 2;
  }
  const std::vector<std::uint8_t> file = read_file(args.positional()[0]);
  const mde::moldudp64::MessageLog log(mde::moldudp64::make_session(args.text("--session", "REPLAY")), file);
  mde::moldudp64::ReplayOptions options;
  options.impairments = {std::stod(args.text("--drop", "0")), std::stod(args.text("--dup", "0")),
                         std::stod(args.text("--reorder", "0")), args.number("--seed", 1)};
  options.retransmit = !args.has("--no-retransmit");
  options.max_packet_size = args.number("--packet-size", 1400);

  const bool check = args.has("--check");
  mde::book::ReferenceMarket market;
  mde::book::BookBuilder builder(market, check);
  mde::moldudp64::ReplayResult result;
  const auto pipeline = drive(args.text("--pipeline", "none"), builder,
                              [&](auto&& deliver) { result = mde::moldudp64::replay(log, options, deliver); });
  std::printf("published %" PRIu64 " packets, %" PRIu64 " retransmissions, session %s\n", result.packets_sent,
              result.retransmissions, result.complete ? "complete" : "INCOMPLETE");
  print_sequencer_stats(result.stats);
  print_pipeline_stats(pipeline);
  std::printf("\n");
  const int rc = finish_report(builder, check, args.number("--top", 10));
  return result.complete && result.stats.messages_lost == 0 ? rc : 1;
}

int cmd_serve(const std::vector<std::string_view>& raw) {
  const Args args(raw, {});
  const auto dest = mde::net::parse_endpoint(args.text("--dest", ""));
  if (args.positional().size() != 1 || !dest) {
    std::fprintf(stderr, "usage: mde serve FILE --dest IP:PORT [--bind IP:PORT] [--rate PACKETS_PER_SEC] [--drop P]\n"
                         "                       [--session NAME] [--linger-ms N] [--ttl N] [--interface IP]\n"
                         "       --dest may be a multicast group; re-requests are answered on the --bind port\n");
    return 2;
  }
  const std::vector<std::uint8_t> file = read_file(args.positional()[0]);
  const mde::moldudp64::MessageLog log(mde::moldudp64::make_session(args.text("--session", "LIVE")), file);
  const std::string bind_text = args.text("--bind", "0.0.0.0:0");
  const auto bind = mde::net::parse_endpoint(bind_text);
  if (!bind) throw std::invalid_argument("bad --bind");
  mde::net::UdpSocket socket(bind_text.substr(0, bind_text.rfind(':')), ntohs(bind->address.sin_port));
  const bool multicast = (ntohl(dest->address.sin_addr.s_addr) >> 28) == 0xE;  // 224.0.0.0/4
  if (multicast) socket.set_multicast(static_cast<int>(args.number("--ttl", 1)), /*loopback=*/true, args.text("--interface", "0.0.0.0"));

  mde::net::PublishOptions options;
  options.destination = *dest;
  options.packets_per_second = args.number("--rate", 100'000);
  options.drop = std::stod(args.text("--drop", "0"));
  options.linger = std::chrono::milliseconds(args.number("--linger-ms", 3000));
  std::printf("publishing %" PRIu64 " messages to %s; re-request server on port %u\n", log.size(),
              dest->to_string().c_str(), socket.local_port());
  std::fflush(stdout);
  const std::atomic<bool> stop{false};
  const auto s = mde::net::publish(socket, log, options, stop);
  std::printf("sent %" PRIu64 " packets, deliberately dropped %" PRIu64 ", answered %" PRIu64 " of %" PRIu64 " re-requests\n",
              s.packets_sent, s.packets_dropped, s.retransmissions, s.requests);
  return 0;
}

int cmd_listen(const std::vector<std::string_view>& raw) {
  const Args args(raw, {"--check"});
  if (!args.has("--port")) {
    std::fprintf(stderr, "usage: mde listen --port N [--bind IP] [--group IP] [--interface IP]\n"
                         "                  [--rerequest IP:PORT] [--idle-ms N] [--pipeline spsc|mutex|none] [--check] [--top N]\n");
    return 2;
  }
  std::optional<mde::net::Endpoint> rerequest;
  if (args.has("--rerequest")) {
    rerequest = mde::net::parse_endpoint(args.text("--rerequest", ""));
    if (!rerequest) throw std::invalid_argument("bad --rerequest");
  }
  mde::net::UdpSocket socket(args.text("--bind", "0.0.0.0"), static_cast<std::uint16_t>(args.number("--port", 0)),
                             args.text("--group", ""), args.text("--interface", "0.0.0.0"));
  std::printf("listening on port %u (receive buffer %d bytes)\n", socket.local_port(), socket.receive_buffer_bytes());
  std::fflush(stdout);

  const bool check = args.has("--check");
  mde::book::ReferenceMarket market;
  mde::book::BookBuilder builder(market, check);
  mde::net::ReceiveResult result;
  const auto idle = std::chrono::milliseconds(args.number("--idle-ms", 5000));
  const auto pipeline = drive(args.text("--pipeline", "spsc"), builder, [&](auto&& deliver) {
    result = mde::net::receive_session(socket, rerequest, {}, idle, deliver);
  });
  std::printf("session %s\n", result.complete ? "complete" : "INCOMPLETE (idle timeout before end of session)");
  print_sequencer_stats(result.stats);
  print_pipeline_stats(pipeline);
  std::printf("\n");
  const int rc = finish_report(builder, check, args.number("--top", 10));
  return result.complete && result.stats.messages_lost == 0 ? rc : 1;
}

int cmd_sim(const std::vector<std::string_view>& raw) {
  const Args args(raw, {});
  mde::sim::SimConfig config;
  config.seed = args.number("--seed", config.seed);
  config.steps = args.number("--steps", config.steps);
  config.accounts = args.number("--accounts", config.accounts);
  config.instruments = args.number("--instruments", config.instruments);
  config.limits.max_order_quantity = static_cast<mde::sim::Qty>(args.number("--max-qty", static_cast<std::uint64_t>(config.limits.max_order_quantity)));
  config.limits.max_order_notional = static_cast<std::int64_t>(args.number("--max-notional", static_cast<std::uint64_t>(config.limits.max_order_notional)));
  config.limits.max_position = static_cast<std::int64_t>(args.number("--max-position", static_cast<std::uint64_t>(config.limits.max_position)));
  if (config.accounts == 0 || config.instruments == 0) throw std::invalid_argument("need at least one account and instrument");

  mde::sim::Exchange exchange(config.instruments, config.limits);
  const auto s = mde::sim::simulate(config, exchange);
  std::printf("requests: %" PRIu64 " new orders, %" PRIu64 " cancels -> %" PRIu64 " execution reports\n", s.new_orders,
              s.cancels, s.reports);
  std::printf("trades: %" PRIu64 " fill reports, %" PRId64 " shares traded\n", s.fills, s.traded_quantity);
  for (std::size_t r = 1; r < s.rejects.size(); ++r) {
    if (s.rejects[r]) std::printf("rejected (%s): %" PRIu64 "\n", mde::sim::to_string(static_cast<mde::sim::RejectReason>(r)).data(), s.rejects[r]);
  }
  std::printf("\n%-8s %-10s %10s %10s %10s\n", "account", "instrument", "net", "open buy", "open sell");
  for (mde::sim::AccountId a = 0; a < config.accounts; ++a) {
    for (mde::sim::InstrumentId i = 0; i < config.instruments; ++i) {
      const auto& p = exchange.position(a, i);
      std::printf("%-8u %-10u %10" PRId64 " %10" PRId64 " %10" PRId64 "\n", a, i, p.net, p.open_buy, p.open_sell);
    }
  }
  std::printf("report digest %016" PRIx64 "\n", s.digest);
  return 0;
}

struct Command {
  std::string_view name;
  int (*run)(const std::vector<std::string_view>&);
  std::string_view help;
};

constexpr std::array kCommands = {
    Command{"decode", cmd_decode, "decode a length-prefixed ITCH 5.0 file and count messages"},
    Command{"gen", cmd_gen, "generate a synthetic ITCH 5.0 file"},
    Command{"book", cmd_book, "rebuild order books from an ITCH 5.0 file and report top of book"},
    Command{"replay", cmd_replay, "replay a file through MoldUDP64 with simulated loss and recovery"},
    Command{"serve", cmd_serve, "publish a file as MoldUDP64 over UDP and answer re-requests"},
    Command{"listen", cmd_listen, "receive a MoldUDP64 session over UDP and rebuild the books"},
    Command{"sim", cmd_sim, "run the deterministic order-entry, risk and matching simulation"},
};

void usage() {
  std::fprintf(stderr, "usage: mde <command> [args]\n\ncommands:\n");
  for (const auto& c : kCommands) {
    std::fprintf(stderr, "  %-10.*s %.*s\n", static_cast<int>(c.name.size()), c.name.data(),
                 static_cast<int>(c.help.size()), c.help.data());
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    usage();
    return 2;
  }
  const std::string_view name = argv[1];
  const std::vector<std::string_view> args(argv + 2, argv + argc);
  for (const auto& c : kCommands) {
    if (c.name != name) continue;
    try {
      return c.run(args);
    } catch (const std::exception& e) {
      std::fprintf(stderr, "mde %s: %s\n", argv[1], e.what());
      return 1;
    }
  }
  usage();
  return 2;
}
