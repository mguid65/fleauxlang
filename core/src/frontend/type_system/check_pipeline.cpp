#include "fleaux/frontend/type_system/detail/checker_internal.hpp"

#include "fleaux/frontend/type_system/call_shape.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <span>

#include "fleaux/frontend/operators.hpp"

namespace fleaux::frontend::type_system::detail {

namespace {

struct FlowTargetResolution {
  std::optional<std::reference_wrapper<const FunctionOverloadSet>> overloads{std::nullopt};
};

auto resolve_flow_explicit_type_args(const ir::IRCallTarget& target, const StrongTypeIndex& type_index,
                                     const AliasIndex& alias_index,
                                     const GenericScope& generic_params)
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
    return FlowTargetResolution{.overloads = overloads};
  }

  if (const auto* name_ref = std::get_if<ir::IRNameRef>(&target); name_ref != nullptr) {
    if (name_ref->qualifier.has_value()) {
      if (is_removed_symbolic_alias(name_ref->qualifier, name_ref->name)) {
        return tl::unexpected(
            make_unresolved_symbol_error(qualified_symbol_name(name_ref->qualifier, name_ref->name), name_ref->span));
      }
      if (index.has_qualified_symbol(name_ref->qualifier, name_ref->name)) {
        return tl::unexpected(
            make_untyped_symbol_error(qualified_symbol_name(name_ref->qualifier, name_ref->name), name_ref->span));
      }
      return tl::unexpected(
          make_unresolved_symbol_error(qualified_symbol_name(name_ref->qualifier, name_ref->name), name_ref->span));
    }

    if (!index.has_unqualified_symbol(name_ref->name)) {
      return tl::unexpected(make_unresolved_symbol_error(name_ref->name, name_ref->span));
    }
    return tl::unexpected(make_untyped_symbol_error(name_ref->name, name_ref->span));
  }

  return tl::unexpected(make_error("Unresolved call target.", std::nullopt, std::nullopt));
}

// The arguments of a pipeline call. Parentheses written directly left of '->' are a parameter list: each item is
// one argument, and an item written `x...` spreads the items of the tuple `x`. Anything else on the left of '->',
// including a tuple written with a trailing comma, is one argument. A spread of a tuple of unknown length
// (`Tuple<T...>`) contributes one argument marked variadic, standing for zero or more values of T; it must come last.
struct FlowArgs {
  std::vector<Type> args;
  bool spread = false;
};

auto flow_args_from_lhs(const ir::IRExpr& lhs_expr, const Type& lhs_type)
    -> tl::expected<FlowArgs, type_check::AnalysisError> {
  const auto* tuple = std::get_if<ir::IRTupleExpr>(&lhs_expr.node);
  // Only a spread of a tuple of unknown length marks an argument variadic; a type taken from a `T...` item keeps the
  // flag, so every other argument drops it.
  const auto plain = [](Type type) {
    type.variadic = false;
    return type;
  };
  if (tuple == nullptr || tuple->trailing_comma || lhs_type.kind != TypeKind::kTuple ||
      lhs_type.items.size() != tuple->items.size()) {
    return FlowArgs{.args = {plain(lhs_type)}};
  }

  FlowArgs out;
  for (std::size_t index = 0; index < tuple->items.size(); ++index) {
    const Type& item = lhs_type.items[index];
    if (!tuple->is_spread(index)) {
      out.args.push_back(plain(item));
      continue;
    }
    out.spread = true;
    if (item.kind != TypeKind::kTuple) {
      return tl::unexpected(make_error(
          "Invalid spread.",
          std::format("'x...' spreads the items of a tuple, but this value has type {}.", type_debug_name(item)),
          tuple->items[index]->span.has_value() ? tuple->items[index]->span : tuple->span));
    }
    for (const auto& spread_item : item.items) {
      if (spread_item.variadic && index + 1U != tuple->items.size()) {
        return tl::unexpected(make_error(
            "Invalid spread.",
            std::format("A tuple of unknown length ({}) can only be spread as the last argument, into a variadic "
                        "parameter.",
                        type_debug_name(item)),
            tuple->span));
      }
      out.args.push_back(spread_item);
    }
  }
  return out;
}

