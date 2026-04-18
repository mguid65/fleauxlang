#pragma once
// Dictionary builtins (Create, Set, Get, Delete, Keys, Values, Entries, etc.).
// Part of the split runtime support layer; included by fleaux/runtime/runtime_support.hpp.
#include "fleaux/runtime/value.hpp"
namespace fleaux::runtime {
// Dictionary builtins

// Keys are stored as type-prefixed strings to avoid collisions between types
// that have the same serialized form (e.g. integer 1 vs string "1").
//  string     -> "s:<value>"
//  numeric    -> "n:<value>"  (Int64/UInt64/Float64)
//  bool       -> "b:<true|false>"
//  null       -> "z:"
[[nodiscard]] inline auto dict_key_from_value(const Value& key_val) -> std::string {
    switch (sort_tag_of(key_val)) {
        case SortTag::String:
            return "s:" + as_string(key_val);
        case SortTag::Number:
            return "n:" + to_string(key_val);
        case SortTag::Bool:
            return std::string{"b:"} + (as_bool(key_val) ? "true" : "false");
        case SortTag::Null:
            return "z:";
        case SortTag::Array:
        case SortTag::Object:
        case SortTag::Generic:
            throw std::invalid_argument{"Dict keys must be scalar values (null/bool/number/string)"};
    }
    throw std::invalid_argument{"Dict key conversion failed"};
}

// Returns the raw (prefixed) internal keys, sorted.
[[nodiscard]] inline auto sorted_dict_keys(const Object& obj) -> std::vector<std::string> {
    std::vector<std::string> keys;
    keys.reserve(obj.Size());
    for (const auto& internal_key : obj | std::views::keys) {
        keys.push_back(internal_key);
    }
    std::ranges::sort(keys);
    return keys;
}

// Strips the type prefix added by dict_key_from_value and converts back to a
// Fleaux Value so callers (DictKeys, DictEntries) expose the original key type.
[[nodiscard]] inline auto dict_key_to_value(const std::string& internal_key) -> Value {
    if (internal_key.size() >= 2 && internal_key[1] == ':') {
        const char tag = internal_key[0];
        const std::string payload = internal_key.substr(2);
        if (tag == 's') return make_string(payload);
        if (tag == 'b') return make_bool(payload == "true");
        if (tag == 'z') return make_null();
        if (tag == 'n') {
            // parse as number
            std::size_t consumed = 0;
            const double parsed_number = std::stod(payload, &consumed);
            if (consumed == payload.size()) return num_result(parsed_number);
        }
    }
    // Fallback: return as-is (handles any legacy unadorned string keys)
    return make_string(internal_key);
}

struct DictCreate {
    // arg = () -> {}  or  (dict,) -> clone(dict)
    auto operator()(Value arg) const -> Value {
        const auto& arr = arg.TryGetArray();
        if (arr && arr->Size() == 0) {
            return Value{Object{}};
        }

        if (arr) {
            if (arr->Size() != 1) {
                throw std::invalid_argument{"DictCreate expects 0 or 1 arguments"};
            }
            return Value{as_object(*arr->TryGet(0))};
        }

        return Value{as_object(arg)};
    }
};

struct DictSet {
    // arg = (dict, key, value) -> new_dict
    auto operator()(Value arg) const -> Value {
        const auto& args = require_args(arg, 3, "DictSet");
        Object out = as_object(*args.TryGet(0));
        out[dict_key_from_value(*args.TryGet(1))] = *args.TryGet(2);
        return Value{std::move(out)};
    }
};

struct DictGet {
    // arg = (dict, key) -> value
    auto operator()(Value arg) const -> Value {
        const auto& args = require_args(arg, 2, "DictGet");
        const auto& obj = as_object(*args.TryGet(0));
        const auto key = dict_key_from_value(*args.TryGet(1));
        const auto got = obj.TryGet(key);
        if (!got) {
            throw std::runtime_error{"DictGet: key not found"};
        }
        return *got;
    }
};

struct DictGetDefault {
    // arg = (dict, key, default) -> value_or_default
    auto operator()(Value arg) const -> Value {
        const auto& args = require_args(arg, 3, "DictGetDefault");
        const auto& obj = as_object(*args.TryGet(0));
        const auto key = dict_key_from_value(*args.TryGet(1));
        const auto got = obj.TryGet(key);
        if (!got) {
            return *args.TryGet(2);
        }
        return *got;
    }
};

struct DictContains {
    // arg = (dict, key) -> bool
    auto operator()(Value arg) const -> Value {
        const auto& args = require_args(arg, 2, "DictContains");
        const auto& obj = as_object(*args.TryGet(0));
        return make_bool(obj.Contains(dict_key_from_value(*args.TryGet(1))));
    }
};

struct DictDelete {
    // arg = (dict, key) -> new_dict
    auto operator()(Value arg) const -> Value {
        const auto& args = require_args(arg, 2, "DictDelete");
        Object out = as_object(*args.TryGet(0));
        out.Erase(dict_key_from_value(*args.TryGet(1)));
        return Value{std::move(out)};
    }
};

struct DictKeys {
    // arg = (dict) or dict -> (k1, k2, ...), sorted by key
    auto operator()(Value arg) const -> Value {
        const Value dict_val = unwrap_singleton_arg(std::move(arg));
        const auto& obj = as_object(dict_val);
        const auto keys = sorted_dict_keys(obj);
        Array out;
        out.Reserve(keys.size());
        for (const auto& key : keys) {
            out.PushBack(dict_key_to_value(key));
        }
        return Value{std::move(out)};
    }
};

struct DictValues {
    // arg = (dict) or dict -> values sorted by key
    auto operator()(Value arg) const -> Value {
        const Value dict_val = unwrap_singleton_arg(std::move(arg));
        const auto& obj = as_object(dict_val);
        const auto keys = sorted_dict_keys(obj);
        Array out;
        out.Reserve(keys.size());
        for (const auto& key : keys) {
          if (const auto got = obj.TryGet(key)) out.PushBack(*got);
        }
        return Value{std::move(out)};
    }
};

struct DictEntries {
    // arg = (dict) or dict -> ((k1,v1), (k2,v2), ...), sorted by key
    auto operator()(Value arg) const -> Value {
        const Value dict_val = unwrap_singleton_arg(std::move(arg));
        const auto& obj = as_object(dict_val);
        const auto keys = sorted_dict_keys(obj);
        Array out;
        out.Reserve(keys.size());
        for (const auto& key : keys) {
          if (const auto got = obj.TryGet(key)) out.PushBack(make_tuple(dict_key_to_value(key), *got));
        }
        return Value{std::move(out)};
    }
};

struct DictClear {
    // arg = (dict) or dict -> {}
    auto operator()(Value arg) const -> Value {
        // Validate that the argument is actually a dict, then discard it.
        const Value dict_val = unwrap_singleton_arg(std::move(arg));
        (void)as_object(dict_val);
        return Value{Object{}};
    }
};

struct DictMerge {
    // arg = (dict_base, dict_overlay) -> new_dict
    // Keys in dict_overlay overwrite those in dict_base.
    auto operator()(Value arg) const -> Value {
        const auto& args = require_args(arg, 2, "DictMerge");
        Object out = as_object(*args.TryGet(0));
        for (const auto& overlay = as_object(*args.TryGet(1)); const auto& [internal_key, mapped_value] : overlay) {
            out[internal_key] = mapped_value;
        }
        return Value{std::move(out)};
    }
};

struct DictLength {
    // arg = (dict) or dict -> Int64 (count of entries)
    auto operator()(Value arg) const -> Value {
        const Value dict_val = unwrap_singleton_arg(std::move(arg));
        return make_int(static_cast<Int>(as_object(dict_val).Size()));
    }
};

}  // namespace fleaux::runtime

