#pragma once

// Fleaux runtime layer built on mguid::DataTree.
//
// All runtime builtins are callable objects with the signature:
//     Value operator()(Value arg) const;
//
// The pipeline operator   value | Builtin{}   returns a Value.
//
// Fleaux tuples are represented as Value holding an ArrayNodeType.
// Fleaux primitives are Value holding a ValueNodeType (Number/Bool/String/Null).
//
// Control-flow builtins (Loop, LoopN, Apply, Branch, Select) remain templated
// because their function arguments are C++ callables, not Values.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <concepts>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#if __has_include(<format>)
#include <format>
#define FLEAUX_HAS_STD_FORMAT 1
#else
#define FLEAUX_HAS_STD_FORMAT 0
#endif
#include <iostream>
#include <iomanip>
#include <limits>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "data_tree/data_tree.hpp"

namespace fleaux::runtime {

// ── Core type aliases ─────────────────────────────────────────────────────────

using Value     = mguid::DataTree;
using Array     = mguid::ArrayNodeType;
using Object    = mguid::ObjectNodeType;
using ValueNode = mguid::ValueNodeType;
using Number    = mguid::NumberType;
using Null      = mguid::NullType;
using Bool      = mguid::BoolType;
using String    = mguid::StringType;

using Int   = mguid::IntegerType;           // int64_t
using UInt  = mguid::UnsignedIntegerType;   // uint64_t
using Float = mguid::DoubleType;            // double

using RuntimeCallable = std::function<Value(Value)>;
constexpr const char* kCallableTag = "__fleaux_callable__";

inline std::vector<std::string>& process_args_storage() {
    static std::vector<std::string> args;
    return args;
}

inline void set_process_args(int argc, char** argv) {
    auto& args = process_args_storage();
    args.clear();
    if (argc <= 0) {
        return;
    }
    args.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        args.emplace_back((argv != nullptr && argv[i] != nullptr) ? argv[i] : "");
    }
}

[[nodiscard]] inline const std::vector<std::string>& get_process_args() {
    return process_args_storage();
}

inline std::unordered_map<UInt, RuntimeCallable>& callable_registry() {
    static std::unordered_map<UInt, RuntimeCallable> registry;
    return registry;
}

inline UInt register_callable(RuntimeCallable fn) {
    static UInt next_id = 1;
    const UInt id = next_id++;
    callable_registry().emplace(id, std::move(fn));
    return id;
}

[[nodiscard]] inline Value make_callable_ref(RuntimeCallable fn) {
    const UInt id = register_callable(std::move(fn));
    Array out;
    out.Reserve(2);
    out.PushBack(Value{String{kCallableTag}});
    out.PushBack(Value{id});
    return Value{std::move(out)};
}

template <typename F>
Value make_callable_ref(F&& fn) {
    return make_callable_ref(RuntimeCallable(std::forward<F>(fn)));
}

[[nodiscard]] inline std::optional<UInt> callable_id_from_value(const Value& ref) {
    const auto arr = ref.TryGetArray();
    if (!arr) {
        return std::nullopt;
    }
    if (arr->Size() != 2) {
        return std::nullopt;
    }
    const auto tag = (*arr->TryGet(0)).TryGetString();
    if (!tag || *tag != kCallableTag) {
        return std::nullopt;
    }
    const auto num = (*arr->TryGet(1)).TryGetNumber();
    if (!num) {
        return std::nullopt;
    }
    return num->Visit(
        [](Int i) -> std::optional<UInt> {
            if (i < 0) return std::nullopt;
            return static_cast<UInt>(i);
        },
        [](UInt u) -> std::optional<UInt> { return u; },
        [](Float d) -> std::optional<UInt> {
            if (d < 0.0 || std::floor(d) != d) return std::nullopt;
            return static_cast<UInt>(d);
        }
    );
}

[[nodiscard]] inline Value invoke_callable_ref(const Value& ref, Value arg) {
    const auto id = callable_id_from_value(ref);
    if (!id) {
        throw std::runtime_error{"Expected callable reference"};
    }
    auto it = callable_registry().find(*id);
    if (it == callable_registry().end()) {
        throw std::runtime_error{"Unknown callable reference"};
    }
    return it->second(std::move(arg));
}

// Pipeline for builtins that receive a Value and return a Value.
template <typename Node>
requires requires(Node&& node, Value arg) {
    std::invoke(std::forward<Node>(node), std::move(arg));
}
inline Value operator|(Value arg, Node&& node) {
    return std::invoke(std::forward<Node>(node), std::move(arg));
}

inline Value operator|(Value arg, const Value& callable_ref) {
    return invoke_callable_ref(callable_ref, std::move(arg));
}

