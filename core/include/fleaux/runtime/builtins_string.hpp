#pragma once
// String, conversion, and math helper builtins.
// Part of the split fleaux_runtime; included by fleaux/runtime/fleaux_runtime.hpp.
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
// ── String / conversion ───────────────────────────────────────────────────────

namespace detail {

#if FLEAUX_HAS_PCRE2

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

[[nodiscard]] inline RegexCodePtr regex_compile_or_throw(const std::string& pattern) {
    int error_code = 0;
    PCRE2_SIZE error_offset = 0;
    pcre2_code* compiled = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.c_str()),
                                         PCRE2_ZERO_TERMINATED,
                                         0,
                                         &error_code,
                                         &error_offset,
                                         nullptr);
    if (compiled == nullptr) {
        PCRE2_UCHAR message[256] = {0};
        const int msg_rc = pcre2_get_error_message(error_code, message, sizeof(message));
        const std::string msg =
            (msg_rc >= 0) ? reinterpret_cast<const char*>(message) : std::string{"unknown regex error"};
        throw std::invalid_argument{"Regex compile failed at offset " + std::to_string(error_offset) + ": " + msg};
    }
    return RegexCodePtr{compiled};
}

[[nodiscard]] inline RegexMatchDataPtr regex_match_data_or_throw(const pcre2_code* code) {
    pcre2_match_data* data = pcre2_match_data_create_from_pattern(code, nullptr);
    if (data == nullptr) {
        throw std::runtime_error{"Regex: failed to allocate match data"};
    }
    return RegexMatchDataPtr{data};
}

#endif

}  // namespace detail

struct ToString {
    Value operator()(Value arg) const {
        return make_string(to_string(unwrap_singleton_arg(std::move(arg))));
    }
};

struct ToNum {
    // arg = [string_value]
    Value operator()(Value arg) const {
        const std::string& s = as_string(array_at(arg, 0));
        std::size_t consumed = 0;
        const double d = std::stod(s, &consumed);
        if (consumed != s.size()) {
            throw std::invalid_argument{"ToNum: trailing characters in input"};
        }
        return num_result(d);
    }
};

