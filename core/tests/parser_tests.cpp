#include <string>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/frontend/ast.hpp"
#include "fleaux/frontend/parser.hpp"

TEST_CASE("Parser handles import let and expression statements", "[parser]") {
  const std::string src =
      "import Std;\n"
      "let Inc(x: Float64): Float64 = (x, 1) -> Std.Add;\n"
      "(41) -> Inc -> Std.Println;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "unit_parser.fleaux");

  REQUIRE(parsed.has_value());
  REQUIRE(parsed->statements.size() == 3);
  REQUIRE(std::holds_alternative<fleaux::frontend::model::ImportStatement>(parsed->statements[0]));
  REQUIRE(std::holds_alternative<fleaux::frontend::model::LetStatement>(parsed->statements[1]));
  REQUIRE(std::holds_alternative<fleaux::frontend::model::ExpressionStatement>(parsed->statements[2]));
}

TEST_CASE("Parser accepts strong type declarations with both separators", "[parser][types]") {
  const std::string src =
      "type Id = Int64;\n"
      "type Username :: String;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "type_declarations_parser.fleaux");

  REQUIRE(parsed.has_value());
  REQUIRE(parsed->statements.size() == 2);
  REQUIRE(std::holds_alternative<fleaux::frontend::model::TypeStatement>(parsed->statements[0]));
  REQUIRE(std::holds_alternative<fleaux::frontend::model::TypeStatement>(parsed->statements[1]));

  const auto& first = std::get<fleaux::frontend::model::TypeStatement>(parsed->statements[0]);
  const auto& second = std::get<fleaux::frontend::model::TypeStatement>(parsed->statements[1]);
  REQUIRE(first.name == "Id");
  REQUIRE(second.name == "Username");
}