// Pipeline for control-flow builtins (Loop, LoopN, Branch, Apply) whose
// argument is a std::tuple containing a mix of Values and C++ callables.
template <typename L, typename Node>
requires (!std::same_as<std::remove_cvref_t<L>, Value>) &&
    requires(Node&& node, L arg) {
        { std::invoke(std::forward<Node>(node), std::move(arg)) } -> std::convertible_to<Value>;
    }
constexpr Value operator|(L&& lhs, Node&& node) {
    return std::invoke(std::forward<Node>(node), std::forward<L>(lhs));
}

// ── Construction helpers ──────────────────────────────────────────────────────

inline Value make_null()                 { return Value{Null{}}; }
inline Value make_bool(bool v)           { return Value{v}; }
inline Value make_int(Int v)             { return Value{v}; }
inline Value make_uint(UInt v)           { return Value{v}; }
inline Value make_float(Float v)         { return Value{v}; }
inline Value make_string(String v)       { return Value{std::move(v)}; }
inline Value make_array()                { return Value{Array{}}; }

// Build a tuple Value from a variadic list of Values.
template <typename... Vs>
requires (std::same_as<std::remove_cvref_t<Vs>, Value> && ...)
Value make_tuple(Vs&&... vals) {
    Array arr;
    arr.Reserve(sizeof...(Vs));
    (arr.PushBack(std::forward<Vs>(vals)), ...);
    return Value{std::move(arr)};
}

// ── Extraction helpers ────────────────────────────────────────────────────────

[[nodiscard]] inline const Array& as_array(const Value& v) {
    auto r = v.TryGetArray();
    if (!r) throw std::runtime_error{"fleaux::runtime: expected Array"};
    return *r;
}

[[nodiscard]] inline Array& as_array(Value& v) {
    auto r = v.TryGetArray();
    if (!r) throw std::runtime_error{"fleaux::runtime: expected Array"};
    return *r;
}

[[nodiscard]] inline const Number& as_number(const Value& v) {
    auto r = v.TryGetNumber();
    if (!r) throw std::runtime_error{"fleaux::runtime: expected Number"};
    return *r;
}

[[nodiscard]] inline Bool as_bool(const Value& v) {
    auto r = v.TryGetBool();
    if (!r) throw std::runtime_error{"fleaux::runtime: expected Bool"};
    return *r;
}

[[nodiscard]] inline const String& as_string(const Value& v) {
    auto r = v.TryGetString();
    if (!r) throw std::runtime_error{"fleaux::runtime: expected String"};
    return *r;
}

// Get the Nth element of an Array Value (throws on out-of-range).
[[nodiscard]] inline const Value& array_at(const Value& v, std::size_t i) {
    auto r = as_array(v).TryGet(i);
    if (!r) throw std::out_of_range{"fleaux::runtime: array index out of range"};
    return *r;
}

[[nodiscard]] inline Value& array_at(Value& v, std::size_t i) {
    auto r = v.TryGetArray();
    if (!r) throw std::runtime_error{"fleaux::runtime: expected Array"};
    Array& arr = *r;
    if (i >= arr.Size()) throw std::out_of_range{"fleaux::runtime: array index out of range"};
    return arr[i];
}

// Convert any numeric Value to double.
[[nodiscard]] inline double to_double(const Value& v) {
    return as_number(v).Visit(
        [](Int   i) -> double { return static_cast<double>(i); },
        [](UInt  u) -> double { return static_cast<double>(u); },
        [](Float d) -> double { return d; }
    );
}

// Convert a double result back to the most compact Number Value.
[[nodiscard]] inline Value num_result(double d) {
    if (d == std::floor(d) &&
        d >= static_cast<double>(std::numeric_limits<Int>::min()) &&
        d <= static_cast<double>(std::numeric_limits<Int>::max())) {
        return make_int(static_cast<Int>(d));
    }
    return make_float(d);
}

// Extract the integer index embedded in a Number Value.
[[nodiscard]] inline std::size_t as_index(const Value& v) {
    return as_number(v).Visit(
        [](Int   i) -> std::size_t { return static_cast<std::size_t>(i); },
        [](UInt  u) -> std::size_t { return static_cast<std::size_t>(u); },
        [](Float d) -> std::size_t { return static_cast<std::size_t>(d); }
    );
}

// Python make_node semantics: when one argument is provided, the callee sees
// the scalar value instead of a 1-tuple wrapper.
[[nodiscard]] inline Value unwrap_singleton_arg(Value v) {
    if (v.HasArray()) {
        const auto& arr = as_array(v);
        if (arr.Size() == 1) {
            return *arr.TryGet(0);
        }
    }
    return v;
}

// ── Value printing ────────────────────────────────────────────────────────────

[[nodiscard]] inline std::string format_number_python_style(const Number& n) {
    return n.Visit(
        [](Int i) -> std::string { return std::to_string(i); },
        [](UInt u) -> std::string { return std::to_string(u); },
        [](Float d) -> std::string {
            std::ostringstream oss;
            // Keep stable, high-precision decimal output close to Python str(float).
            oss << std::setprecision(std::numeric_limits<Float>::max_digits10) << d;
            return oss.str();
        }
    );
}

