#include <string>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/frontend/ast.hpp"
#include "fleaux/frontend/lowering.hpp"
#include "fleaux/frontend/parser.hpp"

TEST_CASE("Lowerer maps parser program into import let and expression buckets", "[lowering]") {
  const std::string src =
      "import Std;\n"
      "let Add4(x: Float64): Float64 = (4.0, x) -> Std.Add;\n"
      "(4.0) -> Add4 -> Std.Println;\n";

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

TEST_CASE("Lowerer preserves type declarations in a dedicated IR bucket", "[lowering][types]") {
  const std::string src =
      "type Id = Int64;\n"
      "type Username :: String;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "lowering_type_decl_shape.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower_only(parsed.value());

  REQUIRE(lowered.has_value());
  REQUIRE(lowered->type_decls.size() == 2);
  REQUIRE(lowered->type_decls[0].name == "Id");
  REQUIRE(lowered->type_decls[0].target.name == "Int64");
  REQUIRE(lowered->type_decls[1].name == "Username");
  REQUIRE(lowered->type_decls[1].target.name == "String");
}

TEST_CASE("Lowerer preserves explicit type argument application on named targets", "[lowering][types][generics]") {
  const std::string src = "(10) -> Std.Cast<Id>;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "lowering_explicit_type_args_named_target.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower_only(parsed.value());
  REQUIRE(lowered.has_value());
  REQUIRE(lowered->expressions.size() == 1);

  const auto& [node, span] = lowered->expressions[0].expr;
  REQUIRE(std::holds_alternative<fleaux::frontend::ir::IRFlowExpr>(node));
  const auto& flow = std::get<fleaux::frontend::ir::IRFlowExpr>(node);
  REQUIRE(std::holds_alternative<fleaux::frontend::ir::IRNameRef>(flow.rhs));

  const auto& target = std::get<fleaux::frontend::ir::IRNameRef>(flow.rhs);
  REQUIRE(target.qualifier.has_value());
  REQUIRE(target.qualifier.value() == "Std");
  REQUIRE(target.name == "Cast");
  REQUIRE(target.explicit_type_args.size() == 1);
  REQUIRE(target.explicit_type_args[0].name == "Id");
}

TEST_CASE("Lowerer marks builtin lets and clears body", "[lowering]") {
  const std::string src = "let Pi(): Float64 :: __builtin__;\n";

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

TEST_CASE("Lowerer preserves let generic parameters in lower_only", "[lowering]") {
  const std::string src = "let Identity<T>(x: T): T = x;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "generic_let_lower_only.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower_only(parsed.value());

  REQUIRE(lowered.has_value());
  REQUIRE(lowered->lets.size() == 1);
  REQUIRE(lowered->lets[0].generic_params.size() == 1);
  REQUIRE(lowered->lets[0].generic_params[0] == "T");
}

TEST_CASE("Lowerer accepts user-defined generic lets during analysis", "[lowering][generics]") {
  const std::string src = "let Identity<T>(x: T): T = x;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "generic_let_lower_error.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());

  REQUIRE(lowered.has_value());
}

TEST_CASE("Lowerer accepts builtin generic lets during analysis", "[lowering][generics]") {
  const std::string src =
      "let Std.Identity<T>(x: T): T :: __builtin__;\n"
      "(1) -> Std.Identity;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "builtin_generic_let_lower_ok.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());
}

TEST_CASE("Lowerer accepts composite variadic tuple element return type", "[lowering][generics]") {
  const std::string src =
      "let Std.Tuple.Zip<A, B>(a: Tuple(A...), b: Tuple(B...)): Tuple(Tuple(A, B)...) :: __builtin__;\n"
      "((1, 2, 3), (\"a\", \"b\", \"c\")) -> Std.Tuple.Zip;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "lowering_composite_variadic_type.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());
}

