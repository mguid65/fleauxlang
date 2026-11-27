#pragma once
// String, conversion, and math helper builtins.
// Part of the split runtime support layer; included by fleaux/runtime/runtime_support.hpp.
#include "fleaux/frontend/parser.hpp"
#include "fleaux/runtime/builtins_core.hpp"
#include "fleaux/runtime/value.hpp"
#include <charconv>
#include <format>
#include <memory>
#include <optional>
#include <string_view>
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
[[nodiscard]] constexpr auto parsed_type_name() -> std::string_view {
  if constexpr (std::is_floating_point_v<Number>) {
    return "Float64";
  } else if constexpr (std::is_signed_v<Number>) {
    return "Int64";
  } else {
    return "UInt64";
  }
}

template <typename Number>
[[nodiscard]] constexpr auto parsed_type_article() -> std::string_view {
  return std::is_signed_v<Number> && !std::is_floating_point_v<Number> ? "an" : "a";
}

// The number `text` holds in decimal, or nothing, with why in `error`. Leading ASCII whitespace and a '+' are allowed.
template <typename Number>
[[nodiscard]] inline auto parse_decimal(const std::string_view text, std::string& error) -> std::optional<Number> {
  std::string_view input = skip_ascii_leading_whitespace(text);
  const auto not_a_number = [&]() -> std::optional<Number> {
    error = std::format("'{}' is not {} {}", text, parsed_type_article<Number>(), parsed_type_name<Number>());
    return std::nullopt;
  };
  if (input.empty()) {
    return not_a_number();
  }

  if (input.front() == '+') {
    input.remove_prefix(1);
    if (input.empty()) {
      return not_a_number();
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
    return not_a_number();
  }
  if (result.ec == std::errc::result_out_of_range) {
    error = std::format("'{}' is out of range for {} {}", text, parsed_type_article<Number>(), parsed_type_name<Number>());
    return std::nullopt;
  }
  if (result.ptr != end) {
    error = std::format("'{}' has characters after the number", text);
    return std::nullopt;
  }

  return parsed_value;
}

// Std.String.ParseInt64 and the like: Ok(number), or Err with why the text is not one.
template <typename Number, typename Make>
[[nodiscard]] inline auto parse_result(const Value& text, const std::string_view builtin, Make&& make) -> Value {
  std::string error;
  if (const auto parsed = parse_decimal<Number>(as_string(text), error); parsed.has_value()) {
    return ResultOk(std::forward<Make>(make)(*parsed));
  }
  return ResultErr(make_string(std::format("{}: {}", builtin, error)));
}

}  // namespace detail

[[nodiscard]] inline auto ToString(Value arg) -> Value {
  return make_string(to_string(arg));
}

[[nodiscard]] inline auto StringParseInt64(Value arg) -> Value {
  return detail::parse_result<std::int64_t>(arg, "Std.String.ParseInt64", [](const Int value) { return make_int(value); });
}

[[nodiscard]] inline auto StringParseUInt64(Value arg) -> Value {
  return detail::parse_result<std::uint64_t>(arg, "Std.String.ParseUInt64", [](const UInt value) { return make_uint(value); });
}

[[nodiscard]] inline auto StringParseFloat64(Value arg) -> Value {
  return detail::parse_result<double>(arg, "Std.String.ParseFloat64", [](const Float value) { return make_float(value); });
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
    throw std::invalid_argument{"the separator cannot be empty"};
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

// args = [str, index] -> the byte at index, as a string of one
[[nodiscard]] inline auto StringCharAt(const Arguments& args) -> Value {
  const std::string& str = as_string(args[0]);
  const std::size_t idx = position_index(args[1]);
  if (idx >= str.size()) {
    throw std::out_of_range{
        std::format("index {} is out of range for a string of {} byte{}", to_string(args[1]), str.size(), str.size() == 1U ? "" : "s")};
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

[[nodiscard]] inline auto StringSliceStringInt64(const Arguments& args) -> Value {
  const std::string str = as_string(args[0]);
  const std::size_t stop = count_index(args[1], "stop");
  return string_slice_impl(str, 0, stop);
}

[[nodiscard]] inline auto StringSliceStringInt64Int64(const Arguments& args) -> Value {
  const std::string str = as_string(args[0]);
  const std::size_t start = count_index(args[1], "start");
  const std::size_t stop = count_index(args[2], "stop");
  return string_slice_impl(str, start, stop);
}

// Ok(the position of the first `needle` from `start`), or an Err when there is none.
[[nodiscard]] inline auto string_find_impl(const std::string& str, const std::string& needle, const std::size_t start) -> Value {
  if (const auto pos = start > str.size() ? std::string::npos : str.find(needle, start); pos != std::string::npos) {
    return ResultOk(make_int(static_cast<Int>(pos)));
  }
  return ResultErr(make_string(std::format("Std.String.Find: '{}' is not in the string", needle)));
}

[[nodiscard]] inline auto StringFindStringString(const Arguments& args) -> Value {
  return string_find_impl(as_string(args[0]), as_string(args[1]), 0);
}

[[nodiscard]] inline auto StringFindStringStringInt64(const Arguments& args) -> Value {
  return string_find_impl(as_string(args[0]), as_string(args[1]), count_index(args[2], "start"));
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

// args = [str, count] -> str count times over
[[nodiscard]] inline auto StringRepeat(const Arguments& args) -> Value {
  const std::string& str = as_string(args[0]);
  const std::size_t count = count_index(args[1], "count");
  if (!str.empty() && count > std::string{}.max_size() / str.size()) {
    throw std::length_error{std::format("{} copies of {} bytes are too long for a string", count, str.size())};
  }
  std::string out;
  out.reserve(str.size() * count);
  for (std::size_t index = 0; index < count; ++index) {
    out += str;
  }
  return make_string(std::move(out));
}

// The bytes of a string in the other order. A string is bytes, as Std.String.Length counts them.
[[nodiscard]] inline auto StringReverse(Value arg) -> Value {
  std::string reversed = as_string(arg);
  std::ranges::reverse(reversed);
  return make_string(std::move(reversed));
}

// arg = str -> each byte, as a string of one, as Std.String.CharAt gives it
[[nodiscard]] inline auto StringChars(Value arg) -> Value {
  const std::string& str = as_string(arg);
  TupleBuilder out(str.size());
  for (const char byte : str) {
    out.push(make_string(std::string(1U, byte)));
  }
  return std::move(out).finish();
}

}  // namespace fleaux::runtime
