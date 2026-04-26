#pragma once
// String, conversion, and math helper builtins.
// Part of the split runtime support layer; included by fleaux/runtime/runtime_support.hpp.
#include <memory>
#if defined(__has_include)
#if __has_include(<pcre2.h>)
#ifndef PCRE2_CODE_UNIT_WIDTH
#define PCRE2_CODE_UNIT_WIDTH 8
#endif
#include <pcre2.h>
#define FLEAUX_HAS_PCRE2 1
#else
#define FLEAUX_HAS_PCRE2 0
#endif
#else
#define FLEAUX_HAS_PCRE2 0
#endif
#include "fleaux/runtime/value.hpp"
namespace fleaux::runtime {
// String / conversion

namespace detail {

#if FLEAUX_HAS_PCRE2

struct RegexCodeDeleter {
  void operator()(pcre2_code* code) const {
    if (code != nullptr) { pcre2_code_free(code); }
  }
};

struct RegexMatchDataDeleter {
  void operator()(pcre2_match_data* data) const {
    if (data != nullptr) { pcre2_match_data_free(data); }
  }
};

using RegexCodePtr = std::unique_ptr<pcre2_code, RegexCodeDeleter>;
using RegexMatchDataPtr = std::unique_ptr<pcre2_match_data, RegexMatchDataDeleter>;

[[nodiscard]] inline auto regex_compile_or_throw(const std::string& pattern) -> RegexCodePtr {
  int error_code = 0;
  PCRE2_SIZE error_offset = 0;
  pcre2_code* compiled = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.c_str()), PCRE2_ZERO_TERMINATED, 0,
                                       &error_code, &error_offset, nullptr);
  if (compiled == nullptr) {
    PCRE2_UCHAR message[256] = {0};
    const int msg_rc = pcre2_get_error_message(error_code, message, sizeof(message));
    const std::string msg = (msg_rc >= 0) ? reinterpret_cast<const char*>(message) : std::string{"unknown regex error"};
    throw std::invalid_argument{"Regex compile failed at offset " + std::to_string(error_offset) + ": " + msg};
  }
  return RegexCodePtr{compiled};
}

[[nodiscard]] inline auto regex_match_data_or_throw(const pcre2_code* code) -> RegexMatchDataPtr {
  pcre2_match_data* data = pcre2_match_data_create_from_pattern(code, nullptr);
  if (data == nullptr) { throw std::runtime_error{"Regex: failed to allocate match data"}; }
  return RegexMatchDataPtr{data};
}

#endif

}  // namespace detail

[[nodiscard]] inline auto ToString(Value arg) -> Value {
  return make_string(to_string(unwrap_singleton_arg(std::move(arg))));
}

// arg = [string_value]
[[nodiscard]] inline auto ToNum(Value arg) -> Value {
  const Value string_value = unwrap_singleton_arg(std::move(arg));
  const std::string& str = as_string(string_value);
  std::size_t consumed = 0;
  const double parsed_number = std::stod(str, &consumed);
  if (consumed != str.size()) { throw std::invalid_argument{"ToNum: trailing characters in input"}; }
  return num_result(parsed_number);
}

[[nodiscard]] inline auto StringUpper(Value arg) -> Value {
  std::string str = to_string(unwrap_singleton_arg(std::move(arg)));
  std::ranges::transform(str, str.begin(),
                         [](const unsigned char ch) -> char { return static_cast<char>(std::toupper(ch)); });
  return make_string(std::move(str));
}

[[nodiscard]] inline auto StringLower(Value arg) -> Value {
  std::string str = to_string(unwrap_singleton_arg(std::move(arg)));
  std::ranges::transform(str, str.begin(),
                         [](const unsigned char ch) -> char { return static_cast<char>(std::tolower(ch)); });
  return make_string(std::move(str));
}

[[nodiscard]] inline auto StringTrim(Value arg) -> Value {
  std::string str = to_string(unwrap_singleton_arg(std::move(arg)));
  return make_string(trim_right(trim_left(std::move(str))));
}