TEST_CASE("Parser accepts explicit type argument application on named targets", "[parser][types][generics]") {
  const std::string src = "(10) -> Std.Cast<Id>;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "explicit_type_args_named_target_parser.fleaux");

  REQUIRE(parsed.has_value());
  REQUIRE(parsed->statements.size() == 1);
  REQUIRE(std::holds_alternative<fleaux::frontend::model::ExpressionStatement>(parsed->statements[0]));

  const auto& expr_stmt = std::get<fleaux::frontend::model::ExpressionStatement>(parsed->statements[0]);
  REQUIRE(expr_stmt.expr.expr.rhs.size() == 1);

  const auto* named_target = std::get_if<fleaux::frontend::model::NamedTargetBox>(&expr_stmt.expr.expr.rhs[0].base.value);
  REQUIRE(named_target != nullptr);
  REQUIRE((*named_target)->explicit_type_args.size() == 1);

  const auto* qualified = std::get_if<fleaux::frontend::model::QualifiedId>(&(*named_target)->target);
  REQUIRE(qualified != nullptr);
  REQUIRE(qualified->qualifier.qualifier == "Std");
  REQUIRE(qualified->id == "Cast");
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

TEST_CASE("Parser accepts inline closure literal in expression position", "[parser]") {
  const std::string src = "(10, (x: Float64): Float64 = (x, 1) -> Std.Add) -> Std.Apply -> Std.Println;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "inline_closure_parser.fleaux");

  REQUIRE(parsed.has_value());
  REQUIRE(parsed->statements.size() == 1);
}

TEST_CASE("Parser accepts ungrouped inline closure pipeline target", "[parser]") {
  const std::string src = "(10) -> (x: Float64): Float64 = (x, 1) -> Std.Add -> Std.Println;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "ungrouped_closure_pipeline_parser.fleaux");

  REQUIRE(parsed.has_value());
  REQUIRE(parsed->statements.size() == 1);
}

TEST_CASE("Parser accepts zero-arg inline closure literal", "[parser]") {
  const std::string src = "(((): Float64 = 42)) -> Std.Println;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "zero_arg_inline_closure_parser.fleaux");

  REQUIRE(parsed.has_value());
  REQUIRE(parsed->statements.size() == 1);
}

TEST_CASE("Parser rejects let with missing sig-body separator", "[parser]") {
  const std::string src = "let Identity(x: Int64): Int64 x;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "generic_let_missing_separator_parser.fleaux");

  REQUIRE_FALSE(parsed.has_value());
  REQUIRE(parsed.error().message.find("Expected one of ['::', '='], got") != std::string::npos);
}

TEST_CASE("Parser rejects closure with missing sig-body separator", "[parser]") {
  const std::string src = "(x: Float64): Float64 (x, 1) -> Std.Add;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "generic_let_missing_separator_parser.fleaux");

  REQUIRE_FALSE(parsed.has_value());
  REQUIRE(parsed.error().message.find("Expected one of ['::', '='], got") != std::string::npos);
}

TEST_CASE("Parser accepts prefix-generic inline closure literal", "[parser][generics]") {
  const std::string src = "(10, <T>(x: T): T = x) -> Std.Apply -> Std.Println;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "prefix_generic_inline_closure_parser.fleaux");

  REQUIRE(parsed.has_value());
  REQUIRE(parsed->statements.size() == 1);
}

TEST_CASE("Parser rejects empty prefix-generic closure parameter list", "[parser][generics]") {
  const std::string src = "(10, <>(x: Any): Any = x) -> Std.Apply;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "prefix_generic_closure_empty_parser.fleaux");

  REQUIRE_FALSE(parsed.has_value());
  REQUIRE(parsed.error().message.find("cannot be empty") != std::string::npos);
}

TEST_CASE("Parser rejects trailing comma in prefix-generic closure parameter list", "[parser][generics]") {
  const std::string src = "(10, <T,>(x: T): T = x) -> Std.Apply;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "prefix_generic_closure_trailing_comma_parser.fleaux");

  REQUIRE_FALSE(parsed.has_value());
  REQUIRE(parsed.error().message.find("Trailing comma") != std::string::npos);
}

TEST_CASE("Parser accepts nested inline closure literals", "[parser]") {
  const std::string src =
      "(2, (x: Float64): Float64 = (x, (y: Float64): Float64 = (y, 1) -> Std.Add) -> Std.Apply) -> Std.Apply;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "nested_inline_closure_parser.fleaux");

  REQUIRE(parsed.has_value());
  REQUIRE(parsed->statements.size() == 1);
}

TEST_CASE("Parser reports malformed inline closure body diagnostics", "[parser]") {
  const std::string src = "(10) -> (x: Float64): Float64 = -> Std.Println;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "malformed_inline_closure_body_parser.fleaux");

  REQUIRE_FALSE(parsed.has_value());
  // The parser correctly identifies this as a closure with a malformed body:
  // after parsing the full signature (x: Float64): Float64 =, the body starts
  // with '->' which is not a valid expression start.
  REQUIRE(parsed.error().message.find("expected an expression") != std::string::npos);
  REQUIRE(parsed.error().hint.has_value());
}

TEST_CASE("Parser reports 'expected an expression' for unexpected symbol in expression position", "[parser]") {
  const std::string src = "let Foo(): Any = );\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "unexpected_symbol.fleaux");

  REQUIRE_FALSE(parsed.has_value());
  REQUIRE(parsed.error().message.find("expected an expression") != std::string::npos);
  REQUIRE(parsed.error().hint.has_value());
}

TEST_CASE("Parser accepts concrete numeric type names", "[parser]") {
  const std::string src = "let UsesTypedNumerics(a: Int64, b: UInt64, c: Float64): Float64 = (a, c) -> Std.Add;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typed_numeric_types_parser.fleaux");

  REQUIRE(parsed.has_value());
  REQUIRE(parsed->statements.size() == 1);
  REQUIRE(std::holds_alternative<fleaux::frontend::model::LetStatement>(parsed->statements[0]));
}

TEST_CASE("Parser accepts let generic parameter list", "[parser]") {
  const std::string src = "let Identity<T>(x: T): T = x;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "generic_let_parser.fleaux");

  REQUIRE(parsed.has_value());
  REQUIRE(parsed->statements.size() == 1);
  const auto& let_stmt = std::get<fleaux::frontend::model::LetStatement>(parsed->statements[0]);
  REQUIRE(let_stmt.generic_params.size() == 1);
  REQUIRE(let_stmt.generic_params[0] == "T");
}

TEST_CASE("Parser rejects empty let generic parameter list", "[parser]") {
  const std::string src = "let Identity<>(x: Any): Any = x;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "generic_let_empty_parser.fleaux");

  REQUIRE_FALSE(parsed.has_value());
  REQUIRE(parsed.error().message.find("cannot be empty") != std::string::npos);
}

TEST_CASE("Parser rejects trailing comma in let generic parameter list", "[parser]") {
  const std::string src = "let Identity<T,>(x: T): T = x;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "generic_let_trailing_comma_parser.fleaux");

  REQUIRE_FALSE(parsed.has_value());
  REQUIRE(parsed.error().message.find("Trailing comma") != std::string::npos);
}

TEST_CASE("Parser rejects malformed numeric exponent", "[parser]") {
  const std::string src = "(1e+) -> Std.Println;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "malformed_exponent_parser.fleaux");

  REQUIRE_FALSE(parsed.has_value());
  REQUIRE(parsed.error().message.find("Malformed numeric literal") != std::string::npos);
  REQUIRE(parsed.error().hint.has_value());
  REQUIRE(parsed.error().hint->find("Exponent requires at least one digit") != std::string::npos);
}

