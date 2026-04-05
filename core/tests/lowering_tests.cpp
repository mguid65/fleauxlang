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
  REQUIRE(lowered->lets[0].body != nullptr);
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
  REQUIRE(lowered->lets[0].body == nullptr);
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

  const auto body = lowered->lets[0].body;
  REQUIRE(body != nullptr);
  REQUIRE(std::holds_alternative<fleaux::frontend::ir::IRFlowExpr>(body->node));

  const auto& top_flow = std::get<fleaux::frontend::ir::IRFlowExpr>(body->node);
  REQUIRE(std::holds_alternative<fleaux::frontend::ir::IRNameRef>(top_flow.rhs));
  const auto& target = std::get<fleaux::frontend::ir::IRNameRef>(top_flow.rhs);
  REQUIRE(target.qualifier.has_value());
  REQUIRE(target.qualifier.value() == "Std");
  REQUIRE(target.name == "Add");

  REQUIRE(top_flow.lhs != nullptr);
  REQUIRE(std::holds_alternative<fleaux::frontend::ir::IRTupleExpr>(top_flow.lhs->node));
}

