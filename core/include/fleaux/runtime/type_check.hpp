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
// - FileHandle and TaskHandle are checked by their runtime tag; RandomGenerator by its whole token.
// - Callables (host entry checks only) are checked by their tag: parameter and return types are not stored.

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

namespace detail {

// For each generic parameter of a host entry check, the bound alternatives that every use so far has matched, as a
// bit set. Casts have no generic parameters and pass no candidates.
using TypeParamCandidates = std::vector<std::uint64_t>;

[[nodiscard]] inline auto matches(const Value& value, const common::RuntimeTypeCheck& check,
                                  TypeParamCandidates* candidates) -> bool;

template <typename Integer>
[[nodiscard]] auto parses_as_integer(const std::string_view text) -> bool {
  Integer parsed{};
  const auto* const end = text.data() + text.size();
  const auto [ptr, error] = std::from_chars(text.data(), end, parsed);
  return error == std::errc{} && ptr == end;
}

// Dict keys are stored with a type prefix (see dict_key_from_validated_value).
[[nodiscard]] inline auto dict_key_matches(const std::string& internal_key, const common::RuntimeTypeCheck& check,
                                           TypeParamCandidates* candidates) -> bool;

// The bit set of the bound alternatives that `matches_alternative` accepts, narrowed into the parameter's
// candidates. False when no alternative common to every use remains.
template <typename MatchesAlternative>
[[nodiscard]] auto narrow_type_param(const common::RuntimeTypeCheck& check, TypeParamCandidates* candidates,
                                     MatchesAlternative&& matches_alternative) -> bool {
  if (check.items.empty() || candidates == nullptr || check.type_param >= candidates->size()) {
    return true;
  }
  std::uint64_t matched = 0;
  for (std::size_t index = 0; index < check.items.size(); ++index) {
    if (matches_alternative(check.items[index])) {
      matched |= std::uint64_t{1} << index;
    }
  }
  auto& remaining = (*candidates)[check.type_param];
  remaining &= matched;
  return remaining != 0U;
}

// A union member may narrow generic parameters only when it is the member that matches.
template <typename MatchesMember>
[[nodiscard]] auto union_matches(const common::RuntimeTypeCheck& check, TypeParamCandidates* candidates,
                                 MatchesMember&& matches_member) -> bool {
  for (const auto& member : check.items) {
    if (candidates == nullptr) {
      if (matches_member(member, nullptr)) {
        return true;
      }
      continue;
    }
    TypeParamCandidates attempt = *candidates;
    if (matches_member(member, &attempt)) {
      *candidates = std::move(attempt);
      return true;
    }
  }
  return false;
}

[[nodiscard]] inline auto dict_key_matches(const std::string& internal_key, const common::RuntimeTypeCheck& check,
                                           TypeParamCandidates* candidates) -> bool {
  using common::RuntimeTypeCheckKind;
  if (check.kind == RuntimeTypeCheckKind::kAny) {
    return true;
  }
  if (check.kind == RuntimeTypeCheckKind::kUnion) {
    return union_matches(check, candidates, [&](const common::RuntimeTypeCheck& member, TypeParamCandidates* attempt) {
      return dict_key_matches(internal_key, member, attempt);
    });
  }
  if (check.kind == RuntimeTypeCheckKind::kTypeParam) {
    return narrow_type_param(check, candidates, [&](const common::RuntimeTypeCheck& alternative) {
      return dict_key_matches(internal_key, alternative, nullptr);
    });
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

[[nodiscard]] inline auto tuple_matches(const Array& array, const common::RuntimeTypeCheck& check,
                                        TypeParamCandidates* candidates) -> bool {
  const std::size_t fixed_count = check.variadic_tail ? check.items.size() - 1U : check.items.size();
  if (check.variadic_tail ? array.Size() < fixed_count : array.Size() != fixed_count) {
    return false;
  }
  for (std::size_t index = 0; index < array.Size(); ++index) {
    const auto& item_check = index < fixed_count ? check.items[index] : check.items.back();
    if (!matches(*array.TryGet(index), item_check, candidates)) {
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

[[nodiscard]] inline auto matches(const Value& value, const common::RuntimeTypeCheck& check,
                                  TypeParamCandidates* candidates) -> bool {
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
      return array && !is_tagged_runtime_token(value) && tuple_matches(*array, check, candidates);
    }
    case RuntimeTypeCheckKind::kUnion:
      return union_matches(check, candidates, [&](const common::RuntimeTypeCheck& member, TypeParamCandidates* attempt) {
        return matches(value, member, attempt);
      });
    case RuntimeTypeCheckKind::kDict: {
      const auto& object = value.TryGetObject();
      if (!object) {
        return false;
      }
      return std::ranges::all_of(*object, [&](const auto& entry) {
        return dict_key_matches(entry.first, check.items[0], candidates) &&
               matches(entry.second, check.items[1], candidates);
      });
    }
    case RuntimeTypeCheckKind::kResult: {
      const auto& array = value.TryGetArray();
      if (!array || array->Size() != 2U || is_tagged_runtime_token(value) || !array->TryGet(0)->HasBool()) {
        return false;
      }
      const bool is_ok = as_bool(*array->TryGet(0));
      return matches(*array->TryGet(1), is_ok ? check.items[0] : check.items[1], candidates);
    }
    case RuntimeTypeCheckKind::kFileHandle:
      return handle_id_from_value(value).has_value();
    case RuntimeTypeCheckKind::kRandomGenerator:
      return is_valid_random_generator(value);
    case RuntimeTypeCheckKind::kTaskHandle:
      return parse_tagged_registry_token(value, k_task_handle_tag).has_value();
    case RuntimeTypeCheckKind::kCallable:
      return callable_id_from_value(value).has_value();
    case RuntimeTypeCheckKind::kTypeParam:
      return narrow_type_param(check, candidates, [&](const common::RuntimeTypeCheck& alternative) {
        return matches(value, alternative, nullptr);
      });
  }
  return false;
}

}  // namespace detail

[[nodiscard]] inline auto value_matches_type_check(const Value& value, const common::RuntimeTypeCheck& check) -> bool {
  return detail::matches(value, check, nullptr);
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

namespace detail {

inline auto collect_type_param_names(const common::RuntimeTypeCheck& check, const common::HostEntryCheck& entry,
                                     std::vector<std::string>& out) -> void {
  if (check.kind == common::RuntimeTypeCheckKind::kTypeParam && check.type_param < entry.type_params.size()) {
    const auto& name = entry.type_params[check.type_param];
    if (std::ranges::find(out, name) == out.end()) {
      out.push_back(name);
    }
  }
  for (const auto& item : check.items) {
    collect_type_param_names(item, entry, out);
  }
}

}  // namespace detail

// Checks the argument the host passes to an exported function against the function's declared parameter types,
// following the same argument layout the VM uses to bind parameters. Returns an error message when it does not
// match. Every use of a bounded generic parameter must match one alternative of its bound common to all uses.
[[nodiscard]] inline auto check_host_argument(const Value& arg, const common::HostEntryCheck& entry,
                                              const std::string_view function_name) -> std::optional<std::string> {
  detail::TypeParamCandidates candidates(entry.type_params.size(), ~std::uint64_t{0});

  const auto check_one = [&](const Value& value, const common::RuntimeTypeCheck& check,
                             const std::size_t index) -> std::optional<std::string> {
    if (detail::matches(value, check, &candidates)) {
      return std::nullopt;
    }
    if (std::vector<std::string> type_params; (detail::collect_type_param_names(check, entry, type_params),
                                               !type_params.empty())) {
      detail::TypeParamCandidates fresh(entry.type_params.size(), ~std::uint64_t{0});
      if (detail::matches(value, check, &fresh)) {
        std::string names;
        for (const auto& name : type_params) {
          names += (names.empty() ? "" : ", ") + name;
        }
        return std::format("{}: argument {} ({}) does not match the same alternative as the earlier arguments for {}",
                           function_name, index, runtime_value_kind_name(value), names);
      }
    }
    return std::format("{}: argument {} expected {}, got {}", function_name, index, common::describe(check),
                       runtime_value_kind_name(value));
  };

  const std::size_t param_count = entry.params.size();
  if (param_count == 0U) {
    return std::nullopt;
  }

  if (!entry.variadic_tail) {
    if (param_count == 1U) {
      return check_one(arg, entry.params.front(), 0U);
    }
    const auto& array = arg.TryGetArray();
    if (!array || is_tagged_runtime_token(arg) || array->Size() != param_count) {
      return std::format("{}: expected a tuple of {} arguments", function_name, param_count);
    }
    for (std::size_t index = 0; index < param_count; ++index) {
      if (auto error = check_one(*array->TryGet(index), entry.params[index], index)) {
        return error;
      }
    }
    return std::nullopt;
  }

  // Variadic: the VM binds a single variadic parameter to the whole tuple, or to a 1-tuple of a non-tuple value.
  const std::size_t fixed_count = param_count - 1U;
  const auto& tail_check = entry.params.back();
  if (fixed_count == 0U && !arg.HasArray()) {
    return check_one(arg, tail_check, 0U);
  }
  const auto& array = arg.TryGetArray();
  if (!array || array->Size() < fixed_count) {
    return std::format("{}: expected a tuple of at least {} arguments", function_name, fixed_count);
  }
  for (std::size_t index = 0; index < array->Size(); ++index) {
    if (auto error =
            check_one(*array->TryGet(index), index < fixed_count ? entry.params[index] : tail_check, index)) {
      return error;
    }
  }
  return std::nullopt;
}

}  // namespace fleaux::runtime
