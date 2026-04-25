// Internal header: shared stdlib-builtin dispatch map for the VM runtime.
// Include this after fleaux/runtime/runtime_support.hpp is already in scope.
//
// Defines vm_builtin_registry(), an inline function returning a
// map<string, RuntimeCallable> covering all builtins exposed by the
// FLEAUX_VM_BUILTINS and FLEAUX_VM_FUNCTION_BUILTINS X-macros.
#pragma once

#include <string>
#include <unordered_map>

#include "fleaux/runtime/runtime_support.hpp"
#include "fleaux/vm/builtin_catalog.hpp"

namespace fleaux::vm {

// Returns the complete stdlib builtin registry.
// The registry is constructed once (lazily) per program.
[[nodiscard]] inline auto vm_builtin_registry()
    -> const std::unordered_map<std::string, fleaux::runtime::RuntimeCallable>& {
  using namespace fleaux::runtime;

  static const std::unordered_map<std::string, RuntimeCallable> map =
      []() -> std::unordered_map<std::string, RuntimeCallable> {
    std::unordered_map<std::string, RuntimeCallable> out;

#define FLEAUX_INSERT_BUILTIN(name_literal, node_type) \
  out.emplace(name_literal, [](Value arg) -> Value { return node_type{}(std::move(arg)); });
    FLEAUX_VM_BUILTINS(FLEAUX_INSERT_BUILTIN)
#undef FLEAUX_INSERT_BUILTIN

#define FLEAUX_INSERT_FUNCTION_BUILTIN(name_literal, builtin_function) out.emplace(name_literal, builtin_function);
    FLEAUX_VM_FUNCTION_BUILTINS(FLEAUX_INSERT_FUNCTION_BUILTIN)
#undef FLEAUX_INSERT_FUNCTION_BUILTIN

    // Numeric constants (zero-arg: ignore the argument, return the constant).
    auto constant = [](const double constant_value) {
      return [constant_value](Value) -> Value { return make_float(constant_value); };
    };
#define FLEAUX_INSERT_CONST_BUILTIN(name_literal, numeric_value) out.emplace(name_literal, constant(numeric_value));
    FLEAUX_VM_CONSTANT_BUILTINS(FLEAUX_INSERT_CONST_BUILTIN)
#undef FLEAUX_INSERT_CONST_BUILTIN

    return out;
  }();
  return map;
}

[[nodiscard]] inline auto vm_builtin_callables()
    -> const std::unordered_map<std::string, fleaux::runtime::RuntimeCallable>& {
  return vm_builtin_registry();
}

}  // namespace fleaux::vm
