#pragma once
// Runtime checks for casts from Any (Std.Cast and Std.TryCast). See fleaux/common/runtime_type_check.hpp for the
// check description the compiler stores in bytecode.
//
// Limits of the runtime representation:
// - Strong types are checked by their underlying type; nominal identity is not stored with values.
// - A Result is stored as a (Bool, payload) tuple, so a two-item tuple whose first item is a Bool also passes as a
//   Result, and a Result also passes as such a tuple.
// - Numeric Dict keys share one encoding, so a numeric key passes an integer key type when its text is an integer
//   of that type, and passes Float64 whenever it is numeric.
// - FileHandle, RandomGenerator, and TaskHandle are checked by their runtime tag only.

#include <algorithm>
#include <charconv>
#include <string>
#include <string_view>

#include "fleaux/common/runtime_type_check.hpp"
#include "fleaux/runtime/builtins_core.hpp"
#include "fleaux/runtime/builtins_dict.hpp"
#include "fleaux/runtime/builtins_random.hpp"
#include "fleaux/runtime/value.hpp"

namespace fleaux::runtime {

// True when the value is one of the runtime's tagged tokens (callables, handles, value references, tasks, random
// generators). These are stored as tuples but are never plain tuples.
[[nodiscard]] inline auto is_tagged_runtime_token(const Value& value) -> bool {
  const auto& array = value.TryGetArray();
  if (!array || array->Size() < 2U) {
    return false;
  }
  const auto& tag = array->TryGet(0)->TryGetString();
  if (!tag) {
    return false;
  }
  constexpr std::array<std::string_view, 5> kTags{k_callable_tag, k_handle_tag, k_value_ref_tag, k_task_handle_tag,
                                                   detail::k_random_generator_tag};
  return std::ranges::find(kTags, std::string_view{*tag}) != kTags.end();
}

// A short name for the kind of value, for cast failure messages.
[[nodiscard]] inline auto runtime_value_kind_name(const Value& value) -> std::string {
  if (const auto& array = value.TryGetArray(); array && is_tagged_runtime_token(value)) {
    const std::string_view tag = *array->TryGet(0)->TryGetString();
    if (tag == k_callable_tag) {
      return "a function";
    }
    if (tag == k_handle_tag) {
      return "FileHandle";
    }
    if (tag == k_task_handle_tag) {
      return "TaskHandle";
    }
    if (tag == detail::k_random_generator_tag) {
      return "RandomGenerator";
    }
    return "a value reference";
  }
  return type_name(value);
}

[[nodiscard]] inline auto value_matches_type_check(const Value& value, const common::RuntimeTypeCheck& check) -> bool;

namespace detail {

template <typename Integer>
[[nodiscard]] auto parses_as_integer(const std::string_view text) -> bool {
  Integer parsed{};
  const auto* const end = text.data() + text.size();
  const auto [ptr, error] = std::from_chars(text.data(), end, parsed);
  return error == std::errc{} && ptr == end;
}

// Dict keys are stored with a type prefix (see dict_key_from_value).
[[nodiscard]] inline auto dict_key_matches(const std::string& internal_key, const common::RuntimeTypeCheck& check)
    -> bool {
  using common::RuntimeTypeCheckKind;
  if (check.kind == RuntimeTypeCheckKind::kAny) {
    return true;
  }
  if (check.kind == RuntimeTypeCheckKind::kUnion) {
    return std::ranges::any_of(check.items,
                               [&](const common::RuntimeTypeCheck& member) { return dict_key_matches(internal_key, member); });
  }
  if (internal_key.size() < 2U || internal_key[1] != ':') {
    return false;
  }
  const std::string_view payload = std::string_view{internal_key}.substr(2);
  switch (internal_key[0]) {
    case 's':
      return check.kind == RuntimeTypeCheckKind::kString;
    case 'b':
      return check.kind == RuntimeTypeCheckKind::kBool;
    case 'z':
      return check.kind == RuntimeTypeCheckKind::kNull;
    case 'n':
      switch (check.kind) {
        case RuntimeTypeCheckKind::kInt64:
          return parses_as_integer<Int>(payload);
        case RuntimeTypeCheckKind::kUInt64:
          return parses_as_integer<UInt>(payload);
        case RuntimeTypeCheckKind::kFloat64:
          return true;
        default:
          return false;
      }
    default:
      return false;
  }
}

[[nodiscard]] inline auto tuple_matches(const Array& array, const common::RuntimeTypeCheck& check) -> bool {
  const std::size_t fixed_count = check.variadic_tail ? check.items.size() - 1U : check.items.size();
  if (check.variadic_tail ? array.Size() < fixed_count : array.Size() != fixed_count) {
    return false;
  }
  for (std::size_t index = 0; index < array.Size(); ++index) {
    const auto& item_check = index < fixed_count ? check.items[index] : check.items.back();
    if (!value_matches_type_check(*array.TryGet(index), item_check)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] inline auto number_kind_matches(const Value& value, const common::RuntimeTypeCheckKind kind) -> bool {
  const auto& number = value.TryGetNumber();
  if (!number) {
    return false;
  }
  return number->Visit([&](const Int) { return kind == common::RuntimeTypeCheckKind::kInt64; },
                       [&](const UInt) { return kind == common::RuntimeTypeCheckKind::kUInt64; },
                       [&](const Float) { return kind == common::RuntimeTypeCheckKind::kFloat64; });
}

}  // namespace detail

[[nodiscard]] inline auto value_matches_type_check(const Value& value, const common::RuntimeTypeCheck& check) -> bool {
  using common::RuntimeTypeCheckKind;
  switch (check.kind) {
    case RuntimeTypeCheckKind::kAny:
      return true;
    case RuntimeTypeCheckKind::kNever:
      return false;
    case RuntimeTypeCheckKind::kInt64:
    case RuntimeTypeCheckKind::kUInt64:
    case RuntimeTypeCheckKind::kFloat64:
      return detail::number_kind_matches(value, check.kind);
    case RuntimeTypeCheckKind::kString:
      return value.HasString();
    case RuntimeTypeCheckKind::kBool:
      return value.HasBool();
    case RuntimeTypeCheckKind::kNull:
      return value.HasNull();
    case RuntimeTypeCheckKind::kTuple: {
      const auto& array = value.TryGetArray();
      return array && !is_tagged_runtime_token(value) && detail::tuple_matches(*array, check);
    }
    case RuntimeTypeCheckKind::kUnion:
      return std::ranges::any_of(check.items,
                                 [&](const common::RuntimeTypeCheck& member) { return value_matches_type_check(value, member); });
    case RuntimeTypeCheckKind::kDict: {
      const auto& object = value.TryGetObject();
      if (!object) {
        return false;
      }
      return std::ranges::all_of(*object, [&](const auto& entry) {
        return detail::dict_key_matches(entry.first, check.items[0]) &&
               value_matches_type_check(entry.second, check.items[1]);
      });
    }
    case RuntimeTypeCheckKind::kResult: {
      const auto& array = value.TryGetArray();
      if (!array || array->Size() != 2U || is_tagged_runtime_token(value) || !array->TryGet(0)->HasBool()) {
        return false;
      }
      const bool is_ok = as_bool(*array->TryGet(0));
      return value_matches_type_check(*array->TryGet(1), is_ok ? check.items[0] : check.items[1]);
    }
    case RuntimeTypeCheckKind::kFileHandle:
      return handle_id_from_value(value).has_value();
    case RuntimeTypeCheckKind::kRandomGenerator: {
      const auto& array = value.TryGetArray();
      return array && array->Size() == 3U && array->TryGet(0)->HasString() &&
             as_string(*array->TryGet(0)) == detail::k_random_generator_tag;
    }
    case RuntimeTypeCheckKind::kTaskHandle:
      return parse_tagged_registry_token(value, k_task_handle_tag).has_value();
  }
  return false;
}

[[nodiscard]] inline auto type_check_failure_message(const std::string_view builtin_name, const Value& value,
                                                     const common::RuntimeTypeCheck& check) -> std::string {
  return std::format("{}: expected {}, got {}", builtin_name, common::describe(check), runtime_value_kind_name(value));
}

// Std.Cast<T> from Any: the value, or a runtime error when it does not match.
[[nodiscard]] inline auto check_cast(Value value, const common::RuntimeTypeCheck& check) -> Value {
  if (!value_matches_type_check(value, check)) {
    throw std::invalid_argument{type_check_failure_message("Std.Cast", value, check)};
  }
  return value;
}

// Std.TryCast<T> from Any: Ok with the value, or Err with a message when it does not match.
[[nodiscard]] inline auto try_check_cast(Value value, const common::RuntimeTypeCheck& check) -> Value {
  if (!value_matches_type_check(value, check)) {
    return ResultErr(make_tuple(make_string(type_check_failure_message("Std.TryCast", value, check))));
  }
  return ResultOk(make_tuple(std::move(value)));
}

}  // namespace fleaux::runtime