// Print a Value as a Python-like scalar/tuple repr.
inline void print_value_repr(std::ostream& os, const Value& v) {
    v.Visit(
        [&](const Array& arr) {
            os << '(';
            for (std::size_t i = 0; i < arr.Size(); ++i) {
                if (i > 0) {
                    os << ", ";
                }
                print_value_repr(os, *arr.TryGet(i));
            }
            if (arr.Size() == 1) {
                os << ',';
            }
            os << ')';
        },
        [&](const Object& /*obj*/) {
            os << "{...}";  // objects are not a first-class Fleaux concept yet
        },
        [&](const ValueNode& vn) {
            vn.Visit(
                [&](const Null&)    { os << "None"; },
                [&](Bool b)         { os << (b ? "True" : "False"); },
                [&](const Number& n) { os << format_number_python_style(n); },
                [&](const String& s) { os << s; }
            );
        }
    );
}

// Python Println semantics: tuple args are printed as varargs, space-separated.
inline void print_value_varargs(std::ostream& os, const Value& v) {
    if (!v.HasArray()) {
        print_value_repr(os, v);
        return;
    }

    const auto& arr = as_array(v);
    for (std::size_t i = 0; i < arr.Size(); ++i) {
        if (i > 0) {
            os << ' ';
        }
        print_value_repr(os, *arr.TryGet(i));
    }
}

inline std::string to_string(const Value& v) {
    std::ostringstream oss;
    print_value_repr(oss, v);
    return oss.str();
}

[[nodiscard]] inline const Array& require_args(const Value& arg, std::size_t n, const char* name) {
    const auto& args = as_array(arg);
    if (args.Size() != n) {
        throw std::invalid_argument(std::string{name} + " expects " + std::to_string(n) + " arguments");
    }
    return args;
}

[[nodiscard]] inline std::string as_path_string_unary(Value arg) {
    return to_string(unwrap_singleton_arg(std::move(arg)));
}

[[nodiscard]] inline std::string random_suffix(std::size_t size = 12) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    static constexpr char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    std::uniform_int_distribution<std::size_t> dist(0, sizeof(alphabet) - 2);
    std::string out;
    out.reserve(size);
    for (std::size_t i = 0; i < size; ++i) {
        out.push_back(alphabet[dist(rng)]);
    }
    return out;
}

[[nodiscard]] inline std::string trim_left(std::string s) {
    const auto it = std::find_if_not(s.begin(), s.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    s.erase(s.begin(), it);
    return s;
}

[[nodiscard]] inline std::string trim_right(std::string s) {
    const auto it = std::find_if_not(s.rbegin(), s.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    s.erase(it.base(), s.end());
    return s;
}

[[nodiscard]] inline std::string format_values(const std::string& fmt, const std::vector<std::string>& values) {
#if FLEAUX_HAS_STD_FORMAT
    switch (values.size()) {
        case 0: return std::vformat(fmt, std::make_format_args());
        case 1: return std::vformat(fmt, std::make_format_args(values[0]));
        case 2: return std::vformat(fmt, std::make_format_args(values[0], values[1]));
        case 3: return std::vformat(fmt, std::make_format_args(values[0], values[1], values[2]));
        case 4: return std::vformat(fmt, std::make_format_args(values[0], values[1], values[2], values[3]));
        case 5: return std::vformat(fmt, std::make_format_args(values[0], values[1], values[2], values[3], values[4]));
        case 6: return std::vformat(fmt, std::make_format_args(values[0], values[1], values[2], values[3], values[4], values[5]));
        case 7: return std::vformat(fmt, std::make_format_args(values[0], values[1], values[2], values[3], values[4], values[5], values[6]));
        case 8: return std::vformat(fmt, std::make_format_args(values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7]));
        default:
            throw std::invalid_argument{"Printf supports up to 8 formatting arguments"};
    }
#else
    (void)values;
    return fmt;
#endif
}

// ── Runtime builtins ──────────────────────────────────────────────────────────

struct Wrap {
    // arg = any Value  →  [arg]
    Value operator()(Value v) const {
        return make_tuple(std::move(v));
    }
};

struct Unwrap {
    // arg = [v]  →  v
    Value operator()(Value v) const {
        return array_at(v, 0);
    }
};

struct ElementAt {
    // arg = [sequence, index]
    Value operator()(Value arg) const {
        const auto& seq = as_array(array_at(arg, 0));
        const std::size_t idx = as_index(array_at(arg, 1));
        auto r = seq.TryGet(idx);
        if (!r) throw std::out_of_range{"ElementAt: index out of range"};
        return *r;
    }
};

struct Length {
    // arg = [sequence]  (single-element array wrapping the sequence)
    Value operator()(Value arg) const {
        return make_int(static_cast<Int>(as_array(array_at(arg, 0)).Size()));
    }
};

struct Take {
    // arg = [sequence, count]
    Value operator()(Value arg) const {
        const auto& seq = as_array(array_at(arg, 0));
        const std::size_t n = std::min(as_index(array_at(arg, 1)), seq.Size());
        Array out;
        out.Reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            out.PushBack(*seq.TryGet(i));
        }
        return Value{std::move(out)};
    }
};

