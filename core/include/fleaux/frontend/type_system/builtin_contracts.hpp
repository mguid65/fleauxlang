#pragma once

#include <string>
#include <vector>

#include "fleaux/frontend/type_system/type.hpp"

namespace fleaux::frontend::type_system {

// Validates builtin-specific semantic rules that are not always representable
// in current Std signatures. Some checks are a temporary bridge for Any-based
// higher-order declarations and are intentionally scoped in the implementation.
[[nodiscard]] auto validate_builtin_contract(const std::string& full_name, const std::vector<Type>& args,
                                             std::string& error_message) -> bool;

}  // namespace fleaux::frontend::type_system

