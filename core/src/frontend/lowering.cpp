#include "fleaux/frontend/lowering.hpp"
#include "fleaux/frontend/type_check.hpp"

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <variant>

#include "fleaux/common/overloaded.hpp"

namespace fleaux::frontend::lowering {
namespace {
const std::unordered_set<std::string> kOperators = {
    "^", "/", "*", "%", "+", "-", "==", "!=", "<", ">", ">=", "<=", "!", "&&", "||",
};

const std::string kMatchWildcardSentinel = "__fleaux_match_wildcard__";

auto make_error(const std::string& message, const std::optional<std::string>& hint,
                const std::optional<diag::SourceSpan>& span) -> LoweringError {
  LoweringError err;
  err.message = message;
  err.hint = hint;
  err.span = span;
  return err;
}

auto split_id(const std::variant<std::string, model::QualifiedId>& id)
    -> std::pair<std::optional<std::string>, std::string> {
  return std::visit(
      fleaux::common::overloaded{
          [](const std::string& simple) -> std::pair<std::optional<std::string>, std::string> {
            return {std::nullopt, simple};
          },
          [](const model::QualifiedId& qualified) -> std::pair<std::optional<std::string>, std::string> {
            return {qualified.qualifier.qualifier, qualified.id};
          }},
      id);
}

auto lower_simple_type(const model::TypeNode& type) -> ir::IRSimpleType {
  ir::IRSimpleType out;
  out.span = type.span;
  std::visit(
      fleaux::common::overloaded{
          [&](const std::string& simple) -> void {
            std::string raw = simple;
            if (raw.size() >= 3U && raw.substr(raw.size() - 3U) == "...") {
              out.variadic = true;
              raw = raw.substr(0, raw.size() - 3U);
            }
            out.name = raw;
          },
          [&](const model::QualifiedId& qualified) -> void { out.name = qualified.qualifier.qualifier + "." + qualified.id; },
          [&](const Box<model::UnionTypeList>& union_list) -> void {
            for (const auto& alt : union_list->alternatives) {
              if (const ir::IRSimpleType lowered = lower_simple_type(*alt); !lowered.alternatives.empty()) {
                out.alternatives.insert(out.alternatives.end(), lowered.alternatives.begin(), lowered.alternatives.end());
              } else {
                out.alternatives.push_back(lowered.name);
              }
            }
            if (!out.alternatives.empty()) { out.name = out.alternatives.front(); }
          },
          [&](const auto&) -> void { out.name = "Tuple"; }},
      type.value);

  return out;
}

auto lower_expr(const model::Expression& expr, const std::unordered_set<std::string>& bound_names)
    -> tl::expected<ir::IRExpr, LoweringError>;

auto is_std_match_target(const ir::IRCallTarget& target) -> bool {
  if (const auto* name_ref = std::get_if<ir::IRNameRef>(&target); name_ref != nullptr) {
    return name_ref->qualifier.has_value() && name_ref->qualifier.value() == "Std" && name_ref->name == "Match";
  }
  return false;
}

auto rewrite_match_wildcards(ir::IRExpr& match_lhs, const std::optional<diag::SourceSpan>& span)
    -> tl::expected<void, LoweringError> {
  auto* match_args = std::get_if<ir::IRTupleExpr>(&match_lhs.node);
  if (match_args == nullptr || match_args->items.size() < 2U) {
    return tl::unexpected(make_error("Std.Match expects '(value, (pattern, handler), ... )'.",
                                     "Add at least one '(pattern, handler)' case tuple.", span));
  }

  for (std::size_t idx = 1; idx < match_args->items.size(); ++idx) {
    auto& case_expr = match_args->items[idx];
    auto* case_tuple = std::get_if<ir::IRTupleExpr>(&case_expr->node);
    if (case_tuple == nullptr || case_tuple->items.size() != 2U) {
      return tl::unexpected(make_error("Std.Match case must be a 2-item tuple '(pattern, handler)'.",
                                       "Each case should look like '(0, (): Any = \"zero\")'.", case_expr->span));
    }

    auto& pattern_expr = case_tuple->items[0];
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
      // This is guarded by the check on line 90, specifically the bool in *this* if condition is guarding it, but not from race conditions,
      // I hope one never happens
      wildcard.span = pattern_name->span;
      pattern_expr->node = std::move(wildcard);
    }
  }

