#include <string>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/frontend/ast.hpp"
#include "fleaux/frontend/parser.hpp"

TEST_CASE("Parser accepts applied named type syntax in parameter position", "[parser][type_syntax][applied_type]") {
  const std::string src =
      "let Std.WithOptions(items: Tuple<Any...>, options: Dict<String, Any>) : Any :: __builtin__;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "applied_type_param.fleaux");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->statements.size() == 1);

  const auto& let_stmt = std::get<fleaux::frontend::model::LetStatement>(parsed->statements[0]);
  REQUIRE(let_stmt.params.params.size() == 2);

  const auto& options_param = let_stmt.params.params[1];
  const auto* applied = std::get_if<fleaux::frontend::model::AppliedTypeNodeBox>(&options_param.type.value);
  REQUIRE(applied != nullptr);
  REQUIRE((*applied)->name == "Dict");
  REQUIRE((*applied)->args.types.size() == 2);
}

TEST_CASE("Parser accepts applied named type syntax in return position", "[parser][type_syntax][applied_type]") {
  const std::string src = "let Lookup(key: String) : Dict<String, Any> :: __builtin__;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "applied_type_return.fleaux");
  REQUIRE(parsed.has_value());

  const auto& let_stmt = std::get<fleaux::frontend::model::LetStatement>(parsed->statements[0]);
  const auto* applied = std::get_if<fleaux::frontend::model::AppliedTypeNodeBox>(&let_stmt.rtype.value);
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
  const auto* func_type = std::get_if<fleaux::frontend::model::FunctionTypeNodeBox>(&func_param.type.value);
  REQUIRE(func_type != nullptr);
  REQUIRE((*func_type)->params.types.size() == 1);
}

TEST_CASE("Parser accepts function type syntax in return position", "[parser][type_syntax][function_type]") {
  const std::string src = "let GetFunc() : (String) => Bool :: __builtin__;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "function_type_return.fleaux");
  REQUIRE(parsed.has_value());

  const auto& let_stmt = std::get<fleaux::frontend::model::LetStatement>(parsed->statements[0]);
  const auto* func_type = std::get_if<fleaux::frontend::model::FunctionTypeNodeBox>(&let_stmt.rtype.value);
  REQUIRE(func_type != nullptr);
  REQUIRE((*func_type)->params.types.size() == 1);
}

TEST_CASE("Parser accepts function type with multiple parameters", "[parser][type_syntax][function_type]") {
  const std::string src = "let Reduce(t: Tuple<Any...>, init: Any, func: (Any, Any) => Any) : Any :: __builtin__;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "function_type_multi_param.fleaux");
  REQUIRE(parsed.has_value());

  const auto& let_stmt = std::get<fleaux::frontend::model::LetStatement>(parsed->statements[0]);
  const auto* func_type = std::get_if<fleaux::frontend::model::FunctionTypeNodeBox>(&let_stmt.params.params[2].type.value);
  REQUIRE(func_type != nullptr);
  REQUIRE((*func_type)->params.types.size() == 2);
}

TEST_CASE("Parser accepts function type with no parameters", "[parser][type_syntax][function_type]") {
  const std::string src = "let GetValue() : () => String :: __builtin__;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "function_type_no_params.fleaux");
  REQUIRE(parsed.has_value());

  const auto& let_stmt = std::get<fleaux::frontend::model::LetStatement>(parsed->statements[0]);
  const auto* func_type = std::get_if<fleaux::frontend::model::FunctionTypeNodeBox>(&let_stmt.rtype.value);
  REQUIRE(func_type != nullptr);
  REQUIRE((*func_type)->params.types.empty());
}

TEST_CASE("Parser accepts nested function type syntax", "[parser][type_syntax][function_type]") {
  const std::string src = "let Fn(x: ((Any) => Bool) => String) : Any :: __builtin__;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "function_type_nested.fleaux");
  REQUIRE(parsed.has_value());

  const auto& let_stmt = std::get<fleaux::frontend::model::LetStatement>(parsed->statements[0]);
  const auto* outer_func =
      std::get_if<fleaux::frontend::model::FunctionTypeNodeBox>(&let_stmt.params.params[0].type.value);
  REQUIRE(outer_func != nullptr);
  REQUIRE((*outer_func)->params.types.size() == 1);

  // The parameter should be a function type.
  const auto* inner_func = std::get_if<fleaux::frontend::model::FunctionTypeNodeBox>(&(*outer_func)->params.types[0]->value);
  REQUIRE(inner_func != nullptr);
  REQUIRE((*inner_func)->params.types.size() == 1);
}

