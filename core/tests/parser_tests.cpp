#include <string>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/frontend/ast.hpp"
#include "fleaux/frontend/parser.hpp"

TEST_CASE("Parser handles import let and expression statements", "[parser]") {
  const std::string src =
      "import Std;\n"
      "let Inc(x: Number): Number = (x, 1) -> Std.Add;\n"
      "(41) -> Inc -> Std.Println;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "unit_parser.fleaux");

  REQUIRE(parsed.has_value());
  REQUIRE(parsed->statements.size() == 3);
  REQUIRE(std::holds_alternative<fleaux::frontend::model::ImportStatement>(parsed->statements[0]));
  REQUIRE(std::holds_alternative<fleaux::frontend::model::LetStatement>(parsed->statements[1]));
  REQUIRE(std::holds_alternative<fleaux::frontend::model::ExpressionStatement>(parsed->statements[2]));
}

TEST_CASE("Parser supports digit-leading import module names", "[parser]") {
  const std::string src = "import 20_export;\n(4) -> Add4 -> Std.Println;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "digit_import.fleaux");

  REQUIRE(parsed.has_value());
  REQUIRE(parsed->statements.size() == 2);
  const auto& import_stmt = std::get<fleaux::frontend::model::ImportStatement>(parsed->statements[0]);
  REQUIRE(import_stmt.module_name == "20_export");
}

TEST_CASE("Parser reports semicolon hint on missing terminator", "[parser]") {
  const std::string src = "(1) -> Std.Println";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "missing_semicolon.fleaux");

  REQUIRE_FALSE(parsed.has_value());
  REQUIRE(parsed.error().hint.has_value());
  REQUIRE(parsed.error().hint->find("Add ';'") != std::string::npos);
}