  return {};
}

void collect_unqualified_names_from_expr(const model::Expression& expr, std::vector<std::string>& out,
                                         std::unordered_set<std::string>& seen);

void collect_unqualified_names_from_atom(const model::Atom& atom, std::vector<std::string>& out,
                                         std::unordered_set<std::string>& seen) {
  std::visit(fleaux::common::overloaded{
                 [](const std::monostate&) -> void {},
                 [](const model::Constant&) -> void {},
                 [](const model::QualifiedId&) -> void {},
                 [&](const std::string& value) -> void {
                   if (!kOperators.contains(value) && seen.insert(value).second) { out.push_back(value); }
                 },
                 [&](const fleaux::frontend::Box<model::DelimitedExpression>& value) -> void {
                   for (const auto& item : value->items) { collect_unqualified_names_from_expr(*item, out, seen); }
                 },
                 [&](const model::ClosureExpressionBox& value) -> void {
                   collect_unqualified_names_from_expr(*value->body, out, seen);
                 },
             },
             atom.value);
}

void collect_unqualified_names_from_expr(const model::Expression& expr, std::vector<std::string>& out,
                                         std::unordered_set<std::string>& seen) {
  collect_unqualified_names_from_atom(expr.expr.lhs.base, out, seen);
  for (const auto& stage : expr.expr.rhs) { collect_unqualified_names_from_atom(stage.base, out, seen); }
}

auto extract_call_target_from_primary(const model::Primary& primary) -> tl::expected<ir::IRCallTarget, LoweringError> {
  return std::visit(
      fleaux::common::overloaded{
          [&](const model::QualifiedId& qualified) -> tl::expected<ir::IRCallTarget, LoweringError> {
            return ir::IRNameRef{
                .qualifier = qualified.qualifier.qualifier,
                .name = qualified.id,
                .span = qualified.span,
            };
          },
          [&](const std::string& value) -> tl::expected<ir::IRCallTarget, LoweringError> {
            if (kOperators.contains(value)) { return ir::IROperatorRef{.op = value, .span = primary.span}; }
            return ir::IRNameRef{.qualifier = std::nullopt, .name = value, .span = primary.span};
          },
          [&](const auto&) -> tl::expected<ir::IRCallTarget, LoweringError> {
            return tl::unexpected(make_error("Primary used as flow target must be a simple name, qualified name, or operator.",
                                             "Valid targets: 'Std.Add', 'MyFunc', '+', '/', '&&'.", primary.span));
          }},
      primary.base.value);
}

auto contains_placeholder(const ir::IRExpr& expr) -> bool;

auto replace_placeholder_impl(const ir::IRExpr& expr, const ir::IRExpr& replacement) -> ir::IRExpr {
  return std::visit(
      fleaux::common::overloaded{
          [&](const ir::IRNameRef& name_ref) -> ir::IRExpr {
            if (!name_ref.qualifier.has_value() && name_ref.name == "_") { return replacement; }
            return expr;
          },
          [&](const ir::IRTupleExpr& tuple_expr) -> ir::IRExpr {
            ir::IRTupleExpr out_tuple;
            out_tuple.span = tuple_expr.span;
            for (const auto& item : tuple_expr.items) {
              out_tuple.items.emplace_back(replace_placeholder_impl(*item, replacement));
            }

            return ir::IRExpr{.node = std::move(out_tuple), .span = expr.span};
          },
          [&](const ir::IRFlowExpr& flow_expr) -> ir::IRExpr {
            ir::IRFlowExpr out_flow;
            out_flow.lhs = replace_placeholder_impl(*flow_expr.lhs, replacement);
            out_flow.rhs = flow_expr.rhs;
            out_flow.span = flow_expr.span;

            return ir::IRExpr{.node = std::move(out_flow), .span = expr.span};
          },
          [&](const auto&) -> ir::IRExpr { return expr; }},
      expr.node);
}

auto contains_placeholder(const ir::IRExpr& expr) -> bool {
  return std::visit(
      fleaux::common::overloaded{
          [](const ir::IRNameRef& name_ref) -> bool {
            return !name_ref.qualifier.has_value() && name_ref.name == "_";
          },
          [](const ir::IRTupleExpr& tuple_expr) -> bool {
            return std::ranges::any_of(tuple_expr.items,
                                       [](const ir::IRExprBox& item) -> bool { return contains_placeholder(*item); });
          },
          [](const ir::IRFlowExpr& flow_expr) -> bool { return contains_placeholder(*flow_expr.lhs); },
          [](const auto&) -> bool { return false; }},
      expr.node);
}

auto replace_placeholder(const ir::IRExpr& template_expr, const ir::IRExpr& current_value)
    -> tl::expected<ir::IRExpr, LoweringError> {
  auto replaced = replace_placeholder_impl(template_expr, current_value);
  if (contains_placeholder(replaced)) {
    return tl::unexpected(make_error("Unresolved '_' placeholder remained in tuple template.",
                                     "Use '_' only as an argument position inside tuple templates like '(_, 2)'.",
                                     template_expr.span));
  }
  return replaced;
}

auto lower_atom(const model::Atom& atom, const std::unordered_set<std::string>& bound_names)
    -> tl::expected<ir::IRExpr, LoweringError> {
  return std::visit(
      fleaux::common::overloaded{
          [&](const fleaux::frontend::Box<model::DelimitedExpression>& inner)
              -> tl::expected<ir::IRExpr, LoweringError> {
            if (inner->items.size() == 1) { return lower_expr(*inner->items[0], bound_names); }

            ir::IRTupleExpr tuple;
            tuple.span = inner->span;
            for (const auto& item : inner->items) {
              auto lowered_item = lower_expr(*item, bound_names);
              if (!lowered_item) { return tl::unexpected(lowered_item.error()); }
              tuple.items.emplace_back(lowered_item.value());
            }
            return ir::IRExpr{.node = std::move(tuple), .span = atom.span};
          },
          [&](const model::ClosureExpressionBox& closure)
              -> tl::expected<ir::IRExpr, LoweringError> {
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
                return tl::unexpected(make_error(
                    "Variadic parameter must be the final parameter in a closure declaration.",
                    "Move the '...' parameter to the end of the closure parameter list.", params[idx].span));
              }
            }

            std::unordered_set<std::string> closure_bound = bound_names;
            for (const auto& p : params) { closure_bound.insert(p.name); }

            std::vector<std::string> discovered_names;
            std::unordered_set<std::string> seen_names;
            collect_unqualified_names_from_expr(*closure->body, discovered_names, seen_names);

            std::vector<std::string> captures;
            for (const auto& candidate : discovered_names) {
              if (param_names.contains(candidate)) { continue; }
              if (bound_names.contains(candidate)) { captures.push_back(candidate); }
            }

            auto lowered_body = lower_expr(*closure->body, closure_bound);
            if (!lowered_body) { return tl::unexpected(lowered_body.error()); }

            ir::IRClosureExpr closure_ir;
            closure_ir.params = std::move(params);
            closure_ir.return_type = lower_simple_type(closure->rtype);
            closure_ir.body = lowered_body.value();
            closure_ir.captures = std::move(captures);
            closure_ir.span = closure->span;

            return ir::IRExpr{.node = std::move(closure_ir), .span = atom.span};
          },
          [&](const model::Constant& constant) -> tl::expected<ir::IRExpr, LoweringError> {
            ir::IRConstant c;
            c.val = constant.val;
            c.span = constant.span;
            return ir::IRExpr{.node = std::move(c), .span = atom.span};
          },
          [&](const model::QualifiedId& qid) -> tl::expected<ir::IRExpr, LoweringError> {
            // Desugar Std.Ok to true and Std.Err to false
            if (qid.qualifier.qualifier == "Std" && (qid.id == "Ok" || qid.id == "Err")) {
              ir::IRConstant c;
              c.val = (qid.id == "Ok") ? true : false;
              c.span = qid.span;
              return ir::IRExpr{.node = std::move(c), .span = atom.span};
            }
            return ir::IRExpr{.node =
                                  ir::IRNameRef{
                                      .qualifier = qid.qualifier.qualifier,
                                      .name = qid.id,
                                      .span = qid.span,
                                  },
                              .span = atom.span};
          },
          [&](const std::string& name) -> tl::expected<ir::IRExpr, LoweringError> {
            return ir::IRExpr{.node =
                                  ir::IRNameRef{
                                      .qualifier = std::nullopt,
                                      .name = name,
                                      .span = atom.span,
                                  },
                              .span = atom.span};
          },
          [&](const std::monostate&) -> tl::expected<ir::IRExpr, LoweringError> {
            ir::IRTupleExpr tuple;
            tuple.span = atom.span;
            return ir::IRExpr{.node = std::move(tuple), .span = atom.span};
          }},
      atom.value);
}