TEST_CASE("Parser accepts function type syntax in alias targets", "[parser][aliases][type_syntax][function_type]") {
  const std::string src = "alias Handler = (String, Int64) => Bool;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "alias_function_type_target.fleaux");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->statements.size() == 1);

  const auto& alias_stmt = std::get<fleaux::frontend::model::AliasStatement>(parsed->statements[0]);
  const auto* func_type = std::get_if<fleaux::frontend::model::FunctionTypeNodeBox>(&alias_stmt.target.value);
  REQUIRE(func_type != nullptr);
  REQUIRE((*func_type)->params.types.size() == 2);
  REQUIRE(std::holds_alternative<std::string>((*func_type)->return_type->value));
  REQUIRE(std::get<std::string>((*func_type)->return_type->value) == "Bool");
}

TEST_CASE("Parser accepts union type syntax in alias targets", "[parser][aliases][type_syntax][union_type]") {
  const std::string src = "alias MaybeName = String | Null;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "alias_union_type_target.fleaux");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->statements.size() == 1);

  const auto& alias_stmt = std::get<fleaux::frontend::model::AliasStatement>(parsed->statements[0]);
  const auto* union_type = std::get_if<fleaux::frontend::model::UnionTypeListBox>(&alias_stmt.target.value);
  REQUIRE(union_type != nullptr);
  REQUIRE((*union_type)->alternatives.size() == 2);
}

TEST_CASE("Parser dump_ast prints composite type syntax nodes", "[parser][dump_ast][type_syntax]") {
  const std::string src =
      "alias Handler = (String, Int64) => Bool;\n"
      "alias MaybeMap = Dict<String, Any> | Null;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "dump_ast_types.fleaux");
  REQUIRE(parsed.has_value());

  const std::string dumped = parser.dump_ast(*parsed);
  REQUIRE(dumped.find("FunctionTypeNode {") != std::string::npos);
  REQUIRE(dumped.find("AppliedTypeNode {") != std::string::npos);
  REQUIRE(dumped.find("UnionTypeList {") != std::string::npos);
  REQUIRE(dumped.find("kind: \"RawType\"") != std::string::npos);
  REQUIRE(dumped.find("name: \"Dict\"") != std::string::npos);
}


TEST_CASE("Parser reads nested angle-bracket type arguments", "[parser][type_syntax][angle_brackets]") {
  const std::string src =
      "let Run(t: Tuple<Result<Tuple<Int64...>, String>, Dict<String, (Int64) => Bool>>): Tuple<> :: __builtin__;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "angle_nested.fleaux");
  REQUIRE(parsed.has_value());

  const auto& let_stmt = std::get<fleaux::frontend::model::LetStatement>(parsed->statements[0]);
  const auto* tuple = std::get_if<fleaux::frontend::model::TypeListBox>(&let_stmt.params.params[0].type.value);
  REQUIRE(tuple != nullptr);
  REQUIRE((*tuple)->types.size() == 2);

  const auto* result = std::get_if<fleaux::frontend::model::AppliedTypeNodeBox>(&(*tuple)->types[0]->value);
  REQUIRE(result != nullptr);
  REQUIRE((*result)->name == "Result");
  REQUIRE((*result)->args.types.size() == 2);

  const auto* dict = std::get_if<fleaux::frontend::model::AppliedTypeNodeBox>(&(*tuple)->types[1]->value);
  REQUIRE(dict != nullptr);
  REQUIRE((*dict)->name == "Dict");
  REQUIRE(std::holds_alternative<fleaux::frontend::model::FunctionTypeNodeBox>((*dict)->args.types[1]->value));

  const auto* empty_tuple = std::get_if<fleaux::frontend::model::TypeListBox>(&let_stmt.rtype.value);
  REQUIRE(empty_tuple != nullptr);
  REQUIRE((*empty_tuple)->types.empty());
}

TEST_CASE("Parser closes a type argument list directly before '='", "[parser][type_syntax][angle_brackets]") {
  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program("let Pair: Tuple<Int64, Int64>= (1, 2);\n", "angle_greater_equal.fleaux");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->statements.size() == 1);
}