struct Drop {
    // arg = [sequence, count]
    Value operator()(Value arg) const {
        const auto& seq   = as_array(array_at(arg, 0));
        const std::size_t start = as_index(array_at(arg, 1));
        Array out;
        for (std::size_t i = start; i < seq.Size(); ++i) {
            out.PushBack(*seq.TryGet(i));
        }
        return Value{std::move(out)};
    }
};

struct Slice {
    // arg = [sequence, stop]
    //     | [sequence, start, stop]
    //     | [sequence, start, stop, step]
    Value operator()(Value arg) const {
        const auto& arr = as_array(arg);
        if (arr.Size() < 2) throw std::invalid_argument{"Slice: need at least 2 arguments"};
        const auto& seq = as_array(*arr.TryGet(0));

        std::size_t real_start, real_stop, real_step;
        if (arr.Size() == 2) {
            real_start = 0;
            real_stop  = as_index(*arr.TryGet(1));
            real_step  = 1;
        } else if (arr.Size() == 3) {
            real_start = as_index(*arr.TryGet(1));
            real_stop  = as_index(*arr.TryGet(2));
            real_step  = 1;
        } else {
            real_start = as_index(*arr.TryGet(1));
            real_stop  = as_index(*arr.TryGet(2));
            real_step  = as_index(*arr.TryGet(3));
            if (real_step == 0) throw std::invalid_argument{"Slice: step cannot be 0"};
        }

        Array out;
        const std::size_t end = std::min(real_stop, seq.Size());
        for (std::size_t i = real_start; i < end; i += real_step) {
            out.PushBack(*seq.TryGet(i));
        }
        return Value{std::move(out)};
    }
};

// ── Arithmetic ────────────────────────────────────────────────────────────────

struct Add {
    // arg = [lhs, rhs]
    Value operator()(Value arg) const {
        const Value& l = array_at(arg, 0);
        const Value& r = array_at(arg, 1);
        // String concatenation
        if (l.HasString() && r.HasString()) {
            return make_string(as_string(l) + as_string(r));
        }
        return num_result(to_double(l) + to_double(r));
    }
};

struct Subtract {
    Value operator()(Value arg) const {
        return num_result(to_double(array_at(arg, 0)) - to_double(array_at(arg, 1)));
    }
};

struct Multiply {
    Value operator()(Value arg) const {
        return num_result(to_double(array_at(arg, 0)) * to_double(array_at(arg, 1)));
    }
};

struct Divide {
    Value operator()(Value arg) const {
        return num_result(to_double(array_at(arg, 0)) / to_double(array_at(arg, 1)));
    }
};

struct Mod {
    Value operator()(Value arg) const {
        const double l = to_double(array_at(arg, 0));
        const double r = to_double(array_at(arg, 1));
        return num_result(std::fmod(l, r));
    }
};

struct Pow {
    Value operator()(Value arg) const {
        return num_result(std::pow(to_double(array_at(arg, 0)), to_double(array_at(arg, 1))));
    }
};

struct Sqrt {
    Value operator()(Value arg) const {
        return num_result(std::sqrt(to_double(unwrap_singleton_arg(std::move(arg)))));
    }
};

struct UnaryMinus {
    Value operator()(Value arg) const {
        return num_result(-to_double(unwrap_singleton_arg(std::move(arg))));
    }
};

struct UnaryPlus {
    Value operator()(Value arg) const {
        return num_result(+to_double(unwrap_singleton_arg(std::move(arg))));
    }
};

struct Sin {
    Value operator()(Value arg) const {
        return num_result(std::sin(to_double(unwrap_singleton_arg(std::move(arg)))));
    }
};
struct Cos {
    Value operator()(Value arg) const {
        return num_result(std::cos(to_double(unwrap_singleton_arg(std::move(arg)))));
    }
};
struct Tan {
    Value operator()(Value arg) const {
        return num_result(std::tan(to_double(unwrap_singleton_arg(std::move(arg)))));
    }
};

// ── Comparison & logical ──────────────────────────────────────────────────────