auto lower_primary(const model::Primary& primary, const std::unordered_set<std::string>& bound_names)
    -> tl::expected<ir::IRExpr, LoweringError> {
  return lower_atom(primary.base, bound_names);
}

auto lower_flow(const model::FlowExpression& flow, const std::unordered_set<std::string>& bound_names)
    -> tl::expected<ir::IRExpr, LoweringError> {
  auto result = lower_primary(flow.lhs, bound_names);
  if (!result) { return tl::unexpected(result.error()); }

  const auto apply_direct_target_stage = [&](const model::Primary& stage_primary)
      -> tl::expected<bool, LoweringError> {
    auto maybe_target = extract_call_target_from_primary(stage_primary);
    if (!maybe_target) { return false; }

    if (is_std_match_target(maybe_target.value())) {
      if (auto rewritten = rewrite_match_wildcards(result.value(), stage_primary.span); !rewritten) {
        return tl::unexpected(rewritten.error());
      }
    }

    ir::IRFlowExpr ir_flow;
    ir_flow.lhs = result.value();
    ir_flow.rhs = maybe_target.value();
    ir_flow.span = flow.span;
    result = ir::IRExpr{.node = std::move(ir_flow), .span = flow.span};
    return true;
  };

  const auto apply_closure_stage = [&](const model::Primary& stage_primary, const ir::IRExpr& stage_expr) -> bool {
    if (std::get_if<ir::IRClosureExprBox>(&stage_expr.node) == nullptr) { return false; }

    ir::IRTupleExpr apply_args;
    apply_args.span = stage_primary.span;
    apply_args.items.emplace_back(result.value());
    apply_args.items.emplace_back(stage_expr);

    const ir::IRExpr apply_lhs{.node = std::move(apply_args), .span = stage_primary.span};

    ir::IRFlowExpr apply_flow;
    apply_flow.lhs = apply_lhs;
    apply_flow.rhs = ir::IRNameRef{
        .qualifier = std::optional<std::string>{"Std"},
        .name = "Apply",
        .span = stage_primary.span,
    };
    apply_flow.span = stage_primary.span;

    result = ir::IRExpr{.node = std::move(apply_flow), .span = stage_primary.span};
    return true;
  };

  const auto apply_template_stage = [&](const model::Primary& stage_primary, const ir::IRExpr& stage_expr,
                                        const model::Primary& next_primary)
      -> tl::expected<void, LoweringError> {
    if (std::get_if<ir::IRTupleExpr>(&stage_expr.node) == nullptr) {
      return tl::unexpected(make_error(
          "Invalid pipeline stage shape: non-call stages must be tuple templates.",
          "Use a call target like '-> Std.Add' or a tuple template like '-> (_, 2) -> Std.Add'.", stage_primary.span));
    }

    auto next_target = extract_call_target_from_primary(next_primary);
    if (!next_target) { return tl::unexpected(next_target.error()); }

    auto replaced = replace_placeholder(stage_expr, result.value());
    if (!replaced) { return tl::unexpected(replaced.error()); }

    ir::IRFlowExpr ir_flow;
    ir_flow.lhs = replaced.value();
    ir_flow.rhs = next_target.value();
    ir_flow.span = flow.span;

    result = ir::IRExpr{.node = std::move(ir_flow), .span = flow.span};
    return {};
  };

  std::size_t stage_index = 0;
  while (stage_index < flow.rhs.size()) {
    if (auto handled_direct_target = apply_direct_target_stage(flow.rhs[stage_index]); !handled_direct_target) {
      return tl::unexpected(handled_direct_target.error());
    } else if (*handled_direct_target) {
      ++stage_index;
      continue;
    }

    auto stage_expr = lower_primary(flow.rhs[stage_index], bound_names);
    if (!stage_expr) { return tl::unexpected(stage_expr.error()); }

    if (apply_closure_stage(flow.rhs[stage_index], stage_expr.value())) {
      ++stage_index;
      continue;
    }

    if (stage_index + 1 >= flow.rhs.size()) {
      return tl::unexpected(make_error("Tuple template stage is missing a following call target.",
                                       "Append a call target, e.g. '-> (_, 2) -> Std.Divide'.",
                                       flow.rhs[stage_index].span));
    }

    if (auto applied_template =
            apply_template_stage(flow.rhs[stage_index], stage_expr.value(), flow.rhs[stage_index + 1]);
        !applied_template) {
      return tl::unexpected(applied_template.error());
    }
    stage_index += 2;
  }

  return result;
}

