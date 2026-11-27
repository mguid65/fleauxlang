#pragma once
// Dictionary builtins (Create, Set, Get, Delete, Keys, Values, Entries, etc.).
// Part of the split runtime support layer; included by fleaux/runtime/runtime_support.hpp.
//
// A dict keeps its entries in the order their keys were first set. The builtins that return a changed dict take the
// dict out of their argument tuple, so a dict no other value holds is changed in place rather than copied.
#include "fleaux/runtime/builtins_core.hpp"
#include "fleaux/runtime/value.hpp"

namespace fleaux::runtime {

// A dict key is a scalar (Null, Bool, a number, or a String) by type.
[[nodiscard]] inline auto validated_dict_key(const Value& key, const std::string_view builtin_name) -> const Value& {
  switch (key.kind()) {
    case ValueKind::kNull:
    case ValueKind::kBool:
    case ValueKind::kInt:
    case ValueKind::kUInt:
    case ValueKind::kFloat:
    case ValueKind::kString:
      return key;
    default:
      throw std::logic_error(std::format("{} invariant violated: expected validated scalar Dict key", builtin_name));
  }
}

[[nodiscard]] inline auto validated_dict(const Value& dict, const std::string_view builtin_name) -> const Dict& {
  if (!dict.is_dict()) {
    throw std::logic_error(std::format("{} invariant violated: expected validated Dict value", builtin_name));
  }
  return dict.dict();
}

// args = () -> {}
[[nodiscard]] inline auto DictCreate_Void(const Arguments& /*args*/) -> Value {
  return make_dict();
}

// arg = (dict,) or dict -> the same dict
[[nodiscard]] inline auto DictCreate_Dict(Value arg) -> Value {
  Value dict = std::move(arg);
  (void)validated_dict(dict, "Std.Dict.Create");
  return dict;
}

// args = (dict, key, value) -> new_dict
[[nodiscard]] inline auto DictSet(const Arguments& args) -> Value {
  Value dict = args.take(0);
  (void)validated_dict(dict, "Std.Dict.Set");
  dict.mutable_dict().set(validated_dict_key(args[1], "Std.Dict.Set"), args.take(2));
  return dict;
}

// args = (dict, key) -> value
[[nodiscard]] inline auto DictGet(const Arguments& args) -> Value {
  const Dict& dict = validated_dict(args[0], "Std.Dict.Get");
  const Value& key = validated_dict_key(args[1], "Std.Dict.Get");
  if (const Value* found = dict.find(key); found != nullptr) {
    return ResultOk(*found);
  }
  return ResultErr(make_string(std::format("Std.Dict.Get: the key {} is not in the dict", to_string(key))));
}

// args = (dict, key, default) -> value_or_default
[[nodiscard]] inline auto DictGetDefault(const Arguments& args) -> Value {
  const Dict& dict = validated_dict(args[0], "Std.Dict.GetDefault");
  const Value* found = dict.find(validated_dict_key(args[1], "Std.Dict.GetDefault"));
  return found == nullptr ? args[2] : *found;
}

// args = (dict, key) -> bool
[[nodiscard]] inline auto DictContains(const Arguments& args) -> Value {
  const Dict& dict = validated_dict(args[0], "Std.Dict.Contains");
  return make_bool(dict.contains(validated_dict_key(args[1], "Std.Dict.Contains")));
}

// args = (dict, key) -> new_dict
[[nodiscard]] inline auto DictDelete(const Arguments& args) -> Value {
  Value dict = args.take(0);
  const Value& key = validated_dict_key(args[1], "Std.Dict.Delete");
  if (validated_dict(dict, "Std.Dict.Delete").contains(key)) {
    dict.mutable_dict().erase(key);
  }
  return dict;
}

// arg = (dict) or dict -> (k1, k2, ...), in insertion order
[[nodiscard]] inline auto DictKeys(Value arg) -> Value {
  const Value dict_value = std::move(arg);
  const Dict& dict = validated_dict(dict_value, "Std.Dict.Keys");
  TupleBuilder out(dict.size());
  for (const auto& [key, item] : dict) {
    out.push(key);
  }
  return std::move(out).finish();
}

// arg = (dict) or dict -> (v1, v2, ...), in insertion order
[[nodiscard]] inline auto DictValues(Value arg) -> Value {
  const Value dict_value = std::move(arg);
  const Dict& dict = validated_dict(dict_value, "Std.Dict.Values");
  TupleBuilder out(dict.size());
  for (const auto& [key, item] : dict) {
    out.push(item);
  }
  return std::move(out).finish();
}

// arg = (dict) or dict -> ((k1, v1), (k2, v2), ...), in insertion order
[[nodiscard]] inline auto DictEntries(Value arg) -> Value {
  const Value dict_value = std::move(arg);
  const Dict& dict = validated_dict(dict_value, "Std.Dict.Entries");
  TupleBuilder out(dict.size());
  for (const auto& [key, item] : dict) {
    out.push(make_tuple(Value{key}, Value{item}));
  }
  return std::move(out).finish();
}

// arg = (dict) or dict -> {}
[[nodiscard]] inline auto DictClear(Value arg) -> Value {
  const Value dict_value = std::move(arg);
  (void)validated_dict(dict_value, "Std.Dict.Clear");
  return make_dict();
}

// args = (dict_base, dict_overlay) -> new_dict
// Keys in dict_overlay overwrite those in dict_base, keeping their place in it.
[[nodiscard]] inline auto DictMerge(const Arguments& args) -> Value {
  Value base = args.take(0);
  (void)validated_dict(base, "Std.Dict.Merge");
  const Dict& overlay = validated_dict(args[1], "Std.Dict.Merge");
  if (overlay.empty()) {
    return base;
  }
  Dict& merged = base.mutable_dict();
  for (const auto& [key, item] : overlay) {
    merged.set(key, item);
  }
  return base;
}

// arg = (dict) or dict -> Int64 (count of entries)
[[nodiscard]] inline auto DictLength(Value arg) -> Value {
  const Value dict_value = std::move(arg);
  return make_int(static_cast<Int>(validated_dict(dict_value, "Std.Dict.Length").size()));
}

// args = (dict, func) -> a dict of the same keys, with func of each value
[[nodiscard]] inline auto DictMap(const Arguments& args) -> Value {
  const Dict& dict = validated_dict(args[0], "Std.Dict.Map");
  const Value& function = args[1];
  Value out = make_dict();
  Dict& mapped = out.mutable_dict();
  mapped.reserve(dict.size());
  for (const auto& [key, item] : dict) {
    mapped.set(key, invoke_callable(function, item));
  }
  return out;
}

// args = (dict, pred) -> the entries pred(key, value) holds for, in their order
[[nodiscard]] inline auto DictFilter(const Arguments& args) -> Value {
  const Dict& dict = validated_dict(args[0], "Std.Dict.Filter");
  const Value& predicate = args[1];
  Value out = make_dict();
  Dict& kept = out.mutable_dict();
  for (const auto& [key, item] : dict) {
    if (as_bool(invoke_binary_callable(predicate, key, item))) {
      kept.set(key, item);
    }
  }
  return out;
}

// args = (dict, key, default, func) -> the dict with key set to func of its value, or of default when it has none.
// A dict the argument holds alone changes in place.
[[nodiscard]] inline auto DictUpdate(const Arguments& args) -> Value {
  Value dict = args.take(0);
  const Value& key = validated_dict_key(args[1], "Std.Dict.Update");
  const Value* found = validated_dict(dict, "Std.Dict.Update").find(key);
  Value updated = invoke_callable(args[3], found != nullptr ? *found : args.take(2));
  dict.mutable_dict().set(key, std::move(updated));
  return dict;
}

}  // namespace fleaux::runtime
