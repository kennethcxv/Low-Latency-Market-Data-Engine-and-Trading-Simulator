// Small building blocks shared by every component: big-endian loads/stores, length-prefixed
// framing, a deterministic PRNG and a stable hash for state digests.
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace mde {

using Bytes = std::span<const std::uint8_t>;

// Apple Silicon reports 128-byte lines (sysctl hw.cachelinesize); on x86 the adjacent-line
// prefetcher pulls 64-byte lines in pairs. 128 avoids false sharing on both.
inline constexpr std::size_t kCacheLine = 128;

// ---- Byte order -------------------------------------------------------------------------------
// memcpy keeps these alignment- and aliasing-safe; GCC and Clang lower each to one load + rev.

[[nodiscard]] inline std::uint16_t load_be16(const std::uint8_t* p) noexcept {
  std::uint16_t v;
  std::memcpy(&v, p, sizeof v);
  if constexpr (std::endian::native == std::endian::little) v = __builtin_bswap16(v);
  return v;
}

[[nodiscard]] inline std::uint32_t load_be32(const std::uint8_t* p) noexcept {
  std::uint32_t v;
  std::memcpy(&v, p, sizeof v);
  if constexpr (std::endian::native == std::endian::little) v = __builtin_bswap32(v);
  return v;
}

[[nodiscard]] inline std::uint64_t load_be64(const std::uint8_t* p) noexcept {
  std::uint64_t v;
  std::memcpy(&v, p, sizeof v);
  if constexpr (std::endian::native == std::endian::little) v = __builtin_bswap64(v);
  return v;
}

// ITCH timestamps are 6-byte big-endian integers.
[[nodiscard]] inline std::uint64_t load_be48(const std::uint8_t* p) noexcept {
  return (std::uint64_t{load_be16(p)} << 32) | load_be32(p + 2);
}

inline void store_be16(std::uint8_t* p, std::uint16_t v) noexcept {
  if constexpr (std::endian::native == std::endian::little) v = __builtin_bswap16(v);
  std::memcpy(p, &v, sizeof v);
}

inline void store_be32(std::uint8_t* p, std::uint32_t v) noexcept {
  if constexpr (std::endian::native == std::endian::little) v = __builtin_bswap32(v);
  std::memcpy(p, &v, sizeof v);
}

inline void store_be64(std::uint8_t* p, std::uint64_t v) noexcept {
  if constexpr (std::endian::native == std::endian::little) v = __builtin_bswap64(v);
  std::memcpy(p, &v, sizeof v);
}

inline void store_be48(std::uint8_t* p, std::uint64_t v) noexcept {
  store_be16(p, static_cast<std::uint16_t>(v >> 32));
  store_be32(p + 2, static_cast<std::uint32_t>(v));
}

// ---- Framing ----------------------------------------------------------------------------------
// [u16 big-endian length][length bytes], repeated. This is both the MoldUDP64 message block and
// the framing of Nasdaq's historical ITCH files, so one bounds-checked reader serves both.
class FramedReader {
 public:
  enum class Status : std::uint8_t { kOk, kEnd, kTruncated };

  explicit FramedReader(Bytes buf) noexcept : buf_(buf) {}

  Status next(Bytes& out) noexcept {
    const std::size_t left = buf_.size() - pos_;
    if (left == 0) return Status::kEnd;
    if (left < 2) return Status::kTruncated;
    const std::size_t len = load_be16(buf_.data() + pos_);
    if (left - 2 < len) return Status::kTruncated;
    out = buf_.subspan(pos_ + 2, len);
    pos_ += 2 + len;
    return Status::kOk;
  }

  [[nodiscard]] std::size_t offset() const noexcept { return pos_; }

 private:
  Bytes buf_;
  std::size_t pos_ = 0;
};

// ---- Determinism ------------------------------------------------------------------------------
// SplitMix64. Used instead of <random> distributions because their output differs between
// libstdc++ and libc++, which would make fixtures and digests platform-dependent.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) noexcept : s_(seed) {}

  std::uint64_t next() noexcept {
    std::uint64_t z = (s_ += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
  }

  // Uniform in [0, n). Modulo bias is < n / 2^64, irrelevant at the ranges used here.
  std::uint64_t below(std::uint64_t n) noexcept { return next() % n; }

  // True with probability `percent`/100.
  bool percent(unsigned percent) noexcept { return below(100) < percent; }

 private:
  std::uint64_t s_;
};

// FNV-1a over values fed in a fixed byte order, so digests match across hosts.
class Digest {
 public:
  void add(std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
      h_ ^= (v >> (8 * i)) & 0xff;
      h_ *= 0x100000001b3ULL;
    }
  }
  [[nodiscard]] std::uint64_t value() const noexcept { return h_; }

 private:
  std::uint64_t h_ = 0xcbf29ce484222325ULL;
};

}  // namespace mde
