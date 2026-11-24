#pragma once
// Runtime checks for casts from Any (Std.Cast and Std.TryCast). See fleaux/common/runtime_type_check.hpp for the
// check description the compiler stores in bytecode.
//
// Limits of the runtime representation:
// - Strong types are checked by their underlying type; nominal identity is not stored with values.
// - A Result is stored as a (Bool, payload) tuple, so a two-item tuple whose first item is a Bool also passes as a
//   Result, and a Result also passes as such a tuple.
// - FileHandle, TaskHandle, and RandomGenerator are checked by the value's kind.
// - Callables (host entry checks only) are checked by the value's kind: parameter and return types are not stored.

#include <algorithm>
#include <bit>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fleaux/common/runtime_type_check.hpp"
#include "fleaux/runtime/builtins_core.hpp"
#include "fleaux/runtime/value.hpp"

namespace fleaux::runtime {

// A short name for the kind of value, for cast failure messages.
[[nodiscard]] inline auto runtime_value_kind_name(const Value& value) -> std::string {
  switch (value.kind()) {
    case ValueKind::kFunction:
      return "a function";
    case ValueKind::kFileHandle:
      return "FileHandle";
    case ValueKind::kTask:
      return "TaskHandle";
    case ValueKind::kRandomGenerator:
      return "RandomGenerator";
    default:
      return type_name(value);
  }
}

namespace detail {

// For each generic parameter of a host entry check, the bound alternatives that every use so far has matched, as a
// bit set. Casts have no generic parameters and pass no candidates.
using TypeParamCandidates = std::vector<std::uint64_t>;

[[nodiscard]] inline auto matches(const Value& value, const common::RuntimeTypeCheck& check,
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

[[nodiscard]] inline auto tuple_matches(const TupleView items, const common::RuntimeTypeCheck& check,
                                        TypeParamCandidates* candidates) -> bool {
  const std::size_t fixed_count = check.variadic_tail ? check.items.size() - 1U : check.items.size();
  if (check.variadic_tail ? items.size() < fixed_count : items.size() != fixed_count) {
    return false;
  }
  for (std::size_t index = 0; index < items.size(); ++index) {
    const auto& item_check = index < fixed_count ? check.items[index] : check.items.back();
    if (!matches(items[index], item_check, candidates)) {
      return false;
    }
  }
  return true;
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
      return value.is_int();
    case RuntimeTypeCheckKind::kUInt64:
      return value.is_uint();
    case RuntimeTypeCheckKind::kFloat64:
      return value.is_float();
    case RuntimeTypeCheckKind::kString:
      return value.is_string();
    case RuntimeTypeCheckKind::kBool:
      return value.is_bool();
    case RuntimeTypeCheckKind::kNull:
      return value.is_null();
    case RuntimeTypeCheckKind::kTuple:
      return value.is_tuple() && tuple_matches(value.tuple(), check, candidates);
    case RuntimeTypeCheckKind::kUnion:
      return union_matches(check, candidates, [&](const common::RuntimeTypeCheck& member, TypeParamCandidates* attempt) {
        return matches(value, member, attempt);
      });
    case RuntimeTypeCheckKind::kDict:
      return value.is_dict() && std::ranges::all_of(value.dict(), [&](const Dict::Entry& entry) {
               return matches(entry.first, check.items[0], candidates) &&
                      matches(entry.second, check.items[1], candidates);
             });
    case RuntimeTypeCheckKind::kResult: {
      if (!value.is_tuple() || value.tuple().size() != 2U || !value.tuple()[0].is_bool()) {
        return false;
      }
      const bool is_ok = value.tuple()[0].bool_value();
      return matches(value.tuple()[1], is_ok ? check.items[0] : check.items[1], candidates);
    }
    case RuntimeTypeCheckKind::kFileHandle:
      return value.is_file_handle();
    case RuntimeTypeCheckKind::kRandomGenerator:
      return value.is_random_generator();
    case RuntimeTypeCheckKind::kTaskHandle:
      return value.is_task();
    case RuntimeTypeCheckKind::kCallable:
      return value.is_function();
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
    return ResultErr(make_string(type_check_failure_message("Std.TryCast", value, check)));
  }
  return ResultOk(std::move(value));
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
// When `chosen_alternatives` is given, it receives for each generic parameter the index of a bound alternative that
// every use matched (the first such one), for passing the operator functions of that alternative.
[[nodiscard]] inline auto check_host_argument(const Value& arg, const common::HostEntryCheck& entry,
                                              const std::string_view function_name,
                                              std::vector<std::size_t>* chosen_alternatives = nullptr)
    -> std::optional<std::string> {
  detail::TypeParamCandidates candidates(entry.type_params.size(), ~std::uint64_t{0});
  const auto finish = [&](std::optional<std::string> error) -> std::optional<std::string> {
    if (!error.has_value() && chosen_alternatives != nullptr) {
      chosen_alternatives->clear();
      for (const auto remaining : candidates) {
        chosen_alternatives->push_back(static_cast<std::size_t>(std::countr_zero(remaining)));
      }
    }
    return error;
  };

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
    return finish(std::nullopt);
  }

  if (!entry.variadic_tail) {
    if (param_count == 1U) {
      return finish(check_one(arg, entry.params.front(), 0U));
    }
    if (!arg.is_tuple() || arg.tuple().size() != param_count) {
      return finish(std::format("{}: expected a tuple of {} arguments", function_name, param_count));
    }
    for (std::size_t index = 0; index < param_count; ++index) {
      if (auto error = check_one(arg.tuple()[index], entry.params[index], index)) {
        return finish(std::move(error));
      }
    }
    return finish(std::nullopt);
  }

  // Variadic: the VM binds a single variadic parameter to the whole tuple, or to a 1-tuple of a non-tuple value.
  const std::size_t fixed_count = param_count - 1U;
  const auto& tail_check = entry.params.back();
  if (fixed_count == 0U && !arg.is_tuple()) {
    return finish(check_one(arg, tail_check, 0U));
  }
  if (!arg.is_tuple() || arg.tuple().size() < fixed_count) {
    return finish(std::format("{}: expected a tuple of at least {} arguments", function_name, fixed_count));
  }
  const TupleView items = arg.tuple();
  for (std::size_t index = 0; index < items.size(); ++index) {
    if (auto error = check_one(items[index], index < fixed_count ? entry.params[index] : tail_check, index)) {
      return error;
    }
  }
  return finish(std::nullopt);
}

}  // namespace fleaux::runtime
