// mde: command-line front end. Each subcommand is a thin layer over the libraries.
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "mde/common.hpp"
#include "mde/itch/codec.hpp"
#include "mde/itch/format.hpp"

namespace {

// shortcut: reads the whole file into memory; switch to mmap once multi-GB captures matter.
std::vector<std::uint8_t> read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open " + path);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

int cmd_decode(const std::vector<std::string_view>& args) {
  if (args.empty()) {
    std::fprintf(stderr, "usage: mde decode FILE [--print N]\n");
    return 2;
  }
  long print_limit = 0;
  for (std::size_t i = 1; i + 1 < args.size(); i += 2) {
    if (args[i] == "--print") print_limit = std::stol(std::string(args[i + 1]));
  }

  const std::vector<std::uint8_t> file = read_file(std::string(args[0]));
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
    std::printf("%c %-26.*s %12llu\n", static_cast<char>(t), static_cast<int>(name_by_type[t].size()),
                name_by_type[t].data(), static_cast<unsigned long long>(count_by_type[t]));
  }
  for (std::size_t s = 1; s < count_by_status.size(); ++s) {
    if (count_by_status[s] == 0) continue;
    const auto name = mde::itch::to_string(static_cast<mde::itch::DecodeStatus>(s));
    std::printf("rejected %-19.*s %12llu\n", static_cast<int>(name.size()), name.data(),
                static_cast<unsigned long long>(count_by_status[s]));
  }
  if (framing == mde::FramedReader::Status::kTruncated) {
    std::printf("truncated record at byte offset %zu\n", reader.offset());
    return 1;
  }
  return 0;
}

struct Command {
  std::string_view name;
  int (*run)(const std::vector<std::string_view>&);
  std::string_view help;
};

constexpr std::array kCommands = {
    Command{"decode", cmd_decode, "decode a length-prefixed ITCH 5.0 file and count messages"},
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