struct StringUpper {
    Value operator()(Value arg) const {
        std::string s = to_string(unwrap_singleton_arg(std::move(arg)));
        std::ranges::transform(s, s.begin(), [](const unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        return make_string(std::move(s));
    }
};

struct StringLower {
    Value operator()(Value arg) const {
        std::string s = to_string(unwrap_singleton_arg(std::move(arg)));
        std::ranges::transform(s, s.begin(), [](const unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return make_string(std::move(s));
    }
};

struct StringTrim {
    Value operator()(Value arg) const {
        std::string s = to_string(unwrap_singleton_arg(std::move(arg)));
        return make_string(trim_right(trim_left(std::move(s))));
    }
};

struct StringTrimStart {
    Value operator()(Value arg) const {
        std::string s = to_string(unwrap_singleton_arg(std::move(arg)));
        return make_string(trim_left(std::move(s)));
    }
};

struct StringTrimEnd {
    Value operator()(Value arg) const {
        std::string s = to_string(unwrap_singleton_arg(std::move(arg)));
        return make_string(trim_right(std::move(s)));
    }
};

struct StringSplit {
    Value operator()(Value arg) const {
        const auto& args = as_array(arg);
        if (args.Size() != 2) {
            throw std::invalid_argument{"StringSplit expects 2 arguments"};
        }
        const std::string input = to_string(*args.TryGet(0));
        const std::string sep = to_string(*args.TryGet(1));
        if (sep.empty()) {
            throw std::invalid_argument{"StringSplit separator cannot be empty"};
        }

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
};

struct StringJoin {
    Value operator()(Value arg) const {
        const auto& args = as_array(arg);
        if (args.Size() != 2) {
            throw std::invalid_argument{"StringJoin expects 2 arguments"};
        }
        const std::string sep = to_string(*args.TryGet(0));
        std::ostringstream oss;
        const Value& parts_v = *args.TryGet(1);
        if (parts_v.HasArray()) {
            const auto& parts = as_array(parts_v);
            for (std::size_t i = 0; i < parts.Size(); ++i) {
                if (i > 0) {
                    oss << sep;
                }
                oss << to_string(*parts.TryGet(i));
            }
            return make_string(oss.str());
        }

        // Python parity: joining over a non-tuple second arg iterates its string form.
        const std::string s = to_string(parts_v);
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (i > 0) {
                oss << sep;
            }
            oss << s[i];
        }
        return make_string(oss.str());
    }
};

struct StringReplace {
    Value operator()(Value arg) const {
        const auto& args = as_array(arg);
        if (args.Size() != 3) {
            throw std::invalid_argument{"StringReplace expects 3 arguments"};
        }
        std::string s = to_string(*args.TryGet(0));
        const std::string old_s = to_string(*args.TryGet(1));
        const std::string new_s = to_string(*args.TryGet(2));
        if (old_s.empty()) {
            return make_string(std::move(s));
        }
        std::size_t pos = 0;
        while ((pos = s.find(old_s, pos)) != std::string::npos) {
            s.replace(pos, old_s.size(), new_s);
            pos += new_s.size();
        }
        return make_string(std::move(s));
    }
};

struct StringContains {
    Value operator()(Value arg) const {
        const auto& args = as_array(arg);
        if (args.Size() != 2) {
            throw std::invalid_argument{"StringContains expects 2 arguments"};
        }
        const std::string s = to_string(*args.TryGet(0));
        const std::string sub = to_string(*args.TryGet(1));
        return make_bool(s.find(sub) != std::string::npos);
    }
};

struct StringStartsWith {
    Value operator()(Value arg) const {
        const auto& args = as_array(arg);
        if (args.Size() != 2) {
            throw std::invalid_argument{"StringStartsWith expects 2 arguments"};
        }
        const std::string s = to_string(*args.TryGet(0));
        const std::string prefix = to_string(*args.TryGet(1));
        return make_bool(s.starts_with(prefix));
    }
};

struct StringEndsWith {
    Value operator()(Value arg) const {
        const auto& args = as_array(arg);
        if (args.Size() != 2) {
            throw std::invalid_argument{"StringEndsWith expects 2 arguments"};
        }
        const std::string s = to_string(*args.TryGet(0));
        const std::string suffix = to_string(*args.TryGet(1));
        if (suffix.size() > s.size()) {
            return make_bool(false);
        }
        return make_bool(s.ends_with(suffix));
    }
};

struct StringLength {
    Value operator()(Value arg) const {
        const std::string s = to_string(unwrap_singleton_arg(std::move(arg)));
        return make_int(static_cast<Int>(s.size()));
    }
};

struct StringCharAt {
    // arg = [s, index] -> 1-char string or "" when out of range
    Value operator()(Value arg) const {
        const auto& args = as_array(arg);
        if (args.Size() != 2) {
            throw std::invalid_argument{"StringCharAt expects 2 arguments"};
        }
        const std::string s = to_string(*args.TryGet(0));
        const std::size_t idx = as_index(*args.TryGet(1));
        if (idx >= s.size()) {
            return make_string("");
        }
        return make_string(s.substr(idx, 1));
    }
};

struct StringSlice {
    // arg = [s, stop] | [s, start, stop]
    Value operator()(Value arg) const {
        const auto& args = as_array(arg);
        if (args.Size() != 2 && args.Size() != 3) {
            throw std::invalid_argument{"StringSlice expects 2 or 3 arguments"};
        }
        const std::string s = to_string(*args.TryGet(0));
        std::size_t start = 0;
        std::size_t stop = 0;
        if (args.Size() == 2) {
            stop = as_index(*args.TryGet(1));
        } else {
            start = as_index(*args.TryGet(1));
            stop = as_index(*args.TryGet(2));
        }
        if (start > s.size()) start = s.size();
        if (stop > s.size()) stop = s.size();
        if (stop < start) stop = start;
        return make_string(s.substr(start, stop - start));
    }
};

struct StringFind {
    // arg = [s, needle] | [s, needle, start]
    Value operator()(Value arg) const {
        const auto& args = as_array(arg);
        if (args.Size() != 2 && args.Size() != 3) {
            throw std::invalid_argument{"StringFind expects 2 or 3 arguments"};
        }
        const std::string s = to_string(*args.TryGet(0));
        const std::string needle = to_string(*args.TryGet(1));
        std::size_t start = 0;
        if (args.Size() == 3) {
            start = as_index(*args.TryGet(2));
        }
        if (start > s.size()) {
            return make_int(-1);
        }
        const auto pos = s.find(needle, start);
        if (pos == std::string::npos) {
            return make_int(-1);
        }
        return make_int(static_cast<Int>(pos));
    }
};

struct StringFormat {
    // arg = [format, arg0, arg1, ...]
    // Returns the formatted string without printing.
    Value operator()(Value arg) const {
        const auto& args = as_array(arg);
        if (args.Size() < 1) {
            throw std::invalid_argument{"String.Format expects at least 1 argument"};
        }
        const std::string fmt = to_string(*args.TryGet(0));
        std::vector<Value> values;
        values.reserve(args.Size() > 0 ? args.Size() - 1 : 0);
        for (std::size_t i = 1; i < args.Size(); ++i) {
            values.push_back(*args.TryGet(i));
        }
        return make_string(format_values(fmt, values));
    }
};

struct StringRegexIsMatch {
    // arg = [s, pattern]
    Value operator()(Value arg) const {
#if FLEAUX_HAS_PCRE2
        const auto& args = as_array(arg);
        if (args.Size() != 2) {
            throw std::invalid_argument{"StringRegexIsMatch expects 2 arguments"};
        }
        const std::string s = to_string(*args.TryGet(0));
        const std::string pattern = to_string(*args.TryGet(1));

        const auto code = detail::regex_compile_or_throw(pattern);
        const auto match_data = detail::regex_match_data_or_throw(code.get());
        const int rc = pcre2_match(code.get(),
                                   reinterpret_cast<PCRE2_SPTR>(s.c_str()),
                                   s.size(),
                                   0,
                                   0,
                                   match_data.get(),
                                   nullptr);
        if (rc == PCRE2_ERROR_NOMATCH) {
            return make_bool(false);
        }
        if (rc < 0) {
            throw std::runtime_error{"StringRegexIsMatch: match failed with code " + std::to_string(rc)};
        }
        return make_bool(true);
#else
        (void)arg;
        throw std::runtime_error{"StringRegexIsMatch: regex support unavailable (PCRE2 header not found)"};
#endif
    }
};

struct StringRegexFind {
    // arg = [s, pattern]
    Value operator()(Value arg) const {
#if FLEAUX_HAS_PCRE2
        const auto& args = as_array(arg);
        if (args.Size() != 2) {
            throw std::invalid_argument{"StringRegexFind expects 2 arguments"};
        }
        const std::string s = to_string(*args.TryGet(0));
        const std::string pattern = to_string(*args.TryGet(1));

        const auto code = detail::regex_compile_or_throw(pattern);
        const auto match_data = detail::regex_match_data_or_throw(code.get());
        const int rc = pcre2_match(code.get(),
                                   reinterpret_cast<PCRE2_SPTR>(s.c_str()),
                                   s.size(),
                                   0,
                                   0,
                                   match_data.get(),
                                   nullptr);
        if (rc == PCRE2_ERROR_NOMATCH) {
            return make_int(-1);
        }
        if (rc < 0) {
            throw std::runtime_error{"StringRegexFind: match failed with code " + std::to_string(rc)};
        }
        const PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(match_data.get());
        return make_int(static_cast<Int>(ovector[0]));
#else
        (void)arg;
        throw std::runtime_error{"StringRegexFind: regex support unavailable (PCRE2 header not found)"};
#endif
    }
};

struct StringRegexReplace {
    // arg = [s, pattern, repl]
    Value operator()(Value arg) const {
#if FLEAUX_HAS_PCRE2
        const auto& args = as_array(arg);
        if (args.Size() != 3) {
            throw std::invalid_argument{"StringRegexReplace expects 3 arguments"};
        }
        const std::string s = to_string(*args.TryGet(0));
        const std::string pattern = to_string(*args.TryGet(1));
        const std::string repl = to_string(*args.TryGet(2));

        const auto code = detail::regex_compile_or_throw(pattern);
        const auto match_data = detail::regex_match_data_or_throw(code.get());

        std::string out;
        std::size_t search_from = 0;
        std::size_t copy_from = 0;
        while (search_from <= s.size()) {
            const int rc = pcre2_match(code.get(),
                                       reinterpret_cast<PCRE2_SPTR>(s.c_str()),
                                       s.size(),
                                       search_from,
                                       0,
                                       match_data.get(),
                                       nullptr);
            if (rc == PCRE2_ERROR_NOMATCH) {
                break;
            }
            if (rc < 0) {
                throw std::runtime_error{"StringRegexReplace: match failed with code " + std::to_string(rc)};
            }

            const PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(match_data.get());
            const std::size_t begin = static_cast<std::size_t>(ovector[0]);
            const std::size_t end = static_cast<std::size_t>(ovector[1]);
            if (end < begin || begin > s.size() || end > s.size()) {
                throw std::runtime_error{"StringRegexReplace: invalid match bounds"};
            }
            out.append(s, copy_from, begin - copy_from);
            out += repl;
            copy_from = end;
            if (end == begin) {
                if (search_from >= s.size()) {
                    break;
                }
                search_from += 1;
            } else {
                search_from = end;
            }
        }
        out.append(s, copy_from, std::string::npos);
        return make_string(std::move(out));
#else
        (void)arg;
        throw std::runtime_error{"StringRegexReplace: regex support unavailable (PCRE2 header not found)"};
#endif
    }
};

struct StringRegexSplit {
    // arg = [s, pattern]
    Value operator()(Value arg) const {
#if FLEAUX_HAS_PCRE2
        const auto& args = as_array(arg);
        if (args.Size() != 2) {
            throw std::invalid_argument{"StringRegexSplit expects 2 arguments"};
        }
        const std::string s = to_string(*args.TryGet(0));
        const std::string pattern = to_string(*args.TryGet(1));

        const auto code = detail::regex_compile_or_throw(pattern);
        const auto match_data = detail::regex_match_data_or_throw(code.get());

        Array out;
        std::size_t search_from = 0;
        std::size_t copy_from = 0;
        while (search_from <= s.size()) {
            const int rc = pcre2_match(code.get(),
                                       reinterpret_cast<PCRE2_SPTR>(s.c_str()),
                                       s.size(),
                                       search_from,
                                       0,
                                       match_data.get(),
                                       nullptr);
            if (rc == PCRE2_ERROR_NOMATCH) {
                break;
            }
            if (rc < 0) {
                throw std::runtime_error{"StringRegexSplit: match failed with code " + std::to_string(rc)};
            }

            const PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(match_data.get());
            const std::size_t begin = static_cast<std::size_t>(ovector[0]);
            const std::size_t end = static_cast<std::size_t>(ovector[1]);
            if (end < begin || begin > s.size() || end > s.size()) {
                throw std::runtime_error{"StringRegexSplit: invalid match bounds"};
            }
            out.PushBack(make_string(s.substr(copy_from, begin - copy_from)));
            copy_from = end;
            if (end == begin) {
                if (search_from >= s.size()) {
                    break;
                }
                search_from += 1;
            } else {
                search_from = end;
            }
        }
        out.PushBack(make_string(s.substr(copy_from)));
        return Value{std::move(out)};
#else
        (void)arg;
        throw std::runtime_error{"StringRegexSplit: regex support unavailable (PCRE2 header not found)"};
#endif
    }
};

struct MathFloor {
    Value operator()(Value arg) const {
        return num_result(std::floor(to_double(unwrap_singleton_arg(std::move(arg)))));
    }
};

struct MathCeil {
    Value operator()(Value arg) const {
        return num_result(std::ceil(to_double(unwrap_singleton_arg(std::move(arg)))));
    }
};

struct MathAbs {
    Value operator()(Value arg) const {
        return num_result(std::abs(to_double(unwrap_singleton_arg(std::move(arg)))));
    }
};

struct MathLog {
    Value operator()(Value arg) const {
        return num_result(std::log(to_double(unwrap_singleton_arg(std::move(arg)))));
    }
};

struct MathClamp {
    Value operator()(Value arg) const {
        const auto& args = as_array(arg);
        if (args.Size() != 3) {
            throw std::invalid_argument{"MathClamp expects 3 arguments"};
        }
        const double x = to_double(*args.TryGet(0));
        const double lo = to_double(*args.TryGet(1));
        const double hi = to_double(*args.TryGet(2));
        return num_result(std::clamp(x, lo, hi));
    }
};

}  // namespace fleaux::runtime
