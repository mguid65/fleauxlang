// Internal header: shared stdlib-builtin dispatch map for the bytecode runtime.
// Include this after fleaux/runtime/fleaux_runtime.hpp is already in scope.
//
// Defines vm_builtin_callables(), an inline function returning a
// map<string, RuntimeCallable> covering all builtins exposed by the
// FLEAUX_VM_BUILTINS X-macro.
#pragma once

#include <string>
#include <unordered_map>

#include "fleaux/vm/builtin_catalog.hpp"
#include "fleaux/runtime/fleaux_runtime.hpp"

namespace fleaux::vm {

// Returns the complete stdlib builtin dispatch map.
// The map is constructed once (lazily) per program.
[[nodiscard]] inline const std::unordered_map<std::string, fleaux::runtime::RuntimeCallable>&
vm_builtin_callables() {
  using namespace fleaux::runtime;

  static const std::unordered_map<std::string, RuntimeCallable> map = [] {
    std::unordered_map<std::string, RuntimeCallable> out;

#define FLEAUX_INSERT_BUILTIN(name_literal, node_type)                 \
    out.emplace(name_literal, [](Value arg) -> Value {                 \
      return node_type{}(std::move(arg));                              \
    });
    FLEAUX_VM_BUILTINS(FLEAUX_INSERT_BUILTIN)
#undef FLEAUX_INSERT_BUILTIN

    // Numeric constants (zero-arg: ignore the argument, return the constant).
    auto constant = [](const double v) {
      return [v](Value) -> Value { return make_float(v); };
    };
#define FLEAUX_INSERT_CONST_BUILTIN(name_literal, numeric_value) \
    out.emplace(name_literal, constant(numeric_value));
    FLEAUX_VM_CONSTANT_BUILTINS(FLEAUX_INSERT_CONST_BUILTIN)
#undef FLEAUX_INSERT_CONST_BUILTIN

    return out;
  }();
  return map;
}

}  // namespace fleaux::vm