[[nodiscard]] inline auto StringTrimStart(Value arg) -> Value {
  std::string str = to_string(unwrap_singleton_arg(std::move(arg)));
  return make_string(trim_left(std::move(str)));
}

[[nodiscard]] inline auto StringTrimEnd(Value arg) -> Value {
  std::string str = to_string(unwrap_singleton_arg(std::move(arg)));
  return make_string(trim_right(std::move(str)));
}

[[nodiscard]] inline auto StringSplit(Value arg) -> Value {
  const auto& args = require_args(arg, 2, "StringSplit");
  const std::string input = to_string(*args.TryGet(0));
  const std::string sep = to_string(*args.TryGet(1));
  if (sep.empty()) { throw std::invalid_argument{"StringSplit separator cannot be empty"}; }

  Array out;
  std::size_t pos = 0;
  while (true) {
    const std::size_t found = input.find(sep, pos);
    if (found == std::string::npos) {
      out.PushBack(make_string(input.substr(pos)));
      break;
    }
    out.PushBack(make_string(input.substr(pos, found - pos)));
    pos = found + sep.size();
  }
  return Value{std::move(out)};
}

[[nodiscard]] inline auto StringJoin(Value arg) -> Value {
  const auto& args = require_args(arg, 2, "StringJoin");
  const std::string sep = to_string(*args.TryGet(0));
  std::ostringstream oss;
  const Value& parts_v = *args.TryGet(1);
  if (parts_v.HasArray()) {
    const auto& parts = as_array(parts_v);
    for (std::size_t part_index = 0; part_index < parts.Size(); ++part_index) {
      if (part_index > 0) { oss << sep; }
      oss << to_string(*parts.TryGet(part_index));
    }
    return make_string(oss.str());
  }

  // Python parity: joining over a non-tuple second arg iterates its string form.
  const std::string str = to_string(parts_v);
  for (std::size_t char_index = 0; char_index < str.size(); ++char_index) {
    if (char_index > 0) { oss << sep; }
    oss << str[char_index];
  }
  return make_string(oss.str());
}

[[nodiscard]] inline auto StringReplace(Value arg) -> Value {
  const auto& args = require_args(arg, 3, "StringReplace");
  std::string str = to_string(*args.TryGet(0));
  const std::string old_s = to_string(*args.TryGet(1));
  const std::string new_s = to_string(*args.TryGet(2));
  if (old_s.empty()) { return make_string(std::move(str)); }
  std::size_t pos = 0;
  while ((pos = str.find(old_s, pos)) != std::string::npos) {
    str.replace(pos, old_s.size(), new_s);
    pos += new_s.size();
  }
  return make_string(std::move(str));
}

[[nodiscard]] inline auto StringContains(Value arg) -> Value {
  const auto& args = require_args(arg, 2, "StringContains");
  const std::string str = to_string(*args.TryGet(0));
  const std::string sub = to_string(*args.TryGet(1));
  return make_bool(str.find(sub) != std::string::npos);
}

[[nodiscard]] inline auto StringStartsWith(Value arg) -> Value {
  const auto& args = require_args(arg, 2, "StringStartsWith");
  const std::string str = to_string(*args.TryGet(0));
  const std::string prefix = to_string(*args.TryGet(1));
  return make_bool(str.starts_with(prefix));
}

[[nodiscard]] inline auto StringEndsWith(Value arg) -> Value {
  const auto& args = require_args(arg, 2, "StringEndsWith");
  const std::string str = to_string(*args.TryGet(0));
  const std::string suffix = to_string(*args.TryGet(1));
  if (suffix.size() > str.size()) { return make_bool(false); }
  return make_bool(str.ends_with(suffix));
}

[[nodiscard]] inline auto StringLength(Value arg) -> Value {
  const std::string str = to_string(unwrap_singleton_arg(std::move(arg)));
  return make_int(static_cast<Int>(str.size()));
}

// arg = [str, index] -> 1-char string or "" when out of range
[[nodiscard]] inline auto StringCharAt(Value arg) -> Value {
  const auto& args = require_args(arg, 2, "StringCharAt");
  const std::string str = to_string(*args.TryGet(0));
  const std::size_t idx = as_index_strict(*args.TryGet(1), "StringCharAt index");
  if (idx >= str.size()) { return make_string(""); }
  return make_string(str.substr(idx, 1));
}

