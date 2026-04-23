#include <string>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/frontend/ast.hpp"
#include "fleaux/frontend/parser.hpp"

TEST_CASE("Parser accepts applied named type syntax in parameter position", "[parser][type_syntax][applied_type]") {
  const std::string src =
      "let Std.WithOptions(items: Tuple(Any...), options: Dict(String, Any)) : Any :: __builtin__;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "applied_type_param.fleaux");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->statements.size() == 1);

  const auto& let_stmt = std::get<fleaux::frontend::model::LetStatement>(parsed->statements[0]);
  REQUIRE(let_stmt.params.params.size() == 2);

  const auto& options_param = let_stmt.params.params[1];
  const auto* applied =
      std::get_if<fleaux::frontend::Box<fleaux::frontend::model::AppliedTypeNode>>(&options_param.type.value);
  REQUIRE(applied != nullptr);
  REQUIRE((*applied)->name == "Dict");
  REQUIRE((*applied)->args.types.size() == 2);
}

TEST_CASE("Parser accepts applied named type syntax in return position", "[parser][type_syntax][applied_type]") {
  const std::string src = "let Lookup(key: String) : Dict(String, Any) :: __builtin__;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "applied_type_return.fleaux");
  REQUIRE(parsed.has_value());

  const auto& let_stmt = std::get<fleaux::frontend::model::LetStatement>(parsed->statements[0]);
  const auto* applied =
      std::get_if<fleaux::frontend::Box<fleaux::frontend::model::AppliedTypeNode>>(&let_stmt.rtype.value);
  REQUIRE(applied != nullptr);
  REQUIRE((*applied)->name == "Dict");
  REQUIRE((*applied)->args.types.size() == 2);
}

TEST_CASE("Parser accepts function type syntax in parameter position", "[parser][type_syntax][function_type]") {
  const std::string src = "let Std.Apply(value: Any, func: (Any) => Any) : Any :: __builtin__;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "function_type_param.fleaux");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->statements.size() == 1);

  const auto& let_stmt = std::get<fleaux::frontend::model::LetStatement>(parsed->statements[0]);
  REQUIRE(let_stmt.params.params.size() == 2);

  const auto& func_param = let_stmt.params.params[1];
  const auto* func_type =
      std::get_if<fleaux::frontend::Box<fleaux::frontend::model::FunctionTypeNode>>(&func_param.type.value);
  REQUIRE(func_type != nullptr);
  REQUIRE((*func_type)->params.types.size() == 1);
}

TEST_CASE("Parser accepts function type syntax in return position", "[parser][type_syntax][function_type]") {
  const std::string src = "let GetFunc() : (String) => Bool :: __builtin__;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "function_type_return.fleaux");
  REQUIRE(parsed.has_value());

  const auto& let_stmt = std::get<fleaux::frontend::model::LetStatement>(parsed->statements[0]);
  const auto* func_type =
      std::get_if<fleaux::frontend::Box<fleaux::frontend::model::FunctionTypeNode>>(&let_stmt.rtype.value);
  REQUIRE(func_type != nullptr);
  REQUIRE((*func_type)->params.types.size() == 1);
}

TEST_CASE("Parser accepts function type with multiple parameters", "[parser][type_syntax][function_type]") {
  const std::string src = "let Reduce(t: Tuple(Any...), init: Any, func: (Any, Any) => Any) : Any :: __builtin__;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "function_type_multi_param.fleaux");
  REQUIRE(parsed.has_value());

  const auto& let_stmt = std::get<fleaux::frontend::model::LetStatement>(parsed->statements[0]);
  const auto* func_type = std::get_if<fleaux::frontend::Box<fleaux::frontend::model::FunctionTypeNode>>(
      &let_stmt.params.params[2].type.value);
  REQUIRE(func_type != nullptr);
  REQUIRE((*func_type)->params.types.size() == 2);
}

TEST_CASE("Parser accepts function type with no parameters", "[parser][type_syntax][function_type]") {
  const std::string src = "let GetValue() : () => String :: __builtin__;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "function_type_no_params.fleaux");
  REQUIRE(parsed.has_value());

  const auto& let_stmt = std::get<fleaux::frontend::model::LetStatement>(parsed->statements[0]);
  const auto* func_type =
      std::get_if<fleaux::frontend::Box<fleaux::frontend::model::FunctionTypeNode>>(&let_stmt.rtype.value);
  REQUIRE(func_type != nullptr);
  REQUIRE((*func_type)->params.types.empty());
}

TEST_CASE("Parser accepts nested function type syntax", "[parser][type_syntax][function_type]") {
  const std::string src = "let Fn(x: ((Any) => Bool) => String) : Any :: __builtin__;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "function_type_nested.fleaux");
  REQUIRE(parsed.has_value());

  const auto& let_stmt = std::get<fleaux::frontend::model::LetStatement>(parsed->statements[0]);
  const auto* outer_func = std::get_if<fleaux::frontend::Box<fleaux::frontend::model::FunctionTypeNode>>(
      &let_stmt.params.params[0].type.value);
  REQUIRE(outer_func != nullptr);
  REQUIRE((*outer_func)->params.types.size() == 1);

  // The parameter should be a function type.
  const auto* inner_func = std::get_if<fleaux::frontend::Box<fleaux::frontend::model::FunctionTypeNode>>(
      &(*outer_func)->params.types[0]->value);
  REQUIRE(inner_func != nullptr);
  REQUIRE((*inner_func)->params.types.size() == 1);
}
