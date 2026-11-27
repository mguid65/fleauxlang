#include "fleaux/frontend/lowering.hpp"
#include "fleaux/frontend/analysis.hpp"
#include "fleaux/frontend/source_loader.hpp"
#include "fleaux/frontend/type_check.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>

#include "fleaux/common/match_wildcard.hpp"
#include "fleaux/common/overloaded.hpp"
#include "fleaux/common/symbol_name.hpp"

namespace fleaux::frontend::lowering {
namespace {
const std::unordered_set<std::string> kOperators = {
    "^", "/", "*", "%", "+", "-", "==", "!=", "<", ">", ">=", "<=", "!", "&&", "||",
};

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
  ir::IRSimpleType out;
  out.span = type.span;
  out.variadic = type.variadic;
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
                                  // A union of unions is one union of all their alternatives.
                                  for (const auto& alt : union_list->alternatives) {
                                    ir::IRSimpleType lowered = lower_simple_type(*alt);
                                    if (lowered.alternative_types.empty()) {
                                      out.alternative_types.push_back(std::move(lowered));
                                    } else {
                                      std::ranges::move(lowered.alternative_types, std::back_inserter(out.alternative_types));
                                    }
                                  }
                                  if (!out.alternative_types.empty()) {
                                    out.name = out.alternative_types.front().name;
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

auto lower_generic_bounds(const std::vector<model::TypeBox>& bounds) -> std::vector<ir::IRSimpleTypeBox> {
  std::vector<ir::IRSimpleTypeBox> lowered;
  lowered.reserve(bounds.size());
  for (const auto& bound : bounds) {
    if (bound.has_value()) {
      lowered.emplace_back(lower_simple_type(*bound));
    } else {
      lowered.emplace_back(std::nullopt);
    }
  }
  return lowered;
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
        wildcard.val = std::string{common::kMatchWildcardSentinel};
        wildcard.span = pattern_name->span;
        pattern_expr->node = std::move(wildcard);
      }
    }
  }

  return {};
}

// Collects the free unqualified names an expression uses, in first-use order. `bound` holds the names the
// expression's own scopes bind (block lets in scope, parameters of nested closures); those are not free.
void collect_free_names_from_expr(const model::Expression& expr, const std::unordered_set<std::string>& bound,
                                  std::vector<std::string>& out, std::unordered_set<std::string>& seen);

void collect_free_names_from_atom(const model::Atom& atom, const std::unordered_set<std::string>& bound,
                                  std::vector<std::string>& out, std::unordered_set<std::string>& seen) {
  const auto use = [&](const std::string& name) -> void {
    if (!kOperators.contains(name) && !bound.contains(name) && seen.insert(name).second) {
      out.push_back(name);
    }
  };
  std::visit(common::overloaded{
                 [](const std::monostate&) -> void {},
                 [](const model::Constant&) -> void {},
                 [](const model::QualifiedId&) -> void {},
                 [&](const model::NamedTargetBox& value) -> void {
                   if (const auto* simple = std::get_if<std::string>(&value->target); simple != nullptr) {
                     use(*simple);
                   }
                 },
                 [&](const std::string& value) -> void { use(value); },
                 [&](const model::DelimitedExpressionBox& value) -> void {
                   for (const auto& item : value->items) {
                     collect_free_names_from_expr(*item, bound, out, seen);
                   }
                 },
                 [&](const model::BlockExpressionBox& value) -> void {
                   // A local let is in scope after its declaration, so its own initializer sees the outer name.
                   std::unordered_set<std::string> block_bound = bound;
                   for (const auto& item : value->items) {
                     std::visit(common::overloaded{
                                    [&](const model::LocalLetBinding& let) -> void {
                                      if (let.expr.has_value()) {
                                        collect_free_names_from_expr(*let.expr, block_bound, out, seen);
                                      }
                                      block_bound.insert(let.name);
                                    },
                                    [&](const model::ExpressionStatement& statement) -> void {
                                      collect_free_names_from_expr(statement.expr, block_bound, out, seen);
                                    },
                                },
                                item);
                   }
                   if (value->result.has_value()) {
                     collect_free_names_from_expr(*value->result, block_bound, out, seen);
                   }
                 },
                 [&](const model::ClosureExpressionBox& value) -> void {
                   std::unordered_set<std::string> closure_bound = bound;
                   for (const auto& param : value->params.params) {
                     closure_bound.insert(param.param_name);
                   }
                   collect_free_names_from_expr(*value->body, closure_bound, out, seen);
                 },
             },
             atom.value);
}