TEST_CASE("Lowerer resolves tuple-template placeholder flow stage", "[lowering]") {
  const std::string src =
      "import Std;\n"
      "let Sum3(a: Float64, b: Float64, c: Float64): Float64 =\n"
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
  const std::string src = "import Std;\nlet MakeAdder(n: Float64): Any = (x: Float64): Float64 = (x, n) -> Std.Add;\n";

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

TEST_CASE("Lowerer preserves prefix-generic closure parameters in IR", "[lowering][generics]") {
  const std::string src = "let MakeIdentity(): Any = <T>(x: T): T = x;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "closure_generic_params_lowering.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());
  REQUIRE(lowered->lets.size() == 1);

  const auto& [node, span] = *lowered->lets[0].body;
  REQUIRE(std::holds_alternative<fleaux::frontend::ir::IRClosureExprBox>(node));
  const auto& closure_ptr = std::get<fleaux::frontend::ir::IRClosureExprBox>(node);

  REQUIRE(closure_ptr->generic_params.size() == 1);
  REQUIRE(closure_ptr->generic_params[0] == "T");
}

TEST_CASE("Lowerer desugars closure pipeline stage to Std.Apply", "[lowering]") {
  const std::string src = "import Std;\n(10.0) -> (x: Float64): Float64 = (x, 1.0) -> Std.Add -> Std.Println;\n";

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
  const std::string src = "import Std;\nlet Shadowed(n: Float64): Any = (n: Float64): Float64 = (n, 1.0) -> Std.Add;\n";

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
  const std::string src = "let Bad(): Any = (rest: Any..., x: Float64): Float64 = x;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "closure_bad_variadic_param.fleaux");
  REQUIRE(parsed.has_value());

  constexpr fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());

  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().message.find("Variadic parameter must be the final parameter") != std::string::npos);
}

TEST_CASE("Lowerer rewrites Std.Match wildcard pattern", "[lowering]") {
  const std::string src = "import Std;\n(1, (0, (): Any = \"zero\"), (_, (): Any = \"many\")) -> Std.Match;\n";

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
  const std::string src = "import Std;\n(1, (_, (): Any = \"many\"), (1, (): Any = \"one\")) -> Std.Match;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "match_wildcard_order_lowering.fleaux");
  REQUIRE(parsed.has_value());

  constexpr fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());

  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().message.find("wildcard '_' must be the final case") != std::string::npos);
}

TEST_CASE("Lowerer rejects Std.Match with no case tuples", "[lowering]") {
  const std::string src = "import Std;\n(1) -> Std.Match;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "match_requires_case_lowering.fleaux");
  REQUIRE(parsed.has_value());

  constexpr fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());

  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().message.find("Std.Match expects") != std::string::npos);
}

TEST_CASE("Lowerer rejects malformed Std.Match case tuples", "[lowering]") {
  const std::string src = "import Std;\n(1, (0, (): Any = \"zero\", \"extra\")) -> Std.Match;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "match_case_tuple_shape_lowering.fleaux");
  REQUIRE(parsed.has_value());

  constexpr fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());

  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().message.find("case must be a 2-item tuple") != std::string::npos);
}

TEST_CASE("Lowerer rejects implicit Int64 to Float64 promotion", "[lowering]") {
  const std::string src =
      "let KeepFloat(x: Float64): Float64 = x;\n"
      "(1) -> KeepFloat;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typed_numeric_transition_lowering.fleaux");
  REQUIRE(parsed.has_value());

  constexpr fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
}

TEST_CASE("Lowerer accepts UInt64 literal arguments for UInt64 parameters", "[lowering]") {
  const std::string src =
      "let EchoU(x: UInt64): UInt64 = x;\n"
      "(42u64) -> EchoU;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "uint64_literal_lowering.fleaux");
  REQUIRE(parsed.has_value());

  constexpr fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());

  REQUIRE(lowered.has_value());
}

TEST_CASE("Lowerer accepts Float64 values where Float64 is expected", "[lowering]") {
  const std::string src =
      "let Std.Pi(): Float64 = 3.14159;\n"
      "let AcceptFloat(x: Float64): Float64 = x;\n"
      "(Std.Pi) -> AcceptFloat;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "float64_to_number_ok.fleaux");
  REQUIRE(parsed.has_value());

  constexpr fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());
}