// Sets the call shape from the arguments. A parameter list whose spreads leave exactly one fixed argument becomes a
// call with that one value, so the callee receives the value itself; spreads of empty tuples are dropped.
void apply_call_shape(ir::IRFlowExpr& flow, const FlowArgs& flow_args, const Type& lhs_type,
                      const FunctionIndex& index) {
  if (!flow_args.spread) {
    flow.call_shape =
        flow_args.args.size() == 1U ? ir::IRFlowCallShape::kDirectValue : ir::IRFlowCallShape::kTupleExpanded;
    return;
  }
  flow.call_shape = ir::IRFlowCallShape::kTupleExpanded;
  if (flow_args.args.size() != 1U || flow_args.args.front().variadic) {
    return;
  }

  auto& tuple = std::get<ir::IRTupleExpr>(flow.lhs->node);
  for (std::size_t position = 0; position < tuple.items.size(); ++position) {
    const bool supplies_argument =
        !tuple.is_spread(position) || (position < lhs_type.items.size() && lhs_type.items[position].items.size() == 1U);
    if (!supplies_argument) {
      continue;
    }
    ir::IRExprBox item = std::move(tuple.items[position]);
    if (!tuple.is_spread(position)) {
      flow.lhs = std::move(item);
      flow.call_shape = ir::IRFlowCallShape::kDirectValue;
      return;
    }
    // The spread of a one-item tuple: take the item out with Std.ElementAt.
    const auto span = tuple.span;
    std::string element_at_key = "Std.ElementAt";
    if (const auto overloads = index.resolve_name(std::optional<std::string>{"Std"}, "ElementAt");
        overloads.has_value() && !overloads->get().empty()) {
      element_at_key = overloads->get().front().resolved_symbol_key;
    }
    ir::IRTupleExpr element_args{.span = span};
    element_args.items.push_back(std::move(item));
    element_args.items.emplace_back(
        std::in_place, ir::IRExpr{.node = ir::IRConstant{.val = std::uint64_t{0}, .span = span}, .span = span});
    ir::IRFlowExpr element{.lhs = ir::IRExprBox(std::in_place, ir::IRExpr{.node = std::move(element_args), .span = span}),
                           .rhs = ir::IRNameRef{.qualifier = std::string{"Std"},
                                                .name = "ElementAt",
                                                .resolved_symbol_key = element_at_key,
                                                .span = span},
                           .call_shape = ir::IRFlowCallShape::kTupleExpanded,
                           .span = span};
    flow.lhs = ir::IRExprBox(std::in_place, ir::IRExpr{.node = std::move(element), .span = span});
    flow.call_shape = ir::IRFlowCallShape::kDirectValue;
    return;
  }
}

auto describe_operator_builtins(const std::span<const OperatorBuiltin> builtins) -> std::string {
  std::string out;
  for (std::size_t i = 0; i < builtins.size(); ++i) {
    if (i > 0U) {
      out += i + 1U == builtins.size() ? " or " : ", ";
    }
    out += std::format("{}.{}", kOperatorBuiltinQualifier, builtins[i].name);
  }
  return out;
}

// The declared strong types (not builtin types, not aliases) among the operand types, in order, without repeats.
// These are the types whose `Name.Op` functions an operator may call.
auto strong_operand_type_names(const std::vector<Type>& args, const StrongTypeIndex& type_index)
    -> std::vector<std::string> {
  std::vector<std::string> names;
  for (const auto& arg : args) {
    const bool is_named = arg.kind == TypeKind::kNominal || arg.kind == TypeKind::kApplied;
    if (!is_named || type_index.is_builtin_type(arg)) {
      continue;
    }
    const std::string& name = arg.kind == TypeKind::kNominal ? arg.nominal_name : arg.applied_name;
    if (type_index.resolve_name(name).has_value() && std::ranges::find(names, name) == names.end()) {
      names.push_back(name);
    }
  }
  return names;
}

// The function an operator calls for given operand types.
struct OperatorResolution {
  std::optional<std::string> qualifier;
  std::string name;
  ResolvedInvocation invocation;
};

