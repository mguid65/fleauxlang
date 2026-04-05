#include "fleaux/frontend/lowering.hpp"

#include <optional>
#include <string>
#include <unordered_set>
#include <variant>

namespace fleaux::frontend::lowering {
namespace {

const std::unordered_set<std::string> kOperators = {
    "^",  "/",  "*",  "%",  "+",  "-",  "==", "!=",
    "<",  ">",  ">=", "<=", "!",  "&&", "||",
};

LoweringError make_error(const std::string& message,
                         const std::optional<std::string>& hint,
                         const std::optional<diag::SourceSpan>& span) {
  LoweringError err;
  err.message = message;
  err.hint = hint;
  err.span = span;
  return err;
}

std::pair<std::optional<std::string>, std::string> split_id(
    const std::variant<std::string, model::QualifiedId>& id) {
  if (std::holds_alternative<std::string>(id)) {
    return {std::nullopt, std::get<std::string>(id)};
  }
  const auto& qid = std::get<model::QualifiedId>(id);
  return {qid.qualifier.qualifier, qid.id};
}

ir::IRSimpleType lower_simple_type(const model::TypeRef& type) {
  ir::IRSimpleType out;
  if (!type) {
    out.name = "Any";
    return out;
  }

  out.span = type->span;
  if (std::holds_alternative<std::string>(type->value)) {
    std::string raw = std::get<std::string>(type->value);
    if (raw.size() >= 3U && raw.substr(raw.size() - 3U) == "...") {
      out.variadic = true;
      raw = raw.substr(0, raw.size() - 3U);
    }
    out.name = raw;
    return out;
  }

  if (std::holds_alternative<model::QualifiedId>(type->value)) {
    const auto& qid = std::get<model::QualifiedId>(type->value);
    out.name = qid.qualifier.qualifier + "." + qid.id;
    return out;
  }

  out.name = "Tuple";
  return out;
}

tl::expected<ir::IRExprPtr, LoweringError> lower_expr(const model::ExpressionPtr& expr);

tl::expected<ir::IRCallTarget, LoweringError> extract_call_target_from_primary(
    const model::Primary& primary) {
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
    if (kOperators.contains(value)) {
      return ir::IROperatorRef{.op = value, .span = primary.span};
    }
    return ir::IRNameRef{.qualifier = std::nullopt, .name = value, .span = primary.span};
  }

