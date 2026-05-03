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
    if (tag == 's')
      return make_string(payload);
    if (tag == 'b')
      return make_bool(payload == "true");
    if (tag == 'z')
      return make_null();
    if (tag == 'n') {
      // parse as number
      std::size_t consumed = 0;
      const double parsed_number = std::stod(payload, &consumed);
      if (consumed == payload.size())
        return num_result(parsed_number);
    }
  }
  // Fallback: return as-is (handles any legacy unadorned string keys)
  return make_string(internal_key);
}

[[nodiscard]] inline auto merge_dict_objects(Object base, const Object& overlay) -> Object {
  for (const auto& [internal_key, mapped_value] : overlay) {
    base[internal_key] = mapped_value;
  }
  return base;
}

[[nodiscard]] inline auto merge_dict_values(const Value& base_dict, const Value& overlay_dict) -> Value {
  return Value{merge_dict_objects(as_object(base_dict), as_object(overlay_dict))};
}

// arg = () -> {}
[[nodiscard]] inline auto DictCreate_Void(Value arg) -> Value {
  if (const auto& arr = arg.TryGetArray(); !arr || arr->Size() != 0) {
    throw std::invalid_argument{"DictCreate_Void expects 0 arguments"};
  }
  return Value{Object{}};
}

// arg = (dict,) or dict -> clone(dict)
[[nodiscard]] inline auto DictCreate_Dict(Value arg) -> Value {
  if (const auto& arr = arg.TryGetArray()) {
    if (arr->Size() != 1) {
      throw std::invalid_argument{"DictCreate_Dict expects 1 argument"};
    }
    return Value{as_object(*arr->TryGet(0))};
  }

  return Value{as_object(arg)};
}

// arg = (dict, key, value) -> new_dict
[[nodiscard]] inline auto DictSet(Value arg) -> Value {
  const auto& args = require_args(arg, 3, "DictSet");
  Object out = as_object(*args.TryGet(0));
  out[dict_key_from_value(*args.TryGet(1))] = *args.TryGet(2);
  return Value{std::move(out)};
}

// arg = (dict, key) -> value
[[nodiscard]] inline auto DictGet(Value arg) -> Value {
  const auto& args = require_args(arg, 2, "DictGet");
  const auto& obj = as_object(*args.TryGet(0));
  const auto key = dict_key_from_value(*args.TryGet(1));
  const auto got = obj.TryGet(key);
  if (!got) {
    throw std::runtime_error{"DictGet: key not found"};
  }
  return *got;
}

// arg = (dict, key, default) -> value_or_default
[[nodiscard]] inline auto DictGetDefault(Value arg) -> Value {
  const auto& args = require_args(arg, 3, "DictGetDefault");
  const auto& obj = as_object(*args.TryGet(0));
  const auto key = dict_key_from_value(*args.TryGet(1));
  const auto got = obj.TryGet(key);
  if (!got) {
    return *args.TryGet(2);
  }
  return *got;
}

// arg = (dict, key) -> bool
[[nodiscard]] inline auto DictContains(Value arg) -> Value {
  const auto& args = require_args(arg, 2, "DictContains");
  const auto& obj = as_object(*args.TryGet(0));
  return make_bool(obj.Contains(dict_key_from_value(*args.TryGet(1))));
}

// arg = (dict, key) -> new_dict
[[nodiscard]] inline auto DictDelete(Value arg) -> Value {
  const auto& args = require_args(arg, 2, "DictDelete");
  Object out = as_object(*args.TryGet(0));
  out.Erase(dict_key_from_value(*args.TryGet(1)));
  return Value{std::move(out)};
}

// arg = (dict) or dict -> (k1, k2, ...), sorted by key
[[nodiscard]] inline auto DictKeys(Value arg) -> Value {
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

// arg = (dict) or dict -> values sorted by key
[[nodiscard]] inline auto DictValues(Value arg) -> Value {
  const Value dict_val = unwrap_singleton_arg(std::move(arg));
  const auto& obj = as_object(dict_val);
  const auto keys = sorted_dict_keys(obj);
  Array out;
  out.Reserve(keys.size());
  for (const auto& key : keys) {
    if (const auto got = obj.TryGet(key))
      out.PushBack(*got);
  }
  return Value{std::move(out)};
}

// arg = (dict) or dict -> ((k1,v1), (k2,v2), ...), sorted by key
[[nodiscard]] inline auto DictEntries(Value arg) -> Value {
  const Value dict_val = unwrap_singleton_arg(std::move(arg));
  const auto& obj = as_object(dict_val);
  const auto keys = sorted_dict_keys(obj);
  Array out;
  out.Reserve(keys.size());
  for (const auto& key : keys) {
    if (const auto got = obj.TryGet(key))
      out.PushBack(make_tuple(dict_key_to_value(key), *got));
  }
  return Value{std::move(out)};
}

// arg = (dict) or dict -> {}
[[nodiscard]] inline auto DictClear(Value arg) -> Value {
  const Value dict_val = unwrap_singleton_arg(std::move(arg));
  (void)as_object(dict_val);
  return Value{Object{}};
}

// arg = (dict_base, dict_overlay) -> new_dict
// Keys in dict_overlay overwrite those in dict_base.
[[nodiscard]] inline auto DictMerge(Value arg) -> Value {
  const auto& args = require_args(arg, 2, "DictMerge");
  return merge_dict_values(*args.TryGet(0), *args.TryGet(1));
}

// arg = (dict) or dict -> Int64 (count of entries)
[[nodiscard]] inline auto DictLength(Value arg) -> Value {
  const Value dict_val = unwrap_singleton_arg(std::move(arg));
  return make_int(static_cast<Int>(as_object(dict_val).Size()));
}

}  // namespace fleaux::runtime