// arg = [str, stop] | [str, start, stop]
[[nodiscard]] inline auto StringSlice(Value arg) -> Value {
  const auto& args = as_array(arg);
  if (args.Size() != 2 && args.Size() != 3) { throw std::invalid_argument{"StringSlice expects 2 or 3 arguments"}; }
  const std::string str = to_string(*args.TryGet(0));
  std::size_t start = 0;
  std::size_t stop = 0;
  if (args.Size() == 2) {
    stop = as_index_strict(*args.TryGet(1), "StringSlice stop");
  } else {
    start = as_index_strict(*args.TryGet(1), "StringSlice start");
    stop = as_index_strict(*args.TryGet(2), "StringSlice stop");
  }
  if (start > str.size()) start = str.size();
  if (stop > str.size()) stop = str.size();
  if (stop < start) stop = start;
  return make_string(str.substr(start, stop - start));
}

// arg = [str, needle] | [str, needle, start]
[[nodiscard]] inline auto StringFind(Value arg) -> Value {
  const auto& args = as_array(arg);
  if (args.Size() != 2 && args.Size() != 3) { throw std::invalid_argument{"StringFind expects 2 or 3 arguments"}; }
  const std::string str = to_string(*args.TryGet(0));
  const std::string needle = to_string(*args.TryGet(1));
  std::size_t start = 0;
  if (args.Size() == 3) { start = as_index_strict(*args.TryGet(2), "StringFind start"); }
  if (start > str.size()) { return make_int(-1); }
  const auto pos = str.find(needle, start);
  if (pos == std::string::npos) { return make_int(-1); }
  return make_int(static_cast<Int>(pos));
}

// arg = [format, arg0, arg1, ...]
// Returns the formatted string without printing.
[[nodiscard]] inline auto StringFormat(Value arg) -> Value {
  const auto& args = as_array(arg);
  if (args.Size() < 1) { throw std::invalid_argument{"String.Format expects at least 1 argument"}; }
  const std::string fmt = to_string(*args.TryGet(0));
  std::vector<Value> values;
  values.reserve(args.Size() > 0 ? args.Size() - 1 : 0);
  for (std::size_t arg_index = 1; arg_index < args.Size(); ++arg_index) { values.push_back(*args.TryGet(arg_index)); }
  return make_string(format_values(fmt, values));
}

[[nodiscard]] inline auto StringRegexIsMatch(const Value& arg) -> Value {
#if FLEAUX_HAS_PCRE2
  const auto& args = require_args(arg, 2, "StringRegexIsMatch");
  const std::string str = to_string(*args.TryGet(0));
  const std::string pattern = to_string(*args.TryGet(1));

  const auto code = detail::regex_compile_or_throw(pattern);
  const auto match_data = detail::regex_match_data_or_throw(code.get());
  const int rc =
      pcre2_match(code.get(), reinterpret_cast<PCRE2_SPTR>(str.c_str()), str.size(), 0, 0, match_data.get(), nullptr);
  if (rc == PCRE2_ERROR_NOMATCH) { return make_bool(false); }
  if (rc < 0) { throw std::runtime_error{"StringRegexIsMatch: match failed with code " + std::to_string(rc)}; }
  return make_bool(true);
#else
  (void)arg;
  throw std::runtime_error{"StringRegexIsMatch: regex support unavailable (PCRE2 header not found)"};
#endif
}

[[nodiscard]] inline auto StringRegexFind(const Value& arg) -> Value {
#if FLEAUX_HAS_PCRE2
  const auto& args = require_args(arg, 2, "StringRegexFind");
  const std::string str = to_string(*args.TryGet(0));
  const std::string pattern = to_string(*args.TryGet(1));

  const auto code = detail::regex_compile_or_throw(pattern);
  const auto match_data = detail::regex_match_data_or_throw(code.get());
  const int rc =
      pcre2_match(code.get(), reinterpret_cast<PCRE2_SPTR>(str.c_str()), str.size(), 0, 0, match_data.get(), nullptr);
  if (rc == PCRE2_ERROR_NOMATCH) { return make_int(-1); }
  if (rc < 0) { throw std::runtime_error{"StringRegexFind: match failed with code " + std::to_string(rc)}; }
  const PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(match_data.get());
  return make_int(static_cast<Int>(ovector[0]));
#else
  (void)arg;
  throw std::runtime_error{"StringRegexFind: regex support unavailable (PCRE2 header not found)"};
#endif
}