struct GreaterThan {
    Value operator()(Value arg) const {
        return make_bool(to_double(array_at(arg, 0)) > to_double(array_at(arg, 1)));
    }
};
struct LessThan {
    Value operator()(Value arg) const {
        return make_bool(to_double(array_at(arg, 0)) < to_double(array_at(arg, 1)));
    }
};
struct GreaterOrEqual {
    Value operator()(Value arg) const {
        return make_bool(to_double(array_at(arg, 0)) >= to_double(array_at(arg, 1)));
    }
};
struct LessOrEqual {
    Value operator()(Value arg) const {
        return make_bool(to_double(array_at(arg, 0)) <= to_double(array_at(arg, 1)));
    }
};

struct Equal {
    Value operator()(Value arg) const {
        return make_bool(array_at(arg, 0) == array_at(arg, 1));
    }
};
struct NotEqual {
    Value operator()(Value arg) const {
        return make_bool(!(array_at(arg, 0) == array_at(arg, 1)));
    }
};

struct Not {
    Value operator()(Value arg) const {
        return make_bool(!as_bool(unwrap_singleton_arg(std::move(arg))));
    }
};
struct And {
    Value operator()(Value arg) const {
        return make_bool(as_bool(array_at(arg, 0)) && as_bool(array_at(arg, 1)));
    }
};
struct Or {
    Value operator()(Value arg) const {
        return make_bool(as_bool(array_at(arg, 0)) || as_bool(array_at(arg, 1)));
    }
};

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
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        return make_string(std::move(s));
    }
};

struct StringLower {
    Value operator()(Value arg) const {
        std::string s = to_string(unwrap_singleton_arg(std::move(arg)));
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
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
        return make_bool(s.rfind(prefix, 0) == 0);
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
        return make_bool(s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0);
    }
};

struct StringLength {
    Value operator()(Value arg) const {
        const std::string s = to_string(unwrap_singleton_arg(std::move(arg)));
        return make_int(static_cast<Int>(s.size()));
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
        return num_result(std::max(lo, std::min(x, hi)));
    }
};

// ── Output ────────────────────────────────────────────────────────────────────

struct Println {
    // Prints the value, returns it unchanged.
    Value operator()(Value arg) const {
        print_value_varargs(std::cout, arg);
        std::cout << '\n';
        return arg;
    }
};

struct Printf {
    // arg = [format, arg0, arg1, ...]
    // Prints formatted text and returns (format, (arg0, arg1, ...)).
    Value operator()(Value arg) const {
        const auto& args = as_array(arg);
        if (args.Size() < 1) {
            throw std::invalid_argument{"Printf expects at least 1 argument"};
        }
        const std::string fmt = to_string(*args.TryGet(0));
        std::vector<std::string> values;
        values.reserve(args.Size() > 0 ? args.Size() - 1 : 0);

        Array returned_args;
        returned_args.Reserve(args.Size() > 0 ? args.Size() - 1 : 0);
        for (std::size_t i = 1; i < args.Size(); ++i) {
            values.push_back(to_string(*args.TryGet(i)));
            returned_args.PushBack(*args.TryGet(i));
        }

        std::cout << format_values(fmt, values) << '\n';
        return make_tuple(make_string(fmt), Value{std::move(returned_args)});
    }
};

struct Input {
    // arg = [] | [prompt] | prompt
    Value operator()(Value arg) const {
        auto read_line = []() -> Value {
            std::string line;
            if (!std::getline(std::cin, line)) {
                return make_string("");
            }
            return make_string(line);
        };

        if (!arg.HasArray()) {
            std::cout << to_string(arg);
            std::cout.flush();
            return read_line();
        }

        const auto& args = as_array(arg);
        if (args.Size() == 0) {
            return read_line();
        }
        if (args.Size() == 1) {
            std::cout << to_string(*args.TryGet(0));
            std::cout.flush();
            return read_line();
        }
        throw std::invalid_argument{"Input expects 0 or 1 argument"};
    }
};

struct GetArgs {
    Value operator()(Value arg) const {
        (void)require_args(arg, 0, "GetArgs");
        Array out;
        const auto& args = get_process_args();
        out.Reserve(args.size());
        for (const auto& s : args) {
            out.PushBack(make_string(s));
        }
        return Value{std::move(out)};
    }
};

struct Exit {
    Value operator()(Value arg) const {
        if (arg.HasArray()) {
            const auto& args = as_array(arg);
            if (args.Size() == 0) {
                std::exit(0);
            }
            if (args.Size() == 1) {
                std::exit(static_cast<int>(to_double(*args.TryGet(0))));
            }
            throw std::invalid_argument{"Exit expects 0 or 1 argument"};
        }
        std::exit(static_cast<int>(to_double(arg)));
    }
};

// ── Path/File/Dir/OS ──────────────────────────────────────────────────────────

struct Cwd {
    Value operator()(Value arg) const {
        (void)require_args(arg, 0, "Cwd");
        return make_string(std::filesystem::current_path().string());
    }
};

