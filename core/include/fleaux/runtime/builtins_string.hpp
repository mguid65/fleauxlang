#pragma once
// String, conversion, and math helper builtins.
// Part of the split fleaux_runtime; included by fleaux/runtime/fleaux_runtime.hpp.
#include "value.hpp"
namespace fleaux::runtime {
// ── String / conversion ───────────────────────────────────────────────────────

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