// Resolves an operator for these operand types: the Std builtin selected by the argument count when it accepts them,
// otherwise the user operator function of a strong operand type (`Money.Add` for `+`).
auto resolve_operator(const std::string& op, const std::vector<Type>& args, const std::optional<diag::SourceSpan>& span,
                      const FunctionIndex& index, const StrongTypeIndex& type_index,
                      const GenericScope& generic_params) -> tl::expected<OperatorResolution, type_check::AnalysisError> {
  const auto builtins = operator_builtins(op);
  const std::optional<std::string> qualifier{std::string{kOperatorBuiltinQualifier}};

  // Pick the builtin whose signature accepts this many arguments. When none does, report against the last
  // candidate so the arity diagnostic names a real builtin.
  std::string builtin_name{builtins.back().name};
  for (const auto& builtin : builtins) {
    const auto overloads = index.resolve_name(qualifier, std::string{builtin.name});
    if (overloads.has_value() && std::ranges::any_of(overloads->get(), [&](const FunctionSig& sig) -> bool {
          return call_shape_matches(sig, args.size());
        })) {
      builtin_name = std::string{builtin.name};
      break;
    }
  }

  const std::string full_name = std::format("{}.{}", kOperatorBuiltinQualifier, builtin_name);
  const auto overloads = index.resolve_name(qualifier, builtin_name);
  if (!overloads.has_value()) {
    return tl::unexpected(make_unresolved_symbol_error(full_name, span));
  }

  auto checked = resolve_overload_invocation(full_name, span, overloads->get(), args, type_index, generic_params, {});
  if (checked) {
    return OperatorResolution{.qualifier = qualifier, .name = builtin_name, .invocation = std::move(*checked)};
  }

  // The Std builtin rejects the operands: try the operator functions of the strong operand types.
  std::vector<OperatorResolution> matches;
  std::vector<std::string> tried;
  for (const auto& type_name : strong_operand_type_names(args, type_index)) {
    for (const auto& builtin : builtins) {
      const std::string function_name{builtin.name};
      const auto user_overloads = index.resolve_name(std::optional<std::string>{type_name}, function_name);
      if (!user_overloads.has_value()) {
        continue;
      }
      const std::string user_full_name = std::format("{}.{}", type_name, function_name);
      tried.push_back(user_full_name);
      if (auto user_checked = resolve_overload_invocation(user_full_name, span, user_overloads->get(), args, type_index,
                                                          generic_params, {})) {
        matches.push_back(
            OperatorResolution{.qualifier = type_name, .name = function_name, .invocation = std::move(*user_checked)});
      }
    }
  }

  const auto join = [](const auto& items, const auto& describe) {
    std::string out;
    for (const auto& item : items) {
      out += (out.empty() ? "" : ", ") + describe(item);
    }
    return out;
  };
  if (matches.size() > 1U) {
    return tl::unexpected(make_error(
        "Ambiguous operator.",
        std::format("Operator '{}' matches more than one user operator function: {}.", op,
                    join(matches, [](const OperatorResolution& match) { return *match.qualifier + "." + match.name; })),
        span));
  }
  if (matches.empty()) {
    auto error = checked.error();
    std::string context = std::format("Operator '{}' calls {}.", op, full_name);
    if (!tried.empty()) {
      context += std::format(" No user operator function accepts these operands either (tried {}).",
                             join(tried, [](const std::string& name) { return name; }));
    }
    error.hint = error.hint.has_value() ? context + " " + *error.hint : context;
    return tl::unexpected(std::move(error));
  }
  return std::move(matches.front());
}

}  // namespace

auto substitute_type_param(const Type& type, const std::string& name, const Type& replacement) -> Type {
  if (type.kind == TypeKind::kTypeVar && type.nominal_name == name) {
    Type out = replacement;
    out.variadic = type.variadic;
    return out;
  }
  Type out = type;
  const auto substitute_all = [&](std::vector<Type>& items) {
    for (auto& item : items) {
      item = substitute_type_param(item, name, replacement);
    }
  };
  substitute_all(out.items);
  substitute_all(out.union_members);
  substitute_all(out.applied_args);
  substitute_all(out.function_params);
  if (out.function_return.has_value()) {
    out.function_return =
        common::make_indirect_optional<Type>(substitute_type_param(*out.function_return, name, replacement));
  }
  return normalize_type(std::move(out));
}

