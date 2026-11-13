#include "fleaux/frontend/type_system/detail/checker_internal.hpp"

#include <format>

#include "fleaux/frontend/operators.hpp"

namespace fleaux::frontend::type_system {

namespace {

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

auto validate_let_signature_types(ir::IRProgram& program, const StrongTypeIndex& type_index,
                                  const AliasIndex& alias_index) -> tl::expected<void, type_check::AnalysisError> {
  for (auto& let : program.lets) {
    auto generic_scope =
        detail::make_generic_scope(let.generic_params, let.generic_bounds, type_index, alias_index, let.span);
    if (!generic_scope) {
      return tl::unexpected(generic_scope.error());
    }
    const GenericScope& generic_param_set = *generic_scope;

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

    auto host_entry_check = detail::host_entry_check_for_let(let, type_index, alias_index);
    if (!host_entry_check) {
      return tl::unexpected(host_entry_check.error());
    }
    let.host_entry_check = std::move(*host_entry_check);
  }

  return {};
}

auto make_let_local_types(const ir::IRLet& let, const StrongTypeIndex& type_index, const AliasIndex& alias_index,
                          const GenericScope& generic_param_set)
    -> tl::expected<detail::LocalTypes, type_check::AnalysisError> {
  detail::LocalTypes locals;
  for (const auto& [name, type, span] : let.params) {
    const Type param_type = detail::rewrite_generic_type(from_ir_type(type), generic_param_set);
    auto expanded_param = detail::expand_aliases_in_type(param_type, type_index, alias_index, generic_param_set, span);
    if (!expanded_param) {
      return tl::unexpected(expanded_param.error());
    }
    locals.insert_or_assign(name, param_local_type(*expanded_param, type.variadic));
  }
  return locals;
}

// A function `Name.Op` on a strong type `Name` is an operator function (`Money.Add` is `+` for Money). The Std builtin
// always wins, so an operator function the Std builtin already accepts could never be called by its operator; that is
// an error rather than a silent no-op. In practice this rejects `Name.Equal` and `Name.NotEqual`.
auto validate_operator_functions(const ir::IRProgram& program, const FunctionIndex& index,
                                 const StrongTypeIndex& type_index, const AliasIndex& alias_index)
    -> tl::expected<void, type_check::AnalysisError> {
  for (const auto& let : program.lets) {
    if (!let.qualifier.has_value()) {
      continue;
    }
    const auto op = operator_for_function_name(let.name);
    const auto type_decl = type_index.resolve_name(*let.qualifier);
    if (op.empty() || !type_decl.has_value() || type_decl->get().is_builtin) {
      continue;
    }
    const auto std_overloads = index.resolve_name(std::optional<std::string>{"Std"}, let.name);
    if (!std_overloads.has_value()) {
      continue;
    }

    auto generic_scope =
        detail::make_generic_scope(let.generic_params, let.generic_bounds, type_index, alias_index, let.span);
    if (!generic_scope) {
      return tl::unexpected(generic_scope.error());
    }
    std::vector<Type> param_types;
    param_types.reserve(let.params.size());
    for (const auto& param : let.params) {
      const Type param_type = detail::rewrite_generic_type(from_ir_type(param.type), *generic_scope);
      auto expanded = detail::expand_aliases_in_type(param_type, type_index, alias_index, *generic_scope, param.span);
      if (!expanded) {
        return tl::unexpected(expanded.error());
      }
      param_types.push_back(std::move(*expanded));
    }

    const std::string std_name = "Std." + let.name;
    if (detail::resolve_overload_invocation(std_name, let.span, std_overloads->get(), param_types, type_index,
                                            *generic_scope, {})) {
      return tl::unexpected(detail::make_error(
          "Operator function redefines a builtin operator.",
          std::format("'{}' on {} is provided by {} and cannot be redefined, so {}.{} would never be called by '{}'.",
                      op, *let.qualifier, std_name, *let.qualifier, let.name, op),
          let.span));
    }
  }
  return {};
}

auto infer_let_bodies(ir::IRProgram& program, const FunctionIndex& index, const StrongTypeIndex& type_index,
                      const AliasIndex& alias_index) -> tl::expected<void, type_check::AnalysisError> {
  for (auto& let : program.lets) {
    if (!let.body.has_value()) {
      continue;
    }

    auto generic_scope =
        detail::make_generic_scope(let.generic_params, let.generic_bounds, type_index, alias_index, let.span);
    if (!generic_scope) {
      return tl::unexpected(generic_scope.error());
    }
    const GenericScope& generic_param_set = *generic_scope;

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
          std::format("{} declares return type {} but its body has type {}.{}", let.name,
                      detail::type_debug_name(*expanded_return), detail::type_debug_name(*inferred_body),
                      detail::any_cast_note(*expanded_return, *inferred_body)),
          let.span));
    }
  }

  return {};
}

auto infer_top_level_expressions(ir::IRProgram& program, const FunctionIndex& index, const StrongTypeIndex& type_index,
                                 const AliasIndex& alias_index) -> tl::expected<void, type_check::AnalysisError> {
  for (auto& [expr, span] : program.expressions) {
    (void)span;
    detail::LocalTypes empty_locals;
    const GenericScope empty_generic_params;
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
  if (auto operator_functions = validate_operator_functions(annotated_program, index, type_index, *alias_index);
      !operator_functions) {
    return tl::unexpected(operator_functions.error());
  }
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
