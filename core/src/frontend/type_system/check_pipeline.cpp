#include "fleaux/frontend/type_system/detail/checker_internal.hpp"

#include "fleaux/frontend/type_system/call_shape.hpp"

namespace fleaux::frontend::type_system::detail {

auto is_std_match_target(const ir::IRCallTarget& target) -> bool {
  const auto* name_ref = std::get_if<ir::IRNameRef>(&target);
  return name_ref != nullptr && name_ref->qualifier.has_value() && *name_ref->qualifier == "Std" &&
         name_ref->name == "Match";
}

auto is_match_wildcard_pattern(const ir::IRExpr& expr) -> bool {
  const auto* constant = std::get_if<ir::IRConstant>(&expr.node);
  return constant != nullptr && std::holds_alternative<std::string>(constant->val) &&
         std::get<std::string>(constant->val) == kMatchWildcardSentinel;
}

auto merge_match_result_types(const Type& lhs, const Type& rhs) -> std::optional<Type> {
  if (is_consistent(lhs, rhs)) { return lhs; }
  if (is_consistent(rhs, lhs)) { return rhs; }
  return std::nullopt;
}

auto match_handler_return_type(const Type& handler_type, const Type& subject_type,
                               const std::optional<diag::SourceSpan>& span)
    -> tl::expected<Type, type_check::AnalysisError> {
  if (is_deferred_callable_type(handler_type)) { return Type{.kind = TypeKind::kAny}; }

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

  return **handler_type.function_return;
}

auto validate_match_pattern_type(const Type& pattern_type, const Type& subject_type,
                                 const std::optional<diag::SourceSpan>& span)
    -> tl::expected<void, type_check::AnalysisError> {
  if (is_deferred_callable_type(pattern_type)) { return {}; }

  if (pattern_type.kind == TypeKind::kFunction) {
    const bool accepts_subject =
        callable_has_fixed_arity(pattern_type, 1U) && callable_accepts_arg(pattern_type, 0U, subject_type);
    if (!accepts_subject || !callable_returns_type(pattern_type, Type{.kind = TypeKind::kBool})) {
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

auto infer_std_match_expr(ir::IRFlowExpr& flow, const FunctionIndex& index, const LocalTypes& locals,
                          const std::unordered_set<std::string>& generic_params)
    -> tl::expected<Type, type_check::AnalysisError> {
  auto* match_args = std::get_if<ir::IRTupleExpr>(&flow.lhs->node);
  if (match_args == nullptr || match_args->items.size() < 2U) {
    return tl::unexpected(
        make_error("Invalid Std.Match invocation.",
                   std::optional<std::string>{"Std.Match expects '(value, (pattern, handler), ... )'."}, flow.span));
  }

  auto subject_type = infer_expr(*match_args->items[0], index, locals, generic_params);
  if (!subject_type) { return tl::unexpected(subject_type.error()); }

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
      auto pattern_type = infer_expr(pattern_expr, index, locals, generic_params);
      if (!pattern_type) { return tl::unexpected(pattern_type.error()); }

      if (auto checked_pattern = validate_match_pattern_type(*pattern_type, *subject_type, pattern_expr.span);
          !checked_pattern) {
        return tl::unexpected(checked_pattern.error());
      }
    }

    auto handler_type = infer_expr(handler_expr, index, locals, generic_params);
    if (!handler_type) { return tl::unexpected(handler_type.error()); }

    auto handler_return = match_handler_return_type(*handler_type, *subject_type, handler_expr.span);
    if (!handler_return) { return tl::unexpected(handler_return.error()); }

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

  return result_type.value_or(Type{.kind = TypeKind::kAny});
}

auto infer_flow_expr(ir::IRFlowExpr& flow, const FunctionIndex& index, const LocalTypes& locals,
                     const std::unordered_set<std::string>& generic_params)
    -> tl::expected<Type, type_check::AnalysisError> {
  if (is_std_match_target(flow.rhs)) { return infer_std_match_expr(flow, index, locals, generic_params); }

  auto lhs_type = infer_expr(*flow.lhs, index, locals, generic_params);
  if (!lhs_type) { return tl::unexpected(lhs_type.error()); }

  const auto* overloads = resolve_signature(index, flow.rhs);
  if (overloads == nullptr) {
    if (const auto* name_ref = std::get_if<ir::IRNameRef>(&flow.rhs); name_ref != nullptr) {
      if (name_ref->qualifier.has_value()) {
        if (is_removed_symbolic_alias(name_ref->qualifier, name_ref->name)) {
          return tl::unexpected(
              make_unresolved_symbol_error(qualified_symbol_name(name_ref->qualifier, name_ref->name), name_ref->span));
        }
        if (is_symbolic_qualifier(name_ref->qualifier)) { return Type{.kind = TypeKind::kAny}; }
        if (index.has_qualified_symbol(name_ref->qualifier, name_ref->name)) { return Type{.kind = TypeKind::kAny}; }
        return tl::unexpected(
            make_unresolved_symbol_error(qualified_symbol_name(name_ref->qualifier, name_ref->name), name_ref->span));
      }
      if (!index.has_unqualified_symbol(name_ref->name)) {
        return tl::unexpected(make_unresolved_symbol_error(name_ref->name, name_ref->span));
      }
    }
    return Type{.kind = TypeKind::kAny};
  }

  std::vector<Type> args;
  if (overloads->size() == 1U && overloads->front().params.size() == 1U && !overloads->front().params[0].variadic) {
    args = {*lhs_type};
  } else {
    args = args_from_lhs_type(*lhs_type);
  }

  const auto full_name = target_name(flow.rhs).value_or("<operator>");
  auto checked = resolve_overload_invocation(full_name, flow.span, *overloads, args, generic_params);
  if (!checked) { return tl::unexpected(checked.error()); }

  if (auto* name_ref = std::get_if<ir::IRNameRef>(&flow.rhs); name_ref != nullptr) {
    name_ref->resolved_symbol_key = checked->resolved_symbol_key;
  }

  return checked->return_type;
}

}  // namespace fleaux::frontend::type_system::detail
