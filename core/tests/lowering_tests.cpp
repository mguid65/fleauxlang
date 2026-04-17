#include <string>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/frontend/ast.hpp"
#include "fleaux/frontend/lowering.hpp"
#include "fleaux/frontend/parser.hpp"

TEST_CASE("Lowerer maps parser program into import let and expression buckets", "[lowering]") {
  const std::string src =
      "import Std;\n"
      "let Add4(x: Number): Number = (4, x) -> Std.Add;\n"
      "(4) -> Add4 -> Std.Println;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "lowering_shape.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());

  REQUIRE(lowered.has_value());
  REQUIRE(lowered->imports.size() == 1);
  REQUIRE(lowered->lets.size() == 1);
  REQUIRE(lowered->expressions.size() == 1);
  REQUIRE(lowered->lets[0].name == "Add4");
  REQUIRE_FALSE(lowered->lets[0].is_builtin);
  REQUIRE(lowered->lets[0].body.has_value());
}

TEST_CASE("Lowerer marks builtin lets and clears body", "[lowering]") {
  const std::string src = "let Pi(): Number :: __builtin__;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "builtin_let.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());

  REQUIRE(lowered.has_value());
  REQUIRE(lowered->lets.size() == 1);
  REQUIRE(lowered->lets[0].is_builtin);
  REQUIRE_FALSE(lowered->lets[0].body.has_value());
}

TEST_CASE("Lowerer resolves tuple-template placeholder flow stage", "[lowering]") {
  const std::string src =
      "let Sum3(a: Number, b: Number, c: Number): Number =\n"
      "  (a, b) -> Std.Add -> (_, c) -> Std.Add;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "placeholder_flow.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());
  REQUIRE(lowered->lets.size() == 1);

  const auto& body = *lowered->lets[0].body;
  REQUIRE(std::holds_alternative<fleaux::frontend::ir::IRFlowExpr>(body.node));

  const auto& top_flow = std::get<fleaux::frontend::ir::IRFlowExpr>(body.node);
  REQUIRE(std::holds_alternative<fleaux::frontend::ir::IRNameRef>(top_flow.rhs));
  const auto& target = std::get<fleaux::frontend::ir::IRNameRef>(top_flow.rhs);
  REQUIRE(target.qualifier.has_value());
  REQUIRE(target.qualifier.value() == "Std");
  REQUIRE(target.name == "Add");

  REQUIRE(std::holds_alternative<fleaux::frontend::ir::IRTupleExpr>(top_flow.lhs->node));
}

TEST_CASE("Lowerer emits closure IR with captured lexical names", "[lowering]") {
  const std::string src =
      "let MakeAdder(n: Number): Any = (x: Number): Number = (x, n) -> Std.Add;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "closure_lowering.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());
  REQUIRE(lowered->lets.size() == 1);

  const auto& [node, span] = *lowered->lets[0].body;
  REQUIRE(std::holds_alternative<fleaux::frontend::ir::IRClosureExprBox>(node));

  const auto& closure_ptr = std::get<fleaux::frontend::ir::IRClosureExprBox>(node);
  REQUIRE(closure_ptr->params.size() == 1);
  REQUIRE(closure_ptr->params[0].name == "x");
  REQUIRE(closure_ptr->captures.size() == 1);
  REQUIRE(closure_ptr->captures[0] == "n");
}

TEST_CASE("Lowerer desugars closure pipeline stage to Std.Apply", "[lowering]") {
  const std::string src =
      "(10) -> (x: Number): Number = (x, 1) -> Std.Add -> Std.Println;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "closure_pipeline_desugar.fleaux");
  REQUIRE(parsed.has_value());

  constexpr fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());
  REQUIRE(lowered->expressions.size() == 1);

  const auto& [node, span] = lowered->expressions[0].expr;
  REQUIRE(std::holds_alternative<fleaux::frontend::ir::IRFlowExpr>(node));

  const auto& outer = std::get<fleaux::frontend::ir::IRFlowExpr>(node);
  REQUIRE(std::holds_alternative<fleaux::frontend::ir::IRNameRef>(outer.rhs));
  const auto& outer_target = std::get<fleaux::frontend::ir::IRNameRef>(outer.rhs);
  REQUIRE(outer_target.qualifier.has_value());
  REQUIRE(outer_target.qualifier.value() == "Std");
  REQUIRE(outer_target.name == "Println");

  REQUIRE(std::holds_alternative<fleaux::frontend::ir::IRFlowExpr>(outer.lhs->node));
  const auto& apply_flow = std::get<fleaux::frontend::ir::IRFlowExpr>(outer.lhs->node);
  REQUIRE(std::holds_alternative<fleaux::frontend::ir::IRNameRef>(apply_flow.rhs));
  const auto& apply_target = std::get<fleaux::frontend::ir::IRNameRef>(apply_flow.rhs);
  REQUIRE(apply_target.qualifier.has_value());
  REQUIRE(apply_target.qualifier.value() == "Std");
  REQUIRE(apply_target.name == "Apply");

  REQUIRE(std::holds_alternative<fleaux::frontend::ir::IRTupleExpr>(apply_flow.lhs->node));
}

