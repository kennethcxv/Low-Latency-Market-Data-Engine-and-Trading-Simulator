// Human-readable rendering of decoded messages for the CLI and test failure output.
// Field names come from the MDE_WIRE_FIELDS declaration; values are printed raw (prices as their
// fixed-point integers) so the output is lossless.
#pragma once

#include <algorithm>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>

#include "mde/itch/codec.hpp"

namespace mde::itch {

namespace detail {

inline std::string_view next_name(std::string_view& names) {
  while (!names.empty() && (names.front() == ' ' || names.front() == ',')) names.remove_prefix(1);
  const std::size_t end = std::min(names.find(','), names.size());
  std::string_view name = names.substr(0, end);
  names.remove_prefix(end);
  return name;
}

template <class T>
void append_fields(std::string& out, const T& m);

template <class F>
void append_field(std::string& out, std::string_view& names, const F& value) {
  const std::string_view name = next_name(names);
  if constexpr (Composite<F>) {
    append_fields(out, value);
    return;
  } else {
    out += ' ';
    out += name;
    out += '=';
    if constexpr (std::is_same_v<F, Timestamp>) {
      out += std::to_string(value.ns);
    } else if constexpr (is_alpha<F>::value) {
      out += '"';
      out += trim(value);
      out += '"';
    } else if constexpr (std::is_same_v<F, char>) {
      out += value;
    } else {
      out += std::to_string(value);
    }
  }
}

template <class T>
void append_fields(std::string& out, const T& m) {
  std::string_view names = T::kFieldNames;
  std::apply([&](const auto&... f) { (append_field(out, names, f), ...); }, m.fields());
}

}  // namespace detail

template <class T>
[[nodiscard]] std::string to_string(const T& m) {
  std::string out{T::kName};
  detail::append_fields(out, m);
  return out;
}

}  // namespace mde::itch
