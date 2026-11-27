#pragma once
// String, conversion, and math helper builtins.
// Part of the split runtime support layer; included by fleaux/runtime/runtime_support.hpp.
#include "fleaux/frontend/parser.hpp"
#include <charconv>
#include <memory>
#include <string_view>
#include "fleaux/runtime/value.hpp"
namespace fleaux::runtime {
// String / conversion

namespace detail {

[[nodiscard]] inline auto skip_ascii_leading_whitespace(const std::string_view input) -> std::string_view {
  std::size_t start = 0;
  while (start < input.size()) {
    switch (input[start]) {
      case ' ':
      case '\f':
      case '\n':
      case '\r':
      case '\t':
      case '\v':
        ++start;
        break;
      default:
        return input.substr(start);
    }
  }

  return input.substr(start);
}

template <typename Number>
[[nodiscard]] inline auto parse_decimal_or_throw(std::string_view input, const char* builtin_name) -> Number {
  input = skip_ascii_leading_whitespace(input);
  if (input.empty()) {
    throw std::invalid_argument{std::string{builtin_name} + ": invalid numeric input"};
  }

  if (input.front() == '+') {
    input.remove_prefix(1);
    if (input.empty()) {
      throw std::invalid_argument{std::string{builtin_name} + ": invalid numeric input"};
    }
  }

  Number parsed_value{};
  const char* const begin = input.data();
  const char* const end = begin + input.size();

  std::from_chars_result result{};
  if constexpr (std::is_floating_point_v<Number>) {
    result = std::from_chars(begin, end, parsed_value, std::chars_format::general);
  } else {
    result = std::from_chars(begin, end, parsed_value, 10);
  }

  if (result.ec == std::errc::invalid_argument || result.ptr == begin) {
    throw std::invalid_argument{std::string{builtin_name} + ": invalid numeric input"};
  }
  if (result.ec == std::errc::result_out_of_range) {
    throw std::out_of_range{std::string{builtin_name} + ": value out of range"};
  }
  if (result.ptr != end) {
    throw std::invalid_argument{std::string{builtin_name} + ": trailing characters in input"};
  }

  return parsed_value;
}

}  // namespace detail

[[nodiscard]] inline auto ToString(Value arg) -> Value {
  return make_string(to_string(arg));
}

// arg = [string_value]
[[nodiscard]] inline auto ToNum(Value arg) -> Value {
  const Value string_value = std::move(arg);
  const std::string& str = as_string(string_value);
  // Std.ToNum is declared to return Float64, so "42" is 42.0.
  return make_float(detail::parse_decimal_or_throw<double>(str, "ToNum"));
}

[[nodiscard]] inline auto StringParseInt64(Value arg) -> Value {
  const Value string_value = std::move(arg);
  const std::string& str = as_string(string_value);

  const auto parsed_number = detail::parse_decimal_or_throw<std::int64_t>(str, "ParseInt64");
  return make_int(parsed_number);
}

[[nodiscard]] inline auto StringParseUInt64(Value arg) -> Value {
  const Value string_value = std::move(arg);
  const std::string& str = as_string(string_value);

  const auto parsed_number = detail::parse_decimal_or_throw<std::uint64_t>(str, "ParseUInt64");
  return make_uint(parsed_number);
}

[[nodiscard]] inline auto StringParseFloat64(Value arg) -> Value {
  const Value string_value = std::move(arg);
  const std::string& str = as_string(string_value);
  const auto parsed_number = detail::parse_decimal_or_throw<double>(str, "ParseFloat64");
  return make_float(parsed_number);
}

[[nodiscard]] inline auto StringUpper(Value arg) -> Value {
  const Value string_value = std::move(arg);
  std::string str = as_string(string_value);
  std::ranges::transform(str, str.begin(),
                         [](const unsigned char ch) -> char { return static_cast<char>(std::toupper(ch)); });
  return make_string(std::move(str));
}

[[nodiscard]] inline auto StringLower(Value arg) -> Value {
  const Value string_value = std::move(arg);
  std::string str = as_string(string_value);
  std::ranges::transform(str, str.begin(),
                         [](const unsigned char ch) -> char { return static_cast<char>(std::tolower(ch)); });
  return make_string(std::move(str));
}

[[nodiscard]] inline auto StringTrim(Value arg) -> Value {
  const Value string_value = std::move(arg);
  std::string str = as_string(string_value);
  return make_string(trim_right(trim_left(std::move(str))));
}

[[nodiscard]] inline auto StringTrimStart(Value arg) -> Value {
  const Value string_value = std::move(arg);
  std::string str = as_string(string_value);
  return make_string(trim_left(std::move(str)));
}

[[nodiscard]] inline auto StringTrimEnd(Value arg) -> Value {
  const Value string_value = std::move(arg);
  std::string str = as_string(string_value);
  return make_string(trim_right(std::move(str)));
}

[[nodiscard]] inline auto StringSplit(const Arguments& args) -> Value {
  const std::string input = as_string(args[0]);
  const std::string sep = as_string(args[1]);
  if (sep.empty()) {
    throw std::invalid_argument{"StringSplit separator cannot be empty"};
  }

  std::size_t pieces = 1;
  for (std::size_t found = input.find(sep); found != std::string::npos; found = input.find(sep, found + sep.size())) {
    ++pieces;
  }
  TupleBuilder out(pieces);
  std::size_t pos = 0;
  while (true) {
    const std::size_t found = input.find(sep, pos);
    if (found == std::string::npos) {
      out.push(make_string(input.substr(pos)));
      break;
    }
    out.push(make_string(input.substr(pos, found - pos)));
    pos = found + sep.size();
  }
  return std::move(out).finish();
}

[[nodiscard]] inline auto StringJoin(const Arguments& args) -> Value {
  const std::string& sep = as_string(args[0]);
  const TupleView parts = as_tuple(args[1]);
  std::string joined;
  for (std::size_t part_index = 0; part_index < parts.size(); ++part_index) {
    if (part_index > 0) {
      joined += sep;
    }
    append_value_repr(joined, parts[part_index]);
  }
  return make_string(std::move(joined));
}

[[nodiscard]] inline auto StringReplace(const Arguments& args) -> Value {
  std::string str = as_string(args[0]);
  const std::string old_s = as_string(args[1]);
  const std::string new_s = as_string(args[2]);
  if (old_s.empty()) {
    return make_string(std::move(str));
  }
  std::size_t pos = 0;
  while ((pos = str.find(old_s, pos)) != std::string::npos) {
    str.replace(pos, old_s.size(), new_s);
    pos += new_s.size();
  }
  return make_string(std::move(str));
}

[[nodiscard]] inline auto StringContains(const Arguments& args) -> Value {
  const std::string str = as_string(args[0]);
  const std::string sub = as_string(args[1]);
  return make_bool(str.find(sub) != std::string::npos);
}

[[nodiscard]] inline auto StringStartsWith(const Arguments& args) -> Value {
  const std::string str = as_string(args[0]);
  const std::string prefix = as_string(args[1]);
  return make_bool(str.starts_with(prefix));
}

[[nodiscard]] inline auto StringEndsWith(const Arguments& args) -> Value {
  const std::string str = as_string(args[0]);
  const std::string suffix = as_string(args[1]);
  if (suffix.size() > str.size()) {
    return make_bool(false);
  }
  return make_bool(str.ends_with(suffix));
}

[[nodiscard]] inline auto StringLength(Value arg) -> Value {
  const Value string_value = std::move(arg);
  const std::string& str = as_string(string_value);
  return make_int(static_cast<Int>(str.size()));
}

// args = [str, index] -> 1-char string or "" when out of range
[[nodiscard]] inline auto StringCharAt(const Arguments& args) -> Value {
  const std::string str = as_string(args[0]);
  const std::size_t idx = uint64_index(args[1]);
  if (idx >= str.size()) {
    return make_string("");
  }
  return make_string(str.substr(idx, 1));
}

[[nodiscard]] inline auto string_slice_impl(const std::string& str, std::size_t start, std::size_t stop) -> Value {
  if (start > str.size())
    start = str.size();
  if (stop > str.size())
    stop = str.size();
  if (stop < start)
    stop = start;
  return make_string(str.substr(start, stop - start));
}

[[nodiscard]] inline auto StringSliceStringUInt64(const Arguments& args) -> Value {
  const std::string str = as_string(args[0]);
  const std::size_t stop = uint64_index(args[1]);
  return string_slice_impl(str, 0, stop);
}

[[nodiscard]] inline auto StringSliceStringUInt64UInt64(const Arguments& args) -> Value {
  const std::string str = as_string(args[0]);
  const std::size_t start = uint64_index(args[1]);
  const std::size_t stop = uint64_index(args[2]);
  return string_slice_impl(str, start, stop);
}

[[nodiscard]] inline auto string_find_impl(const std::string& str, const std::string& needle, const std::size_t start) -> Value {
  if (start > str.size()) {
    return make_int(-1);
  }
  const auto pos = str.find(needle, start);
  if (pos == std::string::npos) {
    return make_int(-1);
  }
  return make_int(static_cast<Int>(pos));
}

[[nodiscard]] inline auto StringFindStringString(const Arguments& args) -> Value {
  return string_find_impl(as_string(args[0]), as_string(args[1]), 0);
}

[[nodiscard]] inline auto StringFindStringStringUInt64(const Arguments& args) -> Value {
  return string_find_impl(as_string(args[0]), as_string(args[1]),
                          uint64_index(args[2]));
}

// args = [format, arg0, arg1, ...]
// Returns the formatted string without printing.
[[nodiscard]] inline auto StringFormat(const Arguments& args) -> Value {
  if (args.size() == 0U) {
    throw std::logic_error{"internal error: StringFormat called with unexpected validated arity"};
  }
  return make_string(format_values(as_string(args[0]), args.items().subspan(1U)));
}

// The regex builtins, defined in src/vm/builtins_regex.cpp: only the VM links PCRE2.
[[nodiscard]] auto StringRegexIsMatch(const Arguments& args) -> Value;
[[nodiscard]] auto StringRegexFind(const Arguments& args) -> Value;
[[nodiscard]] auto StringRegexReplace(const Arguments& args) -> Value;
[[nodiscard]] auto StringRegexSplit(const Arguments& args) -> Value;

}  // namespace fleaux::runtime
