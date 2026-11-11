#pragma once
// A structural description of a type that the VM can check a value against at runtime.
// The type checker builds one for each Std.Cast or Std.TryCast whose argument has static type Any, the compiler
// stores it in the bytecode module, and the VM checks the cast value against it.

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
};

inline constexpr auto kRuntimeTypeCheckKindCount = static_cast<std::uint8_t>(RuntimeTypeCheckKind::kTaskHandle) + 1U;

struct RuntimeTypeCheck {
  RuntimeTypeCheckKind kind{RuntimeTypeCheckKind::kAny};
  std::vector<RuntimeTypeCheck> items{};
  bool variadic_tail{false};

  friend auto operator==(const RuntimeTypeCheck&, const RuntimeTypeCheck&) -> bool = default;
};

// Number of `items` a well-formed check of this kind has, or -1 when any number is allowed.
[[nodiscard]] constexpr auto runtime_type_check_item_count(const RuntimeTypeCheckKind kind) -> int {
  switch (kind) {
    case RuntimeTypeCheckKind::kTuple:
    case RuntimeTypeCheckKind::kUnion:
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
  for (const auto& item : check.items) {
    if (!is_well_formed(item)) {
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
  }
  return "<invalid>";
}

}  // namespace fleaux::common
