#include "fleaux/frontend/type_system/detail/checker_internal.hpp"

#include "fleaux/frontend/type_system/call_shape.hpp"

#include <functional>

namespace fleaux::frontend::type_system::detail {

namespace {

struct FlowTargetResolution {
  std::optional<std::reference_wrapper<const FunctionOverloadSet>> overloads{std::nullopt};
  bool fallback_to_any = false;
};

auto resolve_flow_explicit_type_args(const ir::IRCallTarget& target, const StrongTypeIndex& type_index,
                                     const AliasIndex& alias_index,
                                     const std::unordered_set<std::string>& generic_params)
    -> tl::expected<std::vector<Type>, type_check::AnalysisError> {
  if (const auto* name_ref = std::get_if<ir::IRNameRef>(&target); name_ref != nullptr) {
    auto resolved =
        resolve_explicit_type_args(name_ref->explicit_type_args, type_index, alias_index, generic_params, name_ref->span);
    if (!resolved) {
      return tl::unexpected(resolved.error());
    }
    return *resolved;
  }

  return std::vector<Type>{};
}

auto resolve_flow_target_or_fallback(const ir::IRCallTarget& target, const FunctionIndex& index)
    -> tl::expected<FlowTargetResolution, type_check::AnalysisError> {
  if (const auto overloads = resolve_signature(index, target); overloads.has_value()) {
    return FlowTargetResolution{.overloads = overloads, .fallback_to_any = false};
  }

  if (const auto* name_ref = std::get_if<ir::IRNameRef>(&target); name_ref != nullptr) {
    if (name_ref->qualifier.has_value()) {
      if (is_removed_symbolic_alias(name_ref->qualifier, name_ref->name)) {
        return tl::unexpected(
            make_unresolved_symbol_error(qualified_symbol_name(name_ref->qualifier, name_ref->name), name_ref->span));
      }
      if (index.has_qualified_symbol(name_ref->qualifier, name_ref->name)) {
        return FlowTargetResolution{.overloads = std::nullopt, .fallback_to_any = true};
      }
      return tl::unexpected(
          make_unresolved_symbol_error(qualified_symbol_name(name_ref->qualifier, name_ref->name), name_ref->span));
    }

    if (!index.has_unqualified_symbol(name_ref->name)) {
      return tl::unexpected(make_unresolved_symbol_error(name_ref->name, name_ref->span));
    }
  }

  return FlowTargetResolution{.overloads = std::nullopt, .fallback_to_any = true};
}

auto flow_args_from_lhs_type(const FunctionOverloadSet& overloads, const Type& lhs_type) -> std::vector<Type> {
  if (overloads.size() == 1U && overloads.front().params.size() == 1U && !overloads.front().params[0].variadic) {
    return {lhs_type};
  }

  return args_from_lhs_type(lhs_type);
}

auto finalize_flow_resolution(ir::IRFlowExpr& flow, const FunctionOverloadSet& overloads, const Type& lhs_type,
                              const StrongTypeIndex& type_index,
                              const std::unordered_set<std::string>& generic_params,
                              const std::vector<Type>& explicit_type_args)
    -> tl::expected<Type, type_check::AnalysisError> {
  const auto args = flow_args_from_lhs_type(overloads, lhs_type);
  const auto full_name = target_name(flow.rhs).value_or("<operator>");
  auto checked =
      resolve_overload_invocation(full_name, flow.span, overloads, args, type_index, generic_params, explicit_type_args);
  if (!checked) {
    return tl::unexpected(checked.error());
  }

  if (auto* name_ref = std::get_if<ir::IRNameRef>(&flow.rhs); name_ref != nullptr) {
    name_ref->resolved_symbol_key = checked->resolved_symbol_key;
  }

  flow.call_shape = args.size() == 1U ? ir::IRFlowCallShape::kDirectValue : ir::IRFlowCallShape::kTupleExpanded;

  return checked->return_type;
}

}  // namespace

auto is_std_match_target(const ir::IRCallTarget& target) -> bool {
  const auto* name_ref = std::get_if<ir::IRNameRef>(&target);
  return name_ref != nullptr && name_ref->qualifier.has_value() && *name_ref->qualifier == "Std" &&
         name_ref->name == "Match";
}

auto is_std_apply_target(const ir::IRCallTarget& target) -> bool {
  const auto* name_ref = std::get_if<ir::IRNameRef>(&target);
  return name_ref != nullptr && name_ref->qualifier.has_value() && *name_ref->qualifier == "Std" &&
         name_ref->name == "Apply";
}

auto is_match_wildcard_pattern(const ir::IRExpr& expr) -> bool {
  const auto* constant = std::get_if<ir::IRConstant>(&expr.node);
  return constant != nullptr && std::holds_alternative<std::string>(constant->val) &&
         std::get<std::string>(constant->val) == kMatchWildcardSentinel;
}

auto merge_match_result_types(const Type& lhs, const Type& rhs) -> std::optional<Type> {
  if (is_consistent(lhs, rhs)) {
    return lhs;
  }
  if (is_consistent(rhs, lhs)) {
    return rhs;
  }
  return std::nullopt;
}

auto match_handler_return_type(const Type& handler_type, const Type& subject_type,
                               const std::optional<diag::SourceSpan>& span)
    -> tl::expected<Type, type_check::AnalysisError> {
  if (is_deferred_callable_type(handler_type)) {
    return make_type(TypeKind::kAny);
  }

  if (handler_type.kind != TypeKind::kFunction || !handler_type.function_return.has_value()) {
    return tl::unexpected(make_error("Invalid Std.Match handler.",
                                     std::optional<std::string>{"Handler must be callable as '() => R' or '(S) => R'."},
                                     span));
  }

  const bool accepts_zero = callable_has_fixed_arity(handler_type, 0U);
  const bool accepts_one =
      callable_has_fixed_arity(handler_type, 1U) && callable_accepts_arg(handler_type, 0U, subject_type);
  if (!accepts_zero && !accepts_one) {
    return tl::unexpected(make_error("Invalid Std.Match handler.",
                                     std::optional<std::string>{"Handler must be callable as '() => R' or '(S) => R'."},
                                     span));
  }

  return *handler_type.function_return;
}

auto validate_match_pattern_type(const Type& pattern_type, const Type& subject_type,
                                 const std::optional<diag::SourceSpan>& span)
    -> tl::expected<void, type_check::AnalysisError> {
  if (is_deferred_callable_type(pattern_type)) {
    return {};
  }

  if (pattern_type.kind == TypeKind::kFunction) {
    const bool accepts_subject =
        callable_has_fixed_arity(pattern_type, 1U) && callable_accepts_arg(pattern_type, 0U, subject_type);
    if (!accepts_subject || !callable_returns_type(pattern_type, make_type(TypeKind::kBool))) {
      return tl::unexpected(make_error(
          "Invalid Std.Match predicate pattern.",
          std::optional<std::string>{"Predicate patterns must be callable as '(S) => Bool' and return Bool."}, span));
    }
    return {};
  }

  if (!is_consistent(subject_type, pattern_type)) {
    return tl::unexpected(make_error(
        "Type mismatch in Std.Match pattern.",
        std::optional<std::string>{"Literal pattern type must be compatible with the matched value."}, span));
  }

  return {};
}

auto infer_std_match_expr(ir::IRFlowExpr& flow, const FunctionIndex& index, const StrongTypeIndex& type_index,
                          const AliasIndex& alias_index, const LocalTypes& locals,
                          const std::unordered_set<std::string>& generic_params)
    -> tl::expected<Type, type_check::AnalysisError> {
  auto* match_args = std::get_if<ir::IRTupleExpr>(&flow.lhs->node);
  if (match_args == nullptr || match_args->items.size() < 2U) {
    return tl::unexpected(
        make_error("Invalid Std.Match invocation.",
                   std::optional<std::string>{"Std.Match expects '(value, (pattern, handler), ... )'."}, flow.span));
  }

  auto subject_type = infer_expr(*match_args->items[0], index, type_index, alias_index, locals, generic_params);
  if (!subject_type) {
    return tl::unexpected(subject_type.error());
  }

  std::optional<Type> result_type;

  for (std::size_t idx = 1; idx < match_args->items.size(); ++idx) {
    auto* case_tuple = std::get_if<ir::IRTupleExpr>(&match_args->items[idx]->node);
    if (case_tuple == nullptr || case_tuple->items.size() != 2U) {
      return tl::unexpected(make_error("Invalid Std.Match case.",
                                       std::optional<std::string>{"Each case must be a '(pattern, handler)' tuple."},
                                       match_args->items[idx]->span));
    }

    auto& pattern_expr = *case_tuple->items[0];
    auto& handler_expr = *case_tuple->items[1];

    if (!is_match_wildcard_pattern(pattern_expr)) {
      auto pattern_type = infer_expr(pattern_expr, index, type_index, alias_index, locals, generic_params);
      if (!pattern_type) {
        return tl::unexpected(pattern_type.error());
      }

      if (auto checked_pattern = validate_match_pattern_type(*pattern_type, *subject_type, pattern_expr.span);
          !checked_pattern) {
        return tl::unexpected(checked_pattern.error());
      }
    }

    auto handler_type = infer_expr(handler_expr, index, type_index, alias_index, locals, generic_params);
    if (!handler_type) {
      return tl::unexpected(handler_type.error());
    }

    auto handler_return = match_handler_return_type(*handler_type, *subject_type, handler_expr.span);
    if (!handler_return) {
      return tl::unexpected(handler_return.error());
    }

    if (!result_type.has_value()) {
      result_type = *handler_return;
      continue;
    }

    const auto merged = merge_match_result_types(*result_type, *handler_return);
    if (!merged.has_value()) {
      return tl::unexpected(
          make_error("Type mismatch in Std.Match handlers.",
                     std::optional<std::string>{"All Std.Match handlers must return mutually compatible types."},
                     handler_expr.span));
    }
    result_type = *merged;
  }

  return result_type.value_or(make_type(TypeKind::kAny));
}

auto infer_std_apply_expr(ir::IRFlowExpr& flow, const FunctionIndex& index, const StrongTypeIndex& type_index,
                          const AliasIndex& alias_index, const LocalTypes& locals,
                          const std::unordered_set<std::string>& generic_params)
    -> std::optional<tl::expected<Type, type_check::AnalysisError>> {
  const auto* target_name_ref = std::get_if<ir::IRNameRef>(&flow.rhs);
  if (target_name_ref == nullptr) {
    return std::nullopt;
  }

  auto* apply_args = std::get_if<ir::IRTupleExpr>(&flow.lhs->node);
  if (apply_args == nullptr || apply_args->items.size() != 2U) {
    return std::nullopt;
  }

  auto value_type = infer_expr(*apply_args->items[0], index, type_index, alias_index, locals, generic_params);
  if (!value_type) {
    return tl::unexpected(value_type.error());
  }

  auto func_type = infer_expr(*apply_args->items[1], index, type_index, alias_index, locals, generic_params);
  if (!func_type) {
    return tl::unexpected(func_type.error());
  }

  if (const bool value_is_empty_tuple = value_type->kind == TypeKind::kTuple && value_type->items.empty();
      !value_is_empty_tuple) {
    return std::nullopt;
  }

  auto explicit_type_args = resolve_explicit_type_args(target_name_ref->explicit_type_args, type_index, alias_index,
                                                       generic_params, target_name_ref->span);
  if (!explicit_type_args) {
    return tl::unexpected(explicit_type_args.error());
  }
  if (!explicit_type_args->empty()) {
    return tl::unexpected(
        make_error("Invalid explicit type argument application.",
                   std::optional<std::string>{"Std.Apply zero-arg shorthand does not accept explicit type arguments."},
                   target_name_ref->span));
  }

  if (is_deferred_callable_type(*func_type)) {
    return make_type(TypeKind::kAny);
  }

  if (func_type->kind == TypeKind::kFunction && callable_has_fixed_arity(*func_type, 0U) &&
      func_type->function_return.has_value()) {
    return *func_type->function_return;
  }

  return std::nullopt;
}

auto infer_flow_expr(ir::IRFlowExpr& flow, const FunctionIndex& index, const StrongTypeIndex& type_index,
                     const AliasIndex& alias_index, const LocalTypes& locals,
                     const std::unordered_set<std::string>& generic_params)
    -> tl::expected<Type, type_check::AnalysisError> {
  if (is_std_match_target(flow.rhs)) {
    return infer_std_match_expr(flow, index, type_index, alias_index, locals, generic_params);
  }

  if (is_std_apply_target(flow.rhs)) {
    if (auto zero_arg_apply = infer_std_apply_expr(flow, index, type_index, alias_index, locals, generic_params);
        zero_arg_apply.has_value()) {
      return *zero_arg_apply;
    }
  }

  auto lhs_type = infer_expr(*flow.lhs, index, type_index, alias_index, locals, generic_params);
  if (!lhs_type) {
    return tl::unexpected(lhs_type.error());
  }

  auto explicit_type_args = resolve_flow_explicit_type_args(flow.rhs, type_index, alias_index, generic_params);
  if (!explicit_type_args) {
    return tl::unexpected(explicit_type_args.error());
  }

  auto target_resolution = resolve_flow_target_or_fallback(flow.rhs, index);
  if (!target_resolution) {
    return tl::unexpected(target_resolution.error());
  }
  if (target_resolution->fallback_to_any) {
    return make_type(TypeKind::kAny);
  }

  return finalize_flow_resolution(flow, target_resolution->overloads->get(), *lhs_type, type_index, generic_params,
                                  *explicit_type_args);
}

}  // namespace fleaux::frontend::type_system::detail
