#pragma once

#include <string>
#include <vector>

#include "fleaux/frontend/type_system/type.hpp"

namespace fleaux::frontend::type_system {

class StrongTypeIndex;

// Validates builtin-specific rules that Std signatures cannot express, such as numeric arguments to builtins whose
// parameter is declared Any and scalar dictionary key types.
[[nodiscard]] auto validate_builtin_contract(const std::string& full_name, const std::vector<Type>& args,
                                             const StrongTypeIndex& type_index, std::string& error_message) -> bool;

}  // namespace fleaux::frontend::type_system
