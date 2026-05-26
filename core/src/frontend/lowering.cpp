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
#include <unordered_set>
#include <utility>
#include <variant>

#include "fleaux/common/overloaded.hpp"
#include "fleaux/common/symbol_name.hpp"
#include "fleaux/common/utility.hpp"
#include "fleaux/frontend/ir_tuple_protocol.hpp"

namespace fleaux::frontend::lowering {
namespace {
const std::unordered_set<std::string> kOperators = {
    "^", "/", "*", "%", "+", "-", "==", "!=", "<", ">", ">=", "<=", "!", "&&", "||",
};

const std::string kMatchWildcardSentinel = "__fleaux_match_wildcard__";

constexpr std::string_view kVariadicTypeSuffix = "...";

auto trim_variadic_type_suffix(std::string_view type_name) -> std::pair<std::string_view, bool> {
  const bool has_variadic_suffix = type_name.ends_with(kVariadicTypeSuffix);
  if (has_variadic_suffix) {
    type_name.remove_suffix(kVariadicTypeSuffix.size());
  }
  return {type_name, has_variadic_suffix};
}

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
                                    const auto [raw, has_variadic_suffix] = trim_variadic_type_suffix(simple);
                                    if (has_variadic_suffix) {
                                      out.variadic = true;
                                    }
                                    out.kind = types::TypeNodeKind::kNamed;
                                    out.name = std::string(raw);
                                  },
                                  [&](const model::QualifiedId& qualified) -> void {
                                    out.kind = types::TypeNodeKind::kNamed;
                                    out.name = common::full_symbol_name(qualified.qualifier.qualifier, qualified.id);
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
                                  const auto [raw, has_variadic_suffix] = trim_variadic_type_suffix(simple);
                                  if (has_variadic_suffix) {
                                    out.variadic = true;
                                  }
                                  out.name = std::string(raw);
                                },
                                [&](const model::QualifiedId& qualified) -> void {
                                  out.name = common::full_symbol_name(qualified.qualifier.qualifier, qualified.id);
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

struct LoweringContext {
  std::unordered_set<std::string> bound_names{};
  std::unordered_set<std::string> visible_top_level_value_symbols{};
  std::unordered_set<std::string> all_top_level_value_symbols{};
};


auto lower_expr(const model::Expression& expr, const LoweringContext& context)
    -> tl::expected<ir::IRExpr, LoweringError>;

auto make_ir_expr_box(ir::IRExpr value) -> ir::IRExprBox {
  ir::IRExprBox boxed;
  boxed.emplace(std::move(value));
  return boxed;
}

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
                 [&](const model::BlockExpressionBox&) -> void {},
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
                              out_tuple.items.push_back(make_ir_expr_box(replace_placeholder_impl(*item, replacement)));
                           }