struct PathJoin {
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "PathJoin");
        const std::filesystem::path lhs = to_string(*args.TryGet(0));
        const std::filesystem::path rhs = to_string(*args.TryGet(1));
        return make_string((lhs / rhs).string());
    }
};

struct PathNormalize {
    Value operator()(Value arg) const {
        return make_string(std::filesystem::path(as_path_string_unary(std::move(arg))).lexically_normal().string());
    }
};

struct PathBasename {
    Value operator()(Value arg) const {
        return make_string(std::filesystem::path(as_path_string_unary(std::move(arg))).filename().string());
    }
};

struct PathDirname {
    Value operator()(Value arg) const {
        return make_string(std::filesystem::path(as_path_string_unary(std::move(arg))).parent_path().string());
    }
};

struct PathExists {
    Value operator()(Value arg) const {
        return make_bool(std::filesystem::exists(std::filesystem::path(as_path_string_unary(std::move(arg)))));
    }
};

struct PathIsFile {
    Value operator()(Value arg) const {
        return make_bool(std::filesystem::is_regular_file(std::filesystem::path(as_path_string_unary(std::move(arg)))));
    }
};

struct PathIsDir {
    Value operator()(Value arg) const {
        return make_bool(std::filesystem::is_directory(std::filesystem::path(as_path_string_unary(std::move(arg)))));
    }
};

struct PathAbsolute {
    Value operator()(Value arg) const {
        return make_string(std::filesystem::absolute(std::filesystem::path(as_path_string_unary(std::move(arg)))).string());
    }
};

struct PathExtension {
    Value operator()(Value arg) const {
        return make_string(std::filesystem::path(as_path_string_unary(std::move(arg))).extension().string());
    }
};

struct PathStem {
    Value operator()(Value arg) const {
        return make_string(std::filesystem::path(as_path_string_unary(std::move(arg))).stem().string());
    }
};

struct PathWithExtension {
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "PathWithExtension");
        std::filesystem::path p = to_string(*args.TryGet(0));
        std::string ext = to_string(*args.TryGet(1));
        if (!ext.empty() && ext[0] != '.') {
            ext.insert(ext.begin(), '.');
        }
        p.replace_extension(ext);
        return make_string(p.string());
    }
};

struct PathWithBasename {
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "PathWithBasename");
        std::filesystem::path p = to_string(*args.TryGet(0));
        p.replace_filename(to_string(*args.TryGet(1)));
        return make_string(p.string());
    }
};

struct FileReadText {
    Value operator()(Value arg) const {
        std::ifstream in(as_path_string_unary(std::move(arg)));
        if (!in) {
            throw std::runtime_error{"FileReadText failed"};
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        return make_string(ss.str());
    }
};

struct FileWriteText {
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "FileWriteText");
        const std::string path = to_string(*args.TryGet(0));
        std::ofstream out(path, std::ios::trunc);
        if (!out) {
            throw std::runtime_error{"FileWriteText failed"};
        }
        out << to_string(*args.TryGet(1));
        return make_string(path);
    }
};

struct FileAppendText {
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "FileAppendText");
        const std::string path = to_string(*args.TryGet(0));
        std::ofstream out(path, std::ios::app);
        if (!out) {
            throw std::runtime_error{"FileAppendText failed"};
        }
        out << to_string(*args.TryGet(1));
        return make_string(path);
    }
};

struct FileReadLines {
    Value operator()(Value arg) const {
        std::ifstream in(as_path_string_unary(std::move(arg)));
        if (!in) {
            throw std::runtime_error{"FileReadLines failed"};
        }
        Array out;
        std::string line;
        while (std::getline(in, line)) {
            out.PushBack(make_string(line));
        }
        return Value{std::move(out)};
    }
};

struct FileDelete {
    Value operator()(Value arg) const {
        std::error_code ec;
        const bool removed = std::filesystem::remove(std::filesystem::path(as_path_string_unary(std::move(arg))), ec);
        return make_bool(removed);
    }
};

struct FileSize {
    Value operator()(Value arg) const {
        return make_int(static_cast<Int>(std::filesystem::file_size(std::filesystem::path(as_path_string_unary(std::move(arg))))));
    }
};

struct DirCreate {
    Value operator()(Value arg) const {
        const std::string path = as_path_string_unary(std::move(arg));
        std::filesystem::create_directories(path);
        return make_string(path);
    }
};

struct DirDelete {
    Value operator()(Value arg) const {
        std::error_code ec;
        const auto removed = std::filesystem::remove_all(std::filesystem::path(as_path_string_unary(std::move(arg))), ec);
        return make_bool(removed > 0);
    }
};

struct DirList {
    Value operator()(Value arg) const {
        std::vector<std::string> names;
        for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(as_path_string_unary(std::move(arg))))) {
            names.push_back(entry.path().filename().string());
        }
        std::sort(names.begin(), names.end());
        Array out;
        out.Reserve(names.size());
        for (const auto& n : names) {
            out.PushBack(make_string(n));
        }
        return Value{std::move(out)};
    }
};