namespace {

auto bound_alternatives_of(const Type& bound) -> std::vector<Type> {
  return bound.kind == TypeKind::kUnion ? bound.union_members : std::vector<Type>{bound};
}

using IRExprNode = decltype(ir::IRExpr{}.node);

auto make_expr_box(ir::IRExpr expr) -> ir::IRExprBox { return ir::IRExprBox(std::in_place, std::move(expr)); }

auto make_ir_expr(IRExprNode node, const std::optional<diag::SourceSpan>& span) -> ir::IRExpr {
  return ir::IRExpr{.node = std::move(node), .span = span};
}

auto hidden_param_ref(const std::size_t index, const std::optional<diag::SourceSpan>& span) -> ir::IRExpr {
  return make_ir_expr(ir::IRNameRef{.name = hidden_operator_param_name(index), .span = span}, span);
}

// The Std builtin `name`'s symbol key, for references the checker creates itself.
auto std_symbol_key(const FunctionIndex& index, const std::string& name) -> std::string {
  if (const auto overloads = index.resolve_name(std::optional<std::string>{"Std"}, name);
      overloads.has_value() && !overloads->get().empty()) {
    return overloads->get().front().resolved_symbol_key;
  }
  return "Std." + name;
}

// The hidden operator arguments for a call to a function with operator requirements. For each requirement, the type
// its parameter binds to selects the function: a concrete type gets the operator's function for that type (Std.Add,
// Money.Add); a type parameter of the enclosing generic body whose bound contains a strong type passes the body's
// own hidden parameter, which makes it a requirement of the body too.
auto hidden_operator_args(const std::string& callee, const ResolvedInvocation& invocation,
                          const std::optional<diag::SourceSpan>& span, const FunctionIndex& index,
                          const StrongTypeIndex& type_index, const GenericScope& generic_params)
    -> tl::expected<std::vector<ir::IRExpr>, type_check::AnalysisError> {
  std::vector<ir::IRExpr> hidden;
  for (const auto& requirement : invocation.operator_requirements) {
    const auto binding_it = invocation.bindings.find(requirement.type_param);
    if (binding_it == invocation.bindings.end()) {
      return tl::unexpected(make_error(
          "Unresolved operator requirement.",
          std::format("{} applies '{}' to {}, but {} is not known at this call.", callee, requirement.op,
                      requirement.type_param, requirement.type_param),
          span));
    }
    const Type& binding = binding_it->second;
    const Type signature = substitute_type_param(requirement.signature, requirement.type_param, binding);

    if (binding.kind == TypeKind::kTypeVar && binding.type_var_bound.has_value() &&
        bound_has_strong_type(*binding.type_var_bound, type_index)) {
      auto* context = current_generic_body_context();
      if (context == nullptr || std::ranges::find(context->generic_params, binding.nominal_name) ==
                                    context->generic_params.end()) {
        return tl::unexpected(make_error(
            "Unsupported operator requirement.",
            std::format("{} applies '{}' to {}, which is bound to {} here. Only the type parameters of a named generic "
                        "function can pass operators of strong types on; a generic closure cannot.",
                        callee, requirement.op, requirement.type_param, binding.nominal_name),
            span));
      }
      const auto inherited = operator_requirement_index(
          *context, OperatorRequirement{.op = requirement.op, .type_param = binding.nominal_name, .signature = signature});
      hidden.push_back(hidden_param_ref(inherited, span));
      continue;
    }

    auto resolved = resolve_operator(requirement.op, signature.function_params, span, index, type_index, generic_params);
    if (!resolved) {
      auto error = resolved.error();
      const std::string context = std::format("{} applies '{}' to {}, which is {} here.", callee, requirement.op,
                                              requirement.type_param, type_debug_name(binding));
      error.hint = error.hint.has_value() ? context + " " + *error.hint : context;
      return tl::unexpected(std::move(error));
    }
    hidden.push_back(make_ir_expr(ir::IRNameRef{.qualifier = resolved->qualifier,
                                                .name = resolved->name,
                                                .resolved_symbol_key = resolved->invocation.resolved_symbol_key,
                                                .span = span},
                                  span));
  }
  return hidden;
}

// Puts the hidden operator arguments in front of a call's own arguments: in front of the items of a parameter list,
// or in front of the single argument otherwise.
auto prepend_hidden_args(ir::IRFlowExpr& flow, std::vector<ir::IRExpr> hidden) -> void {
  const auto span = flow.span;
  ir::IRExprBox lhs = std::move(flow.lhs);
  ir::IRTupleExpr arguments{.span = span};
  for (auto& expr : hidden) {
    arguments.items.push_back(make_expr_box(std::move(expr)));
  }
  arguments.spread_items.assign(arguments.items.size(), false);
  if (auto* tuple = std::get_if<ir::IRTupleExpr>(&lhs->node); tuple != nullptr && !tuple->trailing_comma) {
    for (std::size_t position = 0; position < tuple->items.size(); ++position) {
      arguments.items.push_back(std::move(tuple->items[position]));
      arguments.spread_items.push_back(tuple->is_spread(position));
    }
  } else {
    arguments.items.push_back(std::move(lhs));
    arguments.spread_items.push_back(false);
  }
  flow.lhs = make_expr_box(make_ir_expr(std::move(arguments), span));
  flow.call_shape = ir::IRFlowCallShape::kTupleExpanded;
}

auto finalize_flow_resolution(ir::IRFlowExpr& flow, const FunctionOverloadSet& overloads, const Type& lhs_type,
                              const FunctionIndex& index, const StrongTypeIndex& type_index,
                              const GenericScope& generic_params,
                              const std::vector<Type>& explicit_type_args)
    -> tl::expected<Type, type_check::AnalysisError> {
  auto flow_args = flow_args_from_lhs(*flow.lhs, lhs_type);
  if (!flow_args) {
    return tl::unexpected(flow_args.error());
  }
  const auto& args = flow_args->args;
  const auto full_name = target_name(flow.rhs).value_or("<operator>");
  auto checked =
      resolve_overload_invocation(full_name, flow.span, overloads, args, type_index, generic_params, explicit_type_args);
  if (!checked) {
    return tl::unexpected(checked.error());
  }

  if (auto* name_ref = std::get_if<ir::IRNameRef>(&flow.rhs); name_ref != nullptr) {
    name_ref->resolved_symbol_key = checked->resolved_symbol_key;
    name_ref->runtime_type_check = std::nullopt;
    if ((full_name == "Std.Cast" || full_name == "Std.TryCast") && args.size() == 1U &&
        args.front().kind == TypeKind::kAny) {
      const Type target = full_name == "Std.Cast" ? checked->return_type : checked->return_type.applied_args.front();
      auto runtime_check = runtime_type_check_for_cast(full_name, target, type_index, flow.span);
      if (!runtime_check) {
        return tl::unexpected(runtime_check.error());
      }
      name_ref->runtime_type_check = std::move(*runtime_check);
    }
  }

  if (!checked->operator_requirements.empty()) {
    if (flow_args->spread) {
      return tl::unexpected(make_error(
          "Unsupported spread.",
          std::format("{} takes hidden operator arguments; write its arguments out instead of spreading them.",
                      full_name),
          flow.span));
    }
    auto hidden = hidden_operator_args(full_name, *checked, flow.span, index, type_index, generic_params);
    if (!hidden) {
      return tl::unexpected(hidden.error());
    }
    prepend_hidden_args(flow, std::move(*hidden));
    return checked->return_type;
  }
  apply_call_shape(flow, *flow_args, lhs_type, index);
  return checked->return_type;
}

// An operator on a type parameter whose bound contains a strong type, such as `+` on `T: Int64 | Money`. The function
// for each alternative of the bound is checked (Std.Add for Int64, Money.Add for Money), and the operator becomes a
// call through a hidden parameter of the generic body: `(a, b) -> +` becomes `((a, b), 'op0) -> Std.Apply`.
// Returns nullopt when no operand is such a type parameter.
auto infer_operator_requirement(ir::IRFlowExpr& flow, const std::string& op, const std::vector<Type>& args,
                                const std::optional<diag::SourceSpan>& op_span, const FunctionIndex& index,
                                const StrongTypeIndex& type_index, const GenericScope& generic_params)
    -> std::optional<tl::expected<Type, type_check::AnalysisError>> {
  std::vector<Type> params;
  for (const auto& arg : args) {
    if (arg.kind == TypeKind::kTypeVar && arg.type_var_bound.has_value() &&
        bound_has_strong_type(*arg.type_var_bound, type_index) &&
        std::ranges::none_of(params, [&](const Type& seen) { return seen.nominal_name == arg.nominal_name; })) {
      params.push_back(arg);
    }
  }
  if (params.empty()) {
    return std::nullopt;
  }
  if (params.size() > 1U) {
    return tl::unexpected(make_error(
        "Unsupported operator requirement.",
        std::format("Operator '{}' is applied to more than one type parameter whose bound contains a strong type ({} "
                    "and {}). An operator may use at most one such type parameter.",
                    op, type_debug_name(params[0]), type_debug_name(params[1])),
        op_span));
  }

  const Type& param = params.front();
  auto* context = current_generic_body_context();
  if (context == nullptr ||
      std::ranges::find(context->generic_params, param.nominal_name) == context->generic_params.end()) {
    return tl::unexpected(make_error(
        "Unsupported operator requirement.",
        std::format("Operator '{}' is applied to {}, a type parameter of a generic closure whose bound contains a strong "
                    "type. Only the type parameters of a named generic function can use operators of strong types.",
                    op, type_debug_name(param)),
        op_span));
  }

  const auto alternatives = bound_alternatives_of(*param.type_var_bound);
  std::vector<Type> returns;
  returns.reserve(alternatives.size());
  for (const auto& alternative : alternatives) {
    std::vector<Type> alternative_args;
    alternative_args.reserve(args.size());
    for (const auto& arg : args) {
      alternative_args.push_back(substitute_type_param(arg, param.nominal_name, alternative));
    }
    auto resolved = resolve_operator(op, alternative_args, flow.span, index, type_index, generic_params);
    if (!resolved) {
      auto error = resolved.error();
      const std::string context_text = std::format(
          "Operator '{}' on {} needs a function for every alternative of its bound; for {}:", op,
          type_debug_name(param), type_debug_name(alternative));
      error.hint = error.hint.has_value() ? context_text + " " + *error.hint : context_text;
      return tl::unexpected(std::move(error));
    }
    returns.push_back(resolved->invocation.return_type);
  }

  // The operator's result in terms of the type parameter: one type for every alternative (Bool for '<'), or the
  // alternative itself (T for '+').
  const auto same = [](const Type& lhs, const Type& rhs) { return is_consistent(lhs, rhs) && is_consistent(rhs, lhs); };
  Type result;
  if (std::ranges::all_of(returns, [&](const Type& type) { return same(type, returns.front()); })) {
    result = returns.front();
  } else if (std::ranges::equal(returns, alternatives, same)) {
    result = param;
    result.variadic = false;
  } else {
    return tl::unexpected(make_error(
        "Unsupported operator requirement.",
        std::format("Operator '{}' on {} returns types that differ between the alternatives of its bound in a way "
                    "that cannot be written in terms of {}.",
                    op, type_debug_name(param), type_debug_name(param)),
        op_span));
  }

  Type signature = make_type(TypeKind::kFunction);
  for (const auto& arg : args) {
    Type item = arg;
    item.variadic = false;
    signature.function_params.push_back(std::move(item));
  }
  signature.function_return = common::make_indirect_optional<Type>(result);
  const auto hidden_index = operator_requirement_index(
      *context, OperatorRequirement{.op = op, .type_param = param.nominal_name, .signature = signature});

  // Call the hidden parameter with the operands: one operand is passed as it is, several as a tuple.
  const auto span = flow.span;
  ir::IRExprBox value = std::move(flow.lhs);
  if (auto* tuple = std::get_if<ir::IRTupleExpr>(&value->node); tuple != nullptr && !tuple->trailing_comma) {
    if (args.size() == 1U && tuple->items.size() == 1U) {
      ir::IRExprBox item = std::move(tuple->items.front());
      value = std::move(item);
    } else {
      tuple->trailing_comma = true;  // The operands travel to Std.Apply as one tuple value.
    }
  }
  ir::IRTupleExpr apply_args{.span = span};
  apply_args.items.push_back(std::move(value));
  apply_args.items.push_back(make_expr_box(hidden_param_ref(hidden_index, op_span)));
  flow.lhs = make_expr_box(make_ir_expr(std::move(apply_args), span));
  flow.rhs = ir::IRNameRef{.qualifier = std::string{"Std"},
                           .name = "Apply",
                           .resolved_symbol_key = std_symbol_key(index, "Apply"),
                           .span = op_span};
  flow.call_shape = ir::IRFlowCallShape::kTupleExpanded;
  return result;
}

// Operators are shorthand for named functions; see resolve_operator. The operator target is then replaced with a
// reference to the chosen function, so later stages only see ordinary calls.
auto infer_operator_flow_expr(ir::IRFlowExpr& flow, const ir::IROperatorRef& op_ref, const FunctionIndex& index,
                              const StrongTypeIndex& type_index, const AliasIndex& alias_index,
                              const LocalTypes& locals, const GenericScope& generic_params)
    -> tl::expected<Type, type_check::AnalysisError> {
  const std::string op = op_ref.op;
  const auto op_span = op_ref.span;
  const auto builtins = operator_builtins(op);
  if (builtins.empty()) {
    return tl::unexpected(make_error("Unknown operator.", std::format("'{}' is not a Fleaux operator.", op), op_span));
  }

  const std::optional<std::string> qualifier{std::string{kOperatorBuiltinQualifier}};
  FunctionOverloadSet all_candidates;
  for (const auto& builtin : builtins) {
    if (const auto overloads = index.resolve_name(qualifier, std::string{builtin.name}); overloads.has_value()) {
      all_candidates.insert(all_candidates.end(), overloads->get().begin(), overloads->get().end());
    }
  }
  if (all_candidates.empty()) {
    return tl::unexpected(make_error("Unresolved operator.",
                                     std::format("Operator '{}' is shorthand for {}. Add 'import Std;' to use it.", op,
                                                 describe_operator_builtins(builtins)),
                                     op_span));
  }

  auto lhs_type = infer_expr(*flow.lhs, index, type_index, alias_index, locals, generic_params);
  if (!lhs_type) {
    return tl::unexpected(lhs_type.error());
  }

  auto flow_args = flow_args_from_lhs(*flow.lhs, *lhs_type);
  if (!flow_args) {
    return tl::unexpected(flow_args.error());
  }
  const auto& args = flow_args->args;
  auto resolved = resolve_operator(op, args, flow.span, index, type_index, generic_params);
  if (!resolved) {
    if (flow_args->spread) {
      return tl::unexpected(resolved.error());
    }
    if (auto requirement = infer_operator_requirement(flow, op, args, op_span, index, type_index, generic_params)) {
      return std::move(*requirement);
    }
    return tl::unexpected(resolved.error());
  }

  flow.rhs = ir::IRNameRef{
      .qualifier = resolved->qualifier,
      .name = resolved->name,
      .explicit_type_args = {},
      .resolved_symbol_key = resolved->invocation.resolved_symbol_key,
      .materialize_as_value = false,
      .span = op_span,
  };
  apply_call_shape(flow, *flow_args, *lhs_type, index);
  return resolved->invocation.return_type;
}

}  // namespace

