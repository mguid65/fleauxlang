#include "fleaux/frontend/type_system/detail/checker_internal.hpp"

#include <format>

namespace fleaux::frontend::type_system {

namespace {

auto make_generic_param_set(const std::vector<std::string>& generic_params) -> std::unordered_set<std::string> {
  std::unordered_set<std::string> generic_param_set;
  generic_param_set.reserve(generic_params.size());
  for (const auto& generic_param : generic_params) {
    generic_param_set.insert(generic_param);
  }
  return generic_param_set;
}

auto validate_program_declarations(const ir::IRProgram& program,
                                   const std::vector<ir::IRLet>& imported_typed_lets,
                                   const std::vector<ir::IRTypeDecl>& imported_type_decls,
                                   const std::vector<ir::IRAliasDecl>& imported_alias_decls)
    -> tl::expected<AliasIndex, type_check::AnalysisError> {
  if (const auto overload_validation = detail::validate_supported_overload_sets(program, imported_typed_lets);
      !overload_validation) {
    return tl::unexpected(overload_validation.error());
  }

  auto alias_index = detail::validate_alias_declarations(program, imported_type_decls, imported_alias_decls);
  if (!alias_index) {
    return tl::unexpected(alias_index.error());
  }

  if (const auto type_validation = detail::validate_strong_type_declarations(program, imported_type_decls, *alias_index);
      !type_validation) {
    return tl::unexpected(type_validation.error());
  }

  return *alias_index;
}

auto validate_let_signature_types(const ir::IRProgram& program, const StrongTypeIndex& type_index,
                                  const AliasIndex& alias_index) -> tl::expected<void, type_check::AnalysisError> {
  for (const auto& let : program.lets) {
    const auto generic_param_set = make_generic_param_set(let.generic_params);

    for (const auto& [name, type, span] : let.params) {
      (void)name;
      const Type param_type = detail::rewrite_generic_type(from_ir_type(type), generic_param_set);
      if (auto validated = detail::validate_declared_type(param_type, type_index, alias_index, generic_param_set, span);
          !validated) {
        return tl::unexpected(validated.error());
      }
    }

    const Type declared_return = detail::rewrite_generic_type(from_ir_type(let.return_type), generic_param_set);
    if (auto validated =
            detail::validate_declared_type(declared_return, type_index, alias_index, generic_param_set, let.span);
        !validated) {
      return tl::unexpected(validated.error());
    }
  }

  return {};
}

auto make_let_local_types(const ir::IRLet& let, const StrongTypeIndex& type_index, const AliasIndex& alias_index,
                          const std::unordered_set<std::string>& generic_param_set)
    -> tl::expected<detail::LocalTypes, type_check::AnalysisError> {
  detail::LocalTypes locals;
  for (const auto& [name, type, span] : let.params) {
    const Type param_type = detail::rewrite_generic_type(from_ir_type(type), generic_param_set);
    auto expanded_param = detail::expand_aliases_in_type(param_type, type_index, alias_index, generic_param_set, span);
    if (!expanded_param) {
      return tl::unexpected(expanded_param.error());
    }
    locals.insert_or_assign(name, *expanded_param);
  }
  return locals;
}

auto infer_let_bodies(ir::IRProgram& program, const FunctionIndex& index, const StrongTypeIndex& type_index,
                      const AliasIndex& alias_index) -> tl::expected<void, type_check::AnalysisError> {
  for (auto& let : program.lets) {
    if (!let.body.has_value()) {
      continue;
    }

    const auto generic_param_set = make_generic_param_set(let.generic_params);

    auto locals = make_let_local_types(let, type_index, alias_index, generic_param_set);
    if (!locals) {
      return tl::unexpected(locals.error());
    }

    const Type declared_return = detail::rewrite_generic_type(from_ir_type(let.return_type), generic_param_set);
    auto expanded_return =
        detail::expand_aliases_in_type(declared_return, type_index, alias_index, generic_param_set, let.span);
    if (!expanded_return) {
      return tl::unexpected(expanded_return.error());
    }

    auto inferred_body = detail::infer_expr(*let.body, index, type_index, alias_index, *locals, generic_param_set);
    if (!inferred_body) {
      return tl::unexpected(inferred_body.error());
    }

    if (!is_consistent(*expanded_return, *inferred_body)) {
      return tl::unexpected(detail::make_error(
          "Type mismatch in function return.",
          std::format("{} declares return type that does not match inferred body type.", let.name), let.span));
    }
  }

  return {};
}

auto infer_top_level_expressions(ir::IRProgram& program, const FunctionIndex& index, const StrongTypeIndex& type_index,
                                 const AliasIndex& alias_index) -> tl::expected<void, type_check::AnalysisError> {
  for (auto& [expr, span] : program.expressions) {
    (void)span;
    detail::LocalTypes empty_locals;
    const std::unordered_set<std::string> empty_generic_params;
    if (auto inferred = detail::infer_expr(expr, index, type_index, alias_index, empty_locals, empty_generic_params);
        !inferred) {
      return tl::unexpected(inferred.error());
    }
  }

  return {};
}

}  // namespace

auto Checker::analyze(const ir::IRProgram& program, const std::unordered_set<std::string>& imported_symbols,
                      const std::vector<ir::IRLet>& imported_typed_lets,
                      const std::vector<ir::IRTypeDecl>& imported_type_decls,
                      const std::vector<ir::IRAliasDecl>& imported_alias_decls) const
    -> tl::expected<ir::IRProgram, type_check::AnalysisError> {
  auto alias_index =
      validate_program_declarations(program, imported_typed_lets, imported_type_decls, imported_alias_decls);
  if (!alias_index) {
    return tl::unexpected(alias_index.error());
  }

  ir::IRProgram annotated_program = program;

  const StrongTypeIndex type_index(annotated_program, *alias_index, imported_type_decls);
  if (auto signature_validation = validate_let_signature_types(annotated_program, type_index, *alias_index);
      !signature_validation) {
    return tl::unexpected(signature_validation.error());
  }

  const FunctionIndex index(annotated_program, imported_symbols, type_index, *alias_index, imported_typed_lets);
  if (auto inferred_lets = infer_let_bodies(annotated_program, index, type_index, *alias_index); !inferred_lets) {
    return tl::unexpected(inferred_lets.error());
  }

  if (auto inferred_expressions = infer_top_level_expressions(annotated_program, index, type_index, *alias_index);
      !inferred_expressions) {
    return tl::unexpected(inferred_expressions.error());
  }

  return annotated_program;
}

}  // namespace fleaux::frontend::type_system