struct DirListFull {
    Value operator()(Value arg) const {
        std::vector<std::string> names;
        for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(as_path_string_unary(std::move(arg))))) {
            names.push_back(entry.path().string());
        }
        std::sort(names.begin(), names.end());
        Array out;
        out.Reserve(names.size());
        for (const auto& n : names) {
            out.PushBack(make_string(n));
        }
        return Value{std::move(out)};
    }
};

struct OSEnv {
    Value operator()(Value arg) const {
        const std::string key = as_path_string_unary(std::move(arg));
        if (const char* val = std::getenv(key.c_str())) {
            return make_string(val);
        }
        return make_null();
    }
};

struct OSHasEnv {
    Value operator()(Value arg) const {
        const std::string key = as_path_string_unary(std::move(arg));
        return make_bool(std::getenv(key.c_str()) != nullptr);
    }
};

struct OSSetEnv {
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "OSSetEnv");
        const std::string key = to_string(*args.TryGet(0));
        const std::string value = to_string(*args.TryGet(1));
#if defined(_WIN32)
        const int rc = _putenv_s(key.c_str(), value.c_str());
        if (rc != 0) {
            throw std::runtime_error{"OSSetEnv failed"};
        }
#else
        if (setenv(key.c_str(), value.c_str(), 1) != 0) {
            throw std::runtime_error{"OSSetEnv failed"};
        }
#endif
        return make_string(value);
    }
};

struct OSUnsetEnv {
    Value operator()(Value arg) const {
        const std::string key = as_path_string_unary(std::move(arg));
        const bool existed = std::getenv(key.c_str()) != nullptr;
#if defined(_WIN32)
        const int rc = _putenv_s(key.c_str(), "");
        if (rc != 0) {
            throw std::runtime_error{"OSUnsetEnv failed"};
        }
#else
        if (unsetenv(key.c_str()) != 0) {
            throw std::runtime_error{"OSUnsetEnv failed"};
        }
#endif
        return make_bool(existed);
    }
};

struct OSIsWindows {
    Value operator()(Value arg) const {
        (void)require_args(arg, 0, "OSIsWindows");
#if defined(_WIN32)
        return make_bool(true);
#else
        return make_bool(false);
#endif
    }
};

struct OSIsLinux {
    Value operator()(Value arg) const {
        (void)require_args(arg, 0, "OSIsLinux");
#if defined(__linux__)
        return make_bool(true);
#else
        return make_bool(false);
#endif
    }
};

struct OSIsMacOS {
    Value operator()(Value arg) const {
        (void)require_args(arg, 0, "OSIsMacOS");
#if defined(__APPLE__)
        return make_bool(true);
#else
        return make_bool(false);
#endif
    }
};

struct OSHome {
    Value operator()(Value arg) const {
        (void)require_args(arg, 0, "OSHome");
        if (const char* home = std::getenv("HOME")) {
            return make_string(home);
        }
        if (const char* userprofile = std::getenv("USERPROFILE")) {
            return make_string(userprofile);
        }
        return make_string(std::filesystem::current_path().string());
    }
};

struct OSTempDir {
    Value operator()(Value arg) const {
        (void)require_args(arg, 0, "OSTempDir");
        return make_string(std::filesystem::temp_directory_path().string());
    }
};

struct OSMakeTempFile {
    Value operator()(Value arg) const {
        (void)require_args(arg, 0, "OSMakeTempFile");
        const auto dir = std::filesystem::temp_directory_path();
        for (int i = 0; i < 100; ++i) {
            const auto candidate = dir / ("fleaux_" + random_suffix() + ".tmp");
            if (!std::filesystem::exists(candidate)) {
                std::ofstream out(candidate);
                if (out.good()) {
                    return make_string(candidate.string());
                }
            }
        }
        throw std::runtime_error{"OSMakeTempFile failed"};
    }
};

struct OSMakeTempDir {
    Value operator()(Value arg) const {
        (void)require_args(arg, 0, "OSMakeTempDir");
        const auto dir = std::filesystem::temp_directory_path();
        for (int i = 0; i < 100; ++i) {
            const auto candidate = dir / ("fleaux_" + random_suffix());
            if (std::filesystem::create_directory(candidate)) {
                return make_string(candidate.string());
            }
        }
        throw std::runtime_error{"OSMakeTempDir failed"};
    }
};

// ── Tuple builtins ────────────────────────────────────────────────────────────

struct TupleAppend {
    // arg = [sequence, item]
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "TupleAppend");
        const auto& src = as_array(*args.TryGet(0));
        Array out;
        out.Reserve(src.Size() + 1);
        for (std::size_t i = 0; i < src.Size(); ++i) {
            out.PushBack(*src.TryGet(i));
        }
        out.PushBack(*args.TryGet(1));
        return Value{std::move(out)};
    }
};

