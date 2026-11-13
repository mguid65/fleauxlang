#pragma once
// Dictionary builtins (Create, Set, Get, Delete, Keys, Values, Entries, etc.).
// Part of the split runtime support layer; included by fleaux/runtime/runtime_support.hpp.
#include "fleaux/runtime/value.hpp"

namespace fleaux::runtime {
// Dictionary builtins

// Keys are stored as type-prefixed strings; see encode_number_dict_key in value.hpp for the encoding.
[[nodiscard]] inline auto dict_key_from_validated_value(const Value& key_val, const std::string_view builtin_name)
    -> std::string {
  switch (sort_tag_of(key_val)) {
    case SortTag::kString:
      return "s:" + as_string(key_val);
    case SortTag::kNumber:
      return encode_number_dict_key(as_number(key_val));
    case SortTag::kBool:
      return std::string{"b:"} + (as_bool(key_val) ? "true" : "false");
    case SortTag::kNull:
      return "z:";
    case SortTag::kArray:
    case SortTag::kObject:
    case SortTag::kGeneric:
      throw std::logic_error(std::format("{} invariant violated: expected validated scalar Dict key", builtin_name));
  }
  throw std::logic_error(std::format("{} invariant violated: Dict key conversion failed", builtin_name));
}

[[nodiscard]] inline auto validated_dict_object(const Value& dict_val, const std::string_view builtin_name) -> const Object& {
  const auto object = dict_val.TryGetObject();
  if (!object) {
    throw std::logic_error(std::format("{} invariant violated: expected validated Dict value", builtin_name));
  }
  return *object;
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

// Strips the type prefix added by dict_key_from_validated_value and converts back to a
// Fleaux Value so callers (DictKeys, DictEntries) expose the original key type.
[[nodiscard]] inline auto dict_key_to_value(const std::string& internal_key) -> Value {
  if (auto key = decode_dict_key(internal_key)) {
    return std::move(*key);
  }
  throw std::logic_error("Dict invariant violated: unrecognised internal key '" + internal_key + "'");
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
  (void)arg;
  return Value{Object{}};
}

// arg = (dict,) or dict -> clone(dict)
[[nodiscard]] inline auto DictCreate_Dict(Value arg) -> Value {
  return Value{validated_dict_object(unwrap_singleton_arg(std::move(arg)), "Std.Dict.Create")};
}

// arg = (dict, key, value) -> new_dict
[[nodiscard]] inline auto DictSet(Value arg) -> Value {
  Object out = validated_dict_object(array_at(arg, 0), "Std.Dict.Set");
  out[dict_key_from_validated_value(array_at(arg, 1), "Std.Dict.Set")] = array_at(arg, 2);
  return Value{std::move(out)};
}

// arg = (dict, key) -> value
[[nodiscard]] inline auto DictGet(Value arg) -> Value {
  const auto& obj = validated_dict_object(array_at(arg, 0), "Std.Dict.Get");
  const auto key = dict_key_from_validated_value(array_at(arg, 1), "Std.Dict.Get");
  const auto got = obj.TryGet(key);
  if (!got) {
    throw std::runtime_error{"DictGet: key not found"};
  }
  return *got;
}

// arg = (dict, key, default) -> value_or_default
[[nodiscard]] inline auto DictGetDefault(Value arg) -> Value {
  const auto& obj = validated_dict_object(array_at(arg, 0), "Std.Dict.GetDefault");
  const auto key = dict_key_from_validated_value(array_at(arg, 1), "Std.Dict.GetDefault");
  const auto got = obj.TryGet(key);
  if (!got) {
    return array_at(arg, 2);
  }
  return *got;
}

// arg = (dict, key) -> bool
[[nodiscard]] inline auto DictContains(Value arg) -> Value {
  const auto& obj = validated_dict_object(array_at(arg, 0), "Std.Dict.Contains");
  return make_bool(obj.Contains(dict_key_from_validated_value(array_at(arg, 1), "Std.Dict.Contains")));
}

// arg = (dict, key) -> new_dict
[[nodiscard]] inline auto DictDelete(Value arg) -> Value {
  Object out = validated_dict_object(array_at(arg, 0), "Std.Dict.Delete");
  out.Erase(dict_key_from_validated_value(array_at(arg, 1), "Std.Dict.Delete"));
  return Value{std::move(out)};
}

// arg = (dict) or dict -> (k1, k2, ...), sorted by key
[[nodiscard]] inline auto DictKeys(Value arg) -> Value {
  const Value dict_val = unwrap_singleton_arg(std::move(arg));
  const auto& obj = validated_dict_object(dict_val, "Std.Dict.Keys");
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
  const auto& obj = validated_dict_object(dict_val, "Std.Dict.Values");
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
  const auto& obj = validated_dict_object(dict_val, "Std.Dict.Entries");
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
  (void)validated_dict_object(dict_val, "Std.Dict.Clear");
  return Value{Object{}};
}

// arg = (dict_base, dict_overlay) -> new_dict
// Keys in dict_overlay overwrite those in dict_base.
[[nodiscard]] inline auto DictMerge(Value arg) -> Value {
  Object base = validated_dict_object(array_at(arg, 0), "Std.Dict.Merge");
  return Value{merge_dict_objects(std::move(base), validated_dict_object(array_at(arg, 1), "Std.Dict.Merge"))};
}

// arg = (dict) or dict -> Int64 (count of entries)
[[nodiscard]] inline auto DictLength(Value arg) -> Value {
  const Value dict_val = unwrap_singleton_arg(std::move(arg));
  return make_int(static_cast<Int>(validated_dict_object(dict_val, "Std.Dict.Length").Size()));
}

}  // namespace fleaux::runtime
