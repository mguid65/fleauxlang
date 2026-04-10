#include "fleaux/frontend/lowering.hpp"

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <unordered_set>
#include <variant>

namespace fleaux::frontend::lowering {
namespace {

const std::unordered_set<std::string> kOperators = {
    "^", "/", "*", "%", "+", "-", "==", "!=", "<", ">", ">=", "<=", "!", "&&", "||",
};

const std::string kMatchWildcardSentinel = "__fleaux_match_wildcard__";

LoweringError make_error(const std::string& message, const std::optional<std::string>& hint,
                         const std::optional<diag::SourceSpan>& span) {
  LoweringError err;
  err.message = message;
  err.hint = hint;
  err.span = span;
  return err;
}

std::pair<std::optional<std::string>, std::string> split_id(const std::variant<std::string, model::QualifiedId>& id) {
  if (const auto* simple = std::get_if<std::string>(&id); simple != nullptr) { return {std::nullopt, *simple}; }
  const auto* qid = std::get_if<model::QualifiedId>(&id);

  return {qid->qualifier.qualifier, qid->id};
}

ir::IRSimpleType lower_simple_type(const model::TypeRef& type) {
  ir::IRSimpleType out;
  if (!type) {
    out.name = "Any";
    return out;
  }

  out.span = type->span;
  if (const auto* simple = std::get_if<std::string>(&type->value); simple != nullptr) {
    std::string raw = *simple;
    if (raw.size() >= 3U && raw.substr(raw.size() - 3U) == "...") {
      out.variadic = true;
      raw = raw.substr(0, raw.size() - 3U);
    }
    out.name = raw;
    return out;
  }

  if (const auto* qid = std::get_if<model::QualifiedId>(&type->value); qid != nullptr) {
    out.name = qid->qualifier.qualifier + "." + qid->id;
    return out;
  }

  out.name = "Tuple";
  return out;
}

tl::expected<ir::IRExprPtr, LoweringError> lower_expr(const model::ExpressionPtr& expr,
                                                      const std::unordered_set<std::string>& bound_names);

bool is_std_match_target(const ir::IRCallTarget& target) {
  if (const auto* name_ref = std::get_if<ir::IRNameRef>(&target); name_ref != nullptr) {
    return name_ref->qualifier.has_value() && name_ref->qualifier.value() == "Std" && name_ref->name == "Match";
  }
  return false;
}

tl::expected<void, LoweringError> rewrite_match_wildcards(const ir::IRExprPtr& match_lhs,
                                                          const std::optional<diag::SourceSpan>& span) {
  if (!match_lhs) {
    return tl::unexpected(make_error("Std.Match requires a non-null lhs tuple.",
                                     "Use '(value, (pattern, handler), ... ) -> Std.Match'.", span));
  }

  auto* match_args = std::get_if<ir::IRTupleExpr>(&match_lhs->node);
  if (match_args == nullptr || match_args->items.size() < 2U) {
    return tl::unexpected(make_error("Std.Match expects '(value, (pattern, handler), ... )'.",
                                     "Add at least one '(pattern, handler)' case tuple.", span));
  }

  for (std::size_t idx = 1; idx < match_args->items.size(); ++idx) {
    const auto& case_expr = match_args->items[idx];
    if (!case_expr) {
      return tl::unexpected(make_error("Std.Match case cannot be null.",
                                       "Use '(pattern, handler)' for each case.", span));
    }

    auto* case_tuple = std::get_if<ir::IRTupleExpr>(&case_expr->node);
    if (case_tuple == nullptr || case_tuple->items.size() != 2U) {
      return tl::unexpected(make_error("Std.Match case must be a 2-item tuple '(pattern, handler)'.",
                                       "Each case should look like '(0, (): Any = \"zero\")'.", case_expr->span));
    }

    auto& pattern_expr = case_tuple->items[0];
    if (!pattern_expr) {
      return tl::unexpected(make_error("Std.Match pattern cannot be null.",
                                       "Use a literal pattern like 0, 1, or _.", case_expr->span));
    }

    const auto* pattern_name = std::get_if<ir::IRNameRef>(&pattern_expr->node);
    const bool is_wildcard_pattern =
        pattern_name != nullptr && !pattern_name->qualifier.has_value() && pattern_name->name == "_";
    if (is_wildcard_pattern && idx + 1U != match_args->items.size()) {
      return tl::unexpected(make_error("Std.Match wildcard '_' must be the final case.",
                                       "Move '(_, handler)' to the end so earlier cases can still match.",
                                       pattern_expr->span));
    }

    if (is_wildcard_pattern) {
      ir::IRConstant wildcard;
      wildcard.val = kMatchWildcardSentinel;
      wildcard.span = pattern_name->span;
      pattern_expr->node = std::move(wildcard);
    }
  }

  return {};
}

void collect_unqualified_names_from_expr(const model::ExpressionPtr& expr,
                                         std::vector<std::string>& out,
                                         std::unordered_set<std::string>& seen);

void collect_unqualified_names_from_atom(const model::Atom& atom,
                                         std::vector<std::string>& out,
                                         std::unordered_set<std::string>& seen) {
  if (atom.qualified_var.has_value() || atom.constant.has_value()) {
    return;
  }

  if (atom.var.has_value()) {
    const auto& name = atom.var.value();
    if (!kOperators.contains(name) && seen.insert(name).second) {
      out.push_back(name);
    }
    return;
  }

  if (atom.inner) {
    for (const auto& item : atom.inner->items) {
      collect_unqualified_names_from_expr(item, out, seen);
    }
    return;
  }

  if (atom.closure) {
    collect_unqualified_names_from_expr(atom.closure->body, out, seen);
  }
}

void collect_unqualified_names_from_expr(const model::ExpressionPtr& expr,
                                         std::vector<std::string>& out,
                                         std::unordered_set<std::string>& seen) {
  if (!expr) {
    return;
  }

  collect_unqualified_names_from_atom(expr->expr.lhs.base, out, seen);
  for (const auto& stage : expr->expr.rhs) {
    collect_unqualified_names_from_atom(stage.base, out, seen);
  }
}

tl::expected<ir::IRCallTarget, LoweringError> extract_call_target_from_primary(const model::Primary& primary) {
  if (primary.base.qualified_var.has_value()) {
    const auto& qid = primary.base.qualified_var.value();
    return ir::IRNameRef{
        .qualifier = qid.qualifier.qualifier,
        .name = qid.id,
        .span = qid.span,
    };
  }

  if (primary.base.var.has_value()) {
    const auto& value = primary.base.var.value();
    if (kOperators.contains(value)) { return ir::IROperatorRef{.op = value, .span = primary.span}; }
    return ir::IRNameRef{.qualifier = std::nullopt, .name = value, .span = primary.span};
  }

  return tl::unexpected(make_error("Primary used as flow target must be a simple name, qualified name, or operator.",
                                   "Valid targets: 'Std.Add', 'MyFunc', '+', '/', '&&'.", primary.span));
}

bool contains_placeholder(const ir::IRExprPtr& expr);

ir::IRExprPtr replace_placeholder_impl(const ir::IRExprPtr& expr, const ir::IRExprPtr& replacement) {
  if (!expr) { return expr; }

  if (const auto* name = std::get_if<ir::IRNameRef>(&expr->node); name != nullptr) {
    if (!name->qualifier.has_value() && name->name == "_") { return replacement; }
    return expr;
  }

  if (const auto* tuple = std::get_if<ir::IRTupleExpr>(&expr->node); tuple != nullptr) {
    ir::IRTupleExpr out_tuple;
    out_tuple.span = tuple->span;
    for (const auto& item : tuple->items) { out_tuple.items.push_back(replace_placeholder_impl(item, replacement)); }

    auto out = std::make_shared<ir::IRExpr>();
    out->node = std::move(out_tuple);
    out->span = expr->span;
    return out;
  }

  if (const auto* flow = std::get_if<ir::IRFlowExpr>(&expr->node); flow != nullptr) {
    ir::IRFlowExpr out_flow;
    out_flow.lhs = replace_placeholder_impl(flow->lhs, replacement);
    out_flow.rhs = flow->rhs;
    out_flow.span = flow->span;

    auto out = std::make_shared<ir::IRExpr>();
    out->node = std::move(out_flow);
    out->span = expr->span;
    return out;
  }

  return expr;
}

bool contains_placeholder(const ir::IRExprPtr& expr) {
  if (!expr) { return false; }
  if (const auto* name = std::get_if<ir::IRNameRef>(&expr->node); name != nullptr) {
    return !name->qualifier.has_value() && name->name == "_";
  }
  if (const auto* tuple = std::get_if<ir::IRTupleExpr>(&expr->node); tuple != nullptr) {
    return std::ranges::any_of(tuple->items, [](const ir::IRExprPtr& item) -> bool { return contains_placeholder(item); });
  }
  if (const auto* flow = std::get_if<ir::IRFlowExpr>(&expr->node); flow != nullptr) {
    return contains_placeholder(flow->lhs);
  }
  return false;
}

tl::expected<ir::IRExprPtr, LoweringError> replace_placeholder(const ir::IRExprPtr& template_expr,
                                                               const ir::IRExprPtr& current_value) {
  auto replaced = replace_placeholder_impl(template_expr, current_value);
  if (contains_placeholder(replaced)) {
    return tl::unexpected(make_error("Unresolved '_' placeholder remained in tuple template.",
                                     "Use '_' only as an argument position inside tuple templates like '(_, 2)'.",
                                     template_expr ? template_expr->span : std::nullopt));
  }
  return replaced;
}

tl::expected<ir::IRExprPtr, LoweringError> lower_atom(const model::Atom& atom,
                                                      const std::unordered_set<std::string>& bound_names) {
  auto out = std::make_shared<ir::IRExpr>();
  out->span = atom.span;

  if (atom.inner) {
    if (atom.inner->items.size() == 1) {
      return lower_expr(atom.inner->items[0], bound_names);
    }

    ir::IRTupleExpr tuple;
    tuple.span = atom.inner->span;
    for (const auto& item : atom.inner->items) {
      auto lowered_item = lower_expr(item, bound_names);
      if (!lowered_item) { return tl::unexpected(lowered_item.error()); }
      tuple.items.push_back(lowered_item.value());
    }
    out->node = std::move(tuple);
    return out;
  }

  if (atom.closure) {
    const auto& closure = atom.closure;
    std::vector<ir::IRParam> params;
    params.reserve(closure->params.params.size());
    std::unordered_set<std::string> param_names;
    for (const auto& [param_name, type, span] : closure->params.params) {
      params.push_back(ir::IRParam{
          .name = param_name,
          .type = lower_simple_type(type),
          .span = span,
      });
      param_names.insert(param_name);
    }

    for (std::size_t idx = 0; idx < params.size(); ++idx) {
      if (params[idx].type.variadic && idx + 1U != params.size()) {
        return tl::unexpected(make_error("Variadic parameter must be the final parameter in a closure declaration.",
                                         "Move the '...' parameter to the end of the closure parameter list.",
                                         params[idx].span));
      }
    }

    std::unordered_set<std::string> closure_bound = bound_names;
    for (const auto& p : params) {
      closure_bound.insert(p.name);
    }

    std::vector<std::string> discovered_names;
    std::unordered_set<std::string> seen_names;
    collect_unqualified_names_from_expr(closure->body, discovered_names, seen_names);

    std::vector<std::string> captures;
    for (const auto& candidate : discovered_names) {
      if (param_names.contains(candidate)) {
        continue;
      }
      if (bound_names.contains(candidate)) {
        captures.push_back(candidate);
      }
    }

    auto lowered_body = lower_expr(closure->body, closure_bound);
    if (!lowered_body) {
      return tl::unexpected(lowered_body.error());
    }

    auto closure_ir = std::make_shared<ir::IRClosureExpr>();
    closure_ir->params = std::move(params);
    closure_ir->return_type = lower_simple_type(closure->rtype);
    closure_ir->body = lowered_body.value();
    closure_ir->captures = std::move(captures);
    closure_ir->span = closure->span;

    out->node = closure_ir;
    return out;
  }

  if (atom.constant.has_value()) {
    ir::IRConstant c;
    c.val = atom.constant->val;
    c.span = atom.constant->span;
    out->node = std::move(c);
    return out;
  }

  if (atom.qualified_var.has_value()) {
    const auto& qid = atom.qualified_var.value();
    out->node = ir::IRNameRef{
        .qualifier = qid.qualifier.qualifier,
        .name = qid.id,
        .span = qid.span,
    };
    return out;
  }

  if (atom.var.has_value()) {
    out->node = ir::IRNameRef{
        .qualifier = std::nullopt,
        .name = atom.var.value(),
        .span = atom.span,
    };
    return out;
  }

  ir::IRTupleExpr tuple;
  tuple.span = atom.span;
  out->node = std::move(tuple);
  return out;
}

tl::expected<ir::IRExprPtr, LoweringError> lower_primary(const model::Primary& primary,
                                                         const std::unordered_set<std::string>& bound_names) {
  return lower_atom(primary.base, bound_names);
}

tl::expected<ir::IRExprPtr, LoweringError> lower_flow(const model::FlowExpression& flow,
                                                      const std::unordered_set<std::string>& bound_names) {
  auto result = lower_primary(flow.lhs, bound_names);
  if (!result) { return tl::unexpected(result.error()); }

  std::size_t i = 0;
  while (i < flow.rhs.size()) {
    if (auto maybe_target = extract_call_target_from_primary(flow.rhs[i])) {
      if (is_std_match_target(maybe_target.value())) {
        auto rewritten = rewrite_match_wildcards(result.value(), flow.rhs[i].span);
        if (!rewritten) {
          return tl::unexpected(rewritten.error());
        }
      }

      ir::IRFlowExpr ir_flow;
      ir_flow.lhs = result.value();
      ir_flow.rhs = maybe_target.value();
      ir_flow.span = flow.span;

      auto wrapped = std::make_shared<ir::IRExpr>();
      wrapped->node = std::move(ir_flow);
      wrapped->span = flow.span;
      result = wrapped;
      ++i;
      continue;
    }

    auto template_expr = lower_primary(flow.rhs[i], bound_names);
    if (!template_expr) { return tl::unexpected(template_expr.error()); }

    if (const auto* closure_ptr = std::get_if<ir::IRClosureExprPtr>(&template_expr.value()->node);
        closure_ptr != nullptr && *closure_ptr != nullptr) {
      ir::IRTupleExpr apply_args;
      apply_args.span = flow.rhs[i].span;
      apply_args.items.push_back(result.value());
      apply_args.items.push_back(template_expr.value());

      auto apply_lhs = std::make_shared<ir::IRExpr>();
      apply_lhs->node = std::move(apply_args);
      apply_lhs->span = flow.rhs[i].span;

      ir::IRFlowExpr apply_flow;
      apply_flow.lhs = apply_lhs;
      apply_flow.rhs = ir::IRNameRef{
          .qualifier = std::optional<std::string>{"Std"},
          .name = "Apply",
          .span = flow.rhs[i].span,
      };
      apply_flow.span = flow.rhs[i].span;

      auto wrapped = std::make_shared<ir::IRExpr>();
      wrapped->node = std::move(apply_flow);
      wrapped->span = flow.rhs[i].span;
      result = wrapped;
      ++i;
      continue;
    }

    if (std::get_if<ir::IRTupleExpr>(&template_expr.value()->node) == nullptr) {
      return tl::unexpected(make_error(
          "Invalid pipeline stage shape: non-call stages must be tuple templates.",
          "Use a call target like '-> Std.Add' or a tuple template like '-> (_, 2) -> Std.Add'.", flow.rhs[i].span));
    }

    if (i + 1 >= flow.rhs.size()) {
      return tl::unexpected(make_error("Tuple template stage is missing a following call target.",
                                       "Append a call target, e.g. '-> (_, 2) -> Std.Divide'.", flow.rhs[i].span));
    }

    auto next_target = extract_call_target_from_primary(flow.rhs[i + 1]);
    if (!next_target) { return tl::unexpected(next_target.error()); }

    auto replaced = replace_placeholder(template_expr.value(), result.value());
    if (!replaced) { return tl::unexpected(replaced.error()); }

    ir::IRFlowExpr ir_flow;
    ir_flow.lhs = replaced.value();
    ir_flow.rhs = next_target.value();
    ir_flow.span = flow.span;

    auto wrapped = std::make_shared<ir::IRExpr>();
    wrapped->node = std::move(ir_flow);
    wrapped->span = flow.span;
    result = wrapped;
    i += 2;
  }

  return result;
}

tl::expected<ir::IRExprPtr, LoweringError> lower_expr(const model::ExpressionPtr& expr,
                                                      const std::unordered_set<std::string>& bound_names) {
  if (!expr) {
    return tl::unexpected(
        make_error("Cannot lower null expression.", "Provide a valid expression node.", std::nullopt));
  }
  return lower_flow(expr->expr, bound_names);
}

}  // namespace

LoweringResult Lowerer::lower(const model::Program& program) const {
  ir::IRProgram ir_program;
  ir_program.span = program.span;

  for (const auto& stmt : program.statements) {
    if (const auto* model_import = std::get_if<model::ImportStatement>(&stmt); model_import != nullptr) {
      ir_program.imports.push_back(ir::IRImport{
          .module_name = model_import->module_name,
          .span = model_import->span,
      });
      continue;
    }

    if (const auto* model_let = std::get_if<model::LetStatement>(&stmt); model_let != nullptr) {
      auto [qualifier, name] = split_id(model_let->id);

      std::vector<ir::IRParam> params;
      for (const auto& [param_name, type, span] : model_let->params.params) {
        params.push_back(ir::IRParam{
            .name = param_name,
            .type = lower_simple_type(type),
            .span = span,
        });
      }

      for (std::size_t idx = 0; idx < params.size(); ++idx) {
        if (params[idx].type.variadic && idx + 1U != params.size()) {
          return tl::unexpected(make_error("Variadic parameter must be the final parameter in a function declaration.",
                                           "Move the '...' parameter to the end of the parameter list.",
                                           params[idx].span));
        }
      }

      const auto* builtin_expr = std::get_if<std::string>(&model_let->expr);
      const bool is_builtin = builtin_expr != nullptr && *builtin_expr == "__builtin__";

      std::unordered_set<std::string> let_bound_names;
      for (const auto& p : params) {
        let_bound_names.insert(p.name);
      }

      ir::IRExprPtr body;
      if (!is_builtin) {
        auto lowered_body = lower_expr(*std::get_if<model::ExpressionPtr>(&model_let->expr), let_bound_names);
        if (!lowered_body) { return tl::unexpected(lowered_body.error()); }
        body = lowered_body.value();
      }

      ir_program.lets.push_back(ir::IRLet{
          .qualifier = qualifier,
          .name = name,
          .params = std::move(params),
          .return_type = lower_simple_type(model_let->rtype),
          .body = body,
          .is_builtin = is_builtin,
          .span = model_let->span,
      });
      continue;
    }

    const auto* model_expr_stmt = std::get_if<model::ExpressionStatement>(&stmt);
    std::unordered_set<std::string> no_bound_names;
    auto lowered_expr = lower_expr(model_expr_stmt->expr, no_bound_names);
    if (!lowered_expr) { return tl::unexpected(lowered_expr.error()); }

    ir_program.expressions.push_back(ir::IRExprStatement{
        .expr = lowered_expr.value(),
        .span = model_expr_stmt->span,
    });
  }

  return ir_program;
}

}  // namespace fleaux::frontend::lowering
