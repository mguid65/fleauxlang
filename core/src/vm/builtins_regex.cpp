// The regex builtins (Std.String.Regex.*), on PCRE2. They are out of line so that only the VM, which links PCRE2,
// needs its header.
#ifndef PCRE2_CODE_UNIT_WIDTH
#define PCRE2_CODE_UNIT_WIDTH 8
#endif
#include <pcre2.h>

#include <memory>
#include <stdexcept>
#include <string>

#include "fleaux/runtime/builtins_string.hpp"

namespace fleaux::runtime {
namespace {

struct RegexCodeDeleter {
  void operator()(pcre2_code* code) const {
    if (code != nullptr) {
      pcre2_code_free(code);
    }
  }
};

struct RegexMatchDataDeleter {
  void operator()(pcre2_match_data* data) const {
    if (data != nullptr) {
      pcre2_match_data_free(data);
    }
  }
};

using RegexCodePtr = std::unique_ptr<pcre2_code, RegexCodeDeleter>;
using RegexMatchDataPtr = std::unique_ptr<pcre2_match_data, RegexMatchDataDeleter>;

[[nodiscard]] auto regex_compile_or_throw(const std::string& pattern) -> RegexCodePtr {
  int error_code = 0;
  PCRE2_SIZE error_offset = 0;
  pcre2_code* compiled =
      pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.c_str()), PCRE2_ZERO_TERMINATED, 0, &error_code, &error_offset, nullptr);
  if (compiled == nullptr) {
    PCRE2_UCHAR message[256] = {0};
    const int msg_rc = pcre2_get_error_message(error_code, message, sizeof(message));
    const std::string msg = (msg_rc >= 0) ? reinterpret_cast<const char*>(message) : std::string{"unknown regex error"};
    throw std::invalid_argument{"Regex compile failed at offset " + std::to_string(error_offset) + ": " + msg};
  }
  return RegexCodePtr{compiled};
}

[[nodiscard]] auto regex_match_data_or_throw(const pcre2_code* code) -> RegexMatchDataPtr {
  pcre2_match_data* data = pcre2_match_data_create_from_pattern(code, nullptr);
  if (data == nullptr) {
    throw std::runtime_error{"Regex: failed to allocate match data"};
  }
  return RegexMatchDataPtr{data};
}

}  // namespace

auto StringRegexIsMatch(const Arguments& args) -> Value {
  const std::string str = as_string(args[0]);
  const std::string pattern = as_string(args[1]);

  const auto code = regex_compile_or_throw(pattern);
  const auto match_data = regex_match_data_or_throw(code.get());
  const int rc = pcre2_match(code.get(), reinterpret_cast<PCRE2_SPTR>(str.c_str()), str.size(), 0, 0, match_data.get(), nullptr);
  if (rc == PCRE2_ERROR_NOMATCH) {
    return make_bool(false);
  }
  if (rc < 0) {
    throw std::runtime_error{"StringRegexIsMatch: match failed with code " + std::to_string(rc)};
  }
  return make_bool(true);
}

auto StringRegexFind(const Arguments& args) -> Value {
  const std::string str = as_string(args[0]);
  const std::string pattern = as_string(args[1]);

  const auto code = regex_compile_or_throw(pattern);
  const auto match_data = regex_match_data_or_throw(code.get());
  const int rc = pcre2_match(code.get(), reinterpret_cast<PCRE2_SPTR>(str.c_str()), str.size(), 0, 0, match_data.get(), nullptr);
  if (rc == PCRE2_ERROR_NOMATCH) {
    return make_int(-1);
  }
  if (rc < 0) {
    throw std::runtime_error{"StringRegexFind: match failed with code " + std::to_string(rc)};
  }
  const PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(match_data.get());
  return make_int(static_cast<Int>(ovector[0]));
}

auto StringRegexReplace(const Arguments& args) -> Value {
  const std::string str = as_string(args[0]);
  const std::string pattern = as_string(args[1]);
  const std::string repl = as_string(args[2]);

  const auto code = regex_compile_or_throw(pattern);
  const auto match_data = regex_match_data_or_throw(code.get());

  std::string out;
  std::size_t search_from = 0;
  std::size_t copy_from = 0;
  while (search_from <= str.size()) {
    const int rc =
        pcre2_match(code.get(), reinterpret_cast<PCRE2_SPTR>(str.c_str()), str.size(), search_from, 0, match_data.get(), nullptr);
    if (rc == PCRE2_ERROR_NOMATCH) {
      break;
    }
    if (rc < 0) {
      throw std::runtime_error{"StringRegexReplace: match failed with code " + std::to_string(rc)};
    }

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
      if (search_from >= str.size()) {
        break;
      }
      search_from += 1;
    } else {
      search_from = end;
    }
  }
  out.append(str, copy_from, std::string::npos);
  return make_string(std::move(out));
}

auto StringRegexSplit(const Arguments& args) -> Value {
  const std::string str = as_string(args[0]);
  const std::string pattern = as_string(args[1]);

  const auto code = regex_compile_or_throw(pattern);
  const auto match_data = regex_match_data_or_throw(code.get());

  TupleBuilder out(4U);
  std::size_t search_from = 0;
  std::size_t copy_from = 0;
  while (search_from <= str.size()) {
    const int rc =
        pcre2_match(code.get(), reinterpret_cast<PCRE2_SPTR>(str.c_str()), str.size(), search_from, 0, match_data.get(), nullptr);
    if (rc == PCRE2_ERROR_NOMATCH) {
      break;
    }
    if (rc < 0) {
      throw std::runtime_error{"StringRegexSplit: match failed with code " + std::to_string(rc)};
    }

    const PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(match_data.get());
    const auto begin = static_cast<std::size_t>(ovector[0]);
    const auto end = static_cast<std::size_t>(ovector[1]);
    if (end < begin || begin > str.size() || end > str.size()) {
      throw std::runtime_error{"StringRegexSplit: invalid match bounds"};
    }
    out.push(make_string(str.substr(copy_from, begin - copy_from)));
    copy_from = end;
    if (end == begin) {
      if (search_from >= str.size()) {
        break;
      }
      search_from += 1;
    } else {
      search_from = end;
    }
  }
  out.push(make_string(str.substr(copy_from)));
  return std::move(out).finish();
}

}  // namespace fleaux::runtime