TEST_CASE("Parser rejects parenthesized type arguments with a hint", "[parser][type_syntax][angle_brackets]") {
  constexpr fleaux::frontend::parse::Parser parser;

  SECTION("Tuple") {
    const auto parsed = parser.parse_program("let F(x: Tuple(Int64)): Int64 = 1;\n", "paren_tuple.fleaux");
    REQUIRE_FALSE(parsed.has_value());
    REQUIRE(parsed.error().message.find("Type arguments use angle brackets") != std::string::npos);
    REQUIRE(parsed.error().hint.has_value());
    REQUIRE(parsed.error().hint->find("Tuple<...>") != std::string::npos);
  }

  SECTION("Applied type") {
    const auto parsed = parser.parse_program("let F(x: Dict(String, Int64)): Int64 = 1;\n", "paren_dict.fleaux");
    REQUIRE_FALSE(parsed.has_value());
    REQUIRE(parsed.error().message.find("Type arguments use angle brackets") != std::string::npos);
    REQUIRE(parsed.error().hint.has_value());
    REQUIRE(parsed.error().hint->find("Dict<...>") != std::string::npos);
  }

  SECTION("Empty applied type argument list") {
    const auto parsed = parser.parse_program("let F(x: Dict<>): Int64 = 1;\n", "empty_dict_args.fleaux");
    REQUIRE_FALSE(parsed.has_value());
    REQUIRE(parsed.error().message.find("Type argument list cannot be empty") != std::string::npos);
  }
}

TEST_CASE("Parser reads generic and builtin type declarations", "[parser][type_syntax][type_decl]") {
  const std::string src =
      "type Id<T> = Int64;\n"
      "type Result<T, E> :: __builtin__;\n"
      "type FileHandle :: __builtin__;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "type_decls.fleaux");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->statements.size() == 3);

  const auto& id_decl = std::get<fleaux::frontend::model::TypeStatement>(parsed->statements[0]);
  REQUIRE(id_decl.name == "Id");
  REQUIRE(id_decl.generic_params == std::vector<std::string>{"T"});
  REQUIRE_FALSE(id_decl.is_builtin);

  const auto& result_decl = std::get<fleaux::frontend::model::TypeStatement>(parsed->statements[1]);
  REQUIRE(result_decl.name == "Result");
  REQUIRE(result_decl.generic_params == std::vector<std::string>{"T", "E"});
  REQUIRE(result_decl.is_builtin);

  const auto& handle_decl = std::get<fleaux::frontend::model::TypeStatement>(parsed->statements[2]);
  REQUIRE(handle_decl.generic_params.empty());
  REQUIRE(handle_decl.is_builtin);
}

TEST_CASE("Parser parses a standalone type", "[parser][type_syntax][standalone]") {
  constexpr fleaux::frontend::parse::Parser parser;

  const auto parsed = parser.parse_type("Result<Tuple<Int64...>, String>", "<type>");
  REQUIRE(parsed.has_value());
  const auto* applied = std::get_if<fleaux::frontend::model::AppliedTypeNodeBox>(&parsed->value);
  REQUIRE(applied != nullptr);
  REQUIRE((*applied)->name == "Result");

  const auto trailing = parser.parse_type("Int64 Int64", "<type>");
  REQUIRE_FALSE(trailing.has_value());
}

TEST_CASE("Parser reads bounds on generic parameters", "[parser][type_syntax][generic_bounds]") {
  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(
      "let F<T: Int64 | UInt64, U, V: Dict<String, Tuple<Int64>>>(a: T, b: U, c: V): T = a;\n",
      "generic_bounds.fleaux");
  REQUIRE(parsed.has_value());

  const auto& let_stmt = std::get<fleaux::frontend::model::LetStatement>(parsed->statements[0]);
  REQUIRE(let_stmt.generic_params == std::vector<std::string>{"T", "U", "V"});
  REQUIRE(let_stmt.generic_bounds.size() == 3U);
  REQUIRE(let_stmt.generic_bounds[0].has_value());
  REQUIRE(std::holds_alternative<fleaux::frontend::model::UnionTypeListBox>(let_stmt.generic_bounds[0]->value));
  REQUIRE_FALSE(let_stmt.generic_bounds[1].has_value());
  REQUIRE(let_stmt.generic_bounds[2].has_value());
  REQUIRE(std::holds_alternative<fleaux::frontend::model::AppliedTypeNodeBox>(let_stmt.generic_bounds[2]->value));
}

TEST_CASE("Parser reads bounds on closure generic parameters", "[parser][type_syntax][generic_bounds]") {
  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed =
      parser.parse_program("let Id: Int64 = (1, <T: Int64>(x: T): T = x) -> Std.Apply;\n", "closure_bounds.fleaux");
  REQUIRE(parsed.has_value());
}

TEST_CASE("Parser rejects bounds on type declaration parameters", "[parser][type_syntax][generic_bounds]") {
  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program("type Box<T: Int64> = Tuple<T>;\n", "type_decl_bound.fleaux");
  REQUIRE_FALSE(parsed.has_value());
  REQUIRE(parsed.error().message.find("Type declaration generic parameters cannot have bounds") != std::string::npos);
}