  return tl::unexpected(make_error(
      "Primary used as flow target must be a simple name, qualified name, or operator.",
      "Valid targets: 'Std.Add', 'MyFunc', '+', '/', '&&'.",
      primary.span));
}

bool contains_placeholder(const ir::IRExprPtr& expr);

ir::IRExprPtr replace_placeholder_impl(const ir::IRExprPtr& expr,
                                       const ir::IRExprPtr& replacement) {
  if (!expr) {
    return expr;
  }

  if (std::holds_alternative<ir::IRNameRef>(expr->node)) {
    const auto& name = std::get<ir::IRNameRef>(expr->node);
    if (!name.qualifier.has_value() && name.name == "_") {
      return replacement;
    }
    return expr;
  }

  if (std::holds_alternative<ir::IRTupleExpr>(expr->node)) {
    const auto& tuple = std::get<ir::IRTupleExpr>(expr->node);
    ir::IRTupleExpr out_tuple;
    out_tuple.span = tuple.span;
    for (const auto& item : tuple.items) {
      out_tuple.items.push_back(replace_placeholder_impl(item, replacement));
    }

    auto out = std::make_shared<ir::IRExpr>();
    out->node = std::move(out_tuple);
    out->span = expr->span;
    return out;
  }

  if (std::holds_alternative<ir::IRFlowExpr>(expr->node)) {
    const auto& flow = std::get<ir::IRFlowExpr>(expr->node);
    ir::IRFlowExpr out_flow;
    out_flow.lhs = replace_placeholder_impl(flow.lhs, replacement);
    out_flow.rhs = flow.rhs;
    out_flow.span = flow.span;

    auto out = std::make_shared<ir::IRExpr>();
    out->node = std::move(out_flow);
    out->span = expr->span;
    return out;
  }

  return expr;
}

bool contains_placeholder(const ir::IRExprPtr& expr) {
  if (!expr) {
    return false;
  }
  if (std::holds_alternative<ir::IRNameRef>(expr->node)) {
    const auto& name = std::get<ir::IRNameRef>(expr->node);
    return !name.qualifier.has_value() && name.name == "_";
  }
  if (std::holds_alternative<ir::IRTupleExpr>(expr->node)) {
    const auto& tuple = std::get<ir::IRTupleExpr>(expr->node);
    for (const auto& item : tuple.items) {
      if (contains_placeholder(item)) {
        return true;
      }
    }
    return false;
  }
  if (std::holds_alternative<ir::IRFlowExpr>(expr->node)) {
    return contains_placeholder(std::get<ir::IRFlowExpr>(expr->node).lhs);
  }
  return false;
}

tl::expected<ir::IRExprPtr, LoweringError> replace_placeholder(const ir::IRExprPtr& template_expr,
                                                               const ir::IRExprPtr& current_value) {
  const auto replaced = replace_placeholder_impl(template_expr, current_value);
  if (contains_placeholder(replaced)) {
    return tl::unexpected(make_error(
        "Unresolved '_' placeholder remained in tuple template.",
        "Use '_' only as an argument position inside tuple templates like '(_, 2)'.",
        template_expr ? template_expr->span : std::nullopt));
  }
  return replaced;
}

tl::expected<ir::IRExprPtr, LoweringError> lower_atom(const model::Atom& atom) {
  auto out = std::make_shared<ir::IRExpr>();
  out->span = atom.span;

  if (atom.inner) {
    ir::IRTupleExpr tuple;
    tuple.span = atom.inner->span;
    for (const auto& item : atom.inner->items) {
      auto lowered_item = lower_expr(item);
      if (!lowered_item) {
        return tl::unexpected(lowered_item.error());
      }
      tuple.items.push_back(lowered_item.value());
    }
    out->node = std::move(tuple);
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

tl::expected<ir::IRExprPtr, LoweringError> lower_primary(const model::Primary& primary) {
  return lower_atom(primary.base);
}

tl::expected<ir::IRExprPtr, LoweringError> lower_flow(const model::FlowExpression& flow) {
  auto result = lower_primary(flow.lhs);
  if (!result) {
    return tl::unexpected(result.error());
  }

  std::size_t i = 0;
  while (i < flow.rhs.size()) {
    auto maybe_target = extract_call_target_from_primary(flow.rhs[i]);
    if (maybe_target) {
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

    auto template_expr = lower_primary(flow.rhs[i]);
    if (!template_expr) {
      return tl::unexpected(template_expr.error());
    }

    if (!std::holds_alternative<ir::IRTupleExpr>(template_expr.value()->node)) {
      return tl::unexpected(make_error(
          "Invalid pipeline stage shape: non-call stages must be tuple templates.",
          "Use a call target like '-> Std.Add' or a tuple template like '-> (_, 2) -> Std.Add'.",
          flow.rhs[i].span));
    }

    if (i + 1 >= flow.rhs.size()) {
      return tl::unexpected(make_error(
          "Tuple template stage is missing a following call target.",
          "Append a call target, e.g. '-> (_, 2) -> Std.Divide'.",
          flow.rhs[i].span));
    }

    auto next_target = extract_call_target_from_primary(flow.rhs[i + 1]);
    if (!next_target) {
      return tl::unexpected(next_target.error());
    }

    auto replaced = replace_placeholder(template_expr.value(), result.value());
    if (!replaced) {
      return tl::unexpected(replaced.error());
    }

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

tl::expected<ir::IRExprPtr, LoweringError> lower_expr(const model::ExpressionPtr& expr) {
  if (!expr) {
    return tl::unexpected(
        make_error("Cannot lower null expression.", "Provide a valid expression node.", std::nullopt));
  }
  return lower_flow(expr->expr);
}

}  // namespace

LoweringResult Lowerer::lower(const model::Program& program) const {
  ir::IRProgram ir_program;
  ir_program.span = program.span;

  for (const auto& stmt : program.statements) {
    if (std::holds_alternative<model::ImportStatement>(stmt)) {
      const auto& model_import = std::get<model::ImportStatement>(stmt);
      ir_program.imports.push_back(ir::IRImport{
          .module_name = model_import.module_name,
          .span = model_import.span,
      });
      continue;
    }

    if (std::holds_alternative<model::LetStatement>(stmt)) {
      const auto& model_let = std::get<model::LetStatement>(stmt);
      auto [qualifier, name] = split_id(model_let.id);

      std::vector<ir::IRParam> params;
      for (const auto& p : model_let.params.params) {
        params.push_back(ir::IRParam{
            .name = p.param_name,
            .type = lower_simple_type(p.type),
            .span = p.span,
        });
      }

      const bool is_builtin = std::holds_alternative<std::string>(model_let.expr) &&
                              std::get<std::string>(model_let.expr) == "__builtin__";

      ir::IRExprPtr body;
      if (!is_builtin) {
        auto lowered_body = lower_expr(std::get<model::ExpressionPtr>(model_let.expr));
        if (!lowered_body) {
          return tl::unexpected(lowered_body.error());
        }
        body = lowered_body.value();
      }

      ir_program.lets.push_back(ir::IRLet{
          .qualifier = qualifier,
          .name = name,
          .params = std::move(params),
          .return_type = lower_simple_type(model_let.rtype),
          .body = body,
          .is_builtin = is_builtin,
          .span = model_let.span,
      });
      continue;
    }

    const auto& model_expr_stmt = std::get<model::ExpressionStatement>(stmt);
    auto lowered_expr = lower_expr(model_expr_stmt.expr);
    if (!lowered_expr) {
      return tl::unexpected(lowered_expr.error());
    }

    ir_program.expressions.push_back(ir::IRExprStatement{
        .expr = lowered_expr.value(),
        .span = model_expr_stmt.span,
    });
  }

  return ir_program;
}

}  // namespace fleaux::frontend::lowering