auto resolve_operator_function(const std::string& op, const std::vector<Type>& args,
                               const std::optional<diag::SourceSpan>& span, const FunctionIndex& index,
                               const StrongTypeIndex& type_index, const GenericScope& generic_params)
    -> tl::expected<ir::IRNameRef, type_check::AnalysisError> {
  auto resolved = resolve_operator(op, args, span, index, type_index, generic_params);
  if (!resolved) {
    return tl::unexpected(resolved.error());
  }
  return ir::IRNameRef{.qualifier = resolved->qualifier,
                       .name = resolved->name,
                       .resolved_symbol_key = resolved->invocation.resolved_symbol_key,
                       .span = span};
}

auto operator_args_for_call(const std::string& callee, const ResolvedInvocation& invocation,
                            const std::optional<diag::SourceSpan>& span, const FunctionIndex& index,
                            const StrongTypeIndex& type_index, const GenericScope& generic_params)
    -> tl::expected<std::vector<ir::IRExpr>, type_check::AnalysisError> {
  return hidden_operator_args(callee, invocation, span, index, type_index, generic_params);
}

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
                          const GenericScope& generic_params)
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
                          const GenericScope& generic_params)
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
  // The value is one argument of the function: parentheses inside the parameter list are a tuple.
  const Type& adapted_value_type = *value_type;

  auto func_type = infer_expr(*apply_args->items[1], index, type_index, alias_index, locals, generic_params);
  if (!func_type) {
    return tl::unexpected(func_type.error());
  }

  if (const bool value_is_empty_tuple = adapted_value_type.kind == TypeKind::kTuple && adapted_value_type.items.empty();
      !value_is_empty_tuple) {
    if (is_deferred_callable_type(*func_type)) {
      return make_type(TypeKind::kAny);
    }

    if (func_type->kind == TypeKind::kFunction && callable_has_fixed_arity(*func_type, 1U) &&
        callable_accepts_arg(*func_type, 0U, adapted_value_type) && func_type->function_return.has_value()) {
      return *func_type->function_return;
    }

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

// True when a call target names a function value rather than a function: a local or parameter (unqualified), or a
// value let. Such a target is called with the value it holds.
auto is_function_value_target(const ir::IRCallTarget& target, const FunctionIndex& index, const LocalTypes& locals)
    -> bool {
  const auto* name_ref = std::get_if<ir::IRNameRef>(&target);
  if (name_ref == nullptr) {
    return false;
  }
  if (!name_ref->qualifier.has_value() && locals.contains(name_ref->name)) {
    return true;
  }
  const auto overloads = resolve_signature(index, target);
  return overloads.has_value() && overloads->get().size() == 1U && overloads->get().front().is_value_binding;
}

// Types the call of a function value of type `callee_type` with the arguments `flow.lhs`, by the parameter-list rule,
// and sets the call shape the compiler passes them by through Std.Apply. A one-parameter function takes its argument
// as is; any other function takes its argument tuple, so a variadic function's single argument is put in a parameter
// list. A generic closure takes its type arguments from the argument types.
auto check_function_value_call(ir::IRFlowExpr& flow, const std::string& callee, Type callee_type,
                               const std::optional<diag::SourceSpan>& callee_span, const FunctionIndex& index,
                               const StrongTypeIndex& type_index, const AliasIndex& alias_index,
                               const LocalTypes& locals, const GenericScope& generic_params)
    -> tl::expected<Type, type_check::AnalysisError> {
  if (callee_type.kind != TypeKind::kFunction || !callee_type.function_return.has_value()) {
    return tl::unexpected(make_error(
        "Invalid call target.",
        std::format("'{}' is a value of type {}, not a function; only a value of function type can be called.", callee,
                    type_debug_name(callee_type)),
        callee_span));
  }

  auto lhs_type = infer_expr(*flow.lhs, index, type_index, alias_index, locals, generic_params);
  if (!lhs_type) {
    return tl::unexpected(lhs_type.error());
  }
  auto flow_args = flow_args_from_lhs(*flow.lhs, *lhs_type);
  if (!flow_args) {
    return tl::unexpected(flow_args.error());
  }

  if (!callee_type.function_generic_params.empty() && flow_args->args.size() == callee_type.function_params.size() &&
      std::ranges::none_of(flow_args->args, [](const Type& arg) -> bool { return arg.variadic; })) {
    Type expected = make_type(TypeKind::kFunction);
    expected.function_params = flow_args->args;
    expected.function_return = make_type(TypeKind::kAny);
    auto instantiated = instantiate_open_function(callee_type, expected);
    if (!instantiated) {
      return tl::unexpected(make_error(
          "Type mismatch in call target arguments.",
          std::format("{} cannot take these arguments: its type variable(s) are taken from the argument types and "
                      "must satisfy their bounds.",
                      callee),
          flow.span));
    }
    callee_type = std::move(*instantiated);
  }

  FunctionSig sig;
  sig.return_type = *callee_type.function_return;
  sig.params.reserve(callee_type.function_params.size());
  for (const auto& param : callee_type.function_params) {
    sig.params.push_back(ParamSig{.name = {}, .type = param, .variadic = param.variadic});
  }
  auto result = check_invocation(callee, flow.span, sig, flow_args->args, type_index, generic_params);
  if (!result) {
    return tl::unexpected(result.error());
  }

  apply_call_shape(flow, *flow_args, *lhs_type, index);
  if (const bool variadic = !sig.params.empty() && sig.params.back().variadic;
      variadic && flow.call_shape == ir::IRFlowCallShape::kDirectValue) {
    const auto* written = std::get_if<ir::IRTupleExpr>(&flow.lhs->node);
    if (written == nullptr || written->trailing_comma || written->items.size() != 1U) {
      const auto span = flow.lhs->span;
      ir::IRTupleExpr arguments{.span = span};
      arguments.items.push_back(std::move(flow.lhs));
      flow.lhs = make_expr_box(make_ir_expr(std::move(arguments), span));
    }
    flow.call_shape = ir::IRFlowCallShape::kTupleExpanded;
  }
  flow.calls_function_value = true;
  return *result;
}

// A call whose target names a function value.
auto infer_function_value_call(ir::IRFlowExpr& flow, const FunctionIndex& index, const StrongTypeIndex& type_index,
                               const AliasIndex& alias_index, const LocalTypes& locals,
                               const GenericScope& generic_params)
    -> tl::expected<Type, type_check::AnalysisError> {
  const auto& name_ref = std::get<ir::IRNameRef>(flow.rhs);
  const std::string callee = qualified_symbol_name(name_ref.qualifier, name_ref.name);
  if (!name_ref.qualifier.has_value() && locals.contains(name_ref.name) &&
      index.has_unqualified_symbol(name_ref.name)) {
    return tl::unexpected(make_error(
        "Ambiguous call target.",
        std::format("'{}' names both a local value and a function. Rename the local: a call target must name exactly "
                    "one of them.",
                    callee),
        name_ref.span));
  }
  if (!name_ref.explicit_type_args.empty()) {
    return tl::unexpected(make_error(
        "Invalid explicit type argument application.",
        std::format("{} is a function value and does not accept explicit type arguments.", callee), name_ref.span));
  }

  // Type the target as a value; this also resolves a value let to the function that computes it.
  const auto callee_span = name_ref.span;
  ir::IRExpr callee_expr = make_ir_expr(name_ref, callee_span);
  auto callee_type = infer_expr(callee_expr, index, type_index, alias_index, locals, generic_params);
  if (!callee_type) {
    return tl::unexpected(callee_type.error());
  }
  flow.rhs = std::get<ir::IRNameRef>(callee_expr.node);
  return check_function_value_call(flow, callee, std::move(*callee_type), callee_span, index, type_index,
                                   alias_index, locals, generic_params);
}

// A closure written as a pipeline stage: `(arguments, closure) -> Std.Apply`, typed as a call of the closure.
auto infer_closure_stage_call(ir::IRFlowExpr& flow, const FunctionIndex& index, const StrongTypeIndex& type_index,
                              const AliasIndex& alias_index, const LocalTypes& locals,
                              const GenericScope& generic_params)
    -> tl::expected<Type, type_check::AnalysisError> {
  auto& stage = std::get<ir::IRTupleExpr>(flow.lhs->node);
  auto closure_type = infer_expr(*stage.items[1], index, type_index, alias_index, locals, generic_params);
  if (!closure_type) {
    return tl::unexpected(closure_type.error());
  }
  ir::IRFlowExpr call{.lhs = std::move(stage.items[0]), .rhs = flow.rhs, .span = flow.span};
  auto result = check_function_value_call(call, "closure", std::move(*closure_type), stage.items[1]->span, index,
                                          type_index, alias_index, locals, generic_params);
  stage.items[0] = std::move(call.lhs);
  if (!result) {
    return tl::unexpected(result.error());
  }
  flow.call_shape = call.call_shape;
  flow.calls_function_value = true;
  return *result;
}

auto infer_flow_expr(ir::IRFlowExpr& flow, const FunctionIndex& index, const StrongTypeIndex& type_index,
                     const AliasIndex& alias_index, const LocalTypes& locals,
                     const GenericScope& generic_params)
    -> tl::expected<Type, type_check::AnalysisError> {
  if (const auto* op_ref = std::get_if<ir::IROperatorRef>(&flow.rhs); op_ref != nullptr) {
    return infer_operator_flow_expr(flow, *op_ref, index, type_index, alias_index, locals, generic_params);
  }

  if (flow.closure_stage) {
    return infer_closure_stage_call(flow, index, type_index, alias_index, locals, generic_params);
  }

  if (is_function_value_target(flow.rhs, index, locals)) {
    return infer_function_value_call(flow, index, type_index, alias_index, locals, generic_params);
  }

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
  return finalize_flow_resolution(flow, target_resolution->overloads->get(), *lhs_type, index, type_index, generic_params,
                                  *explicit_type_args);
}

}  // namespace fleaux::frontend::type_system::detail
