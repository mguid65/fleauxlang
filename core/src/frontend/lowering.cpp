#include "fleaux/frontend/lowering.hpp"
#include "fleaux/frontend/analysis.hpp"
#include "fleaux/frontend/source_loader.hpp"
#include "fleaux/frontend/type_check.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>

#include "fleaux/common/overloaded.hpp"

namespace fleaux::frontend::lowering {
namespace {
const std::unordered_set<std::string> kOperators = {
    "^", "/", "*", "%", "+", "-", "==", "!=", "<", ">", ">=", "<=", "!", "&&", "||",
};

const std::string kMatchWildcardSentinel = "__fleaux_match_wildcard__";

auto analyze_with_symbolic_imports(const ir::IRProgram& program) -> type_check::AnalysisResult {
  std::unordered_set<std::string> imported_symbols;
  std::vector<ir::IRLet> imported_typed_lets;
  std::vector<ir::IRTypeDecl> imported_type_decls;
  std::vector<ir::IRAliasDecl> imported_alias_decls;
  const auto seeded = source_loader::seed_symbolic_imports_for_program<type_check::AnalysisError>(
      program,
      [](const std::string& message, const std::optional<std::string>& hint,
         const std::optional<diag::SourceSpan>& span) -> type_check::AnalysisError {
        return type_check::AnalysisError{
            .message = message,
            .hint = hint,
            .span = span,
        };
      },
      imported_symbols, imported_typed_lets, imported_type_decls, imported_alias_decls);
  if (!seeded) {
    return tl::unexpected(seeded.error());
  }
  return type_check::analyze_program(program, imported_symbols, imported_typed_lets, imported_type_decls,
                                     imported_alias_decls);
}

auto split_id(const std::variant<std::string, model::QualifiedId>& id)
    -> std::pair<std::optional<std::string>, std::string> {
  return std::visit(
      common::overloaded{[](const std::string& simple) -> std::pair<std::optional<std::string>, std::string> {
                           return {std::nullopt, simple};
                         },
                         [](const model::QualifiedId& qualified) -> std::pair<std::optional<std::string>, std::string> {
                           return {qualified.qualifier.qualifier, qualified.id};
                         }},
      id);
}

auto lower_simple_type(const model::TypeNode& type) -> ir::IRSimpleType {
  const auto lower_bridge_type_node = [&](const auto& self, const model::TypeNode& node) -> types::TypeNode {
    types::TypeNode out;
    out.span = node.span;
    out.variadic = node.variadic;
    std::visit(common::overloaded{[&](const std::string& simple) -> void {
                                    std::string raw = simple;
                                    if (raw.size() >= 3U && raw.substr(raw.size() - 3U) == "...") {
                                      out.variadic = true;
                                      raw = raw.substr(0, raw.size() - 3U);
                                    }
                                    out.kind = types::TypeNodeKind::kNamed;
                                    out.name = raw;
                                  },
                                  [&](const model::QualifiedId& qualified) -> void {
                                    out.kind = types::TypeNodeKind::kNamed;
                                    out.name = qualified.qualifier.qualifier + "." + qualified.id;
                                  },
                                  [&](const model::TypeListBox& type_list) -> void {
                                    out.kind = types::TypeNodeKind::kTuple;
                                    out.name = "Tuple";
                                    out.items.reserve(type_list->types.size());
                                    for (const auto& item : type_list->types) {
                                      out.items.push_back(self(self, *item));
                                    }
                                  },
                                  [&](const model::UnionTypeListBox& union_list) -> void {
                                    out.kind = types::TypeNodeKind::kUnion;
                                    for (const auto& alt : union_list->alternatives) {
                                      out.items.push_back(self(self, *alt));
                                    }
                                    if (!out.items.empty()) {
                                      out.name = out.items.front().name;
                                    }
                                  },
                                  [&](const model::AppliedTypeNodeBox& applied) -> void {
                                    out.kind = types::TypeNodeKind::kApplied;
                                    out.name = applied->name;
                                    out.items.reserve(applied->args.types.size());
                                    for (const auto& arg : applied->args.types) {
                                      out.items.push_back(self(self, *arg));
                                    }
                                  },
                                  [&](const model::FunctionTypeNodeBox& func) -> void {
                                    out.kind = types::TypeNodeKind::kFunction;
                                    out.name = "Function";
                                    out.items.reserve(func->params.types.size() + 1);
                                    for (const auto& param : func->params.types) {
                                      out.items.push_back(self(self, *param));
                                    }
                                    out.items.push_back(self(self, *func->return_type));
                                  }},
               node.value);
    return out;
  };

  ir::IRSimpleType out;
  out.span = type.span;
  out.variadic = type.variadic;
  out.bridge_type_node = lower_bridge_type_node(lower_bridge_type_node, type);
  std::visit(common::overloaded{[&](const std::string& simple) -> void {
                                  std::string raw = simple;
                                  if (raw.size() >= 3U && raw.substr(raw.size() - 3U) == "...") {
                                    out.variadic = true;
                                    raw = raw.substr(0, raw.size() - 3U);
                                  }
                                  out.name = raw;
                                },
                                [&](const model::QualifiedId& qualified) -> void {
                                  out.name = qualified.qualifier.qualifier + "." + qualified.id;
                                },
                                [&](const model::UnionTypeListBox& union_list) -> void {
                                  for (const auto& alt : union_list->alternatives) {
                                    const ir::IRSimpleType lowered = lower_simple_type(*alt);
                                    out.alternative_types.push_back(lowered);
                                    if (!lowered.alternatives.empty()) {
                                      out.alternatives.insert(out.alternatives.end(), lowered.alternatives.begin(),
                                                              lowered.alternatives.end());
                                    } else {
                                      out.alternatives.push_back(lowered.name);
                                    }
                                  }
                                  if (!out.alternatives.empty()) {
                                    out.name = out.alternatives.front();
                                  }
                                },
                                [&](const model::TypeListBox& type_list) -> void {
                                  out.name = "Tuple";
                                  out.tuple_items.reserve(type_list->types.size());
                                  for (const auto& item : type_list->types) {
                                    out.tuple_items.push_back(lower_simple_type(*item));
                                  }
                                },
                                [&](const model::AppliedTypeNodeBox& applied) -> void {
                                  out.name = applied->name;
                                  out.type_args.reserve(applied->args.types.size());
                                  for (const auto& arg : applied->args.types) {
                                    out.type_args.push_back(lower_simple_type(*arg));
                                  }
                                },
                                [&](const model::FunctionTypeNodeBox& func) -> void {
                                  out.name = "Function";
                                  std::vector<ir::IRSimpleType> param_types;
                                  param_types.reserve(func->params.types.size());
                                  for (const auto& param : func->params.types) {
                                    param_types.push_back(lower_simple_type(*param));
                                  }
                                  out.function_sig = ir::IRSimpleType::FunctionSignature{
                                      .param_types = std::move(param_types),
                                      .return_type = ir::IRSimpleTypeBox(lower_simple_type(*func->return_type)),
                                  };
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
    return tl::unexpected(LoweringError{.message = "Std.Match expects '(value, (pattern, handler), ... )'.",
                                        .hint = "Add at least one '(pattern, handler)' case tuple.",
                                        .span = span});
  }

  for (std::size_t idx = 1; idx < match_args->items.size(); ++idx) {
    auto& case_expr = match_args->items[idx];
    auto* case_tuple = std::get_if<ir::IRTupleExpr>(&case_expr->node);
    if (case_tuple == nullptr || case_tuple->items.size() != 2U) {
      return tl::unexpected(LoweringError{.message = "Std.Match case must be a 2-item tuple '(pattern, handler)'.",
                                          .hint = "Each case should look like '(0, (): Any = \"zero\")'.",
                                          .span = case_expr->span});
    }

    auto& pattern_expr = case_tuple->items[0];
    if (const auto* pattern_name = std::get_if<ir::IRNameRef>(&pattern_expr->node); pattern_name != nullptr) {
      const bool is_wildcard_pattern = !pattern_name->qualifier.has_value() && pattern_name->name == "_";

      if (is_wildcard_pattern && idx + 1U != match_args->items.size()) {
        return tl::unexpected(LoweringError{.message = "Std.Match wildcard '_' must be the final case.",
                                            .hint = "Move '(_, handler)' to the end so earlier cases can still match.",
                                            .span = pattern_expr->span});
      }

      if (is_wildcard_pattern) {
        ir::IRConstant wildcard;
        wildcard.val = kMatchWildcardSentinel;
        wildcard.span = pattern_name->span;
        pattern_expr->node = std::move(wildcard);
      }
    }
  }

  return {};
}

void collect_unqualified_names_from_expr(const model::Expression& expr, std::vector<std::string>& out,
                                         std::unordered_set<std::string>& seen);

void collect_unqualified_names_from_atom(const model::Atom& atom, std::vector<std::string>& out,
                                         std::unordered_set<std::string>& seen) {
  std::visit(common::overloaded{
                 [](const std::monostate&) -> void {},
                 [](const model::Constant&) -> void {},
                 [](const model::QualifiedId&) -> void {},
                 [&](const model::NamedTargetBox& value) -> void {
                   if (const auto* simple = std::get_if<std::string>(&value->target);
                       simple != nullptr && !kOperators.contains(*simple) && seen.insert(*simple).second) {
                     out.push_back(*simple);
                   }
                 },
                 [&](const std::string& value) -> void {
                   if (!kOperators.contains(value) && seen.insert(value).second) {
                     out.push_back(value);
                   }
                 },
                 [&](const model::DelimitedExpressionBox& value) -> void {
                   for (const auto& item : value->items) {
                     collect_unqualified_names_from_expr(*item, out, seen);
                   }
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
  for (const auto& stage : expr.expr.rhs) {
    collect_unqualified_names_from_atom(stage.base, out, seen);
  }
}

auto extract_call_target_from_primary(const model::Primary& primary) -> tl::expected<ir::IRCallTarget, LoweringError> {
  return std::visit(
      common::overloaded{
          [&](const model::NamedTargetBox& value) -> tl::expected<ir::IRCallTarget, LoweringError> {
            std::vector<ir::IRSimpleType> explicit_type_args;
            explicit_type_args.reserve(value->explicit_type_args.size());
            for (const auto& type_arg : value->explicit_type_args) {
              explicit_type_args.push_back(lower_simple_type(*type_arg));
            }

            return std::visit(
                fleaux::common::overloaded{
                    [&](const model::QualifiedId& qualified) -> tl::expected<ir::IRCallTarget, LoweringError> {
                      return ir::IRNameRef{.qualifier = qualified.qualifier.qualifier,
                                           .name = qualified.id,
                                           .explicit_type_args = std::move(explicit_type_args),
                                           .resolved_symbol_key = std::nullopt,
                                           .span = value->span};
                    },
                    [&](const std::string& simple) -> tl::expected<ir::IRCallTarget, LoweringError> {
                      if (kOperators.contains(simple)) {
                        return tl::unexpected(LoweringError{
                            .message = "Explicit type argument application is only supported on named call targets.",
                            .hint = "Use explicit type arguments only with names such as 'Std.Cast<Id>'.",
                            .span = value->span});
                      }
                      return ir::IRNameRef{.qualifier = std::nullopt,
                                           .name = simple,
                                           .explicit_type_args = std::move(explicit_type_args),
                                           .resolved_symbol_key = std::nullopt,
                                           .span = value->span};
                    }},
                value->target);
          },
          [&](const model::QualifiedId& qualified) -> tl::expected<ir::IRCallTarget, LoweringError> {
            return ir::IRNameRef{.qualifier = qualified.qualifier.qualifier,
                                 .name = qualified.id,
                                 .explicit_type_args = {},
                                 .resolved_symbol_key = std::nullopt,
                                 .span = qualified.span};
          },
          [&](const std::string& value) -> tl::expected<ir::IRCallTarget, LoweringError> {
            if (kOperators.contains(value)) {
              return ir::IROperatorRef{.op = value, .span = primary.span};
            }
            return ir::IRNameRef{
                .qualifier = std::nullopt,
                .name = value,
                .explicit_type_args = {},
                .resolved_symbol_key = std::nullopt,
                .span = primary.span,
            };
          },
          [&](const auto&) -> tl::expected<ir::IRCallTarget, LoweringError> {
            return tl::unexpected(LoweringError{
                .message = "Primary used as flow target must be a simple name, qualified name, or operator.",
                .hint = "Valid targets: 'Std.Add', 'MyFunc', '+', '/', '&&'.",
                .span = primary.span});
          }},
      primary.base.value);
}

auto contains_placeholder(const ir::IRExpr& expr) -> bool;

auto replace_placeholder_impl(const ir::IRExpr& expr, const ir::IRExpr& replacement) -> ir::IRExpr {
  return std::visit(
      common::overloaded{[&](const ir::IRNameRef& name_ref) -> ir::IRExpr {
                           if (!name_ref.qualifier.has_value() && name_ref.name == "_") {
                             return replacement;
                           }
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
                           ir::IRFlowExpr out_flow{
                               .lhs = ir::IRExprBox(replace_placeholder_impl(*flow_expr.lhs, replacement)),
                               .rhs = flow_expr.rhs,
                               .span = flow_expr.span,
                           };
                           return ir::IRExpr{.node = std::move(out_flow), .span = expr.span};
                         },
                         [&](const auto&) -> ir::IRExpr { return expr; }},
      expr.node);
}

auto contains_placeholder(const ir::IRExpr& expr) -> bool {
  return std::visit(
      common::overloaded{
          [](const ir::IRNameRef& name_ref) -> bool { return !name_ref.qualifier.has_value() && name_ref.name == "_"; },
          [](const ir::IRTupleExpr& tuple_expr) -> bool {
            return std::ranges::any_of(tuple_expr.items,
                                       [](const auto& item) -> bool { return contains_placeholder(*item); });
          },
          [](const ir::IRFlowExpr& flow_expr) -> bool { return contains_placeholder(*flow_expr.lhs); },
          [](const auto&) -> bool { return false; }},
      expr.node);
}

auto replace_placeholder(const ir::IRExpr& template_expr, const ir::IRExpr& current_value)
    -> tl::expected<ir::IRExpr, LoweringError> {
  auto replaced = replace_placeholder_impl(template_expr, current_value);
  if (contains_placeholder(replaced)) {
    return tl::unexpected(
        LoweringError{.message = "Unresolved '_' placeholder remained in tuple template.",
                      .hint = "Use '_' only as an argument position inside tuple templates like '(_, 2)'.",
                      .span = template_expr.span});
  }
  return replaced;
}

auto lower_atom(const model::Atom& atom, const std::unordered_set<std::string>& bound_names)
    -> tl::expected<ir::IRExpr, LoweringError> {
  return std::visit(
      common::overloaded{
          [&](const model::DelimitedExpressionBox& inner) -> tl::expected<ir::IRExpr, LoweringError> {
            if (inner->items.size() == 1) {
              return lower_expr(*inner->items[0], bound_names);
            }

            ir::IRTupleExpr tuple;
            tuple.span = inner->span;
            for (const auto& item : inner->items) {
              auto lowered_item = lower_expr(*item, bound_names);
              if (!lowered_item) {
                return tl::unexpected(lowered_item.error());
              }
              tuple.items.emplace_back(lowered_item.value());
            }
            return ir::IRExpr{.node = std::move(tuple), .span = atom.span};
          },
          [&](const model::ClosureExpressionBox& closure) -> tl::expected<ir::IRExpr, LoweringError> {
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
                return tl::unexpected(
                    LoweringError{.message = "Variadic parameter must be the final parameter in a closure declaration.",
                                  .hint = "Move the '...' parameter to the end of the closure parameter list.",
                                  .span = params[idx].span});
              }
            }

            std::unordered_set<std::string> closure_bound = bound_names;
            for (const auto& p : params) {
              closure_bound.insert(p.name);
            }

            std::vector<std::string> discovered_names;
            std::unordered_set<std::string> seen_names;
            collect_unqualified_names_from_expr(*closure->body, discovered_names, seen_names);

            std::vector<std::string> captures;
            for (const auto& candidate : discovered_names) {
              if (param_names.contains(candidate)) {
                continue;
              }
              if (bound_names.contains(candidate)) {
                captures.push_back(candidate);
              }
            }

            auto lowered_body = lower_expr(*closure->body, closure_bound);
            if (!lowered_body) {
              return tl::unexpected(lowered_body.error());
            }

            ir::IRClosureExpr closure_ir{
                .generic_params = closure->generic_params,
                .params = std::move(params),
                .return_type = lower_simple_type(closure->rtype),
                .body = ir::IRExprBox(std::move(*lowered_body)),
                .captures = std::move(captures),
                .span = closure->span,
            };

            return ir::IRExpr{.node = ir::make_ir_closure_expr_box(std::move(closure_ir)), .span = atom.span};
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
            return ir::IRExpr{.node = ir::IRNameRef{.qualifier = qid.qualifier.qualifier,
                                                    .name = qid.id,
                                                    .explicit_type_args = {},
                                                    .resolved_symbol_key = std::nullopt,
                                                    .span = qid.span},
                              .span = atom.span};
          },
          [&](const model::NamedTargetBox& target) -> tl::expected<ir::IRExpr, LoweringError> {
            std::vector<ir::IRSimpleType> explicit_type_args;
            explicit_type_args.reserve(target->explicit_type_args.size());
            for (const auto& type_arg : target->explicit_type_args) {
              explicit_type_args.push_back(lower_simple_type(*type_arg));
            }

            return std::visit(
                common::overloaded{[&](const model::QualifiedId& qid) -> tl::expected<ir::IRExpr, LoweringError> {
                                     if (target->explicit_type_args.empty() && qid.qualifier.qualifier == "Std" &&
                                         (qid.id == "Ok" || qid.id == "Err")) {
                                       ir::IRConstant c;
                                       c.val = qid.id == "Ok";
                                       c.span = qid.span;
                                       return ir::IRExpr{.node = std::move(c), .span = atom.span};
                                     }
                                     return ir::IRExpr{
                                         .node = ir::IRNameRef{.qualifier = qid.qualifier.qualifier,
                                                               .name = qid.id,
                                                               .explicit_type_args = std::move(explicit_type_args),
                                                               .resolved_symbol_key = std::nullopt,
                                                               .span = target->span},
                                         .span = atom.span};
                                   },
                                   [&](const std::string& name) -> tl::expected<ir::IRExpr, LoweringError> {
                                     return ir::IRExpr{.node =
                                                           ir::IRNameRef{
                                                               .qualifier = std::nullopt,
                                                               .name = name,
                                                               .explicit_type_args = std::move(explicit_type_args),
                                                               .resolved_symbol_key = std::nullopt,
                                                               .span = target->span,
                                                           },
                                                       .span = atom.span};
                                   }},
                target->target);
          },
          [&](const std::string& name) -> tl::expected<ir::IRExpr, LoweringError> {
            return ir::IRExpr{.node =
                                  ir::IRNameRef{
                                      .qualifier = std::nullopt,
                                      .name = name,
                                      .explicit_type_args = {},
                                      .resolved_symbol_key = std::nullopt,
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
  if (!result) {
    return tl::unexpected(result.error());
  }

  const auto apply_direct_target_stage = [&](const model::Primary& stage_primary) -> tl::expected<bool, LoweringError> {
    auto maybe_target = extract_call_target_from_primary(stage_primary);
    if (!maybe_target) {
      return false;
    }

    if (is_std_match_target(maybe_target.value())) {
      if (auto rewritten = rewrite_match_wildcards(result.value(), stage_primary.span); !rewritten) {
        return tl::unexpected(rewritten.error());
      }
    }

    ir::IRFlowExpr ir_flow{
        .lhs = ir::IRExprBox(std::move(*result)),
        .rhs = maybe_target.value(),
        .span = flow.span,
    };
    result = ir::IRExpr{.node = std::move(ir_flow), .span = flow.span};
    return true;
  };

  const auto apply_closure_stage = [&](const model::Primary& stage_primary, const ir::IRExpr& stage_expr) -> bool {
    if (std::get_if<ir::IRClosureExprBox>(&stage_expr.node) == nullptr) {
      return false;
    }

    ir::IRTupleExpr apply_args;
    apply_args.span = stage_primary.span;
    apply_args.items.emplace_back(result.value());
    apply_args.items.emplace_back(stage_expr);

    const ir::IRExpr apply_lhs{.node = std::move(apply_args), .span = stage_primary.span};

    ir::IRFlowExpr apply_flow{
        .lhs = ir::IRExprBox(apply_lhs),
        .rhs =
            ir::IRNameRef{
                .qualifier = "Std",
                .name = "Apply",
                .explicit_type_args = {},
                .resolved_symbol_key = std::nullopt,
                .span = stage_primary.span,
            },
        .span = stage_primary.span,
    };

    result = ir::IRExpr{.node = std::move(apply_flow), .span = stage_primary.span};
    return true;
  };

  const auto apply_template_stage = [&](const model::Primary& stage_primary, const ir::IRExpr& stage_expr,
                                        const model::Primary& next_primary) -> tl::expected<void, LoweringError> {
    if (std::get_if<ir::IRTupleExpr>(&stage_expr.node) == nullptr) {
      return tl::unexpected(
          LoweringError{.message = "Invalid pipeline stage shape: non-call stages must be tuple templates.",
                        .hint = "Use a call target like '-> Std.Add' or a tuple template like '-> (_, 2) -> Std.Add'.",
                        .span = stage_primary.span});
    }

    auto next_target = extract_call_target_from_primary(next_primary);
    if (!next_target) {
      return tl::unexpected(next_target.error());
    }

    auto replaced = replace_placeholder(stage_expr, result.value());
    if (!replaced) {
      return tl::unexpected(replaced.error());
    }

    ir::IRFlowExpr ir_flow{
        .lhs = ir::IRExprBox(std::move(*replaced)),
        .rhs = next_target.value(),
        .span = flow.span,
    };

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
    if (!stage_expr) {
      return tl::unexpected(stage_expr.error());
    }

    if (apply_closure_stage(flow.rhs[stage_index], stage_expr.value())) {
      ++stage_index;
      continue;
    }

    if (stage_index + 1 >= flow.rhs.size()) {
      return tl::unexpected(LoweringError{.message = "Tuple template stage is missing a following call target.",
                                          .hint = "Append a call target, e.g. '-> (_, 2) -> Std.Divide'.",
                                          .span = flow.rhs[stage_index].span});
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

class IRDumper {
public:
  [[nodiscard]] auto dump(const ir::IRProgram& program) const -> std::string { return format_program(program); }

private:
  [[nodiscard]] static auto indent(const int level) -> std::string {
    const std::size_t count = static_cast<std::size_t>(level) * 2U;
    // Clang-Tidy is wrong here. If you use braced-init here then the wrong overload is selected.
    // NOLINTNEXTLINE(modernize-return-braced-init-list)
    return std::string(count, ' ');
  }

  [[nodiscard]] static auto escape_string(const std::string_view text) -> std::string {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char ch : text) {
      switch (ch) {
        case '\\':
          escaped += "\\\\";
          break;
        case '\"':
          escaped += "\\\"";
          break;
        case '\n':
          escaped += "\\n";
          break;
        case '\r':
          escaped += "\\r";
          break;
        case '\t':
          escaped += "\\t";
          break;
        default:
          escaped.push_back(ch);
          break;
      }
    }
    return escaped;
  }

  [[nodiscard]] static auto quote(const std::string_view text) -> std::string {
    return std::format("\"{}\"", escape_string(text));
  }

  [[nodiscard]] static auto format_list(const std::vector<std::string>& items, const int level) -> std::string {
    if (items.empty()) {
      return "[]";
    }

    std::string out = "[\n";
    for (std::size_t idx = 0; idx < items.size(); ++idx) {
      out += indent(level + 1) + items[idx];
      if (idx + 1 < items.size()) {
        out += ',';
      }
      out += '\n';
    }
    out += indent(level) + ']';
    return out;
  }

  [[nodiscard]] static auto format_string_list(const std::vector<std::string>& items, const int level) -> std::string {
    std::vector<std::string> formatted;
    formatted.reserve(items.size());
    for (const auto& item : items) {
      formatted.push_back(quote(item));
    }
    return format_list(formatted, level);
  }

  [[nodiscard]] static auto format_block(const std::string_view name, const std::vector<std::string>& fields,
                                         const int level) -> std::string {
    if (fields.empty()) {
      return std::format("{} {{}}", name);
    }

    std::string out = std::format("{} {{\n", name);
    for (std::size_t idx = 0; idx < fields.size(); ++idx) {
      out += indent(level + 1) + fields[idx];
      if (idx + 1 < fields.size()) {
        out += ',';
      }
      out += '\n';
    }
    out += indent(level) + '}';
    return out;
  }

  [[nodiscard]] static auto format_qualifier(const std::optional<std::string>& qualifier) -> std::string {
    return qualifier ? quote(*qualifier) : std::string{"null"};
  }

  [[nodiscard]] auto format_program(const ir::IRProgram& program, const int level = 0) const -> std::string {
    std::vector<std::string> imports;
    imports.reserve(program.imports.size());
    for (const auto& import_stmt : program.imports) {
      imports.push_back(format_import(import_stmt, level + 1));
    }

    std::vector<std::string> type_decls;
    type_decls.reserve(program.type_decls.size());
    for (const auto& type_decl : program.type_decls) {
      type_decls.push_back(format_type_decl(type_decl, level + 1));
    }

    std::vector<std::string> lets;
    lets.reserve(program.lets.size());
    for (const auto& let : program.lets) {
      lets.push_back(format_let(let, level + 1));
    }

    std::vector<std::string> expressions;
    expressions.reserve(program.expressions.size());
    for (const auto& expr_stmt : program.expressions) {
      expressions.push_back(format_expr_statement(expr_stmt, level + 1));
    }

    std::vector<std::string> alias_decls;
    alias_decls.reserve(program.alias_decls.size());
    for (const auto& alias_decl : program.alias_decls) {
      alias_decls.push_back(format_alias_decl(alias_decl, level + 1));
    }

    return format_block("IRProgram",
                        {std::format("imports: {}", format_list(imports, level + 1)),
                         std::format("type_decls: {}", format_list(type_decls, level + 1)),
                         std::format("lets: {}", format_list(lets, level + 1)),
                         std::format("expressions: {}", format_list(expressions, level + 1)),
                         std::format("alias_decls: {}", format_list(alias_decls, level + 1))},
                        level);
  }

  [[nodiscard]] static auto format_import(const ir::IRImport& import_stmt, const int level) -> std::string {
    return format_block("IRImport", {std::format("module_name: {}", quote(import_stmt.module_name))}, level);
  }

  [[nodiscard]] static auto format_type_decl(const ir::IRTypeDecl& type_decl, const int level) -> std::string {
    return format_block("IRTypeDecl",
                        {std::format("name: {}", quote(type_decl.name)),
                         std::format("target: {}", format_simple_type(type_decl.target, level + 1))},
                        level);
  }

  [[nodiscard]] static auto format_alias_decl(const ir::IRAliasDecl& alias_decl, const int level) -> std::string {
    return format_block("IRAliasDecl",
                        {std::format("name: {}", quote(alias_decl.name)),
                         std::format("target: {}", format_simple_type(alias_decl.target, level + 1))},
                        level);
  }

  [[nodiscard]] auto format_let(const ir::IRLet& let, const int level) const -> std::string {
    std::vector<std::string> params;
    params.reserve(let.params.size());
    for (const auto& param : let.params) {
      params.push_back(format_param(param, level + 1));
    }

    return format_block("IRLet",
                        {std::format("qualifier: {}", format_qualifier(let.qualifier)),
                         std::format("name: {}", quote(let.name)), std::format("symbol_key: {}", quote(let.symbol_key)),
                         std::format("generic_params: {}", format_string_list(let.generic_params, level + 1)),
                         std::format("params: {}", format_list(params, level + 1)),
                         std::format("return_type: {}", format_simple_type(let.return_type, level + 1)),
                         std::format("doc_comments: {}", format_string_list(let.doc_comments, level + 1)),
                         std::format("body: {}", let.body ? format_expr(*let.body, level + 1) : std::string{"null"}),
                         std::format("is_builtin: {}", let.is_builtin)},
                        level);
  }

  [[nodiscard]] auto format_expr_statement(const ir::IRExprStatement& stmt, const int level) const -> std::string {
    return format_block("IRExprStatement", {std::format("expr: {}", format_expr(stmt.expr, level + 1))}, level);
  }

  [[nodiscard]] static auto format_param(const ir::IRParam& param, const int level) -> std::string {
    return format_block("IRParam",
                        {std::format("name: {}", quote(param.name)),
                         std::format("type: {}", format_simple_type(param.type, level + 1))},
                        level);
  }

  [[nodiscard]] static auto format_simple_type(const ir::IRSimpleType& type, const int level) -> std::string {
    std::vector<std::string> alternative_types;
    alternative_types.reserve(type.alternative_types.size());
    for (const auto& alt : type.alternative_types) {
      alternative_types.push_back(format_simple_type(alt, level + 1));
    }

    std::vector<std::string> tuple_items;
    tuple_items.reserve(type.tuple_items.size());
    for (const auto& item : type.tuple_items) {
      tuple_items.push_back(format_simple_type(item, level + 1));
    }

    std::vector<std::string> type_args;
    type_args.reserve(type.type_args.size());
    for (const auto& arg : type.type_args) {
      type_args.push_back(format_simple_type(arg, level + 1));
    }

    std::vector fields{
        std::format("name: {}", quote(type.name)),
        std::format("variadic: {}", type.variadic),
        std::format("alternatives: {}", format_string_list(type.alternatives, level + 1)),
        std::format("alternative_types: {}", format_list(alternative_types, level + 1)),
        std::format("tuple_items: {}", format_list(tuple_items, level + 1)),
        std::format("type_args: {}", format_list(type_args, level + 1)),
    };

    if (type.function_sig.has_value()) {
      std::vector<std::string> function_params;
      function_params.reserve(type.function_sig->param_types.size());
      for (const auto& param_type : type.function_sig->param_types) {
        function_params.push_back(format_simple_type(param_type, level + 2));
      }
      fields.push_back(std::format(
          "function_sig: {}",
          format_block(
              "FunctionSignature",
              {std::format("param_types: {}", format_list(function_params, level + 2)),
               std::format("return_type: {}", type.function_sig->return_type
                                                  ? format_simple_type(*type.function_sig->return_type, level + 2)
                                                  : std::string{"null"})},
              level + 1)));
    } else {
      fields.emplace_back("function_sig: null");
    }

    return format_block("IRSimpleType", fields, level);
  }

  [[nodiscard]] auto format_expr(const ir::IRExpr& expr, const int level) const -> std::string {
    return format_block("IRExpr",
                        {std::format("kind: {}", quote(expr_kind_name(expr.node))),
                         std::format("node: {}", format_expr_node(expr.node, level + 1))},
                        level);
  }

  [[nodiscard]] static auto expr_kind_name(
      const std::variant<ir::IRFlowExpr, ir::IRTupleExpr, ir::IRConstant, ir::IRNameRef, ir::IRClosureExprBox>& node)
      -> std::string_view {
    return std::visit(
        common::overloaded{[](const ir::IRFlowExpr&) -> std::string_view { return "IRFlowExpr"; },
                           [](const ir::IRTupleExpr&) -> std::string_view { return "IRTupleExpr"; },
                           [](const ir::IRConstant&) -> std::string_view { return "IRConstant"; },
                           [](const ir::IRNameRef&) -> std::string_view { return "IRNameRef"; },
                           [](const ir::IRClosureExprBox&) -> std::string_view { return "IRClosureExpr"; }},
        node);
  }

  [[nodiscard]] auto format_expr_node(
      const std::variant<ir::IRFlowExpr, ir::IRTupleExpr, ir::IRConstant, ir::IRNameRef, ir::IRClosureExprBox>& node,
      const int level) const -> std::string {
    return std::visit(
        common::overloaded{
            [&](const ir::IRFlowExpr& flow) -> std::string { return format_flow_expr(flow, level); },
            [&](const ir::IRTupleExpr& tuple) -> std::string { return format_tuple_expr(tuple, level); },
            [&](const ir::IRConstant& constant) -> std::string { return format_constant(constant, level); },
            [&](const ir::IRNameRef& name_ref) -> std::string { return format_name_ref(name_ref, level); },
            [&](const ir::IRClosureExprBox& closure) -> std::string {
              return closure ? format_closure_expr(*closure, level) : std::string{"null"};
            }},
        node);
  }

  [[nodiscard]] auto format_flow_expr(const ir::IRFlowExpr& flow, const int level) const -> std::string {
    return format_block("IRFlowExpr",
                        {std::format("lhs: {}", flow.lhs ? format_expr(*flow.lhs, level + 1) : std::string{"null"}),
                         std::format("rhs: {}", format_call_target(flow.rhs, level + 1))},
                        level);
  }

  [[nodiscard]] auto format_tuple_expr(const ir::IRTupleExpr& tuple, const int level) const -> std::string {
    std::vector<std::string> items;
    items.reserve(tuple.items.size());
    for (const auto& item : tuple.items) {
      items.push_back(item ? format_expr(*item, level + 1) : std::string{"null"});
    }
    return format_block("IRTupleExpr", {std::format("items: {}", format_list(items, level + 1))}, level);
  }

  [[nodiscard]] auto format_closure_expr(const ir::IRClosureExpr& closure, const int level) const -> std::string {
    std::vector<std::string> params;
    params.reserve(closure.params.size());
    for (const auto& param : closure.params) {
      params.push_back(format_param(param, level + 1));
    }

    return format_block(
        "IRClosureExpr",
        {std::format("generic_params: {}", format_string_list(closure.generic_params, level + 1)),
         std::format("params: {}", format_list(params, level + 1)),
         std::format("return_type: {}", format_simple_type(closure.return_type, level + 1)),
         std::format("body: {}", closure.body ? format_expr(*closure.body, level + 1) : std::string{"null"}),
         std::format("captures: {}", format_string_list(closure.captures, level + 1))},
        level);
  }

  [[nodiscard]] static auto format_call_target(const ir::IRCallTarget& target, const int level) -> std::string {
    return std::visit(
        common::overloaded{
            [&](const ir::IRNameRef& name_ref) -> std::string { return format_name_ref(name_ref, level); },
            [&](const ir::IROperatorRef& op_ref) -> std::string { return format_operator_ref(op_ref, level); }},
        target);
  }

  [[nodiscard]] static auto format_name_ref(const ir::IRNameRef& name_ref, const int level) -> std::string {
    std::vector<std::string> type_args;
    type_args.reserve(name_ref.explicit_type_args.size());
    for (const auto& type_arg : name_ref.explicit_type_args) {
      type_args.push_back(format_simple_type(type_arg, level + 1));
    }

    return format_block(
        "IRNameRef",
        {std::format("qualifier: {}", format_qualifier(name_ref.qualifier)),
         std::format("name: {}", quote(name_ref.name)),
         std::format("explicit_type_args: {}", format_list(type_args, level + 1)),
         std::format("resolved_symbol_key: {}",
                     name_ref.resolved_symbol_key ? quote(*name_ref.resolved_symbol_key) : std::string{"null"})},
        level);
  }

  [[nodiscard]] static auto format_operator_ref(const ir::IROperatorRef& op_ref, const int level) -> std::string {
    return format_block("IROperatorRef", {std::format("op: {}", quote(op_ref.op))}, level);
  }

  [[nodiscard]] static auto format_constant(const ir::IRConstant& constant, const int level) -> std::string {
    return format_block(
        "IRConstant",
        {std::format(
            "value: {}",
            std::visit(
                common::overloaded{[](const std::int64_t value) -> std::string { return std::format("{}", value); },
                                   [](const std::uint64_t value) -> std::string { return std::format("{}u64", value); },
                                   [](const double value) -> std::string { return std::format("{}", value); },
                                   [](const bool value) -> std::string { return value ? "true" : "false"; },
                                   [](const std::string& value) -> std::string { return quote(value); },
                                   [](const std::monostate&) -> std::string { return std::string{"null"}; }},
                constant.val))},
        level);
  }
};
}  // namespace

auto Lowerer::lower_only(const model::Program& program) const -> LoweringResult {
  ir::IRProgram ir_program;
  ir_program.span = program.span;
  std::unordered_map<std::string, std::size_t> user_symbol_ordinals;

  for (const auto& stmt : program.statements) {
    auto lowered_stmt =
        std::visit(fleaux::common::overloaded{
                       [&](const model::ImportStatement& model_import) -> tl::expected<void, LoweringError> {
                         ir_program.imports.push_back(ir::IRImport{
                             .module_name = model_import.module_name,
                             .span = model_import.span,
                         });
                         return {};
                       },
                       [&](const model::TypeStatement& model_type) -> tl::expected<void, LoweringError> {
                         ir_program.type_decls.push_back(ir::IRTypeDecl{
                             .name = model_type.name,
                             .target = lower_simple_type(model_type.target),
                             .span = model_type.span,
                         });
                         return {};
                       },
                       [&](const model::AliasStatement& model_alias) -> tl::expected<void, LoweringError> {
                         ir_program.alias_decls.push_back(ir::IRAliasDecl{
                             .name = model_alias.name,
                             .target = lower_simple_type(model_alias.target),
                             .span = model_alias.span,
                         });
                         return {};
                       },
                       [&](const model::LetStatement& model_let) -> tl::expected<void, LoweringError> {
                         auto [qualifier, name] = split_id(model_let.id);
                         const std::string public_symbol = qualifier.has_value() ? (*qualifier + "." + name) : name;

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
                             return tl::unexpected(LoweringError{
                                 .message = "Variadic parameter must be the final parameter in a function declaration.",
                                 .hint = "Move the '...' parameter to the end of the parameter list.",
                                 .span = params[idx].span});
                           }
                         }

                         const bool is_builtin = model_let.is_builtin;
                         std::string symbol_key = public_symbol;
                         if (!is_builtin) {
                           const auto ordinal = user_symbol_ordinals[public_symbol]++;
                           symbol_key += "#" + std::to_string(ordinal);
                         }

                         std::unordered_set<std::string> let_bound_names;
                         for (const auto& p : params) {
                           let_bound_names.insert(p.name);
                         }

                         ir::IRExprBox body;
                         if (!is_builtin) {
                           auto lowered_body = lower_expr(*model_let.expr, let_bound_names);
                           if (!lowered_body) {
                             return tl::unexpected(lowered_body.error());
                           }
                           body = std::move(*lowered_body);
                         }

                         ir_program.lets.push_back(ir::IRLet{
                             .qualifier = qualifier,
                             .name = name,
                             .symbol_key = std::move(symbol_key),
                             .generic_params = model_let.generic_params,
                             .params = std::move(params),
                             .return_type = lower_simple_type(model_let.rtype),
                             .doc_comments = model_let.doc_comments,
                             .body = std::move(body),
                             .is_builtin = is_builtin,
                             .span = model_let.span,
                         });
                         return {};
                       },
                       [&](const model::ExpressionStatement& model_expr_stmt) -> tl::expected<void, LoweringError> {
                         const std::unordered_set<std::string> no_bound_names;
                         auto lowered_expr = lower_expr(model_expr_stmt.expr, no_bound_names);
                         if (!lowered_expr) {
                           return tl::unexpected(lowered_expr.error());
                         }

                         ir_program.expressions.push_back(ir::IRExprStatement{
                             .expr = lowered_expr.value(),
                             .span = model_expr_stmt.span,
                         });
                         return {};
                       }},
                   stmt);

    if (!lowered_stmt) {
      return tl::unexpected(lowered_stmt.error());
    }
  }

  std::unordered_map<std::string, std::vector<std::size_t>> builtin_overload_slots;
  for (std::size_t let_index = 0; let_index < ir_program.lets.size(); ++let_index) {
    const auto& let = ir_program.lets[let_index];
    if (!let.is_builtin) {
      continue;
    }
    const std::string public_symbol = let.qualifier.has_value() ? (*let.qualifier + "." + let.name) : let.name;
    builtin_overload_slots[public_symbol].push_back(let_index);
  }

  for (const auto& [public_symbol, overload_slots] : builtin_overload_slots) {
    if (overload_slots.size() <= 1U) {
      continue;
    }
    for (std::size_t ordinal = 0; ordinal < overload_slots.size(); ++ordinal) {
      ir_program.lets[overload_slots[ordinal]].symbol_key = public_symbol + "#" + std::to_string(ordinal);
    }
  }

  return ir_program;
}

auto Lowerer::lower(const model::Program& program) const -> LoweringResult {
  auto lowered = lower_only(program);
  if (!lowered) {
    return tl::unexpected(lowered.error());
  }

  auto analyzed = analyze_with_symbolic_imports(*lowered);
  if (!analyzed) {
    return tl::unexpected(LoweringError{
        .message = analyzed.error().message,
        .hint = analyzed.error().hint,
        .span = analyzed.error().span,
    });
  }

  return *analyzed;
}

auto Lowerer::dump_ir(const ir::IRProgram& program) const -> std::string { return IRDumper{}.dump(program); }

}  // namespace fleaux::frontend::lowering

namespace fleaux::frontend::analysis {

auto Analyzer::lower_only(const model::Program& program) const -> lowering::LoweringResult {
  return lowering::Lowerer{}.lower_only(program);
}

auto Analyzer::analyze(const model::Program& program) const -> AnalysisResult {
  auto lowered = lowering::Lowerer{}.lower_only(program);
  if (!lowered) {
    return tl::unexpected(type_check::AnalysisError{
        .message = lowered.error().message,
        .hint = lowered.error().hint,
        .span = lowered.error().span,
    });
  }

  std::unordered_set<std::string> imported_symbols;
  std::vector<ir::IRLet> imported_typed_lets;
  std::vector<ir::IRTypeDecl> imported_type_decls;
  std::vector<ir::IRAliasDecl> imported_alias_decls;
  const auto seeded = source_loader::seed_symbolic_imports_for_program<type_check::AnalysisError>(
      *lowered,
      [](const std::string& message, const std::optional<std::string>& hint,
         const std::optional<diag::SourceSpan>& span) -> type_check::AnalysisError {
        return type_check::AnalysisError{
            .message = message,
            .hint = hint,
            .span = span,
        };
      },
      imported_symbols, imported_typed_lets, imported_type_decls, imported_alias_decls);
  if (!seeded) {
    return tl::unexpected(seeded.error());
  }
  return type_check::analyze_program(*lowered, imported_symbols, imported_typed_lets, imported_type_decls,
                                     imported_alias_decls);
}

}  // namespace fleaux::frontend::analysis
