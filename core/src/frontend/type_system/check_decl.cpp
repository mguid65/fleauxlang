#include "fleaux/frontend/type_system/detail/checker_internal.hpp"

#include <format>

namespace fleaux::frontend::type_system {

auto Checker::analyze(const ir::IRProgram& program, const std::unordered_set<std::string>& imported_symbols,
                      const std::vector<ir::IRLet>& imported_typed_lets,
                      const std::vector<ir::IRTypeDecl>& imported_type_decls,
                      const std::vector<ir::IRAliasDecl>& imported_alias_decls) const
    -> tl::expected<ir::IRProgram, type_check::AnalysisError> {
  if (const auto overload_validation = detail::validate_supported_overload_sets(program, imported_typed_lets);
      !overload_validation) {
    return tl::unexpected(overload_validation.error());
  }

  const auto alias_index = detail::validate_alias_declarations(program, imported_type_decls, imported_alias_decls);
  if (!alias_index) {
    return tl::unexpected(alias_index.error());
  }

  if (const auto type_validation = detail::validate_strong_type_declarations(program, imported_type_decls, *alias_index);
      !type_validation) {
    return tl::unexpected(type_validation.error());
  }

  ir::IRProgram annotated_program = program;

  const StrongTypeIndex type_index(annotated_program, *alias_index, imported_type_decls);

  for (const auto& let : annotated_program.lets) {
    std::unordered_set<std::string> generic_param_set;
    generic_param_set.reserve(let.generic_params.size());
    for (const auto& generic_param : let.generic_params) {
      generic_param_set.insert(generic_param);
    }

    for (const auto& [name, type, span] : let.params) {
      (void)name;
      const Type param_type = detail::rewrite_generic_type(from_ir_type(type), generic_param_set);
      if (auto validated = detail::validate_declared_type(param_type, type_index, *alias_index, generic_param_set, span);
          !validated) {
        return tl::unexpected(validated.error());
      }
    }

    const Type declared_return = detail::rewrite_generic_type(from_ir_type(let.return_type), generic_param_set);
    if (auto validated = detail::validate_declared_type(declared_return, type_index, *alias_index, generic_param_set,
                                                        let.span);
        !validated) {
      return tl::unexpected(validated.error());
    }
  }

  const FunctionIndex index(annotated_program, imported_symbols, type_index, *alias_index, imported_typed_lets);

  for (auto& let : annotated_program.lets) {
    if (!let.body.has_value()) {
      continue;
    }

    std::unordered_set<std::string> generic_param_set;
    generic_param_set.reserve(let.generic_params.size());
    for (const auto& generic_param : let.generic_params) {
      generic_param_set.insert(generic_param);
    }

    detail::LocalTypes locals;
    for (const auto& [name, type, span] : let.params) {
      const Type param_type = detail::rewrite_generic_type(from_ir_type(type), generic_param_set);
      auto expanded_param = detail::expand_aliases_in_type(param_type, type_index, *alias_index, generic_param_set, span);
      if (!expanded_param) {
        return tl::unexpected(expanded_param.error());
      }
      locals.insert_or_assign(name, *expanded_param);
    }

    const Type declared_return = detail::rewrite_generic_type(from_ir_type(let.return_type), generic_param_set);
    auto expanded_return =
        detail::expand_aliases_in_type(declared_return, type_index, *alias_index, generic_param_set, let.span);
    if (!expanded_return) {
      return tl::unexpected(expanded_return.error());
    }

    auto inferred_body = detail::infer_expr(*let.body, index, type_index, *alias_index, locals, generic_param_set);
    if (!inferred_body) {
      return tl::unexpected(inferred_body.error());
    }

    if (!is_consistent(*expanded_return, *inferred_body)) {
      return tl::unexpected(detail::make_error(
          "Type mismatch in function return.",
          std::format("{} declares return type that does not match inferred body type.", let.name), let.span));
    }
  }

  for (auto& [expr, span] : annotated_program.expressions) {
    (void)span;
    detail::LocalTypes empty_locals;
    const std::unordered_set<std::string> empty_generic_params;
    if (auto inferred = detail::infer_expr(expr, index, type_index, *alias_index, empty_locals, empty_generic_params);
        !inferred) {
      return tl::unexpected(inferred.error());
    }
  }

  return annotated_program;
}

}  // namespace fleaux::frontend::type_system