TEST_CASE("Lowerer preserves union alternatives in param and return types", "[lowering]") {
  const std::string src = "let NumOp(x: Float64 | Int64 | UInt64): Float64 | Int64 | UInt64 = x;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "union_type_lowering.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());
  REQUIRE(lowered->lets.size() == 1);

  const auto& fn = lowered->lets[0];
  REQUIRE(fn.params.size() == 1);
  REQUIRE(fn.params[0].type.alternatives.size() == 3);
  REQUIRE(fn.params[0].type.alternatives[0] == "Float64");
  REQUIRE(fn.params[0].type.alternatives[1] == "Int64");
  REQUIRE(fn.params[0].type.alternatives[2] == "UInt64");

  REQUIRE(fn.return_type.alternatives.size() == 3);
  REQUIRE(fn.return_type.alternatives[0] == "Float64");
  REQUIRE(fn.return_type.alternatives[1] == "Int64");
  REQUIRE(fn.return_type.alternatives[2] == "UInt64");
}

TEST_CASE("Lowerer accepts mixed arithmetic with explicit cast bridge", "[lowering]") {
  const std::string src =
      "let Std.Add(lhs: Float64 | Int64 | UInt64, rhs: Float64 | Int64 | UInt64): Float64 | Int64 | UInt64 :: "
      "__builtin__;\n"
      "let Std.ToFloat64(value: Any): Float64 :: __builtin__;\n"
      "let Good(a: Int64, b: UInt64): Float64 = ((a) -> Std.ToFloat64, (b) -> Std.ToFloat64) -> Std.Add;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "mixed_int_uint_add_cast_ok_lowering.fleaux");
  REQUIRE(parsed.has_value());

  constexpr fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());
}

TEST_CASE("Lowerer preserves let doc comments in IR", "[lowering]") {
  const std::string src =
      "import Std;\n"
      "// @brief Increment a value\n"
      "// @param x input value\n"
      "let Inc(x: Float64): Float64 = (x, 1.0) -> Std.Add;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "lowering_doc_comments.fleaux");
  REQUIRE(parsed.has_value());

  constexpr fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());
  REQUIRE(lowered->lets.size() == 1);
  REQUIRE(lowered->lets[0].doc_comments.size() == 2);
  REQUIRE(lowered->lets[0].doc_comments[0] == "@brief Increment a value");
}

TEST_CASE("Lowerer preserves applied type name and args in IRSimpleType", "[lowering][applied_type]") {
  const std::string src = "let Lookup(key: String) : Dict(String, Any) :: __builtin__;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "applied_type_lower.fleaux");
  REQUIRE(parsed.has_value());

  constexpr fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());

  const auto& fn = lowered->lets[0];
  REQUIRE(fn.return_type.name == "Dict");
  REQUIRE(fn.return_type.type_args.size() == 2);
  REQUIRE(fn.return_type.type_args[0].name == "String");
  REQUIRE(fn.return_type.type_args[1].name == "Any");
}

TEST_CASE("Lowerer preserves function type signature in IRSimpleType", "[lowering][function_type]") {
  const std::string src = "let Apply(value: Any, func: (Any) => Any) : Any :: __builtin__;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "function_type_lower.fleaux");
  REQUIRE(parsed.has_value());

  constexpr fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());

  const auto& fn = lowered->lets[0];
  REQUIRE(fn.params.size() == 2);
  const auto& func_param = fn.params[1];
  REQUIRE(func_param.type.name == "Function");
  REQUIRE(func_param.type.function_sig.has_value());
  REQUIRE(func_param.type.function_sig->param_types.size() == 1);
  REQUIRE(func_param.type.function_sig->param_types[0].name == "Any");
  REQUIRE(func_param.type.function_sig->return_type->name == "Any");
}

TEST_CASE("Lowerer preserves multi-parameter function type signature", "[lowering][function_type]") {
  const std::string src = "let Reduce(t: Tuple(Any...), init: Any, func: (Any, Any) => Any) : Any :: __builtin__;\n";

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "function_type_lower_multi_param.fleaux");
  REQUIRE(parsed.has_value());

  constexpr fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());

  const auto& fn = lowered->lets[0];
  const auto& func_param = fn.params[2];
  REQUIRE(func_param.type.name == "Function");
  REQUIRE(func_param.type.function_sig.has_value());
  REQUIRE(func_param.type.function_sig->param_types.size() == 2);
  REQUIRE(func_param.type.function_sig->param_types[0].name == "Any");
  REQUIRE(func_param.type.function_sig->param_types[1].name == "Any");
  REQUIRE(func_param.type.function_sig->return_type->name == "Any");
}