struct TuplePrepend {
    // arg = [sequence, item]  →  [item, ...sequence]
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "TuplePrepend");
        const auto& src = as_array(*args.TryGet(0));
        Array out;
        out.Reserve(src.Size() + 1);
        out.PushBack(*args.TryGet(1));
        for (std::size_t i = 0; i < src.Size(); ++i) {
            out.PushBack(*src.TryGet(i));
        }
        return Value{std::move(out)};
    }
};

struct TupleReverse {
    // arg = [sequence]  (1-element array wrapping the sequence, same as Length)
    Value operator()(Value arg) const {
        const auto& src = as_array(array_at(arg, 0));
        Array out;
        out.Reserve(src.Size());
        for (std::size_t i = src.Size(); i > 0; --i) {
            out.PushBack(*src.TryGet(i - 1));
        }
        return Value{std::move(out)};
    }
};

struct TupleContains {
    // arg = [sequence, item]
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "TupleContains");
        const auto& src = as_array(*args.TryGet(0));
        const Value& item = *args.TryGet(1);
        for (std::size_t i = 0; i < src.Size(); ++i) {
            if (*src.TryGet(i) == item) {
                return make_bool(true);
            }
        }
        return make_bool(false);
    }
};

struct TupleZip {
    // arg = [sequenceA, sequenceB]  →  [(a0,b0), (a1,b1), ...]
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "TupleZip");
        const auto& a = as_array(*args.TryGet(0));
        const auto& b = as_array(*args.TryGet(1));
        const std::size_t n = std::min(a.Size(), b.Size());
        Array out;
        out.Reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            out.PushBack(make_tuple(*a.TryGet(i), *b.TryGet(i)));
        }
        return Value{std::move(out)};
    }
};

struct TupleMap {
    // arg = [sequence, func_ref]
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "TupleMap");
        const auto& src = as_array(*args.TryGet(0));
        const Value& func = *args.TryGet(1);

        Array out;
        out.Reserve(src.Size());
        for (std::size_t i = 0; i < src.Size(); ++i) {
            out.PushBack(invoke_callable_ref(func, *src.TryGet(i)));
        }
        return Value{std::move(out)};
    }
};

struct TupleFilter {
    // arg = [sequence, pred_ref]
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "TupleFilter");
        const auto& src = as_array(*args.TryGet(0));
        const Value& pred = *args.TryGet(1);

        Array out;
        for (std::size_t i = 0; i < src.Size(); ++i) {
            const Value& item = *src.TryGet(i);
            if (as_bool(invoke_callable_ref(pred, item))) {
                out.PushBack(item);
            }
        }
        return Value{std::move(out)};
    }
};

// ── Control flow (templated: functions remain concrete C++ callables) ─────────

struct Select {
    // arg = [condition, true_val, false_val] — all Values
    Value operator()(Value arg) const {
        return as_bool(array_at(arg, 0))
            ? array_at(arg, 1)
            : array_at(arg, 2);
    }
};

struct Apply {
    // arg = [value, func_ref]
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 2, "Apply");
        return invoke_callable_ref(*args.TryGet(1), *args.TryGet(0));
    }
};

struct Branch {
    // arg = [condition, value, true_func_ref, false_func_ref]
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 4, "Branch");
        const Value& cond = *args.TryGet(0);
        const Value& val = *args.TryGet(1);
        const Value& tf = *args.TryGet(2);
        const Value& ff = *args.TryGet(3);
        return as_bool(cond) ? invoke_callable_ref(tf, val) : invoke_callable_ref(ff, val);
    }
};

struct Loop {
    // arg = [state, continue_func_ref, step_func_ref]
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 3, "Loop");
        Value state = *args.TryGet(0);
        const Value& cf = *args.TryGet(1);
        const Value& sf = *args.TryGet(2);
        while (as_bool(invoke_callable_ref(cf, state))) {
            state = invoke_callable_ref(sf, std::move(state));
        }
        return state;
    }
};

struct LoopN {
    // arg = [state, continue_func_ref, step_func_ref, max_iters]
    Value operator()(Value arg) const {
        const auto& args = require_args(arg, 4, "LoopN");
        Value state = *args.TryGet(0);
        const Value& cf = *args.TryGet(1);
        const Value& sf = *args.TryGet(2);
        const std::size_t max_iters = as_index(*args.TryGet(3));

        std::size_t steps = 0;
        while (as_bool(invoke_callable_ref(cf, state))) {
            if (steps >= max_iters) {
                throw std::runtime_error{"LoopN: exceeded max_iters"};
            }
            state = invoke_callable_ref(sf, std::move(state));
            ++steps;
        }
        return state;
    }
};

}  // namespace fleaux::runtime

