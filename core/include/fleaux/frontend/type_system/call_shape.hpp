#pragma once

#include <cstddef>
#include <vector>

#include "fleaux/frontend/type_system/type.hpp"

namespace fleaux::frontend::type_system {

[[nodiscard]] auto args_from_lhs_type(const Type& lhs_type) -> std::vector<Type>;

[[nodiscard]] auto is_deferred_callable_type(const Type& type) -> bool;

[[nodiscard]] auto callable_has_fixed_arity(const Type& type, std::size_t arity) -> bool;

[[nodiscard]] auto callable_accepts_arg(const Type& callable_type, std::size_t param_index, const Type& arg_type) -> bool;

[[nodiscard]] auto callable_returns_type(const Type& callable_type, const Type& expected_return) -> bool;

}  // namespace fleaux::frontend::type_system

