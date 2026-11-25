#pragma once
// A structural description of a type that the VM can check a value against at runtime.
// The type checker builds one for each Std.Cast or Std.TryCast whose argument has static type Any, and one for each
// parameter of a function the host can call (HostEntryCheck). The compiler stores them in the bytecode module.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fleaux::common {

enum class RuntimeTypeCheckKind : std::uint8_t {
  kAny = 0,
  kNever,
  kInt64,
  kUInt64,
  kFloat64,
  kString,
  kBool,
  kNull,
  // `items` are the tuple items; when `variadic_tail` is set the last item repeats zero or more times.
  kTuple,
  // `items` are the union members.
  kUnion,
  // `items` are {key, value}.
  kDict,
  // `items` are {success, error}.
  kResult,
  // Opaque builtin types: only the runtime tag of the value is checked.
  kFileHandle,
  kRandomGenerator,
  kTaskHandle,
  // Host entry checks only. A callable value, checked by its tag: the runtime does not store parameter and return
  // types with callables.
  kCallable,
  // Host entry checks only. One use of generic parameter `type_param`; `items` are the alternatives of its bound
  // (empty when unbounded). Every use of one parameter must match a common alternative.
  kTypeParam,
};

inline constexpr auto kRuntimeTypeCheckKindCount = static_cast<std::uint8_t>(RuntimeTypeCheckKind::kTypeParam) + 1U;

// A bound may list at most this many alternatives, so the alternatives a value matches fit in one bit set.
inline constexpr std::size_t kMaxTypeParamAlternatives = 64;

struct RuntimeTypeCheck {
  RuntimeTypeCheckKind kind{RuntimeTypeCheckKind::kAny};
  std::vector<RuntimeTypeCheck> items{};
  bool variadic_tail{false};
  std::uint32_t type_param{0};

  // Compares every member; a new member must be compared here too. It is written out because GCC, at -O3, reported a
  // null dereference in the defaulted comparison: when one check's items are known to be empty, it does not see that
  // equal sizes keep the comparison out of their (null) storage. Bounding the loop by both sizes makes that visible.
  friend auto operator==(const RuntimeTypeCheck& lhs, const RuntimeTypeCheck& rhs) -> bool {
    if (lhs.kind != rhs.kind || lhs.variadic_tail != rhs.variadic_tail || lhs.type_param != rhs.type_param ||
        lhs.items.size() != rhs.items.size()) {
      return false;
    }
    for (std::size_t index = 0; index < lhs.items.size() && index < rhs.items.size(); ++index) {
      if (!(lhs.items[index] == rhs.items[index])) {
        return false;
      }
    }
    return true;
  }
};

// Checks for the argument of a function called by the host, one per declared parameter. When `variadic_tail` is
// set, the last check applies to each remaining argument. `type_params` names the generic parameters that
// kTypeParam checks refer to.
struct HostEntryCheck {
  std::vector<RuntimeTypeCheck> params{};
  bool variadic_tail{false};
  std::vector<std::string> type_params{};

  friend auto operator==(const HostEntryCheck&, const HostEntryCheck&) -> bool = default;
};

// Number of `items` a well-formed check of this kind has, or -1 when any number is allowed.
[[nodiscard]] constexpr auto runtime_type_check_item_count(const RuntimeTypeCheckKind kind) -> int {
  switch (kind) {
    case RuntimeTypeCheckKind::kTuple:
    case RuntimeTypeCheckKind::kUnion:
    case RuntimeTypeCheckKind::kTypeParam:
      return -1;
    case RuntimeTypeCheckKind::kDict:
    case RuntimeTypeCheckKind::kResult:
      return 2;
    default:
      return 0;
  }
}