void collect_free_names_from_expr(const model::Expression& expr, const std::unordered_set<std::string>& bound,
                                  std::vector<std::string>& out, std::unordered_set<std::string>& seen) {
  collect_free_names_from_atom(expr.expr.lhs.base, bound, out, seen);
  for (const auto& stage : expr.expr.rhs) {
    collect_free_names_from_atom(stage.base, bound, out, seen);
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
                           out_tuple.trailing_comma = tuple_expr.trailing_comma;
                           out_tuple.spread_items = tuple_expr.spread_items;
                           for (const auto& item : tuple_expr.items) {
                              out_tuple.items.push_back(make_ir_expr_box(replace_placeholder_impl(*item, replacement)));
                           }

                           return ir::IRExpr{.node = std::move(out_tuple), .span = expr.span};
                         },
                         [&](const ir::IRFlowExpr& flow_expr) -> ir::IRExpr {
                           ir::IRFlowExpr out_flow{
                                .lhs = make_ir_expr_box(replace_placeholder_impl(*flow_expr.lhs, replacement)),
                               .rhs = flow_expr.rhs,
                               .call_shape = flow_expr.call_shape,
                               .span = flow_expr.span,
                               .calls_function_value = flow_expr.calls_function_value,
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

// `parameter_list` is true when the parentheses are a parameter list: the left side of '->' or a tuple template
// stage. Only there may an item be spread with `x...`.
auto lower_delimited_atom(const model::DelimitedExpressionBox& inner, const LoweringContext& context,
                          const std::optional<diag::SourceSpan>& expr_span, const bool parameter_list)
    -> tl::expected<ir::IRExpr, LoweringError> {
  const bool has_spread = std::ranges::find(inner->spread_items, true) != inner->spread_items.end();
  if (has_spread && (!parameter_list || inner->trailing_comma)) {
    return tl::unexpected(LoweringError{
        .message = "Invalid spread.",
        .hint = inner->trailing_comma
                    ? "A trailing comma makes the parentheses one tuple value, which cannot spread items. Remove the "
                      "trailing comma to write a parameter list, such as '(pair...) -> F'."
                    : "'x...' spreads a tuple into a parameter list, so it may only appear directly inside the "
                      "parentheses on the left of '->', such as '(pair...) -> F'.",
        .span = inner->span});
  }
  ir::IRTupleExpr tuple;
  tuple.span = inner->span;
  tuple.trailing_comma = inner->trailing_comma;
  if (has_spread) {
    tuple.spread_items = inner->spread_items;
  }
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
  std::vector<std::string> free_names;
  std::unordered_set<std::string> seen_names;
  collect_free_names_from_expr(body, param_names, free_names, seen_names);

  std::vector<std::string> captures;
  for (const auto& candidate : free_names) {
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
      .generic_bounds = lower_generic_bounds(closure->generic_bounds),
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

auto lower_atom(const model::Atom& atom, const LoweringContext& context, const bool parameter_list)
    -> tl::expected<ir::IRExpr, LoweringError> {
  return std::visit(
      common::overloaded{
          [&](const model::DelimitedExpressionBox& inner) -> tl::expected<ir::IRExpr, LoweringError> {
            return lower_delimited_atom(inner, context, atom.span, parameter_list);
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

auto lower_primary(const model::Primary& primary, const LoweringContext& context, const bool parameter_list)
    -> tl::expected<ir::IRExpr, LoweringError> {
  return lower_atom(primary.base, context, parameter_list);
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

  // The stage's left side stays as written: the type checker takes the closure's arguments from it by the
  // parameter-list rule, as for any call.
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
      .calls_function_value = true,
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
  // Parentheses directly left of '->' are a parameter list.
  auto result = lower_primary(flow.lhs, context, !flow.rhs.empty());
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

    // A tuple template stage such as '(_, 2)' is the parameter list of the target after it.
    auto stage_expr = lower_primary(flow.rhs[stage_index], context, true);
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

    // A template followed by a closure is the closure's parameter list, as for any call target.
    if (const auto& next_primary = flow.rhs[stage_index + 1];
        std::holds_alternative<model::ClosureExpressionBox>(next_primary.base.value)) {
      if (std::get_if<ir::IRTupleExpr>(&stage_expr->node) == nullptr) {
        return tl::unexpected(
            LoweringError{.message = "Invalid pipeline stage shape: non-call stages must be tuple templates.",
                          .hint = "Use a call target like '-> Std.Add' or a tuple template like '-> (_, 2) -> Std.Add'.",
                          .span = flow.rhs[stage_index].span});
      }
      auto arguments = replace_placeholder(*stage_expr, *result);
      if (!arguments) {
        return tl::unexpected(arguments.error());
      }
      auto closure = lower_primary(next_primary, context, false);
      if (!closure) {
        return tl::unexpected(closure.error());
      }
      result = std::move(*try_apply_closure_stage(*arguments, next_primary, *closure));
      stage_index += 2;
      continue;
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
      .generic_params = model_type.generic_params,
      .is_builtin = model_type.is_builtin,
      .target = model_type.is_builtin ? ir::IRSimpleType{} : lower_simple_type(model_type.target),
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
  if (is_builtin || model_let.is_native) {
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
      .generic_bounds = lower_generic_bounds(model_let.generic_bounds),
      .params = std::move(params),
      .return_type = lower_simple_type(model_let.rtype),
      .doc_comments = model_let.doc_comments,
      .body = std::move(*body),
      .is_value_binding = model_let.is_value_binding,
      .is_builtin = is_builtin,
      .is_native = model_let.is_native,
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

auto lower_type(const model::TypeNode& type) -> ir::IRSimpleType { return lower_simple_type(type); }

namespace {

auto format_type_text_impl(const ir::IRSimpleType& type, const bool include_variadic) -> std::string {
  const auto join = [](const std::vector<ir::IRSimpleType>& items) -> std::string {
    std::string out;
    for (std::size_t i = 0; i < items.size(); ++i) {
      if (i > 0U) {
        out += ", ";
      }
      out += format_type_text_impl(items[i], true);
    }
    return out;
  };

  std::string out;
  if (!type.alternative_types.empty()) {
    for (std::size_t i = 0; i < type.alternative_types.size(); ++i) {
      if (i > 0U) {
        out += " | ";
      }
      out += format_type_text_impl(type.alternative_types[i], true);
    }
  } else if (type.function_sig.has_value()) {
    out = "(" + join(type.function_sig->param_types) + ") => ";
    out += type.function_sig->return_type.has_value() ? format_type_text_impl(*type.function_sig->return_type, true)
                                                      : std::string{"Any"};
  } else if (type.name == "Tuple") {
    out = "Tuple<" + join(type.tuple_items) + ">";
  } else if (!type.type_args.empty()) {
    out = type.name + "<" + join(type.type_args) + ">";
  } else {
    out = type.name;
  }

  if (include_variadic && type.variadic) {
    out += "...";
  }
  return out;
}

}  // namespace

auto format_type_text(const ir::IRSimpleType& type) -> std::string { return format_type_text_impl(type, false); }

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
