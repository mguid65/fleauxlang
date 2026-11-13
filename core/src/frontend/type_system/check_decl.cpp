#include "fleaux/frontend/type_system/detail/checker_internal.hpp"

#include <format>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>

#include "fleaux/common/overloaded.hpp"
#include "fleaux/frontend/operators.hpp"

namespace fleaux::frontend::type_system {

namespace {

using detail::bound_has_strong_type;
using detail::GenericBodyContext;
using detail::hidden_operator_param_name;
using detail::same_operator_requirements;
using detail::ScopedGenericBodyContext;

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

using OperatorRequirementsByLet = std::unordered_map<std::string, std::vector<OperatorRequirement>>;

[[nodiscard]] auto let_symbol_key(const ir::IRLet& let) -> std::string {
  if (!let.symbol_key.empty()) {
    return let.symbol_key;
  }
  return let.qualifier.has_value() ? *let.qualifier + "." + let.name : let.name;
}

// Type-checks one let body. Operators on its strong-bounded type parameters, and calls passing them to functions with
// operator requirements, add to `context.requirements`.
auto infer_let_body(ir::IRLet& let, const FunctionIndex& index, const StrongTypeIndex& type_index,
                    const AliasIndex& alias_index, GenericBodyContext& context)
    -> tl::expected<void, type_check::AnalysisError> {
  const ScopedGenericBodyContext scope(&context);
  if (!let.body.has_value()) {
    return {};
  }
  {
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

auto infer_let_bodies(ir::IRProgram& program, const FunctionIndex& index, const StrongTypeIndex& type_index,
                      const AliasIndex& alias_index, const OperatorRequirementsByLet& requirements)
    -> tl::expected<void, type_check::AnalysisError> {
  for (auto& let : program.lets) {
    if (!let.body.has_value()) {
      continue;
    }
    const auto key = let_symbol_key(let);
    const auto known = requirements.find(key);
    GenericBodyContext context{.generic_params = let.generic_params,
                               .requirements = known != requirements.end() ? known->second
                                                                           : std::vector<OperatorRequirement>{}};
    if (auto inferred = infer_let_body(let, index, type_index, alias_index, context); !inferred) {
      return tl::unexpected(inferred.error());
    }
    if (!same_operator_requirements(context.requirements,
                                    known != requirements.end() ? known->second : std::vector<OperatorRequirement>{})) {
      return tl::unexpected(detail::make_error(
          "Internal error.", std::format("Operator requirements of {} changed after discovery.", let.name), let.span));
    }
  }
  return {};
}

// True when some generic parameter of the let is bounded by a strong type, so its body can have operator
// requirements or inherit them from the functions it calls.
[[nodiscard]] auto has_strong_bounded_param(const ir::IRLet& let, const StrongTypeIndex& type_index,
                                            const AliasIndex& alias_index) -> bool {
  auto scope = detail::make_generic_scope(let.generic_params, let.generic_bounds, type_index, alias_index, let.span);
  if (!scope) {
    return false;
  }
  for (const auto& name : let.generic_params) {
    const Type param = detail::rewrite_generic_type(make_nominal_type(name), *scope);
    if (param.kind == TypeKind::kTypeVar && param.type_var_bound.has_value() &&
        bound_has_strong_type(*param.type_var_bound, type_index)) {
      return true;
    }
  }
  return false;
}

// Finds the operator requirements of every generic let. A let's requirements come from its own operators and from
// the requirements of the generic functions it calls with its type parameters, so this repeats over copies of the
// bodies until nothing changes; recursion is covered because a let's call to itself uses its previous requirements.
auto discover_operator_requirements(const ir::IRProgram& program, const std::unordered_set<std::string>& imported_symbols,
                                    const StrongTypeIndex& type_index, const AliasIndex& alias_index,
                                    const std::vector<ir::IRLet>& imported_typed_lets)
    -> tl::expected<OperatorRequirementsByLet, type_check::AnalysisError> {
  OperatorRequirementsByLet requirements;
  std::vector<const ir::IRLet*> candidates;
  for (const auto& let : program.lets) {
    if (let.body.has_value() && has_strong_bounded_param(let, type_index, alias_index)) {
      candidates.push_back(&let);
    }
  }
  if (candidates.empty()) {
    return requirements;
  }

  for (bool changed = true; changed;) {
    changed = false;
    FunctionIndex index(program, imported_symbols, type_index, alias_index, imported_typed_lets);
    for (const auto& [key, found] : requirements) {
      index.set_operator_requirements(key, found);
    }
    for (const auto* let : candidates) {
      const auto key = let_symbol_key(*let);
      ir::IRLet copy = *let;
      GenericBodyContext context{.generic_params = let->generic_params, .requirements = requirements[key]};
      if (auto inferred = infer_let_body(copy, index, type_index, alias_index, context); !inferred) {
        return tl::unexpected(inferred.error());
      }
      if (!same_operator_requirements(context.requirements, requirements[key])) {
        requirements[key] = std::move(context.requirements);
        changed = true;
      }
    }
  }
  std::erase_if(requirements, [](const auto& entry) { return entry.second.empty(); });
  return requirements;
}

// The hidden parameter names a subtree uses. Every closure that uses one captures it from the enclosing body.
auto capture_hidden_operator_params(ir::IRExpr& expr) -> std::unordered_set<std::string>;

auto capture_hidden_operator_params_in(ir::IRExprBox& box) -> std::unordered_set<std::string> {
  return box.has_value() ? capture_hidden_operator_params(*box) : std::unordered_set<std::string>{};
}

auto capture_hidden_operator_params(ir::IRExpr& expr) -> std::unordered_set<std::string> {
  std::unordered_set<std::string> used;
  const auto merge = [&](std::unordered_set<std::string> names) { used.insert(names.begin(), names.end()); };
  std::visit(common::overloaded{
                 [&](ir::IRNameRef& name_ref) {
                   if (!name_ref.qualifier.has_value() && name_ref.name.starts_with("'op")) {
                     used.insert(name_ref.name);
                   }
                   for (auto& bound : name_ref.bound_operator_args) {
                     merge(capture_hidden_operator_params_in(bound));
                   }
                 },
                 [&](ir::IRFlowExpr& flow) { merge(capture_hidden_operator_params_in(flow.lhs)); },
                 [&](ir::IRTupleExpr& tuple) {
                   for (auto& item : tuple.items) {
                     merge(capture_hidden_operator_params_in(item));
                   }
                 },
                 [&](ir::IRConstant&) {},
                 [&](ir::IRClosureExprBox& closure_box) {
                   auto& closure = *closure_box;
                   const auto inner = capture_hidden_operator_params_in(closure.body);
                   for (const auto& name : inner) {
                     if (std::ranges::find(closure.captures, name) == closure.captures.end()) {
                       closure.captures.push_back(name);
                     }
                   }
                   merge(inner);
                 },
                 [&](ir::IRBlockExprBox& block_box) {
                   auto& block = *block_box;
                   for (auto& item : block.items) {
                     std::visit(common::overloaded{
                                    [&](ir::IRLocalLet& local) { merge(capture_hidden_operator_params_in(local.expr)); },
                                    [&](ir::IRBlockExprStatement& statement) {
                                      merge(capture_hidden_operator_params_in(statement.expr));
                                    }},
                                item);
                   }
                   merge(capture_hidden_operator_params_in(block.result));
                 }},
             expr.node);
  return used;
}

// Adds the hidden operator parameters in front of the declared parameters of each let with operator requirements,
// records the requirements on the let (with the function serving each alternative of the bound, for host calls),
// and captures the hidden parameters in the closures of its body that use them.
auto add_hidden_operator_params(ir::IRProgram& program, const OperatorRequirementsByLet& requirements,
                                const FunctionIndex& index, const StrongTypeIndex& type_index,
                                const AliasIndex& alias_index) -> tl::expected<void, type_check::AnalysisError> {
  for (auto& let : program.lets) {
    const auto found = requirements.find(let_symbol_key(let));
    if (found == requirements.end()) {
      continue;
    }
    auto scope = detail::make_generic_scope(let.generic_params, let.generic_bounds, type_index, alias_index, let.span);
    if (!scope) {
      return tl::unexpected(scope.error());
    }
    std::vector<ir::IRParam> params;
    params.reserve(found->second.size() + let.params.size());
    let.operator_requirements.clear();
    for (std::size_t position = 0; position < found->second.size(); ++position) {
      const auto& requirement = found->second[position];
      const Type param = detail::rewrite_generic_type(make_nominal_type(requirement.type_param), *scope);
      ir::IROperatorRequirement recorded{.op = requirement.op,
                                         .type_param = requirement.type_param,
                                         .signature = to_ir_type(requirement.signature)};
      if (param.type_var_bound.has_value()) {
        const Type& bound = *param.type_var_bound;
        for (const auto& alternative : bound.kind == TypeKind::kUnion ? bound.union_members : std::vector{bound}) {
          const Type signature = detail::substitute_type_param(requirement.signature, requirement.type_param, alternative);
          auto function = detail::resolve_operator_function(requirement.op, signature.function_params, let.span, index,
                                                            type_index, *scope);
          if (!function) {
            return tl::unexpected(function.error());
          }
          recorded.alternative_functions.push_back(function->resolved_symbol_key.value_or(function->name));
        }
      }
      let.operator_requirements.push_back(std::move(recorded));
      params.push_back(ir::IRParam{.name = hidden_operator_param_name(position),
                                   .type = to_ir_type(requirement.signature),
                                   .span = let.span});
    }
    for (auto& param : let.params) {
      params.push_back(std::move(param));
    }
    let.params = std::move(params);
    let.hidden_operator_params = found->second.size();
    if (let.body.has_value()) {
      std::ignore = capture_hidden_operator_params(*let.body);
    }
  }
  return {};
}

auto infer_top_level_expressions(ir::IRProgram& program, const FunctionIndex& index, const StrongTypeIndex& type_index,
                                 const AliasIndex& alias_index) -> tl::expected<void, type_check::AnalysisError> {
  GenericBodyContext context;
  const ScopedGenericBodyContext scope(&context);
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

  auto requirements =
      discover_operator_requirements(annotated_program, imported_symbols, type_index, *alias_index, imported_typed_lets);
  if (!requirements) {
    return tl::unexpected(requirements.error());
  }

  FunctionIndex index(annotated_program, imported_symbols, type_index, *alias_index, imported_typed_lets);
  for (const auto& [key, found] : *requirements) {
    index.set_operator_requirements(key, found);
  }
  if (auto operator_functions = validate_operator_functions(annotated_program, index, type_index, *alias_index);
      !operator_functions) {
    return tl::unexpected(operator_functions.error());
  }
  if (auto inferred_lets = infer_let_bodies(annotated_program, index, type_index, *alias_index, *requirements);
      !inferred_lets) {
    return tl::unexpected(inferred_lets.error());
  }

  if (auto inferred_expressions = infer_top_level_expressions(annotated_program, index, type_index, *alias_index);
      !inferred_expressions) {
    return tl::unexpected(inferred_expressions.error());
  }

  if (auto hidden = add_hidden_operator_params(annotated_program, *requirements, index, type_index, *alias_index);
      !hidden) {
    return tl::unexpected(hidden.error());
  }

  return annotated_program;
}

}  // namespace fleaux::frontend::type_system