// True when the check has a known kind and the item counts each kind requires. Used to validate bytecode.
[[nodiscard]] inline auto is_well_formed(const RuntimeTypeCheck& check) -> bool {
  if (static_cast<std::uint8_t>(check.kind) >= kRuntimeTypeCheckKindCount) {
    return false;
  }
  if (const int count = runtime_type_check_item_count(check.kind);
      count >= 0 && check.items.size() != static_cast<std::size_t>(count)) {
    return false;
  }
  if (check.variadic_tail && (check.kind != RuntimeTypeCheckKind::kTuple || check.items.empty())) {
    return false;
  }
  if (check.kind == RuntimeTypeCheckKind::kUnion && check.items.size() < 2U) {
    return false;
  }
  if (check.kind == RuntimeTypeCheckKind::kTypeParam) {
    // Bounds are closed types: their alternatives cannot use generic parameters.
    if (check.items.size() > kMaxTypeParamAlternatives) {
      return false;
    }
  } else if (check.type_param != 0U) {
    return false;
  }
  for (const auto& item : check.items) {
    if (!is_well_formed(item)) {
      return false;
    }
  }
  return true;
}

// A check that a cast may use: no host-only kinds anywhere inside.
[[nodiscard]] inline auto is_cast_check(const RuntimeTypeCheck& check) -> bool {
  if (check.kind == RuntimeTypeCheckKind::kCallable || check.kind == RuntimeTypeCheckKind::kTypeParam) {
    return false;
  }
  for (const auto& item : check.items) {
    if (!is_cast_check(item)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] inline auto is_well_formed(const HostEntryCheck& entry) -> bool {
  if (entry.variadic_tail && entry.params.empty()) {
    return false;
  }
  for (const auto& param : entry.params) {
    if (!is_well_formed(param)) {
      return false;
    }
  }
  const auto type_params_in_range = [&](const auto& self, const RuntimeTypeCheck& check) -> bool {
    if (check.kind == RuntimeTypeCheckKind::kTypeParam && check.type_param >= entry.type_params.size()) {
      return false;
    }
    for (const auto& item : check.items) {
      if (!self(self, item)) {
        return false;
      }
    }
    return true;
  };
  for (const auto& param : entry.params) {
    if (!type_params_in_range(type_params_in_range, param)) {
      return false;
    }
  }
  return true;
}

// The check written in Fleaux type syntax, for diagnostics.
[[nodiscard]] inline auto describe(const RuntimeTypeCheck& check) -> std::string {
  const auto join = [](const std::vector<RuntimeTypeCheck>& items, const std::string& separator,
                       const bool variadic_tail) -> std::string {
    std::string out;
    for (std::size_t index = 0; index < items.size(); ++index) {
      if (index > 0U) {
        out += separator;
      }
      out += describe(items[index]);
      if (variadic_tail && index + 1U == items.size()) {
        out += "...";
      }
    }
    return out;
  };

  switch (check.kind) {
    case RuntimeTypeCheckKind::kAny:
      return "Any";
    case RuntimeTypeCheckKind::kNever:
      return "Never";
    case RuntimeTypeCheckKind::kInt64:
      return "Int64";
    case RuntimeTypeCheckKind::kUInt64:
      return "UInt64";
    case RuntimeTypeCheckKind::kFloat64:
      return "Float64";
    case RuntimeTypeCheckKind::kString:
      return "String";
    case RuntimeTypeCheckKind::kBool:
      return "Bool";
    case RuntimeTypeCheckKind::kNull:
      return "Null";
    case RuntimeTypeCheckKind::kTuple:
      return "Tuple<" + join(check.items, ", ", check.variadic_tail) + ">";
    case RuntimeTypeCheckKind::kUnion:
      return join(check.items, " | ", false);
    case RuntimeTypeCheckKind::kDict:
      return "Dict<" + join(check.items, ", ", false) + ">";
    case RuntimeTypeCheckKind::kResult:
      return "Result<" + join(check.items, ", ", false) + ">";
    case RuntimeTypeCheckKind::kFileHandle:
      return "FileHandle";
    case RuntimeTypeCheckKind::kRandomGenerator:
      return "RandomGenerator";
    case RuntimeTypeCheckKind::kTaskHandle:
      return "TaskHandle<Any>";
    case RuntimeTypeCheckKind::kCallable:
      return "a function";
    case RuntimeTypeCheckKind::kTypeParam:
      return check.items.empty() ? std::string{"Any"} : join(check.items, " | ", false);
  }
  return "<invalid>";
}

}  // namespace fleaux::common
