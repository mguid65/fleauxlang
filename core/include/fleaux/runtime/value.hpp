// Foundation: type aliases, registries, construction/extraction helpers, printing/format utilities.
// Part of the split fleaux_runtime; included by fleaux/runtime/fleaux_runtime.hpp.

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
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "data_tree/data_tree.hpp"

namespace fleaux::runtime {

// ── Core type aliases ─────────────────────────────────────────────────────────

using Value     = mguid::DataTree;
using Array     = mguid::ArrayNodeType;
using Object    = mguid::ObjectNodeType;
using Generic   = mguid::GenericNodeType;
using ValueNode = mguid::ValueNodeType;
using Number    = mguid::NumberType;
using Null      = mguid::NullType;
using Bool      = mguid::BoolType;
using String    = mguid::StringType;

using Int   = mguid::IntegerType;           // int64_t
using UInt  = mguid::UnsignedIntegerType;   // uint64_t
using Float = mguid::DoubleType;            // double

using RuntimeCallable = std::function<Value(Value)>;

inline constexpr std::string_view k_callable_tag = "__fleaux_callable__";
inline constexpr std::string_view k_handle_tag   = "__fleaux_handle__";

// Global registry for storing callables (functions)
inline std::vector<RuntimeCallable>& callable_registry() {
    static std::vector<RuntimeCallable> registry;
    return registry;
}

inline std::vector<std::string>& process_args_storage() {
    static std::vector<std::string> args;
    return args;
}

inline void set_process_args(const int argc, char** argv) {
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

// ── Callable/Function handling ────────────────────────────────────────────────

inline UInt register_callable(RuntimeCallable fn) {
    auto& call_reg = callable_registry();
    const UInt id = static_cast<UInt>(call_reg.size());
    call_reg.push_back(std::move(fn));
    return id;
}

// Global function registry stored as GenericNodeType
inline Value& function_registry() {
    static Value registry{mguid::NodeTypeTag::Generic};
    return registry;
}

[[nodiscard]] inline Value make_callable_ref(RuntimeCallable fn) {
    const UInt id = register_callable(std::move(fn));

    // Store in Generic node with metadata
    auto& gen = function_registry().Unsafe([](auto&& proxy) -> decltype(auto) {
        return proxy.GetGeneric();
    });

    Object callable_entry;
    callable_entry["type"] = Value{String{k_callable_tag}};
    callable_entry["id"] = Value{id};

    gen["fn_" + std::to_string(id)] = Value{std::move(callable_entry)};

    // Return simple array with type tag and id for passing around
    Array out;
    out.Reserve(2);
    out.PushBack(Value{String{k_callable_tag}});
    out.PushBack(Value{id});
    return Value{std::move(out)};
}

template <typename F>
Value make_callable_ref(F&& fn) {
    return make_callable_ref(RuntimeCallable(std::forward<F>(fn)));
}

[[nodiscard]] inline std::optional<UInt> callable_id_from_value(const Value& ref) {
    const auto& arr = ref.TryGetArray();
    if (!arr) {
        return std::nullopt;
    }
    if (arr->Size() != 2) {
        return std::nullopt;
    }
    const auto& tag = arr->TryGet(0)->TryGetString();
    if (!tag || *tag != k_callable_tag) {
        return std::nullopt;
    }
    const auto& num = arr->TryGet(1)->TryGetNumber();
    if (!num) {
        return std::nullopt;
    }
    return num->Visit(
        [](const Int i) -> std::optional<UInt> {
            if (i < 0) return std::nullopt;
            return static_cast<UInt>(i);
        },
        [](const UInt u) -> std::optional<UInt> { return u; },
        [](const Float d) -> std::optional<UInt> {
            if (d < 0.0 || std::floor(d) != d) return std::nullopt;
            return static_cast<UInt>(d);
        }
    );
}

// ── File handle registry ─────────────────────────────────────────────────────

struct HandleEntry {
    std::fstream  stream;
    std::string   path;
    std::string   mode;
    bool          closed{false};
    UInt          generation{0};
};

struct HandleRegistry {
    std::vector<HandleEntry> entries;

    UInt open(const std::string& path, const std::string& mode) {
        // find a closed slot to reuse
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (entries[i].closed) {
                auto& e = entries[i];
                e.generation++;
                e.closed = false;
                e.path = path;
                e.mode = mode;
                open_stream(e);
                return static_cast<UInt>(i);
            }
        }
        entries.push_back({});
        auto& e = entries.back();
        e.path = path;
        e.mode = mode;
        e.generation = 0;
        open_stream(e);
        return static_cast<UInt>(entries.size() - 1);
    }

    static void open_stream(HandleEntry& e) {
        std::ios::openmode flags{};
        bool is_read  = (e.mode.find('r') != std::string::npos);
        bool is_write = (e.mode.find('w') != std::string::npos);
        bool is_append= (e.mode.find('a') != std::string::npos);
        bool is_binary= (e.mode.find('b') != std::string::npos);
        if (is_read)   flags |= std::ios::in;
        if (is_write)  flags |= std::ios::out | std::ios::trunc;
        if (is_append) flags |= std::ios::out | std::ios::app;
        if (is_binary) flags |= std::ios::binary;
        if (!is_read && !is_write && !is_append) flags |= std::ios::in;
        e.stream.open(e.path, flags);
        if (!e.stream.is_open()) {
            throw std::runtime_error{"FileOpen: cannot open '" + e.path + "' with mode '" + e.mode + "'"};
        }
    }

    // returns HandleEntry* or nullptr if invalid/closed
    [[nodiscard]] HandleEntry* get(UInt slot, UInt gen) {
        if (slot >= entries.size()) return nullptr;
        auto& e = entries[slot];
        if (e.closed || e.generation != gen) return nullptr;
        return &e;
    }

    bool close(UInt slot, UInt gen) {
        if (slot >= entries.size()) return false;
        auto& e = entries[slot];
        if (e.closed || e.generation != gen) return false;
        e.stream.close();
        e.closed = true;
        return true;
    }
};

inline HandleRegistry& handle_registry() {
    static HandleRegistry reg;
    return reg;
}

// Global handle registry stored as GenericNodeType
inline Value& file_handle_registry() {
    static Value registry{mguid::NodeTypeTag::Generic};
    return registry;
}

// Handle token with type checking
struct HandleId { UInt slot; UInt gen; };

[[nodiscard]] inline Value make_handle_token(UInt slot, UInt gen) {
    // Store in Generic node with metadata
    auto& gen_node = file_handle_registry().Unsafe([](auto&& proxy) -> decltype(auto) {
        return proxy.GetGeneric();
    });

    Object handle_entry;
    handle_entry["type"] = Value{String{k_handle_tag}};
    handle_entry["slot"] = Value{slot};
    handle_entry["gen"] = Value{gen};

    gen_node["handle_" + std::to_string(slot) + "_" + std::to_string(gen)] = Value{std::move(handle_entry)};

    // Return array with type tag, slot, and gen for passing around
    Array token;
    token.Reserve(3);
    token.PushBack(Value{String{k_handle_tag}});
    token.PushBack(Value{slot});
    token.PushBack(Value{gen});
    return Value{std::move(token)};
}

[[nodiscard]] inline std::optional<HandleId> handle_id_from_value(const Value& v) {
    const auto& arr = v.TryGetArray();
    if (!arr || arr->Size() != 3) return std::nullopt;
    const auto& tag = arr->TryGet(0)->TryGetString();
    if (!tag || *tag != k_handle_tag) return std::nullopt;
    const auto& sn = arr->TryGet(1)->TryGetNumber();
    const auto& gn = arr->TryGet(2)->TryGetNumber();
    if (!sn || !gn) return std::nullopt;
    auto as_uint = [](const Number& n) -> std::optional<UInt> {
        return n.Visit(
            [](Int i)   -> std::optional<UInt> { return i >= 0 ? std::optional<UInt>(static_cast<UInt>(i)) : std::nullopt; },
            [](UInt u)  -> std::optional<UInt> { return u; },
            [](Float d) -> std::optional<UInt> { return d >= 0 && std::floor(d)==d ? std::optional<UInt>(static_cast<UInt>(d)) : std::nullopt; }
        );
    };
    auto slot = as_uint(*sn);
    auto gen  = as_uint(*gn);
    if (!slot || !gen) return std::nullopt;
    return HandleId{*slot, *gen};
}

[[nodiscard]] inline HandleEntry& require_handle(const Value& token, const char* op) {
    auto id = handle_id_from_value(token);
    if (!id) throw std::runtime_error{std::string(op) + ": not a valid handle token"};
    HandleEntry* e = handle_registry().get(id->slot, id->gen);
    if (!e) throw std::runtime_error{std::string(op) + ": handle is closed or invalid"};
    return *e;
}

[[nodiscard]] inline Value invoke_callable_ref(const Value& ref, Value arg) {
    const auto id = callable_id_from_value(ref);
    if (!id) {
        throw std::runtime_error{"Expected callable reference"};
    }
    const auto& call_reg = callable_registry();
    if (*id >= call_reg.size()) {
        throw std::runtime_error{"Unknown callable reference"};
    }
    const auto& callable = call_reg.at(*id);

    return callable(std::move(arg));
}

/**
 * @brief A node has a call operator that accepts a Value and returns a Value
 */
template <typename Node>
concept NodeLike = requires(Node&& node, Value arg) {
    { std::invoke(std::forward<Node>(node), std::move(arg)) } -> std::same_as<Value>;
};

// Pipeline for builtins that receive a Value and return a Value.
template <NodeLike Node>
Value operator|(Value arg, Node&& node) {
    return std::invoke(std::forward<Node>(node), std::move(arg));
}

inline Value operator|(Value arg, const Value& callable_ref) {
    return invoke_callable_ref(callable_ref, std::move(arg));
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

template <typename MaybeValue>
concept ValueLike = requires(MaybeValue&& vals) {
    requires (std::same_as<std::remove_cvref_t<MaybeValue>, Value>);
};

template <ValueLike... Values>
Value make_tuple(Values&&... vals) {
    Array arr;
    arr.Reserve(sizeof...(Values));
    (arr.PushBack(std::forward<Values>(vals)), ...);
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

[[nodiscard]] inline const Object& as_object(const Value& v) {
    auto r = v.TryGetObject();
    if (!r) throw std::runtime_error{"fleaux::runtime: expected Object"};
    return *r;
}

[[nodiscard]] inline Object& as_object(Value& v) {
    auto r = v.TryGetObject();
    if (!r) throw std::runtime_error{"fleaux::runtime: expected Object"};
    return *r;
}

// Get the Nth element of an Array Value (throws on out-of-range).
[[nodiscard]] inline const Value& array_at(const Value& v, const std::size_t i) {
    auto r = as_array(v).TryGet(i);
    if (!r) throw std::out_of_range{"fleaux::runtime: array index out of range"};
    return *r;
}

[[nodiscard]] inline Value& array_at(Value& v, const std::size_t i) {
    auto r = v.TryGetArray();
    if (!r) throw std::runtime_error{"fleaux::runtime: expected Array"};
    Array& arr = *r;
    if (i >= arr.Size()) throw std::out_of_range{"fleaux::runtime: array index out of range"};
    return arr[i];
}

// Convert any numeric Value to double.
[[nodiscard]] inline double to_double(const Value& val) {
    return as_number(val).Visit(
        [](const Int   i) -> double { return static_cast<double>(i); },
        [](const UInt  u) -> double { return static_cast<double>(u); },
        [](const Float d) -> double { return d; }
    );
}

// Convert a double result back to the most correct Number Value.
[[nodiscard]] inline Value num_result(const double val) {
    if (val == std::floor(val) &&
        val >= static_cast<double>(std::numeric_limits<Int>::min()) &&
        val <= static_cast<double>(std::numeric_limits<Int>::max())) {
        return make_int(static_cast<Int>(val));
    }
    return make_float(val);
}

// Extract the integer index embedded in a Number Value.
[[nodiscard]] inline std::size_t as_index(const Value& val) {
    return as_number(val).Visit(
        [](const Int   i) -> std::size_t { return static_cast<std::size_t>(i); },
        [](const UInt  u) -> std::size_t { return static_cast<std::size_t>(u); },
        [](const Float d) -> std::size_t { return static_cast<std::size_t>(d); }
    );
}

[[nodiscard]] inline Int as_int_value(const Value& val) {
    return as_number(val).Visit(
        [](const Int i) -> Int { return i; },
        [](const UInt u) -> Int { return static_cast<Int>(u); },
        [](const Float d) -> Int { return static_cast<Int>(d); }
    );
}

// Python make_node semantics: when one argument is provided, the callee sees
// the scalar value instead of a 1-tuple wrapper.
[[nodiscard]] inline Value unwrap_singleton_arg(Value val) {
    if (val.HasArray()) {
        if (const auto& arr = as_array(val); arr.Size() == 1) {
            return *arr.TryGet(0);
        }
    }
    return val;
}

// ── Value printing ────────────────────────────────────────────────────────────

[[nodiscard]] inline std::string format_number(const Number& num) {
    return num.Visit(
        [](const Int i) -> std::string { return std::format("{}", i); },
        [](const UInt u) -> std::string { return std::format("{}", u); },
        [](const Float d) -> std::string { return std::format("{}", d); }
    );
}

// Print a Value as a scalar/tuple repr for the C++ runtime.
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
        [&](const Object& obj) {
            // Objects are the backing store for Fleaux dicts.
            // Keys carry a type prefix ("s:", "n:", "b:", "z:") so that
            // different key types never collide (e.g. int 1 vs string "1").
            os << '{';
            // Collect and sort internal keys so output is deterministic.
            std::vector<std::string> sorted_keys;
            sorted_keys.reserve(obj.Size());
            for (const auto &k: obj | std::views::keys) sorted_keys.push_back(k);
            std::ranges::sort(sorted_keys);
            bool first = true;
            for (const auto& ikey : sorted_keys) {
                if (!first) os << ", ";
                first = false;
                // Strip the type prefix and render the original key value.
                if (ikey.size() >= 2 && ikey[1] == ':') {
                    const char tag  = ikey[0];
                    const auto& pay = ikey.substr(2);
                    if      (tag == 's') os << pay;
                    else if (tag == 'b') os << (pay == "true" ? "True" : "False");
                    else if (tag == 'z') os << "Null";
                    else                 os << pay;   // 'n' (number) or unknown
                } else {
                    os << ikey;  // legacy key without a prefix
                }
                os << ": ";
                if (const auto got = obj.TryGet(ikey)) print_value_repr(os, *got);
            }
            os << '}';
        },
        [&](const Generic& /*gen*/) {
            os << "<generic>";
        },
        [&](const ValueNode& vn) {
            vn.Visit(
                [&](const Null&)    { os << "Null"; },
                [&](const Bool val)         { os << (val ? "True" : "False"); },
                [&](const Number& val) { os << format_number(val); },
                [&](const String& val) { os << val; }
            );
        }
    );
}

// Tuple args are printed as varargs, space-separated.
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

enum class SortTag {
    Array,
    Generic,
    Object,
    Null,
    Bool,
    Number,
    String,
};

[[nodiscard]] inline SortTag sort_tag_of(const Value& v) {
    const auto tag = v.Visit(
        [](const Array&) { return SortTag::Array; },
        [](const Generic&) { return SortTag::Generic; },
        [](const Object&) { return SortTag::Object; },
        [](const ValueNode& vn)-> SortTag {
            return vn.Visit(
                [](const Null&)-> SortTag { return SortTag::Null; },
                [](const Bool&)-> SortTag { return SortTag::Bool; },
                [](const Number&)-> SortTag { return SortTag::Number; },
                [](const String&)-> SortTag { return SortTag::String; }
            );
        }
    );
    return tag;
}

[[nodiscard]] inline int compare_values_for_sort(const Value& lhs, const Value& rhs);

[[nodiscard]] inline int compare_arrays_for_sort(const Array& lhs, const Array& rhs) {
    const std::size_t n = std::min(lhs.Size(), rhs.Size());
    for (std::size_t i = 0; i < n; ++i) {
        if (const int c = compare_values_for_sort(*lhs.TryGet(i), *rhs.TryGet(i)); c != 0) {
            return c;
        }
    }
    if (lhs.Size() < rhs.Size()) {
        return -1;
    }
    if (lhs.Size() > rhs.Size()) {
        return 1;
    }
    return 0;
}

[[nodiscard]] inline int compare_values_for_sort(const Value& lhs, const Value& rhs) {
    const SortTag lhs_tag = sort_tag_of(lhs);
    if (const SortTag rhs_tag = sort_tag_of(rhs); lhs_tag != rhs_tag) {
        throw std::invalid_argument{"TupleSort supports homogeneous comparable values only"};
    }

    switch (lhs_tag) {
        case SortTag::Null:
            return 0;
        case SortTag::Bool: {
            const bool l = as_bool(lhs);
            const bool r = as_bool(rhs);
            return (l < r) ? -1 : ((l > r) ? 1 : 0);
        }
        case SortTag::Number: {
            const double l = to_double(lhs);
            const double r = to_double(rhs);
            return (l < r) ? -1 : ((l > r) ? 1 : 0);
        }
        case SortTag::String: {
            const String& l = as_string(lhs);
            const String& r = as_string(rhs);
            return (l < r) ? -1 : ((l > r) ? 1 : 0);
        }
        case SortTag::Array:
            return compare_arrays_for_sort(as_array(lhs), as_array(rhs));
        case SortTag::Generic:
            throw std::invalid_argument{"TupleSort does not support sorting generic values"};
        case SortTag::Object:
            throw std::invalid_argument{"TupleSort does not support sorting object values"};
    }
    throw std::invalid_argument{"TupleSort internal error"};
}

[[nodiscard]] inline const Array& require_args(const Value& arg, std::size_t n, const char* name) {
    const auto& args = as_array(arg);
    if (args.Size() != n) {
        throw std::invalid_argument(std::format("{} expects {} arguments", name, n));
    }
    return args;
}

[[nodiscard]] inline std::string as_path_string_unary(Value arg) {
    return to_string(unwrap_singleton_arg(std::move(arg)));
}

[[nodiscard]] inline std::string random_suffix(const std::size_t size = 12) {
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

inline void throw_if_filesystem_error(const std::error_code& ec, std::string_view op) {
    if (ec) {
        throw std::runtime_error(std::format("{} failed: {}", op, ec.message()));
    }
}

[[nodiscard]] inline std::string trim_left(std::string s) {
    const auto it = std::ranges::find_if_not(s, [](const unsigned char c) {
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

[[nodiscard]] inline std::string format_value_plain(const Value& value) {
#if FLEAUX_HAS_STD_FORMAT
    if (const auto num = value.TryGetNumber()) {
        return num->Visit(
            [&](const Int i) -> std::string { return std::vformat("{}", std::make_format_args(i)); },
            [&](const UInt u) -> std::string { return std::vformat("{}", std::make_format_args(u)); },
            [&](const Float d) -> std::string { return std::vformat("{}", std::make_format_args(d)); }
        );
    }
    if (const auto b = value.TryGetBool()) {
        return std::vformat("{}", std::make_format_args(*b));
    }
    if (const auto s = value.TryGetString()) {
        return std::vformat("{}", std::make_format_args(*s));
    }
#endif
    return to_string(value);
}

[[nodiscard]] inline std::string format_value_with_spec(const Value& value, const std::string& spec) {
#if FLEAUX_HAS_STD_FORMAT
    const std::string single_fmt = std::format("{{0:{}}}", spec);
    if (const auto num = value.TryGetNumber()) {
        return num->Visit(
            [&](const Int i) -> std::string { return std::vformat(single_fmt, std::make_format_args(i)); },
            [&](const UInt u) -> std::string { return std::vformat(single_fmt, std::make_format_args(u)); },
            [&](const Float d) -> std::string { return std::vformat(single_fmt, std::make_format_args(d)); }
        );
    }
    if (const auto b = value.TryGetBool()) {
        return std::vformat(single_fmt, std::make_format_args(*b));
    }
    if (const auto s = value.TryGetString()) {
        return std::vformat(single_fmt, std::make_format_args(*s));
    }
    const std::string repr = to_string(value);
    return std::vformat(single_fmt, std::make_format_args(repr));
#else
    throw std::invalid_argument{"Format specs require <format> support"};
#endif
}

[[nodiscard]] inline std::string format_values_fallback(const std::string& fmt, const std::vector<Value>& values) {
    std::string out;
    out.reserve(fmt.size());

    std::size_t next_auto_index = 0;
    bool saw_auto_index = false;
    bool saw_manual_index = false;

    for (std::size_t i = 0; i < fmt.size();) {
        const std::size_t next_special = fmt.find_first_of("{}", i);
        if (next_special == std::string::npos) {
            out.append(fmt, i, fmt.size() - i);
            break;
        }

        if (next_special > i) {
            out.append(fmt, i, next_special - i);
            i = next_special;
            continue;
        }

        const char ch = fmt[i];
        if (ch == '{') {
            if (i + 1 < fmt.size() && fmt[i + 1] == '{') {
                out.push_back('{');
                i += 2;
                continue;
            }

            const std::size_t close = fmt.find('}', i + 1);
            if (close == std::string::npos) {
                throw std::invalid_argument{"Printf format string has unmatched '{'"};
            }

            const std::string field = fmt.substr(i + 1, close - (i + 1));
            std::size_t index = 0;
            std::string spec;

            const std::size_t colon = field.find(':');
            const std::string index_part = (colon == std::string::npos) ? field : field.substr(0, colon);
            if (colon != std::string::npos) {
                spec = field.substr(colon + 1);
            }

            if (index_part.empty()) {
                if (saw_manual_index) {
                    throw std::invalid_argument{"Printf format cannot mix automatic and manual field numbering"};
                }
                saw_auto_index = true;
                index = next_auto_index++;
            } else {
                if (saw_auto_index) {
                    throw std::invalid_argument{"Printf format cannot mix automatic and manual field numbering"};
                }
                saw_manual_index = true;
                for (const char c : index_part) {
                    if (!std::isdigit(static_cast<unsigned char>(c))) {
                        throw std::invalid_argument{"Format supports only '{}'/'{N}' with optional ':spec'"};
                    }
                }
                index = static_cast<std::size_t>(std::stoull(index_part));
            }

            if (index >= values.size()) {
                throw std::invalid_argument{"Format string references a missing argument"};
            }

            if (spec.empty()) {
                out += format_value_plain(values[index]);
            } else {
                out += format_value_with_spec(values[index], spec);
            }

            i = close + 1;
            continue;
        }

        if (ch == '}') {
            if (i + 1 < fmt.size() && fmt[i + 1] == '}') {
                out.push_back('}');
                i += 2;
                continue;
            }
            throw std::invalid_argument{"Printf format string has unmatched '}'"};
        }
    }
    return out;
}

[[nodiscard]] inline std::string format_values(const std::string& fmt, const std::vector<Value>& values) {
    return format_values_fallback(fmt, values);
}

[[nodiscard]] inline std::string format_values(const std::string& fmt, const std::vector<std::string>& values) {
    std::vector<Value> wrapped;
    wrapped.reserve(values.size());
    for (const auto& s : values) {
        wrapped.push_back(make_string(s));
    }
    return format_values(fmt, wrapped);
}

}  // namespace fleaux::runtime
