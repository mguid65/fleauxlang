#include "fleaux/frontend/type_system/detail/checker_internal.hpp"

#include <format>

namespace fleaux::frontend::type_system {

auto Checker::analyze(const ir::IRProgram& program, const std::unordered_set<std::string>& imported_symbols,
                      const std::vector<ir::IRLet>& imported_typed_lets) const
    -> tl::expected<ir::IRProgram, type_check::AnalysisError> {
  if (const auto overload_validation = detail::validate_supported_overload_sets(program, imported_typed_lets);
      !overload_validation) {
    return tl::unexpected(overload_validation.error());
  }

  ir::IRProgram annotated_program = program;

  const FunctionIndex index(annotated_program, imported_symbols, imported_typed_lets);

  for (auto& let : annotated_program.lets) {
    if (!let.body.has_value()) { continue; }

    std::unordered_set<std::string> generic_param_set;
    generic_param_set.reserve(let.generic_params.size());
    for (const auto& generic_param : let.generic_params) { generic_param_set.insert(generic_param); }

    detail::LocalTypes locals;
    for (const auto& param : let.params) {
      locals.insert_or_assign(param.name, detail::rewrite_generic_type(from_ir_type(param.type), generic_param_set));
    }

    auto inferred_body = detail::infer_expr(*let.body, index, locals, generic_param_set);
    if (!inferred_body) { return tl::unexpected(inferred_body.error()); }

    const Type declared_return = detail::rewrite_generic_type(from_ir_type(let.return_type), generic_param_set);
    if (!is_consistent(declared_return, *inferred_body)) {
      return tl::unexpected(detail::make_error(
          "Type mismatch in function return.",
          std::format("{} declares return type that does not match inferred body type.", let.name), let.span));
    }
  }

  for (auto& [expr, span] : annotated_program.expressions) {
    (void)span;
    detail::LocalTypes empty_locals;
    const std::unordered_set<std::string> empty_generic_params;
    if (auto inferred = detail::infer_expr(expr, index, empty_locals, empty_generic_params); !inferred) {
      return tl::unexpected(inferred.error());
    }
  }

  return annotated_program;
}

}  // namespace fleaux::frontend::type_system