                           return ir::IRExpr{.node = std::move(out_tuple), .span = expr.span};
                         },
                         [&](const ir::IRFlowExpr& flow_expr) -> ir::IRExpr {
                           ir::IRFlowExpr out_flow{
                                .lhs = make_ir_expr_box(replace_placeholder_impl(*flow_expr.lhs, replacement)),
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

auto is_future_top_level_value_symbol(const LoweringContext& context, const std::optional<std::string>& qualifier,
                                      const std::string& name) -> bool {
  const auto full_name = common::full_symbol_name(qualifier, name);
  return context.all_top_level_value_symbols.contains(full_name) &&
         !context.visible_top_level_value_symbols.contains(full_name);
}

auto make_use_before_declaration_error(const std::string& symbol, const std::optional<diag::SourceSpan>& span)
    -> LoweringError {
  return LoweringError{.message = "Use before declaration in value scope.",
                       .hint = std::format("'{}' is a top-level value and is only visible from its declaration point forward.",
                                           symbol),
                       .span = span};
}

auto lower_delimited_atom(const model::DelimitedExpressionBox& inner, const LoweringContext& context,
                          const std::optional<diag::SourceSpan>& expr_span)
    -> tl::expected<ir::IRExpr, LoweringError> {
  ir::IRTupleExpr tuple;
  tuple.span = inner->span;
  for (const auto& item : inner->items) {
    auto lowered_item = lower_expr(*item, context);
    if (!lowered_item) {
      return tl::unexpected(lowered_item.error());
    }
    tuple.items.push_back(make_ir_expr_box(std::move(*lowered_item)));
  }
  return ir::IRExpr{.node = std::move(tuple), .span = expr_span};
}

auto lower_closure_params(const model::ClosureExpressionBox& closure, std::unordered_set<std::string>& param_names)
    -> std::vector<ir::IRParam> {
  std::vector<ir::IRParam> params;
  params.reserve(closure->params.params.size());
  for (const auto& [param_name, type, span] : closure->params.params) {
    params.push_back(ir::IRParam{
        .name = param_name,
        .type = lower_simple_type(type),
        .span = span,
    });
    param_names.insert(param_name);
  }
  return params;
}

auto validate_closure_variadic_params(const std::vector<ir::IRParam>& params) -> tl::expected<void, LoweringError> {
  for (std::size_t idx = 0; idx < params.size(); ++idx) {
    if (params[idx].type.variadic && idx + 1U != params.size()) {
      return tl::unexpected(
          LoweringError{.message = "Variadic parameter must be the final parameter in a closure declaration.",
                        .hint = "Move the '...' parameter to the end of the closure parameter list.",
                        .span = params[idx].span});
    }
  }
  return {};
}

auto make_closure_bound_names(const std::unordered_set<std::string>& bound_names,
                              const std::vector<ir::IRParam>& params) -> std::unordered_set<std::string> {
  std::unordered_set<std::string> closure_bound = bound_names;
  for (const auto& p : params) {
    closure_bound.insert(p.name);
  }
  return closure_bound;
}

auto collect_closure_captures(const model::Expression& body, const std::unordered_set<std::string>& param_names,
                              const std::unordered_set<std::string>& bound_names) -> std::vector<std::string> {
  std::vector<std::string> discovered_names;
  std::unordered_set<std::string> seen_names;
  collect_unqualified_names_from_expr(body, discovered_names, seen_names);

  std::vector<std::string> captures;
  for (const auto& candidate : discovered_names) {
    if (param_names.contains(candidate)) {
      continue;
    }
    if (bound_names.contains(candidate)) {
      captures.push_back(candidate);
    }
  }
  return captures;
}

auto lower_block_expr(const model::BlockExpressionBox& block, const LoweringContext& context,
                      const std::optional<diag::SourceSpan>& expr_span) -> tl::expected<ir::IRExpr, LoweringError> {
  LoweringContext block_context = context;
  ir::IRBlockExpr block_ir;
  block_ir.span = block->span;

  for (const auto& item : block->items) {
    auto lowered_item = std::visit(
        common::overloaded{
            [&](const model::LocalLetBinding& local_let) -> tl::expected<ir::IRBlockItem, LoweringError> {
              auto lowered_init = lower_expr(*local_let.expr, block_context);
              if (!lowered_init) {
                return tl::unexpected(lowered_init.error());
              }

              ir::IRLocalLet lowered_local{
                  .name = local_let.name,
                  .type = lower_simple_type(local_let.type),
                  .expr = make_ir_expr_box(std::move(*lowered_init)),
                  .span = local_let.span,
              };
              block_context.bound_names.insert(local_let.name);
              return ir::IRBlockItem{std::move(lowered_local)};
            },
            [&](const model::ExpressionStatement& expr_stmt) -> tl::expected<ir::IRBlockItem, LoweringError> {
              auto lowered_expr = lower_expr(expr_stmt.expr, block_context);
              if (!lowered_expr) {
                return tl::unexpected(lowered_expr.error());
              }
              return ir::IRBlockItem{ir::IRBlockExprStatement{
                  .expr = make_ir_expr_box(std::move(*lowered_expr)),
                  .span = expr_stmt.span,
              }};
            }},
        item);
    if (!lowered_item) {
      return tl::unexpected(lowered_item.error());
    }
    block_ir.items.push_back(std::move(*lowered_item));
  }

  auto lowered_result = lower_expr(*block->result, block_context);
  if (!lowered_result) {
    return tl::unexpected(lowered_result.error());
  }
  block_ir.result = make_ir_expr_box(std::move(*lowered_result));
  return ir::IRExpr{.node = ir::make_ir_block_expr_box(std::move(block_ir)), .span = expr_span};
}

auto lower_closure_atom(const model::ClosureExpressionBox& closure, const LoweringContext& context,
                        const std::optional<diag::SourceSpan>& expr_span)
    -> tl::expected<ir::IRExpr, LoweringError> {
  std::unordered_set<std::string> param_names;
  auto params = lower_closure_params(closure, param_names);
  if (auto valid_params = validate_closure_variadic_params(params); !valid_params) {
    return tl::unexpected(valid_params.error());
  }

  auto closure_bound = make_closure_bound_names(context.bound_names, params);
  auto captures = collect_closure_captures(*closure->body, param_names, context.bound_names);

  LoweringContext closure_context = context;
  closure_context.bound_names = std::move(closure_bound);

  auto lowered_body = lower_expr(*closure->body, closure_context);
  if (!lowered_body) {
    return tl::unexpected(lowered_body.error());
  }

  ir::IRClosureExpr closure_ir{
      .generic_params = closure->generic_params,
      .params = std::move(params),
      .return_type = lower_simple_type(closure->rtype),
      .body = make_ir_expr_box(std::move(*lowered_body)),
      .captures = std::move(captures),
      .span = closure->span,
  };

  return ir::IRExpr{.node = ir::make_ir_closure_expr_box(std::move(closure_ir)), .span = expr_span};
}

auto make_name_ref_expr(std::optional<std::string> qualifier, std::string name,
                        std::vector<ir::IRSimpleType> explicit_type_args,
                        const std::optional<diag::SourceSpan>& ref_span,
                        const std::optional<diag::SourceSpan>& expr_span) -> ir::IRExpr {
  return ir::IRExpr{.node =
                        ir::IRNameRef{
                            .qualifier = std::move(qualifier),
                            .name = std::move(name),
                            .explicit_type_args = std::move(explicit_type_args),
                            .resolved_symbol_key = std::nullopt,
                             .materialize_as_value = false,
                            .span = ref_span,
                        },
                    .span = expr_span};
}

auto lower_std_result_atom(const model::QualifiedId& qid, const std::optional<diag::SourceSpan>& expr_span)
    -> std::optional<ir::IRExpr> {
  if (qid.qualifier.qualifier != "Std" || (qid.id != "Ok" && qid.id != "Err")) {
    return std::nullopt;
  }

  ir::IRConstant c;
  c.val = qid.id == "Ok";
  c.span = qid.span;
  return ir::IRExpr{.node = std::move(c), .span = expr_span};
}

auto lower_qualified_id_atom(const model::QualifiedId& qid, const std::optional<diag::SourceSpan>& expr_span)
    -> tl::expected<ir::IRExpr, LoweringError> {
  if (auto std_result = lower_std_result_atom(qid, expr_span); std_result.has_value()) {
    return std::move(*std_result);
  }

  return make_name_ref_expr(qid.qualifier.qualifier, qid.id, {}, qid.span, expr_span);
}

auto lower_named_target_atom(const model::NamedTargetBox& target, const LoweringContext& context,
                            const std::optional<diag::SourceSpan>& expr_span)
    -> tl::expected<ir::IRExpr, LoweringError> {
  std::vector<ir::IRSimpleType> explicit_type_args;
  explicit_type_args.reserve(target->explicit_type_args.size());
  for (const auto& type_arg : target->explicit_type_args) {
    explicit_type_args.push_back(lower_simple_type(*type_arg));
  }

  return std::visit(
      common::overloaded{[&](const model::QualifiedId& qid) -> tl::expected<ir::IRExpr, LoweringError> {
                            if (is_future_top_level_value_symbol(context, qid.qualifier.qualifier, qid.id)) {
                              return tl::unexpected(make_use_before_declaration_error(
                                  common::full_symbol_name(qid.qualifier.qualifier, qid.id), qid.span));
                            }

                           if (target->explicit_type_args.empty()) {
                             if (auto std_result = lower_std_result_atom(qid, expr_span); std_result.has_value()) {
                               return std::move(*std_result);
                             }
                           }

                           return make_name_ref_expr(qid.qualifier.qualifier, qid.id,
                                                     std::move(explicit_type_args), target->span, expr_span);
                         },
                         [&](const std::string& name) -> tl::expected<ir::IRExpr, LoweringError> {
                            if (!context.bound_names.contains(name) &&
                                is_future_top_level_value_symbol(context, std::nullopt, name)) {
                              return tl::unexpected(make_use_before_declaration_error(name, target->span));
                            }
                           return make_name_ref_expr(std::nullopt, name, std::move(explicit_type_args), target->span,
                                                     expr_span);
                         }},
      target->target);
}

auto lower_atom(const model::Atom& atom, const LoweringContext& context)
    -> tl::expected<ir::IRExpr, LoweringError> {
  return std::visit(
      common::overloaded{
          [&](const model::DelimitedExpressionBox& inner) -> tl::expected<ir::IRExpr, LoweringError> {
            return lower_delimited_atom(inner, context, atom.span);
          },
          [&](const model::ClosureExpressionBox& closure) -> tl::expected<ir::IRExpr, LoweringError> {
            return lower_closure_atom(closure, context, atom.span);
          },
          [&](const model::BlockExpressionBox& block) -> tl::expected<ir::IRExpr, LoweringError> {
            return lower_block_expr(block, context, atom.span);
          },
          [&](const model::Constant& constant) -> tl::expected<ir::IRExpr, LoweringError> {
            ir::IRConstant c;
            c.val = constant.val;
            c.span = constant.span;
            return ir::IRExpr{.node = std::move(c), .span = atom.span};
          },
          [&](const model::QualifiedId& qid) -> tl::expected<ir::IRExpr, LoweringError> {
            if (is_future_top_level_value_symbol(context, qid.qualifier.qualifier, qid.id)) {
              return tl::unexpected(make_use_before_declaration_error(
                  common::full_symbol_name(qid.qualifier.qualifier, qid.id), qid.span));
            }
            return lower_qualified_id_atom(qid, atom.span);
          },
          [&](const model::NamedTargetBox& target) -> tl::expected<ir::IRExpr, LoweringError> {
            return lower_named_target_atom(target, context, atom.span);
          },
          [&](const std::string& name) -> tl::expected<ir::IRExpr, LoweringError> {
            if (!context.bound_names.contains(name) && is_future_top_level_value_symbol(context, std::nullopt, name)) {
              return tl::unexpected(make_use_before_declaration_error(name, atom.span));
            }
            return make_name_ref_expr(std::nullopt, name, {}, atom.span, atom.span);
          },
          [&](const std::monostate&) -> tl::expected<ir::IRExpr, LoweringError> {
            ir::IRTupleExpr tuple;
            tuple.span = atom.span;
            return ir::IRExpr{.node = std::move(tuple), .span = atom.span};
          }},
      atom.value);
}

auto lower_primary(const model::Primary& primary, const LoweringContext& context)
    -> tl::expected<ir::IRExpr, LoweringError> {
  return lower_atom(primary.base, context);
}

auto try_apply_direct_target_stage(const ir::IRExpr& current_value, const model::Primary& stage_primary,
                                   const std::optional<diag::SourceSpan>& flow_span)
    -> tl::expected<std::optional<ir::IRExpr>, LoweringError> {
  auto maybe_target = extract_call_target_from_primary(stage_primary);
  if (!maybe_target) {
    return std::nullopt;
  }

  ir::IRExpr stage_value = current_value;

  if (is_std_match_target(maybe_target.value())) {
    if (auto rewritten = rewrite_match_wildcards(stage_value, stage_primary.span); !rewritten) {
      return tl::unexpected(rewritten.error());
    }
  }

  ir::IRFlowExpr ir_flow{
      .lhs = make_ir_expr_box(std::move(stage_value)),
      .rhs = maybe_target.value(),
      .span = flow_span,
  };
  return ir::IRExpr{.node = std::move(ir_flow), .span = flow_span};
}

auto try_apply_closure_stage(const ir::IRExpr& current_value, const model::Primary& stage_primary,
                             const ir::IRExpr& stage_expr) -> std::optional<ir::IRExpr> {
  if (std::get_if<ir::IRClosureExprBox>(&stage_expr.node) == nullptr) {
    return std::nullopt;
  }

  ir::IRTupleExpr apply_args;
  apply_args.span = stage_primary.span;
  apply_args.items.push_back(make_ir_expr_box(current_value));
  apply_args.items.push_back(make_ir_expr_box(stage_expr));

  const ir::IRExpr apply_lhs{.node = std::move(apply_args), .span = stage_primary.span};

  ir::IRFlowExpr apply_flow{
      .lhs = make_ir_expr_box(apply_lhs),
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

  return ir::IRExpr{.node = std::move(apply_flow), .span = stage_primary.span};
}

auto apply_template_stage(const ir::IRExpr& current_value, const std::optional<diag::SourceSpan>& flow_span,
                          const model::Primary& stage_primary, const ir::IRExpr& stage_expr,
                          const model::Primary& next_primary) -> tl::expected<ir::IRExpr, LoweringError> {
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

  auto replaced = replace_placeholder(stage_expr, current_value);
  if (!replaced) {
    return tl::unexpected(replaced.error());
  }

  ir::IRFlowExpr ir_flow{
      .lhs = make_ir_expr_box(std::move(*replaced)),
      .rhs = next_target.value(),
      .span = flow_span,
  };

  return ir::IRExpr{.node = std::move(ir_flow), .span = flow_span};
}

auto lower_flow(const model::FlowExpression& flow, const LoweringContext& context)
    -> tl::expected<ir::IRExpr, LoweringError> {
  auto result = lower_primary(flow.lhs, context);
  if (!result) {
    return tl::unexpected(result.error());
  }

  std::size_t stage_index = 0;
  while (stage_index < flow.rhs.size()) {
    if (auto handled_direct_target = try_apply_direct_target_stage(*result, flow.rhs[stage_index], flow.span);
        !handled_direct_target) {
      return tl::unexpected(handled_direct_target.error());
    } else if (handled_direct_target->has_value()) {
      result = std::move(**handled_direct_target);
      ++stage_index;
      continue;
    }

    auto stage_expr = lower_primary(flow.rhs[stage_index], context);
    if (!stage_expr) {
      return tl::unexpected(stage_expr.error());
    }

    if (auto applied_closure = try_apply_closure_stage(*result, flow.rhs[stage_index], stage_expr.value());
        applied_closure.has_value()) {
      result = std::move(*applied_closure);
      ++stage_index;
      continue;
    }

    if (stage_index + 1 >= flow.rhs.size()) {
      return tl::unexpected(LoweringError{.message = "Tuple template stage is missing a following call target.",
                                          .hint = "Append a call target, e.g. '-> (_, 2) -> Std.Divide'.",
                                          .span = flow.rhs[stage_index].span});
    }

    if (auto applied_template = apply_template_stage(*result, flow.span, flow.rhs[stage_index], stage_expr.value(),
                                                     flow.rhs[stage_index + 1]);
        !applied_template) {
      return tl::unexpected(applied_template.error());
    } else {
      result = std::move(*applied_template);
    }
    stage_index += 2;
  }

  return result;
}

auto lower_expr(const model::Expression& expr, const LoweringContext& context)
    -> tl::expected<ir::IRExpr, LoweringError> {
  return lower_flow(expr.expr, context);
}

class IRDumper {
public:
  [[nodiscard]] auto dump(const ir::IRProgram& program) const -> std::string { return format_program(program, 0); }

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

  template <typename Range, typename Formatter>
  [[nodiscard]] static auto collect_formatted_items(const Range& items, Formatter&& formatter)
      -> std::vector<std::string> {
    std::vector<std::string> formatted;
    formatted.reserve(items.size());
    for (const auto& item : items) {
      formatted.push_back(std::invoke(formatter, item));
    }
    return formatted;
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

  [[nodiscard]] auto format_nullable_expr(const ir::IRExprBox& expr, const int level) const -> std::string {
    return expr ? format_expr(*expr, level) : std::string{"null"};
  }

  [[nodiscard]] auto format_program(const ir::IRProgram& program, const int level) const -> std::string {
    const auto imports = collect_formatted_items(program.imports,
                                                 [&](const auto& import_stmt) { return format_import(import_stmt, level + 2); });
    const auto type_decls = collect_formatted_items(program.type_decls,
                                                    [&](const auto& type_decl) { return format_type_decl(type_decl, level + 2); });
    const auto lets = collect_formatted_items(program.lets,
                                              [&](const auto& let) { return format_let(let, level + 2); });
    const auto expressions = collect_formatted_items(
        program.expressions, [&](const auto& expr_stmt) { return format_expr_statement(expr_stmt, level + 2); });
    const auto alias_decls = collect_formatted_items(
        program.alias_decls, [&](const auto& alias_decl) { return format_alias_decl(alias_decl, level + 2); });

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
    const auto params = collect_formatted_items(let.params, [&](const auto& param) { return format_param(param, level + 2); });

    return format_block("IRLet",
                        {std::format("qualifier: {}", format_qualifier(let.qualifier)),
                         std::format("name: {}", quote(let.name)), std::format("symbol_key: {}", quote(let.symbol_key)),
                         std::format("generic_params: {}", format_string_list(let.generic_params, level + 1)),
                         std::format("params: {}", format_list(params, level + 1)),
                         std::format("return_type: {}", format_simple_type(let.return_type, level + 1)),
                         std::format("doc_comments: {}", format_string_list(let.doc_comments, level + 1)),
                         std::format("body: {}", format_nullable_expr(let.body, level + 1)),
                         std::format("is_value_binding: {}", let.is_value_binding),
                         std::format("is_builtin: {}", let.is_builtin)},
                        level);
  }

  [[nodiscard]] auto format_block_expr_statement(const ir::IRBlockExprStatement& stmt, const int level) const -> std::string {
    return format_block("IRBlockExprStatement", {std::format("expr: {}", format_nullable_expr(stmt.expr, level + 1))},
                        level);
  }

  [[nodiscard]] auto format_local_let(const ir::IRLocalLet& let, const int level) const -> std::string {
    return format_block("IRLocalLet",
                        {std::format("name: {}", quote(let.name)),
                         std::format("type: {}", format_simple_type(let.type, level + 1)),
                         std::format("expr: {}", format_nullable_expr(let.expr, level + 1))},
                        level);
  }

  [[nodiscard]] auto format_block_item(const ir::IRBlockItem& item, const int level) const -> std::string {
    return utility::structured_visit<std::string>(
        common::overloaded{[&](const std::string& name, const ir::IRSimpleType& type, const ir::IRExprBox& expr,
                               const std::optional<diag::SourceSpan>&) -> std::string {
                             return format_local_let(ir::IRLocalLet{.name = name, .type = type, .expr = expr}, level);
                           },
                           [&](const ir::IRExprBox& expr, const std::optional<diag::SourceSpan>&) -> std::string {
                             return format_block_expr_statement(ir::IRBlockExprStatement{.expr = expr}, level);
                           }},
        item);
  }

  [[nodiscard]] auto format_block_expr(const ir::IRBlockExpr& block, const int level) const -> std::string {
    const auto items = collect_formatted_items(block.items, [&](const auto& item) { return format_block_item(item, level + 2); });
    return format_block("IRBlockExpr",
                        {std::format("items: {}", format_list(items, level + 1)),
                         std::format("result: {}", format_nullable_expr(block.result, level + 1))},
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

  [[nodiscard]] static auto format_function_signature(const ir::IRSimpleType::FunctionSignature& signature, const int level)
      -> std::string {
    const auto function_params = collect_formatted_items(
        signature.param_types, [&](const auto& param_type) { return format_simple_type(param_type, level + 2); });
    return format_block("FunctionSignature",
                        {std::format("param_types: {}", format_list(function_params, level + 1)),
                         std::format("return_type: {}",
                                     signature.return_type ? format_simple_type(*signature.return_type, level + 1)
                                                           : std::string{"null"})},
                        level);
  }

  [[nodiscard]] static auto format_simple_type(const ir::IRSimpleType& type, const int level) -> std::string {
    const auto alternative_types = collect_formatted_items(
        type.alternative_types, [&](const auto& alt) { return format_simple_type(alt, level + 2); });
    const auto tuple_items =
        collect_formatted_items(type.tuple_items, [&](const auto& item) { return format_simple_type(item, level + 2); });
    const auto type_args =
        collect_formatted_items(type.type_args, [&](const auto& arg) { return format_simple_type(arg, level + 2); });

    std::vector fields{
        std::format("name: {}", quote(type.name)),
        std::format("variadic: {}", type.variadic),
        std::format("alternatives: {}", format_string_list(type.alternatives, level + 1)),
        std::format("alternative_types: {}", format_list(alternative_types, level + 1)),
        std::format("tuple_items: {}", format_list(tuple_items, level + 1)),
        std::format("type_args: {}", format_list(type_args, level + 1)),
    };

    if (type.function_sig.has_value()) {
      fields.push_back(std::format("function_sig: {}", format_function_signature(*type.function_sig, level + 1)));
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
      const std::variant<ir::IRFlowExpr, ir::IRTupleExpr, ir::IRConstant, ir::IRNameRef, ir::IRClosureExprBox,
                         ir::IRBlockExprBox>& node)
      -> std::string_view {
    return std::visit(
        common::overloaded{[](const ir::IRFlowExpr&) -> std::string_view { return "IRFlowExpr"; },
                           [](const ir::IRTupleExpr&) -> std::string_view { return "IRTupleExpr"; },
                           [](const ir::IRConstant&) -> std::string_view { return "IRConstant"; },
                           [](const ir::IRNameRef&) -> std::string_view { return "IRNameRef"; },
                           [](const ir::IRClosureExprBox&) -> std::string_view { return "IRClosureExpr"; },
                           [](const ir::IRBlockExprBox&) -> std::string_view { return "IRBlockExpr"; }},
        node);
  }

  [[nodiscard]] auto format_expr_node(
      const std::variant<ir::IRFlowExpr, ir::IRTupleExpr, ir::IRConstant, ir::IRNameRef, ir::IRClosureExprBox,
                         ir::IRBlockExprBox>& node,
      const int level) const -> std::string {
    return std::visit(
        common::overloaded{
            [&](const ir::IRFlowExpr& flow) -> std::string { return format_flow_expr(flow, level); },
            [&](const ir::IRTupleExpr& tuple) -> std::string { return format_tuple_expr(tuple, level); },
            [&](const ir::IRConstant& constant) -> std::string { return format_constant(constant, level); },
            [&](const ir::IRNameRef& name_ref) -> std::string { return format_name_ref(name_ref, level); },
            [&](const ir::IRClosureExprBox& closure) -> std::string {
              return closure ? format_closure_expr(*closure, level) : std::string{"null"};
            },
            [&](const ir::IRBlockExprBox& block) -> std::string {
              return block ? format_block_expr(*block, level) : std::string{"null"};
            }},
        node);
  }

  [[nodiscard]] auto format_flow_expr(const ir::IRFlowExpr& flow, const int level) const -> std::string {
    return format_block("IRFlowExpr",
                        {std::format("lhs: {}", format_nullable_expr(flow.lhs, level + 1)),
                         std::format("rhs: {}", format_call_target(flow.rhs, level + 1))},
                        level);
  }

  [[nodiscard]] auto format_tuple_expr(const ir::IRTupleExpr& tuple, const int level) const -> std::string {
    const auto items = collect_formatted_items(tuple.items, [&](const auto& item) { return format_nullable_expr(item, level + 2); });
    return format_block("IRTupleExpr", {std::format("items: {}", format_list(items, level + 1))}, level);
  }

  [[nodiscard]] auto format_closure_expr(const ir::IRClosureExpr& closure, const int level) const -> std::string {
    const auto params =
        collect_formatted_items(closure.params, [&](const auto& param) { return format_param(param, level + 2); });

    return format_block(
        "IRClosureExpr",
        {std::format("generic_params: {}", format_string_list(closure.generic_params, level + 1)),
         std::format("params: {}", format_list(params, level + 1)),
         std::format("return_type: {}", format_simple_type(closure.return_type, level + 1)),
         std::format("body: {}", format_nullable_expr(closure.body, level + 1)),
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
    const auto type_args = collect_formatted_items(
        name_ref.explicit_type_args, [&](const auto& type_arg) { return format_simple_type(type_arg, level + 2); });

    return format_block(
        "IRNameRef",
        {std::format("qualifier: {}", format_qualifier(name_ref.qualifier)),
         std::format("name: {}", quote(name_ref.name)),
         std::format("explicit_type_args: {}", format_list(type_args, level + 1)),
         std::format("resolved_symbol_key: {}",
                     name_ref.resolved_symbol_key ? quote(*name_ref.resolved_symbol_key) : std::string{"null"}),
         std::format("materialize_as_value: {}", name_ref.materialize_as_value)},
        level);
  }

  [[nodiscard]] static auto format_operator_ref(const ir::IROperatorRef& op_ref, const int level) -> std::string {
    return format_block("IROperatorRef", {std::format("op: {}", quote(op_ref.op))}, level);
  }

  [[nodiscard]] static auto format_constant(const ir::IRConstant& constant, const int level) -> std::string {
    const auto format_constant_value = [](const auto& value) -> std::string {
      return common::overloaded{[](const std::int64_t int_value) -> std::string { return std::format("{}", int_value); },
                                [](const std::uint64_t uint_value) -> std::string {
                                  return std::format("{}u64", uint_value);
                                },
                                [](const double double_value) -> std::string {
                                  return std::format("{}", double_value);
                                },
                                [](const bool bool_value) -> std::string { return bool_value ? "true" : "false"; },
                                [](const std::string& string_value) -> std::string { return quote(string_value); },
                                [](const std::monostate&) -> std::string { return std::string{"null"}; }}(value);
    };

    return format_block(
        "IRConstant",
        {std::format("value: {}", std::visit(format_constant_value, constant.val))},
        level);
  }
};

auto lower_import_statement(ir::IRProgram& ir_program, const model::ImportStatement& model_import)
    -> tl::expected<void, LoweringError> {
  ir_program.imports.push_back(ir::IRImport{
      .module_name = model_import.module_name,
      .span = model_import.span,
  });
  return {};
}

auto lower_type_statement(ir::IRProgram& ir_program, const model::TypeStatement& model_type)
    -> tl::expected<void, LoweringError> {
  ir_program.type_decls.push_back(ir::IRTypeDecl{
      .name = model_type.name,
      .target = lower_simple_type(model_type.target),
      .span = model_type.span,
  });
  return {};
}

auto lower_alias_statement(ir::IRProgram& ir_program, const model::AliasStatement& model_alias)
    -> tl::expected<void, LoweringError> {
  ir_program.alias_decls.push_back(ir::IRAliasDecl{
      .name = model_alias.name,
      .target = lower_simple_type(model_alias.target),
      .span = model_alias.span,
  });
  return {};
}

auto lower_let_params(const model::LetStatement& model_let) -> std::vector<ir::IRParam> {
  std::vector<ir::IRParam> params;
  for (const auto& [param_name, type, span] : model_let.params.params) {
    params.push_back(ir::IRParam{
        .name = param_name,
        .type = lower_simple_type(type),
        .span = span,
    });
  }
  return params;
}

auto validate_variadic_params(const std::vector<ir::IRParam>& params) -> tl::expected<void, LoweringError> {
  for (std::size_t idx = 0; idx < params.size(); ++idx) {
    if (params[idx].type.variadic && idx + 1U != params.size()) {
      return tl::unexpected(
          LoweringError{.message = "Variadic parameter must be the final parameter in a function declaration.",
                        .hint = "Move the '...' parameter to the end of the parameter list.",
                        .span = params[idx].span});
    }
  }
  return {};
}

auto make_symbol_key(const std::string& public_symbol, const bool is_builtin,
                     std::unordered_map<std::string, std::size_t>& user_symbol_ordinals) -> std::string {
  if (is_builtin) {
    return public_symbol;
  }

  std::string symbol_key = public_symbol;
  const auto ordinal = user_symbol_ordinals[public_symbol]++;
  symbol_key += "#" + std::to_string(ordinal);
  return symbol_key;
}

auto make_bound_name_set(const std::vector<ir::IRParam>& params) -> std::unordered_set<std::string> {
  std::unordered_set<std::string> let_bound_names;
  for (const auto& p : params) {
    let_bound_names.insert(p.name);
  }
  return let_bound_names;
}

auto lower_let_body_if_needed(const model::LetStatement& model_let, const bool is_builtin,
                              const LoweringContext& context)
    -> tl::expected<ir::IRExprBox, LoweringError> {
  if (is_builtin) {
    return ir::IRExprBox{};
  }

  auto lowered_body = lower_expr(*model_let.expr, context);
  if (!lowered_body) {
    return tl::unexpected(lowered_body.error());
  }
  return make_ir_expr_box(std::move(*lowered_body));
}

auto lower_let_statement(ir::IRProgram& ir_program, std::unordered_map<std::string, std::size_t>& user_symbol_ordinals,
                         const model::LetStatement& model_let, const LoweringContext& context)
    -> tl::expected<void, LoweringError> {
  auto [qualifier, name] = split_id(model_let.id);
  const std::string public_symbol = common::full_symbol_name(qualifier, name);

  auto params = lower_let_params(model_let);
  if (auto valid_params = validate_variadic_params(params); !valid_params) {
    return tl::unexpected(valid_params.error());
  }

  const bool is_builtin = model_let.is_builtin;
  std::string symbol_key = make_symbol_key(public_symbol, is_builtin, user_symbol_ordinals);

  LoweringContext let_context = context;
  let_context.bound_names = make_bound_name_set(params);

  auto body = lower_let_body_if_needed(model_let, is_builtin, let_context);
  if (!body) {
    return tl::unexpected(body.error());
  }

  ir_program.lets.push_back(ir::IRLet{
      .qualifier = qualifier,
      .name = name,
      .symbol_key = std::move(symbol_key),
      .generic_params = model_let.generic_params,
      .params = std::move(params),
      .return_type = lower_simple_type(model_let.rtype),
      .doc_comments = model_let.doc_comments,
      .body = std::move(*body),
      .is_value_binding = model_let.is_value_binding,
      .is_builtin = is_builtin,
      .span = model_let.span,
  });
  return {};
}

auto lower_expression_statement(ir::IRProgram& ir_program, const model::ExpressionStatement& model_expr_stmt,
                                const LoweringContext& context)
    -> tl::expected<void, LoweringError> {
  auto lowered_expr = lower_expr(model_expr_stmt.expr, context);
  if (!lowered_expr) {
    return tl::unexpected(lowered_expr.error());
  }

  ir_program.expressions.push_back(ir::IRExprStatement{
      .expr = lowered_expr.value(),
      .span = model_expr_stmt.span,
  });
  return {};
}

auto lower_statement(ir::IRProgram& ir_program, std::unordered_map<std::string, std::size_t>& user_symbol_ordinals,
                     const model::Statement& stmt, const LoweringContext& context) -> tl::expected<void, LoweringError> {
  return std::visit(
      common::overloaded{[&](const model::ImportStatement& model_import) -> tl::expected<void, LoweringError> {
                           return lower_import_statement(ir_program, model_import);
                         },
                         [&](const model::TypeStatement& model_type) -> tl::expected<void, LoweringError> {
                           return lower_type_statement(ir_program, model_type);
                         },
                         [&](const model::AliasStatement& model_alias) -> tl::expected<void, LoweringError> {
                           return lower_alias_statement(ir_program, model_alias);
                         },
                         [&](const model::LetStatement& model_let) -> tl::expected<void, LoweringError> {
                            return lower_let_statement(ir_program, user_symbol_ordinals, model_let, context);
                         },
                         [&](const model::ExpressionStatement& model_expr_stmt) -> tl::expected<void, LoweringError> {
                            return lower_expression_statement(ir_program, model_expr_stmt, context);
                         }},
      stmt);
}

auto collect_top_level_value_symbols(const model::Program& program) -> std::unordered_set<std::string> {
  std::unordered_set<std::string> symbols;
  for (const auto& stmt : program.statements) {
    const auto* let_stmt = std::get_if<model::LetStatement>(&stmt);
    if (let_stmt == nullptr || !let_stmt->is_value_binding) {
      continue;
    }
    const auto [qualifier, name] = split_id(let_stmt->id);
    symbols.insert(common::full_symbol_name(qualifier, name));
  }
  return symbols;
}

void assign_builtin_overload_symbol_keys(ir::IRProgram& ir_program) {
  std::unordered_map<std::string, std::vector<std::size_t>> builtin_overload_slots;
  for (std::size_t let_index = 0; let_index < ir_program.lets.size(); ++let_index) {
    const auto& let = ir_program.lets[let_index];
    if (!let.is_builtin) {
      continue;
    }
    const std::string public_symbol = common::full_symbol_name(let.qualifier, let.name);
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
}
}  // namespace

auto Lowerer::lower_only(const model::Program& program) const -> LoweringResult {
  ir::IRProgram ir_program;
  ir_program.span = program.span;
  std::unordered_map<std::string, std::size_t> user_symbol_ordinals;
  LoweringContext top_level_context;
  top_level_context.all_top_level_value_symbols = collect_top_level_value_symbols(program);

  for (const auto& stmt : program.statements) {
    if (auto lowered_stmt = lower_statement(ir_program, user_symbol_ordinals, stmt, top_level_context); !lowered_stmt) {
      return tl::unexpected(lowered_stmt.error());
    }

    if (const auto* let_stmt = std::get_if<model::LetStatement>(&stmt);
        let_stmt != nullptr && let_stmt->is_value_binding) {
      const auto [qualifier, name] = split_id(let_stmt->id);
      top_level_context.visible_top_level_value_symbols.insert(common::full_symbol_name(qualifier, name));
    }
  }

  assign_builtin_overload_symbol_keys(ir_program);

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
