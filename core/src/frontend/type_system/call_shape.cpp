#include "fleaux/frontend/type_system/call_shape.hpp"

namespace fleaux::frontend::type_system {

auto args_from_lhs_type(const Type& lhs_type) -> std::vector<Type> {
  if (lhs_type.kind == TypeKind::kTuple) { return lhs_type.items; }
  return {lhs_type};
}

auto is_deferred_callable_type(const Type& type) -> bool {
  return type.kind == TypeKind::kAny || type.kind == TypeKind::kUnknown;
}

auto callable_has_fixed_arity(const Type& type, const std::size_t arity) -> bool {
  if (type.kind != TypeKind::kFunction) { return false; }
  if (!type.function_return.has_value()) { return true; }
  if (type.function_params.size() != arity) { return false; }
  for (const auto& param : type.function_params) {
    if (param.variadic) { return false; }
  }
  return true;
}

auto callable_accepts_arg(const Type& callable_type, const std::size_t param_index, const Type& arg_type) -> bool {
  if (is_deferred_callable_type(callable_type)) { return true; }
  if (callable_type.kind != TypeKind::kFunction) { return false; }
  if (!callable_type.function_return.has_value()) { return true; }
  if (param_index >= callable_type.function_params.size()) { return false; }
  return is_consistent(callable_type.function_params[param_index], arg_type);
}

auto callable_returns_type(const Type& callable_type, const Type& expected_return) -> bool {
  if (is_deferred_callable_type(callable_type)) { return true; }
  if (callable_type.kind != TypeKind::kFunction) { return false; }
  if (!callable_type.function_return.has_value()) { return true; }
  return is_consistent(expected_return, **callable_type.function_return);
}

}  // namespace fleaux::frontend::type_system