TEST_CASE("Lowerer excludes shadowed outer names from closure captures", "[lowering]") {
  const std::string src =
      "let Shadowed(n: Number): Any = (n: Number): Number = (n, 1) -> Std.Add;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "closure_shadow_capture.fleaux");
  REQUIRE(parsed.has_value());

  constexpr fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());
  REQUIRE(lowered->lets.size() == 1);

  const auto& [node, span] = *lowered->lets[0].body;
  REQUIRE(std::holds_alternative<fleaux::frontend::ir::IRClosureExprBox>(node));

  const auto& closure_ptr = std::get<fleaux::frontend::ir::IRClosureExprBox>(node);
  REQUIRE(closure_ptr->captures.empty());
}

TEST_CASE("Lowerer rejects closure declarations with non-final variadic parameter", "[lowering]") {
  const std::string src =
      "let Bad(): Any = (rest: Any..., x: Number): Number = x;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "closure_bad_variadic_param.fleaux");
  REQUIRE(parsed.has_value());

  constexpr fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());

  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().message.find("Variadic parameter must be the final parameter") != std::string::npos);
}

TEST_CASE("Lowerer rewrites Std.Match wildcard pattern", "[lowering]") {
  const std::string src =
      "(1, (0, (): Any = \"zero\"), (_, (): Any = \"many\")) -> Std.Match;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "match_wildcard_lowering.fleaux");
  REQUIRE(parsed.has_value());

  constexpr fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());
  REQUIRE(lowered->expressions.size() == 1);

  const auto& [node, span] = lowered->expressions[0].expr;
  REQUIRE(std::holds_alternative<fleaux::frontend::ir::IRFlowExpr>(node));

  const auto& flow = std::get<fleaux::frontend::ir::IRFlowExpr>(node);
  REQUIRE(std::holds_alternative<fleaux::frontend::ir::IRNameRef>(flow.rhs));
  const auto& target = std::get<fleaux::frontend::ir::IRNameRef>(flow.rhs);
  REQUIRE(target.qualifier.has_value());
  REQUIRE(target.qualifier.value() == "Std");
  REQUIRE(target.name == "Match");

  REQUIRE(std::holds_alternative<fleaux::frontend::ir::IRTupleExpr>(flow.lhs->node));
  const auto& [items_0, span_0] = std::get<fleaux::frontend::ir::IRTupleExpr>(flow.lhs->node);
  REQUIRE(items_0.size() == 3);

  const auto& wildcard_case_expr = items_0[2];
  REQUIRE(std::holds_alternative<fleaux::frontend::ir::IRTupleExpr>(wildcard_case_expr->node));
  const auto& [items_2, span_2] = std::get<fleaux::frontend::ir::IRTupleExpr>(wildcard_case_expr->node);
  REQUIRE(items_2.size() == 2);
  REQUIRE(std::holds_alternative<fleaux::frontend::ir::IRConstant>(items_2[0]->node));
  const auto& wildcard_pattern = std::get<fleaux::frontend::ir::IRConstant>(items_2[0]->node);
  REQUIRE(std::holds_alternative<std::string>(wildcard_pattern.val));
  REQUIRE(std::get<std::string>(wildcard_pattern.val) == "__fleaux_match_wildcard__");
}

TEST_CASE("Lowerer rejects non-final Std.Match wildcard case", "[lowering]") {
  const std::string src =
      "(1, (_, (): Any = \"many\"), (1, (): Any = \"one\")) -> Std.Match;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "match_wildcard_order_lowering.fleaux");
  REQUIRE(parsed.has_value());

  constexpr fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());

  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().message.find("wildcard '_' must be the final case") != std::string::npos);
}

TEST_CASE("Lowerer rejects too few fixed args for variadic functions", "[lowering]") {
  const std::string src =
      "import Std;\n"
      "let HeadTail(head: Number, rest: Any...): Any = rest;\n"
      "() -> HeadTail;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "variadic_fixed_arity_lowering.fleaux");
  REQUIRE(parsed.has_value());

  constexpr fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());

  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
}

TEST_CASE("Lowerer rejects Std.Apply callable arity mismatch", "[lowering]") {
  const std::string src =
      "let Std.Add(lhs: Number, rhs: Number): Number :: __builtin__;\n"
      "let Std.Apply(value: Any, func: Any): Any :: __builtin__;\n"
      "(10, (a: Number, b: Number): Number = (a, b) -> Std.Add) -> Std.Apply;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "apply_callable_arity_mismatch.fleaux");
  REQUIRE(parsed.has_value());

  constexpr fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());

  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
}

TEST_CASE("Lowerer rejects predicate builtins with non-bool callable results", "[lowering]") {
  const std::string src =
      "let Std.Tuple.Filter(t: Tuple(Any...), pred: Any): Tuple(Any...) :: __builtin__;\n"
      "((1, 2, 3), (x: Number): Number = x) -> Std.Tuple.Filter;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "filter_predicate_return_type_mismatch.fleaux");
  REQUIRE(parsed.has_value());

  constexpr fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());

  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().hint.has_value());
  REQUIRE(lowered.error().hint->find("return Bool") != std::string::npos);
}