auto lower_expr(const model::Expression& expr, const std::unordered_set<std::string>& bound_names)
    -> tl::expected<ir::IRExpr, LoweringError> {
  return lower_flow(expr.expr, bound_names);
}
}  // namespace

auto Lowerer::lower(const model::Program& program) const -> LoweringResult {
  ir::IRProgram ir_program;
  ir_program.span = program.span;

  for (const auto& stmt : program.statements) {
    auto lowered_stmt = std::visit(
        fleaux::common::overloaded{
            [&](const model::ImportStatement& model_import) -> tl::expected<void, LoweringError> {
              ir_program.imports.push_back(ir::IRImport{
                  .module_name = model_import.module_name,
                  .span = model_import.span,
              });
              return {};
            },
            [&](const model::LetStatement& model_let) -> tl::expected<void, LoweringError> {
              auto [qualifier, name] = split_id(model_let.id);

              std::vector<ir::IRParam> params;
              for (const auto& [param_name, type, span] : model_let.params.params) {
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

              const bool is_builtin = model_let.is_builtin;

              std::unordered_set<std::string> let_bound_names;
              for (const auto& p : params) { let_bound_names.insert(p.name); }

              std::optional<ir::IRExpr> body;
              if (!is_builtin) {
                auto lowered_body = lower_expr(*model_let.expr, let_bound_names);
                if (!lowered_body) { return tl::unexpected(lowered_body.error()); }
                body = lowered_body.value();
              }

              ir_program.lets.push_back(ir::IRLet{
                  .qualifier = qualifier,
                  .name = name,
                  .params = std::move(params),
                  .return_type = lower_simple_type(model_let.rtype),
                  .doc_comments = model_let.doc_comments,
                  .body = body,
                  .is_builtin = is_builtin,
                  .span = model_let.span,
              });
              return {};
            },
            [&](const model::ExpressionStatement& model_expr_stmt) -> tl::expected<void, LoweringError> {
              std::unordered_set<std::string> no_bound_names;
              auto lowered_expr = lower_expr(model_expr_stmt.expr, no_bound_names);
              if (!lowered_expr) { return tl::unexpected(lowered_expr.error()); }

              ir_program.expressions.push_back(ir::IRExprStatement{
                  .expr = lowered_expr.value(),
                  .span = model_expr_stmt.span,
              });
              return {};
            }},
        stmt);

    if (!lowered_stmt) { return tl::unexpected(lowered_stmt.error()); }
  }

  if (auto type_checked = type_check::validate_program(ir_program); !type_checked) {
    return tl::unexpected(type_checked.error());
  }

  return ir_program;
}
}  // namespace fleaux::frontend::lowering