[[nodiscard]] inline auto StringRegexReplace(const Value& arg) -> Value {
#if FLEAUX_HAS_PCRE2
  const auto& args = require_args(arg, 3, "StringRegexReplace");
  const std::string str = to_string(*args.TryGet(0));
  const std::string pattern = to_string(*args.TryGet(1));
  const std::string repl = to_string(*args.TryGet(2));

  const auto code = detail::regex_compile_or_throw(pattern);
  const auto match_data = detail::regex_match_data_or_throw(code.get());

  std::string out;
  std::size_t search_from = 0;
  std::size_t copy_from = 0;
  while (search_from <= str.size()) {
    const int rc = pcre2_match(code.get(), reinterpret_cast<PCRE2_SPTR>(str.c_str()), str.size(), search_from, 0,
                               match_data.get(), nullptr);
    if (rc == PCRE2_ERROR_NOMATCH) { break; }
    if (rc < 0) { throw std::runtime_error{"StringRegexReplace: match failed with code " + std::to_string(rc)}; }

    const PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(match_data.get());
    const auto begin = static_cast<std::size_t>(ovector[0]);
    const auto end = static_cast<std::size_t>(ovector[1]);
    if (end < begin || begin > str.size() || end > str.size()) {
      throw std::runtime_error{"StringRegexReplace: invalid match bounds"};
    }
    out.append(str, copy_from, begin - copy_from);
    out += repl;
    copy_from = end;
    if (end == begin) {
      if (search_from >= str.size()) { break; }
      search_from += 1;
    } else {
      search_from = end;
    }
  }
  out.append(str, copy_from, std::string::npos);
  return make_string(std::move(out));
#else
  (void)arg;
  throw std::runtime_error{"StringRegexReplace: regex support unavailable (PCRE2 header not found)"};
#endif
}

[[nodiscard]] inline auto StringRegexSplit(const Value& arg) -> Value {
#if FLEAUX_HAS_PCRE2
  const auto& args = require_args(arg, 2, "StringRegexSplit");
  const std::string str = to_string(*args.TryGet(0));
  const std::string pattern = to_string(*args.TryGet(1));

  const auto code = detail::regex_compile_or_throw(pattern);
  const auto match_data = detail::regex_match_data_or_throw(code.get());

  Array out;
  std::size_t search_from = 0;
  std::size_t copy_from = 0;
  while (search_from <= str.size()) {
    const int rc = pcre2_match(code.get(), reinterpret_cast<PCRE2_SPTR>(str.c_str()), str.size(), search_from, 0,
                               match_data.get(), nullptr);
    if (rc == PCRE2_ERROR_NOMATCH) { break; }
    if (rc < 0) { throw std::runtime_error{"StringRegexSplit: match failed with code " + std::to_string(rc)}; }

    const PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(match_data.get());
    const auto begin = static_cast<std::size_t>(ovector[0]);
    const auto end = static_cast<std::size_t>(ovector[1]);
    if (end < begin || begin > str.size() || end > str.size()) {
      throw std::runtime_error{"StringRegexSplit: invalid match bounds"};
    }
    out.PushBack(make_string(str.substr(copy_from, begin - copy_from)));
    copy_from = end;
    if (end == begin) {
      if (search_from >= str.size()) { break; }
      search_from += 1;
    } else {
      search_from = end;
    }
  }
  out.PushBack(make_string(str.substr(copy_from)));
  return Value{std::move(out)};
#else
  (void)arg;
  throw std::runtime_error{"StringRegexSplit: regex support unavailable (PCRE2 header not found)"};
#endif
}

}  // namespace fleaux::runtime