TEST_CASE("Parser rejects out-of-range integer literals with diagnostics", "[parser]") {
  const std::string src = "(9223372036854775808) -> Std.Println;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "integer_overflow_parser.fleaux");

  REQUIRE_FALSE(parsed.has_value());
  REQUIRE(parsed.error().message.find("out of range") != std::string::npos);
  REQUIRE(parsed.error().hint.has_value());
}

TEST_CASE("Parser accepts UInt64 literal suffix", "[parser]") {
  const std::string src = "(42u64) -> Std.Println;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "uint64_literal_parser.fleaux");

  REQUIRE(parsed.has_value());
}

TEST_CASE("Parser rejects negative UInt64 literals", "[parser]") {
  const std::string src = "(-1u64) -> Std.Println;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "negative_uint64_literal_parser.fleaux");

  REQUIRE_FALSE(parsed.has_value());
  REQUIRE(parsed.error().message.find("cannot be negative") != std::string::npos);
}

TEST_CASE("Parser rejects fractional UInt64 literals", "[parser]") {
  const std::string src = "(1.5u64) -> Std.Println;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "fractional_uint64_literal_parser.fleaux");

  REQUIRE_FALSE(parsed.has_value());
  REQUIRE(parsed.error().message.find("Invalid UInt64 literal") != std::string::npos);
}

TEST_CASE("Parser accepts union types in signatures", "[parser]") {
  const std::string src = "let NumOp(x: Float64 | Int64 | UInt64): Float64 | Int64 | UInt64 = x;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "union_types_parser.fleaux");

  REQUIRE(parsed.has_value());
  REQUIRE(parsed->statements.size() == 1);
}

TEST_CASE("Parser accepts tuple items that use union types", "[parser]") {
  const std::string src = "let Pack(x: Tuple(Float64 | Int64, UInt64)): Tuple(Float64 | Int64, UInt64) = x;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "tuple_union_types_parser.fleaux");

  REQUIRE(parsed.has_value());
  REQUIRE(parsed->statements.size() == 1);
}

TEST_CASE("Parser accepts variadic suffix on composite tuple element types", "[parser][generics]") {
  const std::string src =
      "let Std.Tuple.Zip<A, B>(a: Tuple(A...), b: Tuple(B...)): Tuple(Tuple(A, B)...) :: __builtin__;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "parser_composite_variadic_type.fleaux");

  REQUIRE(parsed.has_value());
  REQUIRE(parsed->statements.size() == 1);
  const auto& let_stmt = std::get<fleaux::frontend::model::LetStatement>(parsed->statements[0]);
  const auto* outer_tuple =
      std::get_if<fleaux::frontend::Box<fleaux::frontend::model::TypeList>>(&let_stmt.rtype.value);
  REQUIRE(outer_tuple != nullptr);
  REQUIRE((*outer_tuple)->types.size() == 1);
  REQUIRE((*outer_tuple)->types[0]->variadic);
  const auto* inner_tuple =
      std::get_if<fleaux::frontend::Box<fleaux::frontend::model::TypeList>>(&(*outer_tuple)->types[0]->value);
  REQUIRE(inner_tuple != nullptr);
  REQUIRE((*inner_tuple)->types.size() == 2);
}

TEST_CASE("Parser attaches consecutive comments above let as doc comments", "[parser]") {
  const std::string src =
      "// @brief Increment a value\n"
      "// @param x input value\n"
      "let Inc(x: Float64): Float64 = (x, 1) -> Std.Add;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "parser_doc_comments.fleaux");

  REQUIRE(parsed.has_value());
  REQUIRE(parsed->statements.size() == 1);
  const auto& let_stmt = std::get<fleaux::frontend::model::LetStatement>(parsed->statements[0]);
  REQUIRE(let_stmt.doc_comments.size() == 2);
  REQUIRE(let_stmt.doc_comments[0] == "@brief Increment a value");
  REQUIRE(let_stmt.doc_comments[1] == "@param x input value");
}

TEST_CASE("Parser drops doc comments if a blank line separates comments from let", "[parser]") {
  const std::string src =
      "// @brief Should not attach\n"
      "\n"
      "let Inc(x: Float64): Float64 = (x, 1) -> Std.Add;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "parser_doc_blank_reset.fleaux");

  REQUIRE(parsed.has_value());
  REQUIRE(parsed->statements.size() == 1);
  const auto& let_stmt = std::get<fleaux::frontend::model::LetStatement>(parsed->statements[0]);
  REQUIRE(let_stmt.doc_comments.empty());
}
