#include <algorithm>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/frontend/ast.hpp"
#include "fleaux/frontend/lowering.hpp"
#include "fleaux/frontend/parser.hpp"
#include "fleaux/frontend/type_check.hpp"
#include "fleaux/frontend/type_system/function_index.hpp"
#include "fleaux/frontend/type_system/type.hpp"

namespace {

using fleaux::frontend::ir::IRSimpleType;
using fleaux::frontend::type_system::Type;
using fleaux::frontend::type_system::TypeKind;

auto make_simple_type(const TypeKind kind) -> Type { return fleaux::frontend::type_system::make_type(kind); }

auto make_nominal_type(std::string name) -> Type {
  return fleaux::frontend::type_system::make_nominal_type(std::move(name));
}

auto make_type_var(std::string name) -> Type {
  Type type = make_simple_type(TypeKind::kTypeVar);
  type.nominal_name = std::move(name);
  return type;
}

auto make_variadic_type(Type type) -> Type {
  type.variadic = true;
  return type;
}

auto make_union_type(std::vector<Type> members) -> Type {
  Type type = make_simple_type(TypeKind::kUnion);
  type.union_members = std::move(members);
  return type;
}

auto make_tuple_type(std::vector<Type> items) -> Type {
  Type type = make_simple_type(TypeKind::kTuple);
  type.items = std::move(items);
  return type;
}

auto make_applied_type(std::string name, std::vector<Type> args) -> Type {
  Type type = make_simple_type(TypeKind::kApplied);
  type.applied_name = std::move(name);
  type.applied_args = std::move(args);
  return type;
}

auto make_function_type(std::vector<Type> params, std::optional<Type> return_type = std::nullopt) -> Type {
  Type type = make_simple_type(TypeKind::kFunction);
  type.function_params = std::move(params);
  if (return_type.has_value()) {
    type.function_return = fleaux::common::make_indirect_optional<Type>(std::move(*return_type));
  }
  return type;
}

auto make_ir_simple_type(std::string name, const bool variadic = false) -> IRSimpleType {
  IRSimpleType type{};
  type.name = std::move(name);
  type.variadic = variadic;
  return type;
}

auto make_ir_applied_type(std::string name, std::vector<IRSimpleType> args) -> IRSimpleType {
  IRSimpleType type{};
  type.name = std::move(name);
  type.type_args = std::move(args);
  return type;
}

auto make_ir_param(std::string name, IRSimpleType type) -> fleaux::frontend::ir::IRParam {
  fleaux::frontend::ir::IRParam param{};
  param.name = std::move(name);
  param.type = std::move(type);
  param.span = std::nullopt;
  return param;
}

auto make_ir_type_decl(std::string name, IRSimpleType target) -> fleaux::frontend::ir::IRTypeDecl {
  fleaux::frontend::ir::IRTypeDecl decl{};
  decl.name = std::move(name);
  decl.target = std::move(target);
  decl.span = std::nullopt;
  return decl;
}

auto make_ir_alias_decl(std::string name, IRSimpleType target) -> fleaux::frontend::ir::IRAliasDecl {
  fleaux::frontend::ir::IRAliasDecl decl{};
  decl.name = std::move(name);
  decl.target = std::move(target);
  decl.span = std::nullopt;
  return decl;
}

}  // namespace

TEST_CASE("Type helpers normalize unions and convert IR simple types", "[typecheck][types]") {
  using fleaux::frontend::type_system::from_ir_type;
  using fleaux::frontend::type_system::is_integer_like;
  using fleaux::frontend::type_system::normalize_type;

  SECTION("normalize_type flattens, sorts, and deduplicates union members") {
    Type union_type =
        make_union_type({make_simple_type(TypeKind::kFloat64), make_simple_type(TypeKind::kInt64),
                         make_union_type({make_simple_type(TypeKind::kUInt64), make_simple_type(TypeKind::kInt64)})});

    const auto normalized = normalize_type(std::move(union_type));
    REQUIRE(normalized.kind == TypeKind::kUnion);
    REQUIRE(normalized.union_members.size() == 3);
    REQUIRE(normalized.union_members[0].kind == TypeKind::kInt64);
    REQUIRE(normalized.union_members[1].kind == TypeKind::kUInt64);
    REQUIRE(normalized.union_members[2].kind == TypeKind::kFloat64);
  }

  SECTION("normalize_type collapses a single remaining union member") {
    Type union_type =
        make_union_type({make_simple_type(TypeKind::kInt64), make_union_type({make_simple_type(TypeKind::kInt64)})});

    const auto normalized = normalize_type(std::move(union_type));
    REQUIRE(normalized.kind == TypeKind::kInt64);
  }

  SECTION("normalize_type orders mixed structured members through the internal sort keys") {
    Type variadic_item = make_variadic_type(make_simple_type(TypeKind::kString));
    Type variadic_param = make_variadic_type(make_simple_type(TypeKind::kString));
    Type union_type =
        make_union_type({make_function_type({variadic_param}), make_tuple_type({variadic_item}),
                         make_applied_type("Box", {make_simple_type(TypeKind::kInt64)}), make_type_var("T"),
                         make_simple_type(TypeKind::kBool), make_simple_type(TypeKind::kUnknown),
                         make_union_type({make_simple_type(TypeKind::kString), make_simple_type(TypeKind::kNull)})});

    const auto normalized = normalize_type(std::move(union_type));
    REQUIRE(normalized.kind == TypeKind::kUnion);
    REQUIRE(normalized.union_members.size() == 8);
    REQUIRE(std::ranges::any_of(normalized.union_members,
                                [](const Type& member) { return member.kind == TypeKind::kUnknown; }));
    REQUIRE(std::ranges::any_of(normalized.union_members,
                                [](const Type& member) { return member.kind == TypeKind::kBool; }));
    REQUIRE(std::ranges::any_of(normalized.union_members, [](const Type& member) {
      return member.kind == TypeKind::kTypeVar && member.nominal_name == "T";
    }));
    REQUIRE(std::ranges::any_of(normalized.union_members, [](const Type& member) {
      return member.kind == TypeKind::kApplied && member.applied_name == "Box" && member.applied_args.size() == 1 &&
             member.applied_args[0].kind == TypeKind::kInt64;
    }));
    REQUIRE(std::ranges::any_of(normalized.union_members, [](const Type& member) {
      return member.kind == TypeKind::kTuple && member.items.size() == 1 && member.items[0].variadic;
    }));
    REQUIRE(std::ranges::any_of(normalized.union_members, [](const Type& member) {
      return member.kind == TypeKind::kFunction && member.function_params.size() == 1 &&
             member.function_params[0].variadic && !member.function_return.has_value();
    }));
    REQUIRE(std::ranges::any_of(normalized.union_members,
                                [](const Type& member) { return member.kind == TypeKind::kString; }));
    REQUIRE(std::ranges::any_of(normalized.union_members,
                                [](const Type& member) { return member.kind == TypeKind::kNull; }));
  }

  SECTION("from_ir_type converts structured union, function, applied, and nominal types") {
    IRSimpleType callable_type;
    callable_type.function_sig = IRSimpleType::FunctionSignature{
        .param_types =
            {
                make_ir_simple_type("Int64"),
                make_ir_simple_type("String", true),
            },
        .return_type = fleaux::frontend::ir::IRSimpleTypeBox(
            make_ir_applied_type("Dict", {make_ir_simple_type("String"), make_ir_simple_type("Float64")})),
    };

    IRSimpleType union_type;
    union_type.alternative_types = {
        make_ir_simple_type("UInt64"),
        make_ir_simple_type("Int64"),
        make_ir_simple_type("UInt64"),
    };

    const auto callable = from_ir_type(callable_type);
    const auto structured_union = from_ir_type(union_type);
    const auto nominal = from_ir_type(make_ir_simple_type("Widget"));

    REQUIRE(callable.kind == TypeKind::kFunction);
    REQUIRE(callable.function_params.size() == 2);
    REQUIRE(callable.function_params[0].kind == TypeKind::kInt64);
    REQUIRE(callable.function_params[1].kind == TypeKind::kString);
    REQUIRE(callable.function_params[1].variadic);
    REQUIRE(callable.function_return.has_value());
    REQUIRE((*callable.function_return).kind == TypeKind::kApplied);
    REQUIRE((*callable.function_return).applied_name == "Dict");
    REQUIRE((*callable.function_return).applied_args.size() == 2);
    REQUIRE((*callable.function_return).applied_args[0].kind == TypeKind::kString);
    REQUIRE((*callable.function_return).applied_args[1].kind == TypeKind::kFloat64);

    REQUIRE(structured_union.kind == TypeKind::kUnion);
    REQUIRE(structured_union.union_members.size() == 2);
    REQUIRE(structured_union.union_members[0].kind == TypeKind::kInt64);
    REQUIRE(structured_union.union_members[1].kind == TypeKind::kUInt64);

    REQUIRE(nominal.kind == TypeKind::kNominal);
    REQUIRE(nominal.nominal_name == "Widget");
  }

  SECTION("from_ir_type converts named alternatives through builtin and nominal names") {
    IRSimpleType named_union;
    named_union.alternatives = {"Function", "Tuple", "Bool", "Custom", "Any", "Never"};

    const auto converted = from_ir_type(named_union);
    REQUIRE(converted.kind == TypeKind::kUnion);
    // Never has no values, so normalization drops it from the union.
    REQUIRE(converted.union_members.size() == 5);
    REQUIRE(std::ranges::none_of(converted.union_members,
                                 [](const Type& member) { return member.kind == TypeKind::kNever; }));
    REQUIRE(
        std::ranges::any_of(converted.union_members, [](const Type& member) { return member.kind == TypeKind::kAny; }));
    REQUIRE(std::ranges::any_of(converted.union_members,
                                [](const Type& member) { return member.kind == TypeKind::kBool; }));
    REQUIRE(std::ranges::any_of(converted.union_members, [](const Type& member) {
      return member.kind == TypeKind::kNominal && member.nominal_name == "Custom";
    }));
    REQUIRE(std::ranges::any_of(converted.union_members,
                                [](const Type& member) { return member.kind == TypeKind::kTuple; }));
    REQUIRE(std::ranges::any_of(converted.union_members,
                                [](const Type& member) { return member.kind == TypeKind::kFunction; }));
  }

  SECTION("is_integer_like recognizes only Int64 and UInt64") {
    REQUIRE(is_integer_like(make_simple_type(TypeKind::kInt64)));
    REQUIRE(is_integer_like(make_simple_type(TypeKind::kUInt64)));
    REQUIRE_FALSE(is_integer_like(make_simple_type(TypeKind::kFloat64)));
    REQUIRE_FALSE(is_integer_like(make_nominal_type("Int64")));
  }
}

TEST_CASE("Type consistency handles unions tuples applied names and function signatures", "[typecheck][types]") {
  using fleaux::frontend::type_system::is_consistent;
  using fleaux::frontend::type_system::Type;
  using fleaux::frontend::type_system::TypeKind;

  const auto mk_function = [](std::vector<Type> params, std::optional<Type> return_type) -> Type {
    return make_function_type(std::move(params), std::move(return_type));
  };

  SECTION("Any and Never follow the top and bottom consistency rules") {
    // Any is the top type: every value fits Any, but an Any value fits only Any.
    REQUIRE(is_consistent(make_simple_type(TypeKind::kAny), make_simple_type(TypeKind::kString)));
    REQUIRE_FALSE(is_consistent(make_simple_type(TypeKind::kString), make_simple_type(TypeKind::kAny)));
    REQUIRE(is_consistent(make_simple_type(TypeKind::kAny), make_simple_type(TypeKind::kAny)));
    REQUIRE(is_consistent(make_simple_type(TypeKind::kString), make_simple_type(TypeKind::kNever)));
    REQUIRE_FALSE(is_consistent(make_simple_type(TypeKind::kNever), make_simple_type(TypeKind::kString)));
  }

  SECTION("union consistency handles subsets and empty unions") {
    const Type expected_union =
        make_union_type({make_simple_type(TypeKind::kInt64), make_simple_type(TypeKind::kString)});
    const Type actual_union =
        make_union_type({make_simple_type(TypeKind::kString), make_simple_type(TypeKind::kInt64)});
    const Type smaller_union = make_union_type({make_simple_type(TypeKind::kInt64)});
    const Type empty_union = make_union_type({});

    REQUIRE(is_consistent(expected_union, actual_union));
    REQUIRE(is_consistent(expected_union, smaller_union));
    REQUIRE(is_consistent(expected_union, make_simple_type(TypeKind::kInt64)));
    REQUIRE(is_consistent(make_simple_type(TypeKind::kInt64), smaller_union));
    REQUIRE_FALSE(is_consistent(expected_union, empty_union));
    REQUIRE_FALSE(is_consistent(empty_union, make_simple_type(TypeKind::kInt64)));
  }

  SECTION("tuple consistency enforces variadic placement and repeated item matching") {
    Type repeated_string = make_variadic_type(make_simple_type(TypeKind::kString));
    const Type variadic_tuple = make_tuple_type({make_simple_type(TypeKind::kInt64), repeated_string});
    const Type matching_actual = make_tuple_type(
        {make_simple_type(TypeKind::kInt64), make_simple_type(TypeKind::kString), make_simple_type(TypeKind::kString)});
    const Type too_short_actual = make_tuple_type({});

    Type invalid_variadic_head = make_variadic_type(make_simple_type(TypeKind::kString));
    const Type invalid_expected = make_tuple_type({invalid_variadic_head, make_simple_type(TypeKind::kInt64)});

    REQUIRE(is_consistent(variadic_tuple, matching_actual));
    REQUIRE_FALSE(is_consistent(variadic_tuple, too_short_actual));
    REQUIRE_FALSE(is_consistent(invalid_expected, matching_actual));
    REQUIRE_FALSE(is_consistent(variadic_tuple, make_simple_type(TypeKind::kInt64)));
  }

  SECTION("applied, type-variable, and nominal consistency require matching identities") {
    const Type dict_int_string =
        make_applied_type("Dict", {make_simple_type(TypeKind::kInt64), make_simple_type(TypeKind::kString)});
    const Type dict_int_float =
        make_applied_type("Dict", {make_simple_type(TypeKind::kInt64), make_simple_type(TypeKind::kFloat64)});
    const Type maybe_int = make_applied_type("Maybe", {make_simple_type(TypeKind::kInt64)});

    REQUIRE(is_consistent(dict_int_string, dict_int_string));
    REQUIRE_FALSE(is_consistent(dict_int_string, dict_int_float));
    REQUIRE_FALSE(is_consistent(dict_int_string, maybe_int));
    REQUIRE_FALSE(is_consistent(dict_int_string, make_simple_type(TypeKind::kInt64)));

    REQUIRE(is_consistent(make_type_var("T"), make_type_var("T")));
    REQUIRE_FALSE(is_consistent(make_type_var("T"), make_type_var("U")));

    REQUIRE(is_consistent(make_nominal_type("Widget"), make_nominal_type("Widget")));
    REQUIRE_FALSE(is_consistent(make_nominal_type("Widget"), make_nominal_type("Gadget")));
  }

  SECTION("function consistency handles missing returns, arity, and variadic shape") {
    const Type missing_return_expected = mk_function({make_simple_type(TypeKind::kInt64)}, std::nullopt);
    const Type mismatched_actual = mk_function(
        {make_simple_type(TypeKind::kString), make_simple_type(TypeKind::kString)}, make_simple_type(TypeKind::kBool));

    Type variadic_string = make_variadic_type(make_simple_type(TypeKind::kString));
    const Type variadic_fn = mk_function({variadic_string}, make_simple_type(TypeKind::kInt64));
    const Type non_variadic_fn = mk_function({make_simple_type(TypeKind::kString)}, make_simple_type(TypeKind::kInt64));
    const Type short_fn = mk_function({}, make_simple_type(TypeKind::kInt64));

    const Type named_widget_fn = mk_function({make_nominal_type("Widget")}, make_simple_type(TypeKind::kInt64));
    const Type named_gadget_fn = mk_function({make_nominal_type("Gadget")}, make_simple_type(TypeKind::kInt64));

    REQUIRE(is_consistent(missing_return_expected, mismatched_actual));
    REQUIRE_FALSE(is_consistent(variadic_fn, non_variadic_fn));
    REQUIRE_FALSE(is_consistent(named_widget_fn, short_fn));
    REQUIRE(is_consistent(named_widget_fn, named_widget_fn));
    REQUIRE_FALSE(is_consistent(named_widget_fn, named_gadget_fn));
    REQUIRE_FALSE(is_consistent(named_widget_fn, make_simple_type(TypeKind::kInt64)));
  }

  SECTION("function parameters are contravariant and returns are covariant") {
    const Type int_or_string =
        make_union_type({make_simple_type(TypeKind::kInt64), make_simple_type(TypeKind::kString)});
    const Type int64 = make_simple_type(TypeKind::kInt64);

    const Type int_to_int = mk_function({int64}, int64);
    const Type wide_param = mk_function({int_or_string}, int64);
    const Type wide_return = mk_function({int64}, int_or_string);

    // A function that accepts more arguments can stand in for one that accepts fewer.
    REQUIRE(is_consistent(int_to_int, wide_param));
    REQUIRE_FALSE(is_consistent(wide_param, int_to_int));

    // A function that returns fewer kinds of values can stand in for one that returns more.
    REQUIRE(is_consistent(wide_return, int_to_int));
    REQUIRE_FALSE(is_consistent(int_to_int, wide_return));
  }
}

TEST_CASE("Type checker infers builtin generic return type at call sites", "[typecheck][generics]") {
  const std::string src =
      "let Std.Identity<T>(x: T): T :: __builtin__;\n"
      "let NeedsInt(x: Int64): Int64 = x;\n"
      "(1) -> Std.Identity -> NeedsInt;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_generics_return_inference_ok.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());
}

TEST_CASE("Type checker rejects removed Std.TypeOf alias", "[typecheck][builtins]") {
  const std::string src =
      "import Std;\n"
      "(\"hi\") -> Std.TypeOf;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_removed_std_typeof.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().message.find("Unresolved symbol") != std::string::npos);
  REQUIRE(lowered.error().hint.has_value());
  REQUIRE(lowered.error().hint->find("Std.TypeOf") != std::string::npos);
}

TEST_CASE("Type checker infers user generic return type at call sites", "[typecheck][generics][stage_g3a]") {
  const std::string src =
      "let Identity<T>(x: T): T = x;\n"
      "let NeedsInt(x: Int64): Int64 = x;\n"
      "(1) -> Identity -> NeedsInt;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_stage_g3a_user_generic_return_inference_ok.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());
}

TEST_CASE("Type checker rejects user generic call with conflicting argument bindings",
          "[typecheck][generics][stage_g3a]") {
  const std::string src =
      "let KeepFirst<T>(x: T, y: T): T = x;\n"
      "(1, \"two\") -> KeepFirst;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_stage_g3a_user_generic_conflicting_bindings.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
  REQUIRE(lowered.error().hint.has_value());
  REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
}

TEST_CASE("Type checker rejects user generic let body that violates declared type variable",
          "[typecheck][generics][stage_g3b]") {
  const std::string src =
      "let Bad<T>(x: T): T = 1;\n"
      "(1) -> Bad;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_stage_g3b_user_generic_body_mismatch.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().message.find("Type mismatch in function return") != std::string::npos);
  REQUIRE(lowered.error().hint.has_value());
  REQUIRE(lowered.error().hint->find("Bad declares return type") != std::string::npos);
}

TEST_CASE("Type checker accepts user generic let body with higher-order closure over type variable",
          "[typecheck][generics][stage_g3b]") {
  const std::string src =
      "let Std.Apply<T, U>(value: T, func: (T) => U): U :: __builtin__;\n"
      "let ApplyIdentity<T>(x: T): T = (x, (y: T): T = y) -> Std.Apply;\n"
      "let NeedsInt(x: Int64): Int64 = x;\n"
      "(1) -> ApplyIdentity -> NeedsInt;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_stage_g3b_user_generic_body_hof_ok.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());
}

TEST_CASE("Type checker accepts prefix-generic inline closure with local type variable",
          "[typecheck][generics][closures]") {
  const std::string src = "let MakeInlineGeneric(): (String) => String = <T>(x: T): T = x;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_prefix_generic_closure_ok.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());
}

TEST_CASE("Type checker accepts zero-arg inline closure pipeline sugar", "[typecheck][closures]") {
  const std::string src =
      "import Std;\n"
      "() -> (): Any = (\"Empty Closure\") -> Std.Println;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_zero_arg_inline_closure_pipeline.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());
}

TEST_CASE("Type checker accepts top-level immutable value declarations in expression position",
          "[typecheck][blocks][let]") {
  const std::string src =
      "let Meaning: Int64 = 42;\n"
      "let NeedsInt(x: Int64): Int64 = x;\n"
      "Meaning -> NeedsInt;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_top_level_value_ok.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());
}

TEST_CASE("Type checker gives blocks the type of their final expression", "[typecheck][blocks]") {
  const std::string src =
      "import Std;\n"
      "let Compute(): Int64 = {\n"
      "  let x: Int64 = 1;\n"
      "  let y: Int64 = (x, 2) -> Std.Add;\n"
      "  y;\n"
      "};\n"
      "let NeedsInt(x: Int64): Int64 = x;\n"
      "() -> Compute -> NeedsInt;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_block_result_ok.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());
}

TEST_CASE("Type checker allows nested block shadowing", "[typecheck][blocks][shadowing]") {
  const std::string src =
      "let Compute(): Int64 = {\n"
      "  let value: Int64 = 1;\n"
      "  let shadowed: Int64 = {\n"
      "    let value: Int64 = 2;\n"
      "    value;\n"
      "  };\n"
      "  shadowed;\n"
      "};\n"
      "let NeedsInt(x: Int64): Int64 = x;\n"
      "() -> Compute -> NeedsInt;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_block_shadowing_ok.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());
}

TEST_CASE("Type checker rejects local let initializer type mismatch", "[typecheck][blocks][let]") {
  const std::string src =
      "let Compute(): Int64 = {\n"
      "  let x: Int64 = \"oops\";\n"
      "  x;\n"
      "};\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_local_let_initializer_mismatch.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().message.find("Type mismatch in local let initializer") != std::string::npos);
}

TEST_CASE("Type checker validates explicit type arguments on Std.Apply zero-arg shorthand",
          "[typecheck][generics][apply]") {
  SECTION("Zero-arg Std.Apply shorthand is still accepted without explicit type arguments") {
    const std::string src =
        "let Std.Apply<T, U>(value: T, func: (T) => U): U :: __builtin__;\n"
        "let NeedsString(x: String): String = x;\n"
        "((), (): String = \"fleaux\") -> Std.Apply -> NeedsString;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_std_apply_zero_arg_shorthand_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Zero-arg Std.Apply shorthand rejects explicit type arguments") {
    const std::string src =
        "let Std.Apply<T, U>(value: T, func: (T) => U): U :: __builtin__;\n"
        "((), (): String = \"fleaux\") -> Std.Apply<Tuple<>, String>;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_std_apply_zero_arg_shorthand_explicit_type_args.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Invalid explicit type argument application") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Std.Apply zero-arg shorthand") != std::string::npos);
  }
}

TEST_CASE("Type checker rejects zero-arg Std.Apply shorthand when named generic callable return remains unbound",
          "[typecheck][generics][apply][stage_g3f]") {
  const std::string src =
      "let Std.Apply<T, U>(value: T, func: (T) => U): U :: __builtin__;\n"
      "let MakeUnknown<U>(): U :: __builtin__;\n"
      "let NeedsInt(x: Int64): Int64 = x;\n"
      "((), MakeUnknown) -> Std.Apply -> NeedsInt;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_std_apply_zero_arg_named_generic_unbound.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
  REQUIRE(lowered.error().hint.has_value());
  REQUIRE(lowered.error().hint->find("could not infer generic return type variable") != std::string::npos);
  REQUIRE(lowered.error().hint->find("MakeUnknown") != std::string::npos);
  REQUIRE(lowered.error().hint->find("U") != std::string::npos);
}

TEST_CASE("Type checker validates first-class generic callable references with unresolved returns",
          "[typecheck][generics][stage_g3f]") {
  SECTION("Bare generic callable references reject unresolved return type variables") {
    const std::string src =
        "let MakeUnknown<U>(x: Int64): U :: __builtin__;\n"
        "let Keep(value: Any): Any = value;\n"
        "MakeUnknown -> Keep;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage_g3f_generic_value_ref_unbound_return.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("could not infer generic return type variable") != std::string::npos);
    REQUIRE(lowered.error().hint->find("MakeUnknown") != std::string::npos);
    REQUIRE(lowered.error().hint->find("U") != std::string::npos);
  }

  SECTION("Explicit type arguments still allow first-class callable materialization") {
    const std::string src =
        "let MakeUnknown<U>(x: Int64): U :: __builtin__;\n"
        "let KeepFn(f: (Int64) => Int64): (Int64) => Int64 = f;\n"
        "MakeUnknown<Int64> -> KeepFn;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage_g3f_generic_value_ref_explicit_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }
}

TEST_CASE("Type checker validates first-class generic callable references with unresolved parameter types",
          "[typecheck][generics][stage_g3f]") {
  SECTION("Bare generic callable references reject unresolved parameter type variables") {
    const std::string src =
        "let ToInt<T>(x: T): Int64 :: __builtin__;\n"
        "let Keep(value: Any): Any = value;\n"
        "ToInt -> Keep;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage_g3f_generic_value_ref_unbound_param.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("could not infer generic callable type variable") != std::string::npos);
    REQUIRE(lowered.error().hint->find("ToInt") != std::string::npos);
    REQUIRE(lowered.error().hint->find("T") != std::string::npos);
  }

  SECTION("Explicit type arguments still allow callable references with concrete parameter types") {
    const std::string src =
        "let ToInt<T>(x: T): Int64 :: __builtin__;\n"
        "let KeepFn(f: (String) => Int64): (String) => Int64 = f;\n"
        "ToInt<String> -> KeepFn;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage_g3f_generic_value_ref_explicit_param_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }
}

TEST_CASE("Type checker reports filtered overload candidates for explicit generic value-reference ambiguity",
          "[typecheck][generics][stage_g3f]") {
  const std::string src =
      "let Make<T>(x: T): T = x;\n"
      "let Make<T>(x: T, y: T): T = x;\n"
      "let Make(x: Int64): Int64 = x;\n"
      "let Keep(value: Any): Any = value;\n"
      "Make<String> -> Keep;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_stage_g3f_filtered_value_ref_ambiguity.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().message.find("Ambiguous overloaded function reference") != std::string::npos);
  REQUIRE(lowered.error().hint.has_value());
  REQUIRE(lowered.error().hint->find("Make(T): T") != std::string::npos);
  REQUIRE(lowered.error().hint->find("Make(T, T): T") != std::string::npos);
  REQUIRE(lowered.error().hint->find("Make(Int64): Int64") == std::string::npos);
}

TEST_CASE("Type checker infers user generic variadic tail argument type", "[typecheck][generics][stage_g3c]") {
  const std::string src =
      "let FirstOf<T>(head: T, tail: T...): T = head;\n"
      "let NeedsInt(x: Int64): Int64 = x;\n"
      "(1, 2, 3) -> FirstOf -> NeedsInt;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_stage_g3c_user_generic_variadic_inference_ok.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());
}

TEST_CASE("Type checker rejects user generic variadic tail with conflicting bindings",
          "[typecheck][generics][stage_g3c]") {
  const std::string src =
      "let FirstOf<T>(head: T, tail: T...): T = head;\n"
      "(1, \"two\") -> FirstOf;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_stage_g3c_user_generic_variadic_conflict.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
  REQUIRE(lowered.error().hint.has_value());
  REQUIRE(lowered.error().hint->find("expects variadic argument") != std::string::npos);
}

TEST_CASE("Type checker instantiates user generic nullable-union return from concrete input",
          "[typecheck][generics][stage_g3c]") {
  const std::string src =
      "let Maybe<T>(x: T | Null): T | Null = x;\n"
      "let NeedsMaybeInt(x: Int64 | Null): Int64 | Null = x;\n"
      "(1) -> Maybe -> NeedsMaybeInt;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_stage_g3c_user_generic_nullable_union_ok.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());
}

TEST_CASE("Type checker reports conflicting user generic type-variable names in argument diagnostics",
          "[typecheck][generics][stage_g3d]") {
  const std::string src =
      "let KeepFirst<T>(x: T, y: T): T = x;\n"
      "(1, \"two\") -> KeepFirst;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_stage_g3d_user_generic_conflicting_typevar_name.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
  REQUIRE(lowered.error().hint.has_value());
  REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  REQUIRE(lowered.error().hint->find("type variable(s): T") != std::string::npos);
}

TEST_CASE("Type checker reports conflicting user generic variadic type-variable names in argument diagnostics",
          "[typecheck][generics][stage_g3d]") {
  const std::string src =
      "let FirstOf<T>(head: T, tail: T...): T = head;\n"
      "(1, \"two\") -> FirstOf;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_stage_g3d_user_generic_variadic_typevar_name.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
  REQUIRE(lowered.error().hint.has_value());
  REQUIRE(lowered.error().hint->find("expects variadic argument") != std::string::npos);
  REQUIRE(lowered.error().hint->find("type variable(s): T") != std::string::npos);
}

TEST_CASE("Type checker reports user generic fixed-argument binding detail in diagnostics",
          "[typecheck][generics][stage_g3e]") {
  const std::string src =
      "let KeepFirst<T>(x: T, y: T): T = x;\n"
      "(1, \"two\") -> KeepFirst;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_stage_g3e_user_generic_binding_detail_fixed.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
  REQUIRE(lowered.error().hint.has_value());
  REQUIRE(lowered.error().hint->find("type variable(s): T") != std::string::npos);
  REQUIRE(lowered.error().hint->find("T bound as Int64") != std::string::npos);
  REQUIRE(lowered.error().hint->find("got String") != std::string::npos);
}

TEST_CASE("Type checker reports user generic variadic binding detail in diagnostics",
          "[typecheck][generics][stage_g3e]") {
  const std::string src =
      "let FirstOf<T>(head: T, tail: T...): T = head;\n"
      "(1, \"two\") -> FirstOf;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_stage_g3e_user_generic_binding_detail_variadic.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
  REQUIRE(lowered.error().hint.has_value());
  REQUIRE(lowered.error().hint->find("expects variadic argument") != std::string::npos);
  REQUIRE(lowered.error().hint->find("type variable(s): T") != std::string::npos);
  REQUIRE(lowered.error().hint->find("T bound as Int64") != std::string::npos);
  REQUIRE(lowered.error().hint->find("got String") != std::string::npos);
}

TEST_CASE("Type checker rejects callable-return generic binding that remains unbound after inference",
          "[typecheck][generics][stage_g3f]") {
  const std::string src =
      "let Std.Apply<T, U>(value: T, func: (T) => U): U :: __builtin__;\n"
      "let MakeUnknown<U>(x: Int64): U :: __builtin__;\n"
      "let NeedsInt(x: Int64): Int64 = x;\n"
      "(1, MakeUnknown) -> Std.Apply -> NeedsInt;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_stage_g3f_callable_return_unbound_generic.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
  REQUIRE(lowered.error().hint.has_value());
  REQUIRE(lowered.error().hint->find("could not infer generic return type variable") != std::string::npos);
  REQUIRE(lowered.error().hint->find("U") != std::string::npos);
}

TEST_CASE("Type checker rejects transitive callable-return generic alias that remains unresolved",
          "[typecheck][generics][stage_g3f]") {
  const std::string src =
      "let Std.Apply<T, U>(value: T, func: (T) => U): U :: __builtin__;\n"
      "let MakeUnknown<V>(x: Int64): V :: __builtin__;\n"
      "(1, MakeUnknown) -> Std.Apply;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed =
      parser.parse_program(src, "typecheck_stage_g3f_callable_return_transitive_unbound_generic.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
  REQUIRE(lowered.error().hint.has_value());
  REQUIRE(lowered.error().hint->find("could not infer generic return type variable") != std::string::npos);
  REQUIRE(lowered.error().hint->find("U") != std::string::npos);
}

TEST_CASE("Type checker accepts callable-return generic alias into enclosing generic parameter",
          "[typecheck][generics][stage_g3f]") {
  const std::string src =
      "let Std.Apply<T, U>(value: T, func: (T) => U): U :: __builtin__;\n"
      "let Relay<T, R>(x: T, func: (T) => R): R = (x, func) -> Std.Apply;\n"
      "let IdInt(x: Int64): Int64 = x;\n"
      "(1, IdInt) -> Relay;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed =
      parser.parse_program(src, "typecheck_stage_g3f_callable_return_alias_into_enclosing_generic_ok.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());
}

TEST_CASE("Type checker rejects builtin generic callable mismatch", "[typecheck][generics]") {
  const std::string src =
      "let Std.Apply<T, U>(value: T, func: (T) => U): U :: __builtin__;\n"
      "let NeedsInt(x: Int64): Int64 = x;\n"
      "(1.0, NeedsInt) -> Std.Apply;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_generics_apply_callable_mismatch.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
  REQUIRE(lowered.error().hint.has_value());
  REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  REQUIRE(lowered.error().hint->find("type variable(s): T, U") != std::string::npos);
  REQUIRE(lowered.error().hint->find("T bound as Float64") != std::string::npos);
  REQUIRE(lowered.error().hint->find("got (Int64) => Int64") != std::string::npos);
}

TEST_CASE("Type checker rejects builtin generic call with unbound return type variable", "[typecheck][generics]") {
  const std::string src =
      "let Std.MakeDefault<T>(): T :: __builtin__;\n"
      "let NeedsInt(x: Int64): Int64 = x;\n"
      "() -> Std.MakeDefault -> NeedsInt;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_generics_unbound_return_typevar.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
  REQUIRE(lowered.error().hint.has_value());
  REQUIRE(lowered.error().hint->find("could not infer generic return type variable") != std::string::npos);
  REQUIRE(lowered.error().hint->find("T") != std::string::npos);
}

TEST_CASE("Type checker enforces generic callable return compatibility", "[typecheck][generics]") {
  const std::string src =
      "let Std.ApplySame<T>(value: T, func: (T) => T): T :: __builtin__;\n"
      "let Bad(x: Int64): Float64 = 1.0;\n"
      "(1, Bad) -> Std.ApplySame;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_generics_callable_return_mismatch.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
  REQUIRE(lowered.error().hint.has_value());
  REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  REQUIRE(lowered.error().hint->find("type variable(s): T") != std::string::npos);
  REQUIRE(lowered.error().hint->find("T bound as Int64") != std::string::npos);
  REQUIRE(lowered.error().hint->find("got (Int64) => Float64") != std::string::npos);
}

TEST_CASE("Type checker infers Std.Tuple.Map generic output tuple type", "[typecheck][stage3d][generics]") {
  const std::string src =
      "let Std.Tuple.Map<T, U>(t: Tuple<T...>, func: (T) => U): Tuple<U...> :: __builtin__;\n"
      "let ToInt(x: Float64): Int64 = 1;\n"
      "let NeedsInts(xs: Tuple<Int64...>): Tuple<Int64...> = xs;\n"
      "((1.0, 2.0), ToInt) -> Std.Tuple.Map -> NeedsInts;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_stage3d_tuple_map_output_inference.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());
}

TEST_CASE("Type checker rejects Std.Tuple.Map callable with mismatched input type", "[typecheck][stage3d][generics]") {
  const std::string src =
      "let Std.Tuple.Map<T, U>(t: Tuple<T...>, func: (T) => U): Tuple<U...> :: __builtin__;\n"
      "let NeedsInt(x: Int64): Int64 = x;\n"
      "((1.0, 2.0), NeedsInt) -> Std.Tuple.Map;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_stage3d_tuple_map_callable_mismatch.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
  REQUIRE(lowered.error().hint.has_value());
  REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
}

TEST_CASE("Type checker matrix: Stage-3e broader generic higher-order migrations", "[typecheck][stage3e][generics]") {
  SECTION("Std.Parallel.Map infers Result<Tuple<U...>, Tuple<Int64, String>> output") {
    const std::string src =
        "let Std.Parallel.Map<T, U>(items: Tuple<T...>, func: (T) => U): Result<Tuple<U...>, Tuple<Int64, String>> "
        ":: __builtin__;\n"
        "let ToInt(x: Float64): Int64 = 1;\n"
        "let NeedsMapped(r: Result<Tuple<Int64...>, Tuple<Int64, String>>): Result<Tuple<Int64...>, Tuple<Int64, "
        "String>> = r;\n"
        "((1.0, 2.0), ToInt) -> Std.Parallel.Map -> NeedsMapped;\n" "type Result<T, E> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3e_parallel_map_result_inference.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Parallel.Map rejects callable input mismatch") {
    const std::string src =
        "let Std.Parallel.Map<T, U>(items: Tuple<T...>, func: (T) => U): Result<Tuple<U...>, Tuple<Int64, String>> "
        ":: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "((1.0, 2.0), NeedsInt) -> Std.Parallel.Map;\n" "type Result<T, E> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3e_parallel_map_callable_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Parallel.WithOptions infers Result<Tuple<U...>, Tuple<Int64, String>> output") {
    const std::string src =
        "let Std.Dict.Create(): Dict<Never, Never> :: __builtin__;\n"
        "let Std.Dict.Set<K, V>(dict: Dict<K, V>, key: K, value: V): Dict<K, V> :: __builtin__;\n"
        "let Std.Parallel.WithOptions<T, U>(items: Tuple<T...>, func: (T) => U, options: Dict<String, Any>): "
        "Result<Tuple<U...>, Tuple<Int64, String>> :: __builtin__;\n"
        "let ToInt(x: Float64): Int64 = 1;\n"
        "let NeedsMapped(r: Result<Tuple<Int64...>, Tuple<Int64, String>>): Result<Tuple<Int64...>, Tuple<Int64, "
        "String>> = r;\n"
        "let BuildOptions(): Dict<String, Any> = (() -> Std.Dict.Create, \"max_workers\", 2) -> Std.Dict.Set;\n"
        "((1.0, 2.0), ToInt, () -> BuildOptions) -> Std.Parallel.WithOptions -> NeedsMapped;\n"
        "type Result<T, E> :: __builtin__;\ntype Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3e_parallel_with_options_result_inference.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Parallel.WithOptions rejects callable input mismatch") {
    const std::string src =
        "let Std.Dict.Create(): Dict<Never, Never> :: __builtin__;\n"
        "let Std.Dict.Set<K, V>(dict: Dict<K, V>, key: K, value: V): Dict<K, V> :: __builtin__;\n"
        "let Std.Parallel.WithOptions<T, U>(items: Tuple<T...>, func: (T) => U, options: Dict<String, Any>): "
        "Result<Tuple<U...>, Tuple<Int64, String>> :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "let BuildOptions(): Dict<String, Any> = (() -> Std.Dict.Create, \"max_workers\", 2) -> Std.Dict.Set;\n"
        "((1.0, 2.0), NeedsInt, () -> BuildOptions) -> Std.Parallel.WithOptions;\n"
        "type Result<T, E> :: __builtin__;\ntype Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3e_parallel_with_options_callable_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Task.Spawn infers generic argument compatibility") {
    const std::string src =
        "let Std.Task.Spawn<T, U>(func: (T) => U, value: T): TaskHandle<U> :: __builtin__;\n"
        "let ToInt(x: Float64): Int64 = 1;\n"
        "let NeedsHandle(task: TaskHandle<Int64>): TaskHandle<Int64> = task;\n"
        "(ToInt, 1.0) -> Std.Task.Spawn -> NeedsHandle;\n" "type TaskHandle<T> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3e_task_spawn_generic_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.File.WithOpen propagates callable return type") {
    const std::string src =
        "let Std.File.WithOpen<R>(path: String, mode: String, func: (FileHandle) => R): R :: __builtin__;\n"
        "let NeedsInt(value: Int64): Int64 = value;\n"
        "(\"/tmp/a.txt\", \"r\", (h: FileHandle): Int64 = 1) -> Std.File.WithOpen -> NeedsInt;\n" "type FileHandle :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3e_file_withopen_return_inference.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Tuple.Any and Std.Tuple.FindIndex enforce generic predicate parameter type") {
    const std::string src =
        "let Std.Tuple.Any<T>(t: Tuple<T...>, pred: (T) => Bool): Bool :: __builtin__;\n"
        "let Std.Tuple.FindIndex<T>(t: Tuple<T...>, pred: (T) => Bool): Int64 :: __builtin__;\n"
        "let BadPred(x: Int64): Bool = True;\n"
        "((1.0, 2.0), BadPred) -> Std.Tuple.Any;\n"
        "((1.0, 2.0), BadPred) -> Std.Tuple.FindIndex;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3e_tuple_predicate_param_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }
}

TEST_CASE("Type checker matrix: Stage-3f Dict and Array signature tightening", "[typecheck][stage3f][generics]") {
  SECTION("Std.Dict.Get propagates value type") {
    const std::string src =
        "let Std.Dict.Create(): Dict<Never, Never> :: __builtin__;\n"
        "let Std.Dict.Get<K, V>(dict: Dict<K, V>, key: K): V :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "let Lookup(d: Dict<String, Int64>): Int64 = (d, \"a\") -> Std.Dict.Get;\n"
        "(() -> Std.Dict.Create) -> Lookup -> NeedsInt;\n" "type Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3f_dict_get_value_type.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Dict.Get rejects key-type mismatch") {
    const std::string src =
        "let Std.Dict.Create(): Dict<Never, Never> :: __builtin__;\n"
        "let Std.Dict.Get<K, V>(dict: Dict<K, V>, key: K): V :: __builtin__;\n"
        "let Lookup(d: Dict<String, Int64>): Int64 = (d, 1) -> Std.Dict.Get;\n"
        "(() -> Std.Dict.Create) -> Lookup;\n" "type Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3f_dict_get_key_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Dict.GetDefault rejects default-value mismatch") {
    const std::string src =
        "let Std.Dict.Create(): Dict<Never, Never> :: __builtin__;\n"
        "let Std.Dict.GetDefault<K, V>(dict: Dict<K, V>, key: K, default: V): V :: __builtin__;\n"
        "let Lookup(d: Dict<String, Int64>): Int64 = (d, \"a\", \"oops\") -> Std.Dict.GetDefault;\n"
        "(() -> Std.Dict.Create) -> Lookup;\n" "type Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3f_dict_getdefault_value_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Array.GetAtND rejects fractional indices through typed shape") {
    const std::string src =
        "let Std.Array.GetAtND(value: Any, indices: Tuple<UInt64...>): Any :: __builtin__;\n"
        "(((1, 2), (3, 4)), (1u64, 0.5)) -> Std.Array.GetAtND;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3f_array_getatnd_fractional_indices.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Array.Shape produces Tuple<UInt64...> compatible output") {
    const std::string src =
        "let Std.Array.Shape(value: Any): Tuple<UInt64...> :: __builtin__;\n"
        "let NeedsShape(s: Tuple<UInt64...>): Tuple<UInt64...> = s;\n"
        "(((1, 2), (3, 4))) -> Std.Array.Shape -> NeedsShape;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3f_array_shape_typed_output.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }
}

TEST_CASE("Type checker matrix: Stage-3g typed tuple outputs", "[typecheck][stage3g][generics]") {
  SECTION("Std.String.Split output is typed as Tuple<String...>") {
    const std::string src =
        "let Std.String.Split(s: String, sep: String): Tuple<String...> :: __builtin__;\n"
        "let NeedsStrings(parts: Tuple<String...>): Tuple<String...> = parts;\n"
        "(\"a,b,c\", \",\") -> Std.String.Split -> NeedsStrings;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3g_string_split_typed_output.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.GetArgs rejects non-string tuple expectation") {
    const std::string src =
        "let Std.GetArgs(): Tuple<String...> :: __builtin__;\n"
        "let NeedsInts(args: Tuple<Int64...>): Tuple<Int64...> = args;\n"
        "() -> Std.GetArgs -> NeedsInts;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3g_getargs_type_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Tuple.Range output is typed as Tuple<Int64...> across overloads") {
    const std::string src =
        "let Std.Tuple.Range(stop: Int64): Tuple<Int64...> :: __builtin__;\n"
        "let Std.Tuple.Range(start: Int64, stop: Int64): Tuple<Int64...> :: __builtin__;\n"
        "let Std.Tuple.Range(start: Int64, stop: Int64, step: Int64): Tuple<Int64...> :: __builtin__;\n"
        "let NeedsInts(values: Tuple<Int64...>): Tuple<Int64...> = values;\n"
        "(5) -> Std.Tuple.Range -> NeedsInts;\n"
        "(2, 5) -> Std.Tuple.Range -> NeedsInts;\n"
        "(1, 7, 3) -> Std.Tuple.Range -> NeedsInts;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3g_tuple_range_typed_output.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }
}

TEST_CASE("Type checker matrix: Stage-3h Array 1-D generic tightening", "[typecheck][stage3h][generics]") {
  SECTION("Std.Array.GetAt propagates element type") {
    const std::string src =
        "let Std.Array.GetAt<T>(array: Tuple<T...>, index: UInt64): T :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "((1, 2, 3), 1u64) -> Std.Array.GetAt -> NeedsInt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3h_array_getat_type_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Array.SetAt rejects replacement value type mismatch") {
    const std::string src =
        "let Std.Array.SetAt<T>(array: Tuple<T...>, index: UInt64, value: T): Tuple<T...> :: __builtin__;\n"
        "((1, 2, 3), 1u64, \"oops\") -> Std.Array.SetAt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3h_array_setat_value_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Array.SetAt preserves tuple element type") {
    const std::string src =
        "let Std.Array.SetAt<T>(array: Tuple<T...>, index: UInt64, value: T): Tuple<T...> :: __builtin__;\n"
        "let NeedsInts(values: Tuple<Int64...>): Tuple<Int64...> = values;\n"
        "((1, 2, 3), 1u64, 9) -> Std.Array.SetAt -> NeedsInts;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3h_array_setat_type_preservation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Array.InsertAt preserves tuple element type") {
    const std::string src =
        "let Std.Array.InsertAt<T>(array: Tuple<T...>, index: UInt64, value: T): Tuple<T...> :: __builtin__;\n"
        "let NeedsInts(values: Tuple<Int64...>): Tuple<Int64...> = values;\n"
        "((1, 2, 3), 1u64, 9) -> Std.Array.InsertAt -> NeedsInts;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3h_array_insertat_type_preservation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Array.RemoveAt preserves tuple element type") {
    const std::string src =
        "let Std.Array.RemoveAt<T>(array: Tuple<T...>, index: UInt64): Tuple<T...> :: __builtin__;\n"
        "let NeedsInts(values: Tuple<Int64...>): Tuple<Int64...> = values;\n"
        "((1, 2, 3), 1u64) -> Std.Array.RemoveAt -> NeedsInts;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3h_array_removeat_type_preservation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Array.Concat rejects mismatched tuple element types") {
    const std::string src =
        "let Std.Array.Concat<T>(lhs: Tuple<T...>, rhs: Tuple<T...>): Tuple<T...> :: __builtin__;\n"
        "((1, 2), (\"a\", \"b\")) -> Std.Array.Concat;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3h_array_concat_type_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Array.Concat preserves tuple element type") {
    const std::string src =
        "let Std.Array.Concat<T>(lhs: Tuple<T...>, rhs: Tuple<T...>): Tuple<T...> :: __builtin__;\n"
        "let NeedsInts(values: Tuple<Int64...>): Tuple<Int64...> = values;\n"
        "((1, 2), (3, 4)) -> Std.Array.Concat -> NeedsInts;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3h_array_concat_type_preservation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Array.Slice preserves tuple element type") {
    const std::string src =
        "let Std.Array.Slice<T>(array: Tuple<T...>, start: UInt64, stop: UInt64): Tuple<T...> :: __builtin__;\n"
        "let NeedsInts(values: Tuple<Int64...>): Tuple<Int64...> = values;\n"
        "((1, 2, 3, 4), 1u64, 3u64) -> Std.Array.Slice -> NeedsInts;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3h_array_slice_type_preservation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }
}

TEST_CASE("Type checker matrix: Stage-3i Array.Fill generic tightening", "[typecheck][stage3i][generics]") {
  SECTION("Std.Array.Fill accepts matching replacement element type") {
    const std::string src =
        "let Std.Array.Fill<T>(tuple: Tuple<T...>, start_index: UInt64, length: UInt64, value: T): Tuple<T...> :: "
        "__builtin__;\n"
        "((1, 2, 3, 4), 1u64, 2u64, 9) -> Std.Array.Fill;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3i_array_fill_accepts_matching_type.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Array.Fill rejects mismatched replacement element type") {
    const std::string src =
        "let Std.Array.Fill<T>(tuple: Tuple<T...>, start_index: UInt64, length: UInt64, value: T): Tuple<T...> :: "
        "__builtin__;\n"
        "((1, 2, 3, 4), 1u64, 2u64, \"oops\") -> Std.Array.Fill;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3i_array_fill_rejects_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Array.Fill preserves tuple element type through pipeline") {
    const std::string src =
        "let Std.Array.Fill<T>(tuple: Tuple<T...>, start_index: UInt64, length: UInt64, value: T): Tuple<T...> :: "
        "__builtin__;\n"
        "let NeedsInts(values: Tuple<Int64...>): Tuple<Int64...> = values;\n"
        "((1, 2, 3, 4), 1u64, 2u64, 9) -> Std.Array.Fill -> NeedsInts;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3i_array_fill_preserves_output_type.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }
}

TEST_CASE("Type checker matrix: Stage-3j Std.Take/Drop/ElementAt generic tightening",
          "[typecheck][stage3j][generics]") {
  SECTION("Std.ElementAt propagates tuple element type") {
    const std::string src =
        "let Std.ElementAt<T>(tuple: Tuple<T...>, count: UInt64): T :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "((1, 2, 3), 1u64) -> Std.ElementAt -> NeedsInt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3j_elementat_type_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.ElementAt rejects downstream element-type mismatch") {
    const std::string src =
        "let Std.ElementAt<T>(tuple: Tuple<T...>, count: UInt64): T :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "((1.0, 2.0, 3.0), 1u64) -> Std.ElementAt -> NeedsInt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3j_elementat_type_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Take preserves tuple element type") {
    const std::string src =
        "let Std.Take<T>(tuple: Tuple<T...>, count: UInt64): Tuple<T...> :: __builtin__;\n"
        "let NeedsInts(values: Tuple<Int64...>): Tuple<Int64...> = values;\n"
        "((1, 2, 3, 4), 2u64) -> Std.Take -> NeedsInts;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3j_take_type_preservation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Drop preserves tuple element type") {
    const std::string src =
        "let Std.Drop<T>(tuple: Tuple<T...>, count: UInt64): Tuple<T...> :: __builtin__;\n"
        "let NeedsInts(values: Tuple<Int64...>): Tuple<Int64...> = values;\n"
        "((1, 2, 3, 4), 2u64) -> Std.Drop -> NeedsInts;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3j_drop_type_preservation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }
}

TEST_CASE("Type checker matrix: Stage-3k Std.Slice/Length tuple consistency", "[typecheck][stage3k][generics]") {
  SECTION("Std.Slice preserves tuple element type") {
    const std::string src =
        "let Std.Slice<T>(tuple: Tuple<T...>, stop: UInt64): Tuple<T...> :: __builtin__;\n"
        "let NeedsInts(values: Tuple<Int64...>): Tuple<Int64...> = values;\n"
        "((1, 2, 3, 4), 2u64) -> Std.Slice -> NeedsInts;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3k_slice_type_preservation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Slice rejects downstream tuple element mismatch") {
    const std::string src =
        "let Std.Slice<T>(tuple: Tuple<T...>, stop: UInt64): Tuple<T...> :: __builtin__;\n"
        "let NeedsInts(values: Tuple<Int64...>): Tuple<Int64...> = values;\n"
        "((1.0, 2.0, 3.0, 4.0), 2u64) -> Std.Slice -> NeedsInts;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3k_slice_type_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Length remains Int64 with typed tuple input") {
    const std::string src =
        "let Std.Length<T>(tuple: Tuple<T...>): Int64 :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "((1.0, 2.0, 3.0)) -> Std.Length -> NeedsInt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3k_length_typed_input_returns_int64.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Length accepts nested singleton tuple input") {
    const std::string src =
        "let Std.Length<T>(tuple: Tuple<T...>): Int64 :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "((1)) -> Std.Length -> NeedsInt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3k_length_nested_singleton_tuple_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Length rejects scalar unary inputs during type checking") {
    const std::string src =
        "let Std.Length<T>(tuple: Tuple<T...>): Int64 :: __builtin__;\n"
        "1 -> Std.Length;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3k_length_scalar_reject.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }
}

TEST_CASE("Type checker matrix: Stage-3kc Std.Cast ownership", "[typecheck][stage3k][cast]") {
  SECTION("Std.Cast returns the target strong type for scalar unary input") {
    const std::string src =
        "let Std.Cast<T>(value: Any): T :: __builtin__;\n"
        "type UserId = Int64;\n"
        "let NeedsUserId(x: UserId): UserId = x;\n"
        "1 -> Std.Cast<UserId> -> NeedsUserId;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3kc_cast_scalar_returns_target_type.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Cast preserves tuple-valued unary identity casts") {
    const std::string src =
        "let Std.Cast<T>(value: Any): T :: __builtin__;\n"
        "let NeedsPair(x: Tuple<Int64, Int64>): Tuple<Int64, Int64> = x;\n"
        "((1, 2)) -> Std.Cast<Tuple<Int64, Int64>> -> NeedsPair;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3kc_cast_tuple_identity.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }
}

TEST_CASE("Type checker matrix: Stage-3kd Std.Type ownership", "[typecheck][stage3k][type]") {
  SECTION("Std.Type returns String for scalar unary input") {
    const std::string src =
        "let Std.Type(value: Any): String :: __builtin__;\n"
        "let NeedsString(x: String): String = x;\n"
        "1 -> Std.Type -> NeedsString;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3kd_type_scalar_returns_string.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Type returns String for tuple-valued unary input") {
    const std::string src =
        "let Std.Type(value: Any): String :: __builtin__;\n"
        "let NeedsString(x: String): String = x;\n"
        "((1, 2, 3)) -> Std.Type -> NeedsString;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3kd_type_tuple_returns_string.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }
}

TEST_CASE("Type checker matrix: Stage-3ke Std.ToString ownership", "[typecheck][stage3k][tostring]") {
  SECTION("Std.ToString returns String for scalar unary input") {
    const std::string src =
        "let Std.ToString(value: Any): String :: __builtin__;\n"
        "let NeedsString(x: String): String = x;\n"
        "1 -> Std.ToString -> NeedsString;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3ke_tostring_scalar_returns_string.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.ToString returns String for tuple-valued unary input") {
    const std::string src =
        "let Std.ToString(value: Any): String :: __builtin__;\n"
        "let NeedsString(x: String): String = x;\n"
        "((1, 2, 3)) -> Std.ToString -> NeedsString;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3ke_tostring_multi_tuple_returns_string.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }
}

TEST_CASE("Type checker matrix: Stage-3l Std.Wrap/Unwrap generic tightening", "[typecheck][stage3l][generics]") {
  SECTION("Std.Wrap propagates wrapped element type") {
    const std::string src =
        "let Std.Wrap<T>(val: T): Tuple<T> :: __builtin__;\n"
        "let NeedsIntTuple(x: Tuple<Int64>): Tuple<Int64> = x;\n"
        "(1) -> Std.Wrap -> NeedsIntTuple;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3l_wrap_type_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Wrap rejects downstream tuple element mismatch") {
    const std::string src =
        "let Std.Wrap<T>(val: T): Tuple<T> :: __builtin__;\n"
        "let NeedsIntTuple(x: Tuple<Int64>): Tuple<Int64> = x;\n"
        "(1.0) -> Std.Wrap -> NeedsIntTuple;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3l_wrap_type_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Unwrap propagates unwrapped element type") {
    const std::string src =
        "let Std.Wrap<T>(val: T): Tuple<T> :: __builtin__;\n"
        "let Std.Unwrap<T>(tuple: Tuple<T>): T :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "(1) -> Std.Wrap -> Std.Unwrap -> NeedsInt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3l_unwrap_type_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Unwrap rejects tuple-shape mismatch") {
    const std::string src =
        "let Std.Unwrap<T>(tuple: Tuple<T>): T :: __builtin__;\n"
        "((1, 2)) -> Std.Unwrap;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3l_unwrap_tuple_shape_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }
}

TEST_CASE("Type checker matrix: Stage-3m Std.Tuple unary helper generic tightening", "[typecheck][stage3m][generics]") {
  SECTION("Std.Tuple.Reverse preserves tuple element type") {
    const std::string src =
        "let Std.Tuple.Reverse<T>(t: Tuple<T...>): Tuple<T...> :: __builtin__;\n"
        "let NeedsInts(values: Tuple<Int64...>): Tuple<Int64...> = values;\n"
        "((1, 2, 3)) -> Std.Tuple.Reverse -> NeedsInts;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3m_tuple_reverse_type_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Tuple.Unique rejects downstream tuple element mismatch") {
    const std::string src =
        "let Std.Tuple.Unique<T>(t: Tuple<T...>): Tuple<T...> :: __builtin__;\n"
        "let NeedsInts(values: Tuple<Int64...>): Tuple<Int64...> = values;\n"
        "((1.0, 1.0, 2.0)) -> Std.Tuple.Unique -> NeedsInts;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3m_tuple_unique_type_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Tuple.Min propagates element type") {
    const std::string src =
        "let Std.Tuple.Min<T>(t: Tuple<T...>): T :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "((1, 2, 3)) -> Std.Tuple.Min -> NeedsInt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3m_tuple_min_type_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Tuple.Max rejects downstream element-type mismatch") {
    const std::string src =
        "let Std.Tuple.Max<T>(t: Tuple<T...>): T :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "((1.0, 2.0, 3.0)) -> Std.Tuple.Max -> NeedsInt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3m_tuple_max_type_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }
}

TEST_CASE("Type checker matrix: Stage-3n Std.Tuple.Sort generic tightening", "[typecheck][stage3n][generics]") {
  SECTION("Std.Tuple.Sort preserves tuple element type") {
    const std::string src =
        "let Std.Tuple.Sort<T>(t: Tuple<T...>): Tuple<T...> :: __builtin__;\n"
        "let NeedsInts(values: Tuple<Int64...>): Tuple<Int64...> = values;\n"
        "((3, 1, 2)) -> Std.Tuple.Sort -> NeedsInts;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3n_tuple_sort_type_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Tuple.Sort rejects downstream tuple element mismatch") {
    const std::string src =
        "let Std.Tuple.Sort<T>(t: Tuple<T...>): Tuple<T...> :: __builtin__;\n"
        "let NeedsInts(values: Tuple<Int64...>): Tuple<Int64...> = values;\n"
        "((3.0, 1.0, 2.0)) -> Std.Tuple.Sort -> NeedsInts;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3n_tuple_sort_type_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }
}

TEST_CASE("Type checker matrix: Stage-3o Std.Tuple append/prepend/contains/zip generic tightening",
          "[typecheck][stage3o][generics]") {
  SECTION("Std.Tuple.Append preserves tuple element type") {
    const std::string src =
        "let Std.Tuple.Append<T>(t: Tuple<T...>, item: T): Tuple<T...> :: __builtin__;\n"
        "let NeedsInts(values: Tuple<Int64...>): Tuple<Int64...> = values;\n"
        "((1, 2, 3), 4) -> Std.Tuple.Append -> NeedsInts;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3o_tuple_append_type_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Tuple.Append rejects appended element type mismatch") {
    const std::string src =
        "let Std.Tuple.Append<T>(t: Tuple<T...>, item: T): Tuple<T...> :: __builtin__;\n"
        "((1, 2, 3), \"oops\") -> Std.Tuple.Append;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3o_tuple_append_type_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Tuple.Prepend preserves tuple element type") {
    const std::string src =
        "let Std.Tuple.Prepend<T>(t: Tuple<T...>, item: T): Tuple<T...> :: __builtin__;\n"
        "let NeedsInts(values: Tuple<Int64...>): Tuple<Int64...> = values;\n"
        "((1, 2, 3), 0) -> Std.Tuple.Prepend -> NeedsInts;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3o_tuple_prepend_type_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Tuple.Contains rejects probe type mismatch") {
    const std::string src =
        "let Std.Tuple.Contains<T>(t: Tuple<T...>, item: T): Bool :: __builtin__;\n"
        "((1, 2, 3), \"oops\") -> Std.Tuple.Contains;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3o_tuple_contains_type_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Tuple.Zip accepts distinct input tuple element types") {
    const std::string src =
        "let Std.Tuple.Zip<A, B>(a: Tuple<A...>, b: Tuple<B...>): Tuple<Any...> :: __builtin__;\n"
        "((1, 2, 3), (\"a\", \"b\", \"c\")) -> Std.Tuple.Zip;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3o_tuple_zip_distinct_inputs_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }
}

TEST_CASE("Type checker matrix: Stage-3p Std.Tuple.Zip typed pair output", "[typecheck][stage3p][generics]") {
  SECTION("Std.Tuple.Zip propagates typed pair tuple output") {
    const std::string src =
        "let Std.Tuple.Zip<A, B>(a: Tuple<A...>, b: Tuple<B...>): Tuple<Tuple<A, B>...> :: __builtin__;\n"
        "let NeedsPairs(values: Tuple<Tuple<Int64, String>...>): Tuple<Tuple<Int64, String>...> = values;\n"
        "((1, 2, 3), (\"a\", \"b\", \"c\")) -> Std.Tuple.Zip -> NeedsPairs;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3p_tuple_zip_pair_output_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Tuple.Zip rejects downstream pair element-order mismatch") {
    const std::string src =
        "let Std.Tuple.Zip<A, B>(a: Tuple<A...>, b: Tuple<B...>): Tuple<Tuple<A, B>...> :: __builtin__;\n"
        "let NeedsPairs(values: Tuple<Tuple<String, Int64>...>): Tuple<Tuple<String, Int64>...> = values;\n"
        "((1, 2, 3), (\"a\", \"b\", \"c\")) -> Std.Tuple.Zip -> NeedsPairs;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3p_tuple_zip_pair_order_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Tuple.Zip rejects non-pair downstream tuple expectation") {
    const std::string src =
        "let Std.Tuple.Zip<A, B>(a: Tuple<A...>, b: Tuple<B...>): Tuple<Tuple<A, B>...> :: __builtin__;\n"
        "let NeedsInts(values: Tuple<Int64...>): Tuple<Int64...> = values;\n"
        "((1, 2, 3), (\"a\", \"b\", \"c\")) -> Std.Tuple.Zip -> NeedsInts;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3p_tuple_zip_non_pair_output_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }
}

TEST_CASE("Type checker matrix: Stage-3q Std.Result helper generic tightening", "[typecheck][stage3q][generics]") {
  SECTION("Std.Result.Unwrap propagates Ok payload type") {
    const std::string src =
        "let Std.Result.Unwrap<T, E>(result: Result<T, E>): T :: __builtin__;\n"
        "let MakeOk(): Result<Int64, String> :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "() -> MakeOk -> Std.Result.Unwrap -> NeedsInt;\n" "type Result<T, E> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3q_result_unwrap_type_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Result.UnwrapErr rejects downstream error-type mismatch") {
    const std::string src =
        "let Std.Result.UnwrapErr<T, E>(result: Result<T, E>): E :: __builtin__;\n"
        "let MakeErr(): Result<Int64, String> :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "() -> MakeErr -> Std.Result.UnwrapErr -> NeedsInt;\n" "type Result<T, E> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3q_result_unwraperr_type_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }
}

TEST_CASE("Type checker matrix: Stage-3qc Std.Result accessor family ownership", "[typecheck][stage3q][result]") {
  SECTION("Std.Result.Tag propagates Bool") {
    const std::string src =
        "let Std.Result.Tag<T, E>(result: Result<T, E>): Bool :: __builtin__;\n"
        "let MakeOk(): Result<Int64, String> :: __builtin__;\n"
        "let NeedsBool(x: Bool): Bool = x;\n"
        "() -> MakeOk -> Std.Result.Tag -> NeedsBool;\n" "type Result<T, E> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3qc_result_tag_bool_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Result.IsOk propagates Bool") {
    const std::string src =
        "let Std.Result.IsOk<T, E>(result: Result<T, E>): Bool :: __builtin__;\n"
        "let MakeOk(): Result<Int64, String> :: __builtin__;\n"
        "let NeedsBool(x: Bool): Bool = x;\n"
        "() -> MakeOk -> Std.Result.IsOk -> NeedsBool;\n" "type Result<T, E> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3qc_result_isok_bool_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Result.Payload propagates the payload union") {
    const std::string src =
        "let Std.Result.Payload<T, E>(result: Result<T, E>): T | E :: __builtin__;\n"
        "let MakeEither(): Result<Int64, String> :: __builtin__;\n"
        "let NeedsEither(x: Int64 | String): Int64 | String = x;\n"
        "() -> MakeEither -> Std.Result.Payload -> NeedsEither;\n" "type Result<T, E> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3qc_result_payload_union_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Result.UnwrapErr propagates the error channel") {
    const std::string src =
        "let Std.Result.UnwrapErr<T, E>(result: Result<T, E>): E :: __builtin__;\n"
        "let MakeErr(): Result<Int64, String> :: __builtin__;\n"
        "let NeedsString(x: String): String = x;\n"
        "() -> MakeErr -> Std.Result.UnwrapErr -> NeedsString;\n" "type Result<T, E> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3qc_result_unwraperr_error_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Result.IsErr propagates Bool") {
    const std::string src =
        "let Std.Result.IsErr<T, E>(result: Result<T, E>): Bool :: __builtin__;\n"
        "let MakeErr(): Result<Int64, String> :: __builtin__;\n"
        "let NeedsBool(x: Bool): Bool = x;\n"
        "() -> MakeErr -> Std.Result.IsErr -> NeedsBool;\n" "type Result<T, E> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3qc_result_iserr_bool_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Result.Tag rejects non-result tuple inputs during type checking") {
    const std::string src =
        "let Std.Result.Tag<T, E>(result: Result<T, E>): Bool :: __builtin__;\n"
        "(1, 2,) -> Std.Result.Tag;\n" "type Result<T, E> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3qc_result_tag_non_result_tuple_reject.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Result.IsOk rejects scalar unary inputs during type checking") {
    const std::string src =
        "let Std.Result.IsOk<T, E>(result: Result<T, E>): Bool :: __builtin__;\n"
        "1 -> Std.Result.IsOk;\n" "type Result<T, E> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3qc_result_isok_scalar_reject.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Result.Payload rejects scalar unary inputs during type checking") {
    const std::string src =
        "let Std.Result.Payload<T, E>(result: Result<T, E>): T | E :: __builtin__;\n"
        "1 -> Std.Result.Payload;\n" "type Result<T, E> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3qc_result_payload_scalar_reject.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }
}

TEST_CASE("Type checker matrix: Stage-3qb Std.First and Std.Second pair-shape ownership",
          "[typecheck][stage3q][pairs]") {
  SECTION("Std.First propagates the first pair element type") {
    const std::string src =
        "let Std.First<A, B>(pair: Tuple<A, B>): A :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "(1, \"two\",) -> Std.First -> NeedsInt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3qb_first_type_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Second propagates the second pair element type") {
    const std::string src =
        "let Std.Second<A, B>(pair: Tuple<A, B>): B :: __builtin__;\n"
        "let NeedsString(x: String): String = x;\n"
        "(1, \"two\",) -> Std.Second -> NeedsString;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3qb_second_type_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.First rejects non-pair tuple inputs during type checking") {
    const std::string src =
        "let Std.First<A, B>(pair: Tuple<A, B>): A :: __builtin__;\n"
        "(1, 2, 3,) -> Std.First;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3qb_first_non_pair_tuple_reject.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Second rejects scalar unary inputs during type checking") {
    const std::string src =
        "let Std.Second<A, B>(pair: Tuple<A, B>): B :: __builtin__;\n"
        "(1) -> Std.Second;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3qb_second_scalar_reject.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }
}

TEST_CASE("Type checker matrix: Stage-4c Std.Result constructor policy", "[typecheck][stage4c][generics]") {
  SECTION("Std.Result.Ok propagates known success payload type") {
    const std::string src =
        "let Std.Result.Ok<T>(value: T): Result<T, Any> :: __builtin__;\n"
        "let NeedsOk(r: Result<Int64, Any>): Result<Int64, Any> = r;\n"
        "(1) -> Std.Result.Ok -> NeedsOk;\n" "type Result<T, E> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_result_ok_payload_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Result.Ok rejects downstream success-payload mismatch") {
    const std::string src =
        "let Std.Result.Ok<T>(value: T): Result<T, Any> :: __builtin__;\n"
        "let NeedsWrongOk(r: Result<String, Any>): Result<String, Any> = r;\n"
        "(1) -> Std.Result.Ok -> NeedsWrongOk;\n" "type Result<T, E> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_result_ok_payload_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Result.Err propagates known error payload type") {
    const std::string src =
        "let Std.Result.Err<E>(error: E): Result<Any, E> :: __builtin__;\n"
        "let NeedsErr(r: Result<Any, String>): Result<Any, String> = r;\n"
        "(\"oops\") -> Std.Result.Err -> NeedsErr;\n" "type Result<T, E> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_result_err_payload_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Result.Err rejects downstream error-payload mismatch") {
    const std::string src =
        "let Std.Result.Err<E>(error: E): Result<Any, E> :: __builtin__;\n"
        "let NeedsWrongErr(r: Result<Any, Int64>): Result<Any, Int64> = r;\n"
        "(\"oops\") -> Std.Result.Err -> NeedsWrongErr;\n" "type Result<T, E> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_result_err_payload_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }
}

TEST_CASE("Type checker matrix: Stage-4c Std.Dict.Create surface contract", "[typecheck][stage4c][dict]") {
  SECTION("Std.Dict.Create propagates nullary Dict<Any, Any> output") {
    const std::string src =
        "let Std.Dict.Create(): Dict<Never, Never> :: __builtin__;\n"
        "let Std.Dict.Create<K, V>(dict: Dict<K, V>): Dict<K, V> :: __builtin__;\n"
        "let NeedsDict(d: Dict<Any, Any>): Dict<Any, Any> = d;\n"
        "() -> Std.Dict.Create -> NeedsDict;\n" "type Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_dict_create_nullary_surface.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Dict.Create preserves channels when cloning an existing dictionary") {
    const std::string src =
        "let Std.Dict.Create(): Dict<Never, Never> :: __builtin__;\n"
        "let Std.Dict.Create<K, V>(dict: Dict<K, V>): Dict<K, V> :: __builtin__;\n"
        "let Std.Dict.Set<K, V>(dict: Dict<K, V>, key: K, value: V): Dict<K, V> :: __builtin__;\n"
        "let NeedsStringInt(d: Dict<String, Int64>): Dict<String, Int64> = d;\n"
        "let MakeDict(): Dict<String, Int64> = (() -> Std.Dict.Create, \"a\", 1) -> Std.Dict.Set;\n"
        "() -> MakeDict -> Std.Dict.Create -> NeedsStringInt;\n" "type Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_dict_create_clone_surface_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Annotated analysis records the resolved builtin overload symbol key") {
    const std::string src =
        "let Std.Dict.Create(): Dict<Never, Never> :: __builtin__;\n"
        "let Std.Dict.Create<K, V>(dict: Dict<K, V>): Dict<K, V> :: __builtin__;\n"
        "let Std.Dict.Set<K, V>(dict: Dict<K, V>, key: K, value: V): Dict<K, V> :: __builtin__;\n"
        "let MakeDict(): Dict<String, Int64> = (() -> Std.Dict.Create, \"a\", 1) -> Std.Dict.Set;\n"
        "() -> Std.Dict.Create;\n"
        "() -> MakeDict -> Std.Dict.Create;\n" "type Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_dict_create_overload_annotation_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower_only(parsed.value());
    REQUIRE(lowered.has_value());

    const std::unordered_set<std::string> imported_symbols;
    const auto analyzed = fleaux::frontend::type_check::analyze_program(*lowered, imported_symbols);
    REQUIRE(analyzed.has_value());
    REQUIRE(analyzed->expressions.size() == 2U);

    const auto* nullary_flow = std::get_if<fleaux::frontend::ir::IRFlowExpr>(&analyzed->expressions[0].expr.node);
    REQUIRE(nullary_flow != nullptr);
    const auto* nullary_name = std::get_if<fleaux::frontend::ir::IRNameRef>(&nullary_flow->rhs);
    REQUIRE(nullary_name != nullptr);
    REQUIRE(nullary_name->resolved_symbol_key.has_value());
    REQUIRE(*nullary_name->resolved_symbol_key == "Std.Dict.Create#0");

    const auto* clone_flow = std::get_if<fleaux::frontend::ir::IRFlowExpr>(&analyzed->expressions[1].expr.node);
    REQUIRE(clone_flow != nullptr);
    const auto* clone_name = std::get_if<fleaux::frontend::ir::IRNameRef>(&clone_flow->rhs);
    REQUIRE(clone_name != nullptr);
    REQUIRE(clone_name->resolved_symbol_key.has_value());
    REQUIRE(*clone_name->resolved_symbol_key == "Std.Dict.Create#1");
  }

  SECTION("Bare Std.Dict.Create reference is rejected because the overload set is ambiguous") {
    const std::string src =
        "let Std.Dict.Create(): Dict<Never, Never> :: __builtin__;\n"
        "let Std.Dict.Create<K, V>(dict: Dict<K, V>): Dict<K, V> :: __builtin__;\n"
        "let Keep(value: Any): Any = value;\n"
        "Std.Dict.Create -> Keep;\n" "type Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_dict_create_ambiguous_ref_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Ambiguous overloaded function reference") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("multiple overloads") != std::string::npos);
  }
}

TEST_CASE("Type checker rejects builtin overloads that rely on type-only dispatch",
          "[typecheck][stage4c][builtins][overload]") {
  const std::string src =
      "let Std.Make<T>(x: T): T :: __builtin__;\n"
      "let Std.Make(x: String): String :: __builtin__;\n"
      "(\"ok\") -> Std.Make;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_stage4c_builtin_overload_shape_rejects.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().message.find("Unsupported builtin overload set") != std::string::npos);
  REQUIRE(lowered.error().hint.has_value());
  REQUIRE(lowered.error().hint->find("must differ by call shape") != std::string::npos);
}

TEST_CASE("Type checker resolves bounded builtin overload families by call shape",
          "[typecheck][stage4c][builtins][overload]") {
  SECTION("Annotated analysis records resolved symbol keys for shape-only builtin overloads") {
    const std::string src =
        "let Std.Tuple.Range(stop: Int64): Tuple<Int64...> :: __builtin__;\n"
        "let Std.Tuple.Range(start: Int64, stop: Int64): Tuple<Int64...> :: __builtin__;\n"
        "let Std.Tuple.Range(start: Int64, stop: Int64, step: Int64): Tuple<Int64...> :: __builtin__;\n"
        "let Std.Slice<T>(tuple: Tuple<T...>, stop: UInt64): Tuple<T...> :: __builtin__;\n"
        "let Std.Slice<T>(tuple: Tuple<T...>, start: UInt64, stop: UInt64): Tuple<T...> :: __builtin__;\n"
        "let Std.Slice<T>(tuple: Tuple<T...>, start: UInt64, stop: UInt64, step: UInt64): Tuple<T...> :: __builtin__;\n"
        "let Std.String.Slice(s: String, stop: UInt64): String :: __builtin__;\n"
        "let Std.String.Slice(s: String, start: UInt64, stop: UInt64): String :: __builtin__;\n"
        "let Std.String.Find(s: String, sub: String): Int64 :: __builtin__;\n"
        "let Std.String.Find(s: String, sub: String, start: UInt64): Int64 :: __builtin__;\n"
        "(5) -> Std.Tuple.Range;\n"
        "(2, 5) -> Std.Tuple.Range;\n"
        "(1, 7, 3) -> Std.Tuple.Range;\n"
        "((1, 2, 3, 4), 2u64) -> Std.Slice;\n"
        "((1, 2, 3, 4), 1u64, 3u64) -> Std.Slice;\n"
        "((1, 2, 3, 4), 0u64, 4u64, 2u64) -> Std.Slice;\n"
        "(\"abcdef\", 3u64) -> Std.String.Slice;\n"
        "(\"abcdef\", 1u64, 4u64) -> Std.String.Slice;\n"
        "(\"abcabc\", \"bc\") -> Std.String.Find;\n"
        "(\"abcabc\", \"bc\", 2u64) -> Std.String.Find;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_bounded_builtin_overload_annotation_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower_only(parsed.value());
    REQUIRE(lowered.has_value());

    const std::unordered_set<std::string> imported_symbols;
    const auto analyzed = fleaux::frontend::type_check::analyze_program(*lowered, imported_symbols);
    REQUIRE(analyzed.has_value());
    REQUIRE(analyzed->expressions.size() == 10U);

    const std::array expected_symbol_keys = {
        std::string{"Std.Tuple.Range#0"},  std::string{"Std.Tuple.Range#1"},  std::string{"Std.Tuple.Range#2"},
        std::string{"Std.Slice#0"},        std::string{"Std.Slice#1"},        std::string{"Std.Slice#2"},
        std::string{"Std.String.Slice#0"}, std::string{"Std.String.Slice#1"}, std::string{"Std.String.Find#0"},
        std::string{"Std.String.Find#1"},
    };

    for (std::size_t index = 0; index < expected_symbol_keys.size(); ++index) {
      const auto* flow = std::get_if<fleaux::frontend::ir::IRFlowExpr>(&analyzed->expressions[index].expr.node);
      REQUIRE(flow != nullptr);
      const auto* name_ref = std::get_if<fleaux::frontend::ir::IRNameRef>(&flow->rhs);
      REQUIRE(name_ref != nullptr);
      REQUIRE(name_ref->resolved_symbol_key.has_value());
      REQUIRE(*name_ref->resolved_symbol_key == expected_symbol_keys[index]);
    }
  }

  SECTION("Bare Std.Slice reference is rejected because the overload set is ambiguous") {
    const std::string src =
        "let Std.Slice<T>(tuple: Tuple<T...>, stop: UInt64): Tuple<T...> :: __builtin__;\n"
        "let Std.Slice<T>(tuple: Tuple<T...>, start: UInt64, stop: UInt64): Tuple<T...> :: __builtin__;\n"
        "let Std.Slice<T>(tuple: Tuple<T...>, start: UInt64, stop: UInt64, step: UInt64): Tuple<T...> :: __builtin__;\n"
        "let Keep(value: Any): Any = value;\n"
        "Std.Slice -> Keep;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_slice_ambiguous_ref_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Ambiguous overloaded function reference") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("multiple overloads") != std::string::npos);
  }
}

TEST_CASE("Type checker matrix: Stage-4d Std.Match semantics", "[typecheck][stage4d][match]") {
  SECTION("Std.Match accepts literal, predicate, wildcard, and mixed handler forms") {
    const std::string src =
        "let Std.Match(value: Any, cases: Any...): Any :: __builtin__;\n"
        "let Std.Mod<T: Int64 | UInt64 | Float64>(lhs: T, rhs: T): T :: "
        "__builtin__;\n"
        "let Std.Equal<T>(lhs: T, rhs: T): Bool :: __builtin__;\n"
        "let Std.ToString(value: Any): String :: __builtin__;\n"
        "let Std.String.Join(sep: String, items: Tuple<String...>): String :: __builtin__;\n"
        "let IsEven(n: Int64): Bool = ((n, 2) -> Std.Mod, 0) -> Std.Equal;\n"
        "let NeedsString(x: String): String = x;\n"
        "(8,\n"
        "  (0, (): String = \"zero\"),\n"
        "  (IsEven, (): String = \"even\"),\n"
        "  (_, (n: Int64): String = (\"\", (\"n=\", (n) -> Std.ToString)) -> Std.String.Join)\n"
        ") -> Std.Match -> NeedsString;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4d_match_valid_semantics.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Match rejects predicate patterns that do not return Bool") {
    const std::string src =
        "let Std.Match(value: Any, cases: Any...): Any :: __builtin__;\n"
        "(1,\n"
        "  ((n: Int64): Int64 = n, (): String = \"bad\"),\n"
        "  (_, (): String = \"ok\")\n"
        ") -> Std.Match;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4d_match_predicate_return_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Invalid Std.Match predicate pattern") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("return Bool") != std::string::npos);
  }

  SECTION("Std.Match rejects literal patterns incompatible with the subject type") {
    const std::string src =
        "let Std.Match(value: Any, cases: Any...): Any :: __builtin__;\n"
        "(1,\n"
        "  (\"one\", (): String = \"bad\"),\n"
        "  (_, (): String = \"ok\")\n"
        ") -> Std.Match;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4d_match_literal_type_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in Std.Match pattern") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("compatible with the matched value") != std::string::npos);
  }

  SECTION("Std.Match rejects predicate patterns whose parameter type is incompatible with the subject") {
    const std::string src =
        "let Std.Match(value: Any, cases: Any...): Any :: __builtin__;\n"
        "let Std.Equal<T>(lhs: T, rhs: T): Bool :: __builtin__;\n"
        "(1,\n"
        "  ((s: String): Bool = (1, 1) -> Std.Equal, (): String = \"bad\"),\n"
        "  (_, (): String = \"ok\")\n"
        ") -> Std.Match;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4d_match_predicate_param_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Invalid Std.Match predicate pattern") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("(S) => Bool") != std::string::npos);
  }

  SECTION("Std.Match rejects handlers that are not callable as () => R or (S) => R") {
    const std::string src =
        "let Std.Match(value: Any, cases: Any...): Any :: __builtin__;\n"
        "(1,\n"
        "  (0, \"bad\"),\n"
        "  (_, (): String = \"ok\")\n"
        ") -> Std.Match;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4d_match_handler_shape_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Invalid Std.Match handler") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("() => R") != std::string::npos);
  }

  SECTION("Std.Match rejects handlers whose parameter type is incompatible with the subject") {
    const std::string src =
        "let Std.Match(value: Any, cases: Any...): Any :: __builtin__;\n"
        "(1,\n"
        "  (0, (s: String): String = s),\n"
        "  (_, (): String = \"ok\")\n"
        ") -> Std.Match;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4d_match_handler_param_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Invalid Std.Match handler") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("(S) => R") != std::string::npos);
  }

  SECTION("Std.Match rejects mutually incompatible handler returns") {
    const std::string src =
        "let Std.Match(value: Any, cases: Any...): Any :: __builtin__;\n"
        "(1,\n"
        "  (0, (): String = \"zero\"),\n"
        "  (_, (): Int64 = 1)\n"
        ") -> Std.Match;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4d_match_return_compat_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in Std.Match handlers") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("mutually compatible") != std::string::npos);
  }
}

TEST_CASE("Type checker matrix: Stage-4e generic and union inference hardening", "[typecheck][stage4e][generics]") {
  SECTION("Nullable generic return inference is not sensitive to fixed-first union member order") {
    const std::string src =
        "let PassMaybe<T>(value: Null | T): Null | T :: __builtin__;\n"
        "let GetNull(): Null :: __builtin__;\n"
        "let NeedsNull(value: Null): Null = value;\n"
        "() -> GetNull -> PassMaybe -> NeedsNull;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4e_nullable_union_fixed_first_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Nullable generic passthrough binds the non-null channel as a union when needed") {
    const std::string src =
        "let PassMaybe<T>(value: T | Null): T | Null :: __builtin__;\n"
        "let GetMaybeMixed(): String | Int64 | Null :: __builtin__;\n"
        "let NeedsMaybeMixed(value: String | Int64 | Null): String | Int64 | Null = value;\n"
        "() -> GetMaybeMixed -> PassMaybe -> NeedsMaybeMixed;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4e_nullable_union_multimember_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Applied generic passthrough preserves a fixed-first nullable payload channel") {
    const std::string src =
        "let PassResult<T>(value: Result<Null | T, String>): Result<Null | T, String> :: __builtin__;\n"
        "let GetNullResult(): Result<Null, String> :: __builtin__;\n"
        "let NeedsNullResult(value: Result<Null, String>): Result<Null, String> = value;\n"
        "() -> GetNullResult -> PassResult -> NeedsNullResult;\n" "type Result<T, E> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4e_applied_nullable_union_fixed_first_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Applied generic passthrough preserves a multi-member nullable payload channel") {
    const std::string src =
        "let PassResult<T>(value: Result<T | Null, String>): Result<T | Null, String> :: __builtin__;\n"
        "let GetMaybeMixedResult(): Result<String | Int64 | Null, String> :: __builtin__;\n"
        "let NeedsMaybeMixedResult(value: Result<String | Int64 | Null, String>): Result<String | Int64 | Null, "
        "String> = value;\n"
        "() -> GetMaybeMixedResult -> PassResult -> NeedsMaybeMixedResult;\n" "type Result<T, E> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4e_applied_nullable_union_multimember_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Existing concrete bindings do not widen across a later nullable union argument") {
    const std::string src =
        "let PairMaybe<T>(value: T, maybe: Null | T): T :: __builtin__;\n"
        "let GetMaybeMixed(): String | Int64 | Null :: __builtin__;\n"
        "(1, () -> GetMaybeMixed) -> PairMaybe;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4e_nullable_union_existing_binding_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument 1") != std::string::npos);
  }
}

TEST_CASE("Type checker matrix: Stage-4h diagnostics taxonomy", "[typecheck][stage4h][diagnostics]") {
  SECTION("Binding diagnostics identify the missing symbol") {
    const std::string src = "MissingTopLevel;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4h_missing_symbol_diagnostic.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Unresolved symbol") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("MissingTopLevel") != std::string::npos);
  }

  SECTION("Exact arity mismatches report expected and actual argument counts") {
    const std::string src =
        "let Sum(a: Int64, b: Int64): Int64 = a;\n"
        "(1) -> Sum;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4h_exact_arity_diagnostic.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects 2 argument(s) but got 1") != std::string::npos);
  }

  SECTION("Variadic minimum arity mismatches report the fixed-argument floor") {
    const std::string src =
        "let FirstOf<T>(head: T, tail: T...): T = head;\n"
        "() -> FirstOf;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4h_variadic_minimum_arity_diagnostic.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("requires at least 1 argument(s)") != std::string::npos);
  }

  SECTION("Overload mismatches report the no-match family and candidate list") {
    const std::string src =
        "let Echo(x: Int64): Int64 = x;\n"
        "let Echo(x: String): String = x;\n"
        "(1.5) -> Echo;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4h_overload_no_match_diagnostic.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("No overload of Echo matches the provided argument types") != std::string::npos);
    REQUIRE(lowered.error().hint->find("Candidates:") != std::string::npos);
  }

  SECTION("Overload ambiguity reports the ambiguous-call family") {
    const std::string src =
        "let GetNever(): Never :: __builtin__;\n"
        "let Echo(x: Int64): Int64 = x;\n"
        "let Echo(x: String): String = x;\n"
        "() -> GetNever -> Echo;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4h_overload_ambiguity_diagnostic.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Ambiguous overloaded call target") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("multiple matching overloads") != std::string::npos);
    REQUIRE(lowered.error().hint->find("Candidates:") != std::string::npos);
  }

  SECTION("Overload ambiguity with explicit type arguments reports only matching filtered candidates") {
    const std::string src =
        "let Make<T>(x: T): T = x;\n"
        "let Make<T>(x: T): Any = x;\n"
        "let Make(x: Int64): Int64 = x;\n"
        "(\"ok\") -> Make<String>;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4h_overload_filtered_ambiguity_diagnostic.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Ambiguous overloaded call target") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Make(T): T") != std::string::npos);
    REQUIRE(lowered.error().hint->find("Make(T): Any") != std::string::npos);
    REQUIRE(lowered.error().hint->find("Make(Int64): Int64") == std::string::npos);
  }

  SECTION("Explicit type arguments keep no-shape overload diagnostics narrowed to filtered candidates") {
    const std::string src =
        "let Make<T>(x: T): T = x;\n"
        "let Make<T>(x: T, y: T): T = x;\n"
        "let Make(x: Int64): Int64 = x;\n"
        "(1, 2, 3) -> Make<String>;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4h_overload_filtered_no_shape_diagnostic.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Make has no overload that accepts 3 argument(s)") != std::string::npos);
    REQUIRE(lowered.error().hint->find("Make(T): T") != std::string::npos);
    REQUIRE(lowered.error().hint->find("Make(T, T): T") != std::string::npos);
    REQUIRE(lowered.error().hint->find("Make(Int64): Int64") == std::string::npos);
  }

  SECTION("Explicit type arguments keep no-match overload diagnostics narrowed to matching filtered candidates") {
    const std::string src =
        "let Make<T>(x: T): T = x;\n"
        "let Make<T>(x: Tuple<T>): T :: __builtin__;\n"
        "let Make(x: Int64): Int64 = x;\n"
        "(1.5) -> Make<String>;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4h_overload_filtered_no_match_diagnostic.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("No overload of Make matches the provided argument types") != std::string::npos);
    REQUIRE(lowered.error().hint->find("Make(T): T") != std::string::npos);
    REQUIRE(lowered.error().hint->find("Make(Tuple<T>): T") != std::string::npos);
    REQUIRE(lowered.error().hint->find("Make(Int64): Int64") == std::string::npos);
  }

  SECTION("Bounded numeric builtins reject implicit Float64 promotion") {
    const std::string src =
        "let Std.Add<T: Int64 | UInt64 | Float64>(lhs: T, rhs: T): T :: __builtin__;\n"
        "(1.0, 2) -> Std.Add;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4h_builtin_contract_float_promotion_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Numbers are not converted implicitly") != std::string::npos);
    REQUIRE(lowered.error().hint->find("Std.ToFloat64") != std::string::npos);
  }

}

TEST_CASE("Type checker matrix: Stage-4c Std.Exit surface contract", "[typecheck][stage4c][exit]") {
  SECTION("Std.Exit accepts nullary call shape") {
    const std::string src =
        "let Std.Exit(): Never :: __builtin__;\n"
        "let Std.Exit(code: Int64): Never :: __builtin__;\n"
        "() -> Std.Exit;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_exit_nullary_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Exit accepts unary Int64 call shape") {
    const std::string src =
        "let Std.Exit(): Never :: __builtin__;\n"
        "let Std.Exit(code: Int64): Never :: __builtin__;\n"
        "(1) -> Std.Exit;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_exit_unary_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Exit rejects Float64 argument") {
    const std::string src =
        "let Std.Exit(): Never :: __builtin__;\n"
        "let Std.Exit(code: Int64): Never :: __builtin__;\n"
        "(1.25) -> Std.Exit;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_exit_float_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument 0") != std::string::npos);
  }

  SECTION("Std.Exit rejects more than one argument") {
    const std::string src =
        "let Std.Exit(): Never :: __builtin__;\n"
        "let Std.Exit(code: Int64): Never :: __builtin__;\n"
        "(1, 2) -> Std.Exit;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_exit_too_many_args.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("has no overload that accepts 2 argument") != std::string::npos);
  }

  SECTION("Never return values flow into any declared return type") {
    const std::string src =
        "let Std.Exit(): Never :: __builtin__;\n"
        "let Std.Exit(code: Int64): Never :: __builtin__;\n"
        "let Stops(): Int64 = () -> Std.Exit;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_exit_never_return_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }
}

TEST_CASE("Type checker matrix: Stage-4c Std.Input surface contract", "[typecheck][stage4c][input]") {
  SECTION("Std.Input accepts nullary and prompt call shapes") {
    const std::string src =
        "let Std.Input(): String :: __builtin__;\n"
        "let Std.Input(prompt: String): String :: __builtin__;\n"
        "() -> Std.Input;\n"
        "\"name> \" -> Std.Input;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_input_surface_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Annotated analysis records the resolved Std.Input overload symbol key") {
    const std::string src =
        "let Std.Input(): String :: __builtin__;\n"
        "let Std.Input(prompt: String): String :: __builtin__;\n"
        "() -> Std.Input;\n"
        "\"name> \" -> Std.Input;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_input_overload_annotation_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower_only(parsed.value());
    REQUIRE(lowered.has_value());

    const std::unordered_set<std::string> imported_symbols;
    const auto analyzed = fleaux::frontend::type_check::analyze_program(*lowered, imported_symbols);
    REQUIRE(analyzed.has_value());
    REQUIRE(analyzed->expressions.size() == 2U);

    const auto* nullary_flow = std::get_if<fleaux::frontend::ir::IRFlowExpr>(&analyzed->expressions[0].expr.node);
    REQUIRE(nullary_flow != nullptr);
    const auto* nullary_name = std::get_if<fleaux::frontend::ir::IRNameRef>(&nullary_flow->rhs);
    REQUIRE(nullary_name != nullptr);
    REQUIRE(nullary_name->resolved_symbol_key.has_value());
    REQUIRE(*nullary_name->resolved_symbol_key == "Std.Input#0");

    const auto* prompt_flow = std::get_if<fleaux::frontend::ir::IRFlowExpr>(&analyzed->expressions[1].expr.node);
    REQUIRE(prompt_flow != nullptr);
    const auto* prompt_name = std::get_if<fleaux::frontend::ir::IRNameRef>(&prompt_flow->rhs);
    REQUIRE(prompt_name != nullptr);
    REQUIRE(prompt_name->resolved_symbol_key.has_value());
    REQUIRE(*prompt_name->resolved_symbol_key == "Std.Input#1");
  }

  SECTION("Bare Std.Input reference is rejected because the overload set is ambiguous") {
    const std::string src =
        "let Std.Input(): String :: __builtin__;\n"
        "let Std.Input(prompt: String): String :: __builtin__;\n"
        "let Keep(value: Any): Any = value;\n"
        "Std.Input -> Keep;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_input_ambiguous_ref_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Ambiguous overloaded function reference") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("multiple overloads") != std::string::npos);
  }
}

TEST_CASE("Type checker matrix: Stage-4c user overload dispatch", "[typecheck][stage4c][overload]") {
  SECTION("User-defined overloads are accepted in direct call position") {
    const std::string src =
        "let Echo(x: Int64): Int64 = x;\n"
        "let Echo(x: String): String = x;\n"
        "(1) -> Echo;\n"
        "(\"ok\") -> Echo;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_user_overload_direct_calls_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Annotated analysis records the resolved overload symbol key") {
    const std::string src =
        "let Echo(x: Int64): Int64 = x;\n"
        "let Echo(x: String): String = x;\n"
        "(1) -> Echo;\n"
        "(\"ok\") -> Echo;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_user_overload_annotation_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower_only(parsed.value());
    REQUIRE(lowered.has_value());

    const std::unordered_set<std::string> imported_symbols;
    const auto analyzed = fleaux::frontend::type_check::analyze_program(*lowered, imported_symbols);
    REQUIRE(analyzed.has_value());
    REQUIRE(analyzed->expressions.size() == 2U);

    const auto* int_flow = std::get_if<fleaux::frontend::ir::IRFlowExpr>(&analyzed->expressions[0].expr.node);
    REQUIRE(int_flow != nullptr);
    const auto* int_name = std::get_if<fleaux::frontend::ir::IRNameRef>(&int_flow->rhs);
    REQUIRE(int_name != nullptr);
    REQUIRE(int_name->resolved_symbol_key.has_value());
    REQUIRE(*int_name->resolved_symbol_key == "Echo#0");

    const auto* string_flow = std::get_if<fleaux::frontend::ir::IRFlowExpr>(&analyzed->expressions[1].expr.node);
    REQUIRE(string_flow != nullptr);
    const auto* string_name = std::get_if<fleaux::frontend::ir::IRNameRef>(&string_flow->rhs);
    REQUIRE(string_name != nullptr);
    REQUIRE(string_name->resolved_symbol_key.has_value());
    REQUIRE(*string_name->resolved_symbol_key == "Echo#1");
  }

  SECTION("Bare overloaded user-function references remain ambiguous in value position") {
    const std::string src =
        "let Echo(x: Int64): Int64 = x;\n"
        "let Echo(x: String): String = x;\n"
        "let Keep(value: Any): Any = value;\n"
        "Echo -> Keep;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_user_overload_value_ref_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Ambiguous overloaded function reference") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("multiple overloads") != std::string::npos);
  }
}

TEST_CASE("Type checker matrix: Stage-4c structural Shape and Indices contract", "[typecheck][stage4c][array]") {
  SECTION("Std.Array.ReshapeND accepts structural Tuple<UInt64...> shape") {
    const std::string src =
        "let Std.Array.ReshapeND(flat_array: Tuple<Any...>, shape: Tuple<UInt64...>): Any :: __builtin__;\n"
        "let NeedsAny(x: Any): Any = x;\n"
        "((1, 2, 3, 4), (2u64, 2u64)) -> Std.Array.ReshapeND -> NeedsAny;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_array_reshapend_structural_shape_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Array.ReshapeND rejects fractional structural shape elements") {
    const std::string src =
        "let Std.Array.ReshapeND(flat_array: Tuple<Any...>, shape: Tuple<UInt64...>): Any :: __builtin__;\n"
        "((1, 2, 3, 4), (2u64, 2.5)) -> Std.Array.ReshapeND;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_array_reshapend_fractional_shape_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

}

TEST_CASE("Type checker rejects negative string and file bounds before runtime", "[typecheck][builtins][string][io]") {
  SECTION("Std.String.CharAt rejects negative indices") {
    const std::string src =
        "let Std.String.CharAt(s: String, index: UInt64): String :: __builtin__;\n"
        "(\"abc\", -1) -> Std.String.CharAt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_string_charat_negative_index_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Std.String.CharAt expects argument 1") != std::string::npos);
  }

  SECTION("Std.String.Slice rejects negative bounds") {
    const std::string src =
        "let Std.String.Slice(s: String, start: UInt64, stop: UInt64): String :: __builtin__;\n"
        "(\"abcdef\", -1, 3u64) -> Std.String.Slice;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_string_slice_negative_start_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Std.String.Slice expects argument 1") != std::string::npos);
  }

  SECTION("Std.String.Find rejects negative start indices") {
    const std::string src =
        "let Std.String.Find(s: String, sub: String, start: UInt64): Int64 :: __builtin__;\n"
        "(\"abcabc\", \"bc\", -1) -> Std.String.Find;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_string_find_negative_start_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Std.String.Find expects argument 2") != std::string::npos);
  }

  SECTION("Std.File.ReadChunk rejects negative nbytes") {
    const std::string src =
        "type FileHandle = Any;\n"
        "let Std.File.ReadChunk(handle: FileHandle, nbytes: UInt64): Tuple<FileHandle, String, Bool> :: __builtin__;\n"
        "let Open(): FileHandle :: __builtin__;\n"
        "() -> Open -> (_, -1) -> Std.File.ReadChunk;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_file_readchunk_negative_nbytes_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Std.File.ReadChunk expects argument 1") != std::string::npos);
  }
}

TEST_CASE("Type checker matrix: Stage-4o opaque handle builtin contracts",
          "[typecheck][stage4o][handles][task][random][io]") {
  SECTION("Std.Task.Await rejects non-task values through its typed TaskHandle<T> signature") {
    const std::string src =
        "type Result<T, E> :: __builtin__;\n"
        "type TaskHandle<T> :: __builtin__;\n"
        "let Std.Task.Await<T>(task: TaskHandle<T>): Result<T, String> :: __builtin__;\n"
        "1 -> Std.Task.Await;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4o_task_await_non_handle_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Std.Task.Await expects argument 0") != std::string::npos);
  }

  SECTION("Std.Task.AwaitAll rejects non-task tuples through its typed Tuple<TaskHandle<T>...> signature") {
    const std::string src =
        "type Result<T, E> :: __builtin__;\n"
        "type TaskHandle<T> :: __builtin__;\n"
        "let Std.Task.AwaitAll<T>(tasks: Tuple<TaskHandle<T>...>): Result<Tuple<T...>, Tuple<Int64, String>> :: "
        "__builtin__;\n"
        "((1, 2)) -> Std.Task.AwaitAll;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4o_task_awaitall_non_handle_tuple_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Std.Task.AwaitAll expects argument 0") != std::string::npos);
  }

  SECTION("Std.Task.AwaitAll rejects tasks with different result types") {
    const std::string src =
        "type Result<T, E> :: __builtin__;\n"
        "type TaskHandle<T> :: __builtin__;\n"
        "let Std.Task.AwaitAll<T>(tasks: Tuple<TaskHandle<T>...>): Result<Tuple<T...>, Tuple<Int64, String>> :: "
        "__builtin__;\n"
        "let IntTask(): TaskHandle<Int64> :: __builtin__;\n"
        "let StringTask(): TaskHandle<String> :: __builtin__;\n"
        "((IntTask, StringTask)) -> Std.Task.AwaitAll;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4o_task_awaitall_mixed_results_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
  }

  SECTION("Std.Task.WithTimeout rejects non-task values through its typed TaskHandle<T> signature") {
    const std::string src =
        "type Result<T, E> :: __builtin__;\n"
        "type TaskHandle<T> :: __builtin__;\n"
        "let Std.Task.WithTimeout<T>(task: TaskHandle<T>, timeout_ms: UInt64): Result<T, String> :: __builtin__;\n"
        "(1, 50u64) -> Std.Task.WithTimeout;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4o_task_timeout_non_handle_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Std.Task.WithTimeout expects argument 0") != std::string::npos);
  }

}

TEST_CASE("Type checker matrix: Stage-4p exact UInt64 builtin contracts", "[typecheck][stage4p][uint64][builtins]") {
}

TEST_CASE("Type checker matrix: Stage-4m exit and tuple range integer builtin contracts",
          "[typecheck][stage4m][control][tuple]") {
  SECTION("Std.Tuple.Range still accepts signed Int64 values through widened builtin declarations") {
    const std::string src =
        "let Std.Tuple.Range(start: Any, stop: Any, step: Any): Tuple<Int64...> :: __builtin__;\n"
        "let NeedsInts(values: Tuple<Int64...>): Tuple<Int64...> = values;\n"
        "(-3, 4, 2) -> Std.Tuple.Range -> NeedsInts;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4m_tuple_range_signed_int_contract_accepts.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }
}

TEST_CASE("Type checker matrix: Stage-3r Std.Task result-shape tightening", "[typecheck][stage3r][generics]") {
  SECTION("Std.Task.Await propagates Result<Any, String>") {
    const std::string src =
        "let Std.Task.Await(task: TaskHandle<Int64>): Result<Any, String> :: __builtin__;\n"
        "let MakeTask(): TaskHandle<Int64> :: __builtin__;\n"
        "let NeedsAwaitResult(r: Result<Any, String>): Result<Any, String> = r;\n"
        "() -> MakeTask -> Std.Task.Await -> NeedsAwaitResult;\n" "type Result<T, E> :: __builtin__;\ntype TaskHandle<T> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3r_task_await_result_shape.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Task.Await rejects scalar unary inputs during type checking") {
    const std::string src =
        "type TaskHandle<T> :: __builtin__;\n"
        "let Std.Task.Await(task: TaskHandle<Int64>): Result<Any, String> :: __builtin__;\n"
        "1 -> Std.Task.Await;\n" "type Result<T, E> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3r_task_await_scalar_reject.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument 0") != std::string::npos);
  }

  SECTION("Std.Task.WithTimeout propagates Result<Any, String>") {
    const std::string src =
        "let Std.Task.WithTimeout(task: TaskHandle<Int64>, timeout_ms: UInt64): Result<Any, String> :: __builtin__;\n"
        "let MakeTask(): TaskHandle<Int64> :: __builtin__;\n"
        "let NeedsTimeoutResult(r: Result<Any, String>): Result<Any, String> = r;\n"
        "(() -> MakeTask, 50u64) -> Std.Task.WithTimeout -> NeedsTimeoutResult;\n"
        "type Result<T, E> :: __builtin__;\ntype TaskHandle<T> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3r_task_timeout_result_shape.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Task.WithTimeout rejects scalar task inputs during type checking") {
    const std::string src =
        "type TaskHandle<T> :: __builtin__;\n"
        "let Std.Task.WithTimeout(task: TaskHandle<Int64>, timeout_ms: UInt64): Result<Any, String> :: __builtin__;\n"
        "(1, 50u64) -> Std.Task.WithTimeout;\n" "type Result<T, E> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3r_task_timeout_scalar_task_reject.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument 0") != std::string::npos);
  }

  SECTION("Std.Task.AwaitAll propagates Result<Tuple<Any...>, Tuple<Int64, String>>") {
    const std::string src =
        "let Std.Task.AwaitAll(tasks: Tuple<TaskHandle<Int64>...>): Result<Tuple<Any...>, Tuple<Int64, String>> :: "
        "__builtin__;\n"
        "let MakeTasks(): Tuple<TaskHandle<Int64>...> :: __builtin__;\n"
        "let NeedsTasks(r: Result<Tuple<Any...>, Tuple<Int64, String>>): Result<Tuple<Any...>, Tuple<Int64, "
        "String>> = r;\n"
        "() -> MakeTasks -> Std.Task.AwaitAll -> NeedsTasks;\n" "type Result<T, E> :: __builtin__;\ntype TaskHandle<T> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3r_task_awaitall_result_shape.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Task.AwaitAll rejects scalar unary inputs during type checking") {
    const std::string src =
        "type TaskHandle<T> :: __builtin__;\n"
        "let Std.Task.AwaitAll(tasks: Tuple<TaskHandle<Int64>...>): Result<Tuple<Any...>, Tuple<Int64, String>> :: "
        "__builtin__;\n"
        "1 -> Std.Task.AwaitAll;\n" "type Result<T, E> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3r_task_awaitall_scalar_reject.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument 0") != std::string::npos);
  }

  SECTION("Std.Task.Cancel returns Bool for TaskHandle input") {
    const std::string src =
        "type TaskHandle<T> :: __builtin__;\n"
        "let Std.Task.Cancel(task: TaskHandle<Int64>): Bool :: __builtin__;\n"
        "let MakeTask(): TaskHandle<Int64> :: __builtin__;\n"
        "let NeedsBool(value: Bool): Bool = value;\n"
        "() -> MakeTask -> Std.Task.Cancel -> NeedsBool;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3r_task_cancel_bool_shape.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Task.Cancel rejects scalar unary inputs during type checking") {
    const std::string src =
        "type TaskHandle<T> :: __builtin__;\n"
        "let Std.Task.Cancel(task: TaskHandle<Int64>): Bool :: __builtin__;\n"
        "1 -> Std.Task.Cancel;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3r_task_cancel_scalar_reject.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument 0") != std::string::npos);
  }

  SECTION("Std.Task.AwaitAll rejects downstream error-shape mismatch") {
    const std::string src =
        "let Std.Task.AwaitAll(tasks: Tuple<TaskHandle<Int64>...>): Result<Tuple<Any...>, Tuple<Int64, String>> :: "
        "__builtin__;\n"
        "let MakeTasks(): Tuple<TaskHandle<Int64>...> :: __builtin__;\n"
        "let NeedsWrongShape(r: Result<Tuple<Any...>, Tuple<String, Int64>>): Result<Tuple<Any...>, Tuple<String, "
        "Int64>> = r;\n"
        "() -> MakeTasks -> Std.Task.AwaitAll -> NeedsWrongShape;\n"
        "type Result<T, E> :: __builtin__;\ntype TaskHandle<T> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3r_task_awaitall_error_shape_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }
}

TEST_CASE("Type checker matrix: Stage-3s Std.Array 2-D generic tightening", "[typecheck][stage3s][generics]") {
  SECTION("Std.Array.SetAt2D preserves nested grid element type") {
    const std::string src =
        "let Std.Array.SetAt2D<T>(grid: Tuple<Tuple<T...>...>, row: UInt64, col: UInt64, value: T): "
        "Tuple<Tuple<T...>...> :: __builtin__;\n"
        "let NeedsGrid(g: Tuple<Tuple<Int64...>...>): Tuple<Tuple<Int64...>...> = g;\n"
        "(((1, 2), (3, 4)), 0u64, 1u64, 9) -> Std.Array.SetAt2D -> NeedsGrid;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3s_array_setat2d_grid_type_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Array.SetAt2D rejects replacement value type mismatch") {
    const std::string src =
        "let Std.Array.SetAt2D<T>(grid: Tuple<Tuple<T...>...>, row: UInt64, col: UInt64, value: T): "
        "Tuple<Tuple<T...>...> :: __builtin__;\n"
        "(((1, 2), (3, 4)), 0u64, 1u64, \"oops\") -> Std.Array.SetAt2D;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3s_array_setat2d_value_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Array.Slice2D preserves nested grid element type") {
    const std::string src =
        "let Std.Array.Slice2D<T>(grid: Tuple<Tuple<T...>...>, row_start: UInt64, row_end: UInt64, col_start: UInt64, "
        "col_end: UInt64): Tuple<Tuple<T...>...> :: __builtin__;\n"
        "let NeedsGrid(g: Tuple<Tuple<Int64...>...>): Tuple<Tuple<Int64...>...> = g;\n"
        "(((1, 2, 3), (4, 5, 6)), 0u64, 2u64, 1u64, 3u64) -> Std.Array.Slice2D -> NeedsGrid;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3s_array_slice2d_grid_type_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Array.Reshape preserves nested grid element type") {
    const std::string src =
        "let Std.Array.Reshape<T>(flat_array: Tuple<T...>, rows: UInt64, cols: UInt64): Tuple<Tuple<T...>...> :: "
        "__builtin__;\n"
        "let NeedsGrid(g: Tuple<Tuple<Int64...>...>): Tuple<Tuple<Int64...>...> = g;\n"
        "((1, 2, 3, 4), 2u64, 2u64) -> Std.Array.Reshape -> NeedsGrid;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3s_array_reshape_grid_type_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Array.Transpose2D preserves nested grid element type") {
    const std::string src =
        "let Std.Array.Transpose2D<T>(grid: Tuple<Tuple<T...>...>): Tuple<Tuple<T...>...> :: __builtin__;\n"
        "let NeedsGrid(g: Tuple<Tuple<Int64...>...>): Tuple<Tuple<Int64...>...> = g;\n"
        "((1, 2), (3, 4),) -> Std.Array.Transpose2D -> NeedsGrid;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3s_array_transpose2d_grid_type_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Array.Transpose2D rejects downstream nested element mismatch") {
    const std::string src =
        "let Std.Array.Transpose2D<T>(grid: Tuple<Tuple<T...>...>): Tuple<Tuple<T...>...> :: __builtin__;\n"
        "let NeedsIntGrid(g: Tuple<Tuple<Int64...>...>): Tuple<Tuple<Int64...>...> = g;\n"
        "((\"a\", \"b\"), (\"c\", \"d\"),) -> Std.Array.Transpose2D -> NeedsIntGrid;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3s_array_transpose2d_output_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }
}

TEST_CASE("Type checker matrix: Stage-3t Result and Parallel error-channel tightening",
          "[typecheck][stage3t][generics]") {
  SECTION("Std.OS.Env nullable output composes through Std.Try result channel") {
    const std::string src =
        "let Std.OS.Env(name: String): String | Null :: __builtin__;\n"
        "let Std.Try<T, U>(value: T, func: (T) => U): Result<U, String> :: __builtin__;\n"
        "let EchoMaybe(value: String | Null): String | Null = value;\n"
        "let NeedsTryResult(r: Result<String | Null, String>): Result<String | Null, String> = r;\n"
        "(\"HOME\") -> Std.OS.Env -> (_, EchoMaybe) -> Std.Try -> NeedsTryResult;\n" "type Result<T, E> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3t_os_env_try_nullable_composition_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.OS.Env nullable output rejects Std.Try callback requiring non-null input") {
    const std::string src =
        "let Std.OS.Env(name: String): String | Null :: __builtin__;\n"
        "let Std.Try<T, U>(value: T, func: (T) => U): Result<U, String> :: __builtin__;\n"
        "let NeedsString(value: String): String = value;\n"
        "(\"HOME\") -> Std.OS.Env -> (_, NeedsString) -> Std.Try;\n" "type Result<T, E> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3t_os_env_try_callback_nullable_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Std.Try expects argument 1") != std::string::npos);
  }

  SECTION("Std.Try propagates String error channel") {
    const std::string src =
        "let Std.Try<T, U>(value: T, func: (T) => U): Result<U, String> :: __builtin__;\n"
        "let Id(x: Int64): Int64 = x;\n"
        "let NeedsTryResult(r: Result<Int64, String>): Result<Int64, String> = r;\n"
        "(1, Id) -> Std.Try -> NeedsTryResult;\n" "type Result<T, E> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3t_try_string_error_channel.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Parallel.Map propagates tuple index+message error shape") {
    const std::string src =
        "let Std.Parallel.Map<T, U>(items: Tuple<T...>, func: (T) => U): Result<Tuple<U...>, Tuple<Int64, String>> :: "
        "__builtin__;\n"
        "let ToInt(x: Int64): Int64 = x;\n"
        "let NeedsMapped(r: Result<Tuple<Int64...>, Tuple<Int64, String>>): Result<Tuple<Int64...>, Tuple<Int64, "
        "String>> = r;\n"
        "((1, 2), ToInt) -> Std.Parallel.Map -> NeedsMapped;\n" "type Result<T, E> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3t_parallel_map_error_shape_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Parallel.WithOptions propagates tuple index+message error shape") {
    const std::string src =
        "let Std.Dict.Create(): Dict<Never, Never> :: __builtin__;\n"
        "let Std.Dict.Set<K, V>(dict: Dict<K, V>, key: K, value: V): Dict<K, V> :: __builtin__;\n"
        "let Std.Parallel.WithOptions<T, U>(items: Tuple<T...>, func: (T) => U, options: Dict<String, Any>): "
        "Result<Tuple<U...>, Tuple<Int64, String>> :: __builtin__;\n"
        "let ToInt(x: Int64): Int64 = x;\n"
        "let NeedsMapped(r: Result<Tuple<Int64...>, Tuple<Int64, String>>): Result<Tuple<Int64...>, Tuple<Int64, "
        "String>> = r;\n"
        "let BuildOptions(): Dict<String, Any> = (() -> Std.Dict.Create, \"max_workers\", 2) -> Std.Dict.Set;\n"
        "((1, 2), ToInt, () -> BuildOptions) -> Std.Parallel.WithOptions -> NeedsMapped;\n"
        "type Result<T, E> :: __builtin__;\ntype Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed =
        parser.parse_program(src, "typecheck_stage3t_parallel_with_options_error_shape_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Parallel.Reduce rejects downstream error-shape mismatch") {
    const std::string src =
        "let Std.Add<T: Int64 | UInt64 | Float64>(lhs: T, rhs: T): T :: "
        "__builtin__;\n"
        "let Std.Parallel.Reduce<T, A>(items: Tuple<T...>, init: A, func: (A, T) => A): Result<A, Tuple<Int64, "
        "String>> :: __builtin__;\n"
        "let ReduceFn(acc: Int64, item: Int64): Int64 = (acc, item) -> Std.Add;\n"
        "let NeedsWrongErr(r: Result<Int64, Tuple<String, Int64>>): Result<Int64, Tuple<String, Int64>> = r;\n"
        "((1, 2, 3), 0, ReduceFn) -> Std.Parallel.Reduce -> NeedsWrongErr;\n" "type Result<T, E> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3t_parallel_reduce_error_shape_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("NeedsWrongErr expects argument 0") != std::string::npos);
  }
}

TEST_CASE("Type checker matrix: Stage-3u Std.Select and Std.Parallel.ForEach generic tightening",
          "[typecheck][stage3u][generics]") {
  SECTION("Std.Select propagates branch value type") {
    const std::string src =
        "let Std.Select<T>(condition: Bool, true_val: T, false_val: T): T :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "(True, 1, 2) -> Std.Select -> NeedsInt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3u_select_type_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Select rejects mismatched branch value types") {
    const std::string src =
        "let Std.Select<T>(condition: Bool, true_val: T, false_val: T): T :: __builtin__;\n"
        "(True, 1, \"oops\") -> Std.Select;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3u_select_branch_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Parallel.ForEach accepts callback with concrete return type") {
    const std::string src =
        "let Std.Parallel.ForEach<T, U>(items: Tuple<T...>, func: (T) => U): Result<Tuple<>, Tuple<Int64, String>> :: "
        "__builtin__;\n"
        "let Probe(x: Int64): String = \"ok\";\n"
        "let NeedsResult(r: Result<Tuple<>, Tuple<Int64, String>>): Result<Tuple<>, Tuple<Int64, String>> = r;\n"
        "((1, 2, 3), Probe) -> Std.Parallel.ForEach -> NeedsResult;\n" "type Result<T, E> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3u_parallel_foreach_return_generic.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Parallel.ForEach rejects callback parameter mismatch") {
    const std::string src =
        "let Std.Parallel.ForEach<T, U>(items: Tuple<T...>, func: (T) => U): Result<Tuple<>, Tuple<Int64, String>> :: "
        "__builtin__;\n"
        "let Probe(x: String): String = x;\n"
        "((1, 2, 3), Probe) -> Std.Parallel.ForEach;\n" "type Result<T, E> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3u_parallel_foreach_param_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }
}

TEST_CASE("Type checker matrix: Stage-3v Std.Equal/Std.NotEqual generic tightening", "[typecheck][stage3v][generics]") {
  SECTION("Std.Equal propagates Bool for same-type operands") {
    const std::string src =
        "let Std.Equal<T>(lhs: T, rhs: T): Bool :: __builtin__;\n"
        "let NeedsBool(b: Bool): Bool = b;\n"
        "(1, 1) -> Std.Equal -> NeedsBool;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3v_equal_bool_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.NotEqual propagates Bool for same-type operands") {
    const std::string src =
        "let Std.NotEqual<T>(lhs: T, rhs: T): Bool :: __builtin__;\n"
        "let NeedsBool(b: Bool): Bool = b;\n"
        "(\"a\", \"b\") -> Std.NotEqual -> NeedsBool;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3v_notequal_bool_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Equal rejects mixed operand types") {
    const std::string src =
        "let Std.Equal<T>(lhs: T, rhs: T): Bool :: __builtin__;\n"
        "(1, \"1\") -> Std.Equal;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3v_equal_mixed_type_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.NotEqual rejects mixed operand types") {
    const std::string src =
        "let Std.NotEqual<T>(lhs: T, rhs: T): Bool :: __builtin__;\n"
        "(1, \"1\") -> Std.NotEqual;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3v_notequal_mixed_type_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }
}

TEST_CASE("Type checker matrix: Stage-3w Std.OS.Env nullable-string tightening", "[typecheck][stage3w][generics]") {
  SECTION("Std.OS.Env flows into nullable-string consumer") {
    const std::string src =
        "let Std.OS.Env(name: String): String | Null :: __builtin__;\n"
        "let NeedsMaybeString(value: String | Null): String | Null = value;\n"
        "(\"HOME\") -> Std.OS.Env -> NeedsMaybeString;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3w_os_env_nullable_flow_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.OS.Env rejects non-nullable downstream expectation") {
    const std::string src =
        "let Std.OS.Env(name: String): String | Null :: __builtin__;\n"
        "let NeedsString(value: String): String = value;\n"
        "(\"HOME\") -> Std.OS.Env -> NeedsString;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3w_os_env_rejects_non_nullable_sink.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("NeedsString expects argument 0") != std::string::npos);
  }
}

TEST_CASE("Type checker matrix: Stage-3x union-to-union propagation hardening", "[typecheck][stage3x][generics]") {
  SECTION("Union output flows into matching union input") {
    const std::string src =
        "let GetMaybe(): String | Null :: __builtin__;\n"
        "let NeedsMaybeString(value: String | Null): String | Null = value;\n"
        "() -> GetMaybe -> NeedsMaybeString;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3x_union_to_union_flow_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Generic passthrough preserves union shape") {
    const std::string src =
        "let Pass<T>(x: T): T :: __builtin__;\n"
        "let GetMaybe(): String | Null :: __builtin__;\n"
        "let NeedsMaybeString(value: String | Null): String | Null = value;\n"
        "() -> GetMaybe -> Pass -> NeedsMaybeString;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3x_generic_union_passthrough_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Generic T | Null accepts compatible union input with reordered members") {
    const std::string src =
        "let PassMaybe<T>(x: T | Null): T | Null :: __builtin__;\n"
        "let GetMaybe(): Null | String :: __builtin__;\n"
        "let NeedsMaybeString(value: String | Null): String | Null = value;\n"
        "() -> GetMaybe -> PassMaybe -> NeedsMaybeString;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3x_generic_nullable_union_reordered_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Generic T | Null rejects union input without nullable branch") {
    const std::string src =
        "let PassMaybe<T>(x: T | Null): T | Null :: __builtin__;\n"
        "let GetMixed(): String | Int64 :: __builtin__;\n"
        "() -> GetMixed -> PassMaybe;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed =
        parser.parse_program(src, "typecheck_stage3x_generic_nullable_union_missing_null_reject.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Union output rejects incompatible union sink") {
    const std::string src =
        "let GetMaybe(): String | Null :: __builtin__;\n"
        "let NeedsMaybeInt(value: Int64 | Null): Int64 | Null = value;\n"
        "() -> GetMaybe -> NeedsMaybeInt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3x_union_to_union_flow_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }
}

TEST_CASE("Type checker matrix: strict binding diagnostics", "[typecheck][binding]") {
  SECTION("Unresolved value symbol in let body") {
    const std::string src = "let UseMissing(x: Float64): Float64 = MissingValue;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_binding_missing_value.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Unresolved symbol") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("MissingValue") != std::string::npos);
  }

  SECTION("Unresolved top-level symbol expression") {
    const std::string src = "MissingTopLevel;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_binding_missing_top_level.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Unresolved symbol") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("MissingTopLevel") != std::string::npos);
  }

  SECTION("Unresolved unqualified call target") {
    const std::string src = "(1) -> MissingFunc;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_binding_missing_target.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Unresolved symbol") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("MissingFunc") != std::string::npos);
  }

  SECTION("Unresolved qualified call target is rejected") {
    const std::string src = "(1) -> Foo.Missing;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_binding_missing_qualified_target.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Unresolved symbol") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Foo.Missing") != std::string::npos);
  }

  SECTION("Resolved local and user function symbols still pass") {
    const std::string src =
        "let Std.Add(lhs: Float64, rhs: Float64): Float64 :: __builtin__;\n"
        "let Inc(x: Float64): Float64 = (x, 1.0) -> Std.Add;\n"
        "let UseLocal(x: Float64): Float64 = (x) -> Inc;\n"
        "(2.0) -> UseLocal;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_binding_resolved_symbols.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }
}

TEST_CASE("Type checker matrix: Stage-4b imported-qualified symbol closure", "[typecheck][binding][stage4b]") {
  SECTION("Matching imported qualified symbol without a signature is an error") {
    const std::string src =
        "let NeedsFloat(x: Float64): Float64 = x;\n"
        "(1) -> Foo.Add4 -> NeedsFloat;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4b_imported_qualified_match.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower_only(parsed.value());
    REQUIRE(lowered.has_value());

    const std::unordered_set<std::string> imported_symbols = {"Foo.Add4"};
    const auto analyzed = fleaux::frontend::type_check::analyze_program(*lowered, imported_symbols);
    REQUIRE_FALSE(analyzed.has_value());
    REQUIRE(analyzed.error().message.find("Symbol has no type signature") != std::string::npos);
    REQUIRE(analyzed.error().hint.has_value());
    REQUIRE(analyzed.error().hint->find("'Foo.Add4' is known but has no type signature") != std::string::npos);
  }

  SECTION("A value reference to an imported symbol without a signature is an error") {
    const std::string src = "let F: Any = Foo.Add4;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4b_imported_untyped_value_ref.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower_only(parsed.value());
    REQUIRE(lowered.has_value());

    const std::unordered_set<std::string> imported_symbols = {"Foo.Add4"};
    const auto analyzed = fleaux::frontend::type_check::analyze_program(*lowered, imported_symbols);
    REQUIRE_FALSE(analyzed.has_value());
    REQUIRE(analyzed.error().message.find("Symbol has no type signature") != std::string::npos);
  }

  SECTION("Mismatched qualifier reports unresolved symbol") {
    const std::string src = "(1) -> Bar.Add4;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4b_imported_qualified_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower_only(parsed.value());
    REQUIRE(lowered.has_value());

    const std::unordered_set<std::string> imported_symbols = {"Foo.Add4"};
    const auto analyzed = fleaux::frontend::type_check::analyze_program(*lowered, imported_symbols);
    REQUIRE_FALSE(analyzed.has_value());
    REQUIRE(analyzed.error().message.find("Unresolved symbol") != std::string::npos);
    REQUIRE(analyzed.error().hint.has_value());
    REQUIRE(analyzed.error().hint->find("Bar.Add4") != std::string::npos);
  }

  SECTION("Qualified lookup does not match imported unqualified tail") {
    const std::string src = "(1) -> Foo.Add4;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4b_imported_unqualified_tail_no_match.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower_only(parsed.value());
    REQUIRE(lowered.has_value());

    const std::unordered_set<std::string> imported_symbols = {"Add4"};
    const auto analyzed = fleaux::frontend::type_check::analyze_program(*lowered, imported_symbols);
    REQUIRE_FALSE(analyzed.has_value());
    REQUIRE(analyzed.error().message.find("Unresolved symbol") != std::string::npos);
    REQUIRE(analyzed.error().hint.has_value());
    REQUIRE(analyzed.error().hint->find("Foo.Add4") != std::string::npos);
  }
}

TEST_CASE("FunctionIndex matrix: Stage-4b mixed imported symbol lookup", "[typecheck][binding][stage4b]") {
  const std::string src = "let Local(x: Float64): Float64 = x;\n";
  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_stage4b_function_index_mixed_imports.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower_only(parsed.value());
  REQUIRE(lowered.has_value());

  SECTION("Qualified import does not imply unqualified lookup") {
    const std::unordered_set<std::string> imported_symbols = {"Foo.Add4"};
    const fleaux::frontend::type_system::AliasIndex alias_index(*lowered);
    const fleaux::frontend::type_system::StrongTypeIndex type_index(*lowered, alias_index);
    const fleaux::frontend::type_system::FunctionIndex index(*lowered, imported_symbols, type_index, alias_index);
    REQUIRE(index.has_qualified_symbol(std::optional<std::string>{"Foo"}, "Add4"));
    REQUIRE_FALSE(index.has_unqualified_symbol("Add4"));
  }

  SECTION("Unqualified import does not imply qualified lookup") {
    const std::unordered_set<std::string> imported_symbols = {"Add4"};
    const fleaux::frontend::type_system::AliasIndex alias_index(*lowered);
    const fleaux::frontend::type_system::StrongTypeIndex type_index(*lowered, alias_index);
    const fleaux::frontend::type_system::FunctionIndex index(*lowered, imported_symbols, type_index, alias_index);
    REQUIRE(index.has_unqualified_symbol("Add4"));
    REQUIRE_FALSE(index.has_qualified_symbol(std::optional<std::string>{"Foo"}, "Add4"));
  }

  SECTION("Explicit mixed import keeps both lookups distinct") {
    const std::unordered_set<std::string> imported_symbols = {"Foo.Add4", "Add4"};
    const fleaux::frontend::type_system::AliasIndex alias_index(*lowered);
    const fleaux::frontend::type_system::StrongTypeIndex type_index(*lowered, alias_index);
    const fleaux::frontend::type_system::FunctionIndex index(*lowered, imported_symbols, type_index, alias_index);
    REQUIRE(index.has_unqualified_symbol("Add4"));
    REQUIRE(index.has_qualified_symbol(std::optional<std::string>{"Foo"}, "Add4"));
    REQUIRE_FALSE(index.has_qualified_symbol(std::optional<std::string>{"Bar"}, "Add4"));
  }
}

TEST_CASE("Type checker matrix: Stage-4b symbolic qualifier ownership", "[typecheck][binding][stage4b]") {
  SECTION("Std owned qualifiers require an explicit import surface") {
    const std::string src =
        "(1, 2) -> Std.Add;\n"
        "(1) -> Std.Tuple.Add4;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4b_symbolic_qualifier_owned_roots.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower_only(parsed.value());
    REQUIRE(lowered.has_value());

    const std::unordered_set<std::string> imported_symbols;
    const auto analyzed = fleaux::frontend::type_check::analyze_program(*lowered, imported_symbols);
    REQUIRE_FALSE(analyzed.has_value());
    REQUIRE(analyzed.error().message.find("Unresolved symbol") != std::string::npos);
    REQUIRE(analyzed.error().hint.has_value());
    REQUIRE((analyzed.error().hint->find("Std.Add") != std::string::npos ||
             analyzed.error().hint->find("Std.Tuple.Add4") != std::string::npos));
  }

  SECTION("Explicitly imported Std symbols resolve through the imported surface") {
    const std::string src = "(1, 2) -> Std.Add;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4b_symbolic_qualifier_imported_std.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower_only(parsed.value());
    REQUIRE(lowered.has_value());

    fleaux::frontend::ir::IRLet imported_add;
    imported_add.qualifier = std::string{"Std"};
    imported_add.name = "Add";
    imported_add.is_builtin = true;
    imported_add.params = {make_ir_param("lhs", make_ir_simple_type("Int64")),
                           make_ir_param("rhs", make_ir_simple_type("Int64"))};
    imported_add.return_type = make_ir_simple_type("Int64");

    const std::unordered_set<std::string> imported_symbols = {"Std.Add"};
    const std::vector<fleaux::frontend::ir::IRLet> imported_typed_lets = {imported_add};
    const auto analyzed =
        fleaux::frontend::type_check::analyze_program(*lowered, imported_symbols, imported_typed_lets);
    REQUIRE(analyzed.has_value());
  }

  SECTION("Std-prefixed lookalike qualifier is rejected") {
    const std::string src = "(1) -> StdShadow.Add4;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4b_symbolic_qualifier_std_lookalike.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower_only(parsed.value());
    REQUIRE(lowered.has_value());

    const std::unordered_set<std::string> imported_symbols;
    const auto analyzed = fleaux::frontend::type_check::analyze_program(*lowered, imported_symbols);
    REQUIRE_FALSE(analyzed.has_value());
    REQUIRE(analyzed.error().message.find("Unresolved symbol") != std::string::npos);
    REQUIRE(analyzed.error().hint.has_value());
    REQUIRE(analyzed.error().hint->find("StdShadow.Add4") != std::string::npos);
  }
}

TEST_CASE("Type checker matrix: Stage-4g typed imported signature seeding", "[typecheck][binding][stage4g]") {
  SECTION("Typed imported signature rejects mismatched argument type") {
    const std::string src =
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "(1) -> Foo.Add4 -> NeedsInt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4g_typed_import_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower_only(parsed.value());
    REQUIRE(lowered.has_value());

    fleaux::frontend::ir::IRLet imported_add4;
    imported_add4.qualifier = std::string{"Foo"};
    imported_add4.name = "Add4";
    imported_add4.params = {make_ir_param("x", make_ir_simple_type("String"))};
    imported_add4.return_type = make_ir_simple_type("Int64");

    const std::unordered_set<std::string> imported_symbols = {"Foo.Add4"};
    const std::vector<fleaux::frontend::ir::IRLet> imported_typed_lets = {imported_add4};
    const auto analyzed =
        fleaux::frontend::type_check::analyze_program(*lowered, imported_symbols, imported_typed_lets);
    REQUIRE_FALSE(analyzed.has_value());
    REQUIRE(analyzed.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(analyzed.error().hint.has_value());
    REQUIRE(analyzed.error().hint->find("Foo.Add4 expects argument 0") != std::string::npos);
  }

  SECTION("Typed imported signature resolves matching argument and return flow") {
    const std::string src =
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "(1) -> Foo.Add4 -> NeedsInt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4g_typed_import_match.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower_only(parsed.value());
    REQUIRE(lowered.has_value());

    fleaux::frontend::ir::IRLet imported_add4;
    imported_add4.qualifier = std::string{"Foo"};
    imported_add4.name = "Add4";
    imported_add4.params = {make_ir_param("x", make_ir_simple_type("Int64"))};
    imported_add4.return_type = make_ir_simple_type("Int64");

    const std::unordered_set<std::string> imported_symbols = {"Foo.Add4"};
    const std::vector<fleaux::frontend::ir::IRLet> imported_typed_lets = {imported_add4};
    const auto analyzed =
        fleaux::frontend::type_check::analyze_program(*lowered, imported_symbols, imported_typed_lets);
    REQUIRE(analyzed.has_value());
  }
}

TEST_CASE("Type checker validates strong type environments across local and imported declarations",
          "[typecheck][types][strong]") {
  SECTION("Unknown strong types are rejected in declared signatures") {
    const std::string src = "let Echo(x: UserId): UserId = x;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_unknown_strong_type.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower_only(parsed.value());
    REQUIRE(lowered.has_value());

    const auto analyzed = fleaux::frontend::type_check::analyze_program(*lowered);
    REQUIRE_FALSE(analyzed.has_value());
    REQUIRE(analyzed.error().message.find("Unknown type") != std::string::npos);
    REQUIRE(analyzed.error().hint.has_value());
    REQUIRE(analyzed.error().hint->find("UserId") != std::string::npos);
  }

  SECTION("Imported strong types are visible to local declarations") {
    const std::string src = "let Echo(x: UserId): UserId = x;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_imported_strong_type_visible.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower_only(parsed.value());
    REQUIRE(lowered.has_value());

    const std::vector<fleaux::frontend::ir::IRTypeDecl> imported_type_decls = {
        make_ir_type_decl("UserId", make_ir_simple_type("Int64")),
    };

    const auto analyzed = fleaux::frontend::type_check::analyze_program(*lowered, {}, {}, imported_type_decls);
    REQUIRE(analyzed.has_value());
  }

  SECTION("Imported and local strong type declarations cannot collide") {
    const std::string src = "type UserId = String;\nlet Echo(x: UserId): UserId = x;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_duplicate_imported_strong_type.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower_only(parsed.value());
    REQUIRE(lowered.has_value());

    const std::vector<fleaux::frontend::ir::IRTypeDecl> imported_type_decls = {
        make_ir_type_decl("UserId", make_ir_simple_type("Int64")),
    };

    const auto analyzed = fleaux::frontend::type_check::analyze_program(*lowered, {}, {}, imported_type_decls);
    REQUIRE_FALSE(analyzed.has_value());
    REQUIRE(analyzed.error().message.find("Duplicate strong type declaration") != std::string::npos);
    REQUIRE(analyzed.error().hint.has_value());
    REQUIRE(analyzed.error().hint->find("UserId") != std::string::npos);
  }

  SECTION("Explicit type arguments resolve through imported strong types") {
    const std::string src = "(1) -> Std.Cast<UserId>;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_explicit_type_arg_imported_strong_type.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower_only(parsed.value());
    REQUIRE(lowered.has_value());

    fleaux::frontend::ir::IRLet imported_cast;
    imported_cast.qualifier = std::string{"Std"};
    imported_cast.name = "Cast";
    imported_cast.generic_params = {"T"};
    imported_cast.params = {make_ir_param("x", make_ir_simple_type("Any"))};
    imported_cast.return_type = make_ir_simple_type("T");

    const std::unordered_set<std::string> imported_symbols = {"Std.Cast"};
    const std::vector<fleaux::frontend::ir::IRLet> imported_typed_lets = {imported_cast};
    const std::vector<fleaux::frontend::ir::IRTypeDecl> imported_type_decls = {
        make_ir_type_decl("UserId", make_ir_simple_type("Int64")),
    };

    const auto analyzed = fleaux::frontend::type_check::analyze_program(*lowered, imported_symbols, imported_typed_lets,
                                                                        imported_type_decls);
    REQUIRE(analyzed.has_value());
  }

  SECTION("Nominal strong types stay distinct from their underlying builtins") {
    const std::string src = "type UserId = Int64;\nlet Echo(x: UserId): UserId = x;\n(1) -> Echo;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_nominal_strong_type_distinct.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower_only(parsed.value());
    REQUIRE(lowered.has_value());

    const auto analyzed = fleaux::frontend::type_check::analyze_program(*lowered);
    REQUIRE_FALSE(analyzed.has_value());
    REQUIRE(analyzed.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(analyzed.error().hint.has_value());
    REQUIRE(analyzed.error().hint->find("Echo expects argument 0") != std::string::npos);
  }
}

TEST_CASE("Type checker expands transparent local aliases before structural compatibility checks",
          "[typecheck][types][aliases]") {
  SECTION("Primitive aliases are transparent in parameter and return signatures") {
    const std::string src =
        "alias Name = String;\n"
        "let Echo(x: Name): Name = x;\n"
        "let NeedsString(x: String): String = x;\n"
        "(\"fleaux\") -> Echo -> NeedsString;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_alias_primitive_transparent.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Alias chains stay transparent across tuple-typed signatures") {
    const std::string src =
        "alias Pair = Tuple<Int64, Int64>;\n"
        "alias CounterPair = Pair;\n"
        "let Echo(x: CounterPair): Pair = x;\n"
        "let NeedsPair(x: Pair): Pair = x;\n"
        "(1, 2,) -> Echo -> NeedsPair;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_alias_chain_tuple_transparent.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Aliases to nominal types remain transparent to the nominal target") {
    const std::string src =
        "let Std.Cast<T>(value: Any): T :: __builtin__;\n"
        "type Id = Int64;\n"
        "alias UserId = Id;\n"
        "let MakeUserId(x: Id): UserId = x;\n"
        "let NeedsId(x: Id): Id = x;\n"
        "(1) -> Std.Cast<UserId> -> MakeUserId -> NeedsId;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_alias_nominal_target_transparent.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Aliases to function types are transparent for first-class callable values") {
    const std::string src =
        "alias Handler = (String) => Bool;\n"
        "let Keep(h: Handler): Handler = h;\n"
        "let NeedsHandler(h: (String) => Bool): (String) => Bool = h;\n"
        "let IsOk(s: String): Bool = True;\n"
        "IsOk -> Keep -> NeedsHandler;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_alias_function_target_transparent.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Imported aliases expand transparently in local declarations and imported signatures") {
    const std::string src =
        "let NeedsDistance(x: Distance): Distance = x;\n"
        "(1) -> ImportedEcho -> NeedsDistance;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_imported_alias_transparent.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower_only(parsed.value());
    REQUIRE(lowered.has_value());

    fleaux::frontend::ir::IRLet imported_echo;
    imported_echo.name = "ImportedEcho";
    imported_echo.params = {make_ir_param("x", make_ir_simple_type("Distance"))};
    imported_echo.return_type = make_ir_simple_type("Distance");

    const std::unordered_set<std::string> imported_symbols = {"ImportedEcho"};
    const std::vector<fleaux::frontend::ir::IRLet> imported_typed_lets = {imported_echo};
    const std::vector<fleaux::frontend::ir::IRAliasDecl> imported_alias_decls = {
        make_ir_alias_decl("Distance", make_ir_simple_type("Int64")),
    };

    const auto analyzed = fleaux::frontend::type_check::analyze_program(*lowered, imported_symbols, imported_typed_lets,
                                                                        {}, imported_alias_decls);
    REQUIRE(analyzed.has_value());
  }

  SECTION("Unknown alias targets are rejected during analysis") {
    const std::string src =
        "alias UserId = Missing;\n"
        "let Echo(x: UserId): UserId = x;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_alias_unknown_target.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Unknown type") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Missing") != std::string::npos);
  }

  SECTION("Direct alias cycles are rejected") {
    const std::string src =
        "alias Loop = Loop;\n"
        "let Echo(x: Loop): Loop = x;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_alias_direct_cycle.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Alias cycle detected") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Loop") != std::string::npos);
  }

  SECTION("Transitive alias cycles are rejected") {
    const std::string src =
        "alias A = B;\n"
        "alias B = C;\n"
        "alias C = A;\n"
        "let Echo(x: A): A = x;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_alias_transitive_cycle.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Alias cycle detected") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("A") != std::string::npos);
  }
}

TEST_CASE("Type checker enforces explicit type argument legality and Std.Cast semantics",
          "[typecheck][types][generics][strong]") {
  const auto analyze_program = [](const std::string& src, const std::string& source_name,
                                  const std::unordered_set<std::string>& imported_symbols = {},
                                  const std::vector<fleaux::frontend::ir::IRLet>& imported_typed_lets = {},
                                  const std::vector<fleaux::frontend::ir::IRTypeDecl>& imported_type_decls = {}) {
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, source_name);
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower_only(parsed.value());
    REQUIRE(lowered.has_value());

    return fleaux::frontend::type_check::analyze_program(*lowered, imported_symbols, imported_typed_lets,
                                                         imported_type_decls);
  };

  const auto make_imported_std_cast = [] {
    fleaux::frontend::ir::IRLet imported_cast;
    imported_cast.qualifier = std::string{"Std"};
    imported_cast.name = "Cast";
    imported_cast.generic_params = {"T"};
    imported_cast.params = {make_ir_param("value", make_ir_simple_type("Any"))};
    imported_cast.return_type = make_ir_simple_type("T");
    return imported_cast;
  };

  const auto make_builtin_opaque_type_decl = [](const std::string& name) {
    auto decl = make_ir_type_decl(name, make_ir_simple_type("Any"));
    decl.is_builtin = true;
    if (name == "TaskHandle") {
      decl.generic_params = {"T"};
    }
    return decl;
  };
  // Builtin type declaration name paired with how a program spells a value type of it.
  const std::vector<std::pair<std::string, std::string>> builtin_types = {
      {"TaskHandle", "TaskHandle<Int64>"}, {"FileHandle", "FileHandle"}, {"RandomGenerator", "RandomGenerator"}};

  SECTION("Explicit type arguments reject non-generic call targets") {
    const auto analyzed = analyze_program("let Echo(x: Int64): Int64 = x;\n(1) -> Echo<Int64>;\n",
                                          "typecheck_explicit_type_args_non_generic.fleaux");

    REQUIRE_FALSE(analyzed.has_value());
    REQUIRE(analyzed.error().message.find("Invalid explicit type argument application") != std::string::npos);
    REQUIRE(analyzed.error().hint.has_value());
    REQUIRE(analyzed.error().hint->find("Echo is not generic") != std::string::npos);
  }

  SECTION("Explicit type arguments reject wrong arity") {
    const auto analyzed = analyze_program("let Echo<T>(x: T): T = x;\n(1) -> Echo<Int64, UInt64>;\n",
                                          "typecheck_explicit_type_args_wrong_arity.fleaux");

    REQUIRE_FALSE(analyzed.has_value());
    REQUIRE(analyzed.error().message.find("Invalid explicit type argument application") != std::string::npos);
    REQUIRE(analyzed.error().hint.has_value());
    REQUIRE(analyzed.error().hint->find("expects 1 explicit type argument") != std::string::npos);
  }

  SECTION("Explicit type arguments report generic and non-generic expectations across mixed overload sets") {
    const auto analyzed = analyze_program(
        "let Make(x: Int64): Int64 = x;\n"
        "let Make<T, U>(value: Int64): Int64 = value;\n"
        "(1) -> Make<String>;\n",
        "typecheck_explicit_type_args_mixed_arity_summary.fleaux");

    REQUIRE_FALSE(analyzed.has_value());
    REQUIRE(analyzed.error().message.find("Invalid explicit type argument application") != std::string::npos);
    REQUIRE(analyzed.error().hint.has_value());
    REQUIRE(analyzed.error().hint->find("No overload of Make accepts 1 explicit type argument") != std::string::npos);
    REQUIRE(analyzed.error().hint->find("generic overloads expect 2 explicit type argument") != std::string::npos);
    REQUIRE(analyzed.error().hint->find("non-generic overloads accept none") != std::string::npos);
    REQUIRE(analyzed.error().hint->find("available explicit type argument arities") == std::string::npos);
  }

  SECTION("Explicit type arguments still report available arities across distinct generic overload sets") {
    const auto analyzed = analyze_program(
        "let Make<T>(value: Int64): Int64 = value;\n"
        "let Make<T, U>(value: Int64): Int64 = value;\n"
        "(1) -> Make<String, UInt64, Float64>;\n",
        "typecheck_explicit_type_args_generic_mixed_arity_summary.fleaux");

    REQUIRE_FALSE(analyzed.has_value());
    REQUIRE(analyzed.error().message.find("Invalid explicit type argument application") != std::string::npos);
    REQUIRE(analyzed.error().hint.has_value());
    REQUIRE(analyzed.error().hint->find("No overload of Make accepts 3 explicit type argument") != std::string::npos);
    REQUIRE(analyzed.error().hint->find("available explicit type argument arities are 1, 2") != std::string::npos);
  }

  SECTION("Explicit type arguments report direct expects-got wording for uniform generic arity overload sets") {
    const auto analyzed = analyze_program(
        "let Make<T, U>(value: Int64): Int64 = value;\n"
        "let Make<T, U>(value: String): String = value;\n"
        "(1) -> Make<String>;\n",
        "typecheck_explicit_type_args_uniform_arity_summary.fleaux");

    REQUIRE_FALSE(analyzed.has_value());
    REQUIRE(analyzed.error().message.find("Invalid explicit type argument application") != std::string::npos);
    REQUIRE(analyzed.error().hint.has_value());
    REQUIRE(analyzed.error().hint->find("Make expects 2 explicit type argument(s) but got 1") != std::string::npos);
    REQUIRE(analyzed.error().hint->find("available explicit type argument arities") == std::string::npos);
  }

  SECTION(
      "Value-position explicit type arguments report generic and non-generic expectations across mixed overload sets") {
    const std::string src =
        "let Make(x: Int64): Int64 = x;\n"
        "let Make<T, U>(value: Int64): Int64 = value;\n"
        "let Keep(value: Any): Any = value;\n"
        "Make<String> -> Keep;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_explicit_type_args_value_mixed_arity_summary.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Invalid explicit type argument application") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("No overload of Make accepts 1 explicit type argument") != std::string::npos);
    REQUIRE(lowered.error().hint->find("generic overloads expect 2 explicit type argument") != std::string::npos);
    REQUIRE(lowered.error().hint->find("non-generic overloads accept none") != std::string::npos);
    REQUIRE(lowered.error().hint->find("available explicit type argument arities") == std::string::npos);
  }

  SECTION("Value-position explicit type arguments still reuse the generic mixed-arity summary") {
    const std::string src =
        "let Make<T>(value: Int64): Int64 = value;\n"
        "let Make<T, U>(value: Int64): Int64 = value;\n"
        "let Keep(value: Any): Any = value;\n"
        "Make<String, UInt64, Float64> -> Keep;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed =
        parser.parse_program(src, "typecheck_explicit_type_args_value_generic_mixed_arity_summary.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Invalid explicit type argument application") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("No overload of Make accepts 3 explicit type argument") != std::string::npos);
    REQUIRE(lowered.error().hint->find("available explicit type argument arities are 1, 2") != std::string::npos);
  }

  SECTION("Value-position explicit type arguments reuse the uniform-arity expects-got wording") {
    const std::string src =
        "let Make<T, U>(value: Int64): Int64 = value;\n"
        "let Make<T, U>(value: String): String = value;\n"
        "let Keep(value: Any): Any = value;\n"
        "Make<String> -> Keep;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_explicit_type_args_value_uniform_arity_summary.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Invalid explicit type argument application") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Make expects 2 explicit type argument(s) but got 1") != std::string::npos);
    REQUIRE(lowered.error().hint->find("available explicit type argument arities") == std::string::npos);
  }

  SECTION("Explicit type arguments reject local values") {
    const auto analyzed =
        analyze_program("let Bad(x: Int64): Int64 = x<Int64>;\n", "typecheck_explicit_type_args_local_value.fleaux");

    REQUIRE_FALSE(analyzed.has_value());
    REQUIRE(analyzed.error().message.find("Invalid explicit type argument application") != std::string::npos);
    REQUIRE(analyzed.error().hint.has_value());
    REQUIRE(analyzed.error().hint->find("x is a local value") != std::string::npos);
  }

  SECTION("Explicit type arguments filter overloads before checking call shape") {
    const auto analyzed = analyze_program(
        "let Make<T>(value: T): T = value;\n"
        "let Make(lhs: Int64, rhs: Int64): Int64 = lhs;\n"
        "let NeedString(value: String): String = value;\n"
        "(\"ok\") -> Make<String> -> NeedString;\n",
        "typecheck_explicit_type_args_overload_filter.fleaux");

    REQUIRE(analyzed.has_value());
  }

  SECTION("Std.Cast allows underlying to strong casts") {
    const std::unordered_set<std::string> imported_symbols = {"Std.Cast"};
    const std::vector<fleaux::frontend::ir::IRLet> imported_typed_lets = {make_imported_std_cast()};
    const auto analyzed = analyze_program(
        "type UserId = Int64;\n"
        "let Accept(x: UserId): UserId = x;\n"
        "(42) -> Std.Cast<UserId> -> Accept;\n",
        "typecheck_std_cast_underlying_to_strong.fleaux", imported_symbols, imported_typed_lets);

    REQUIRE(analyzed.has_value());
  }

  SECTION("Std.Cast allows strong to underlying casts") {
    const std::unordered_set<std::string> imported_symbols = {"Std.Cast"};
    const std::vector<fleaux::frontend::ir::IRLet> imported_typed_lets = {make_imported_std_cast()};
    const auto analyzed = analyze_program(
        "type UserId = Int64;\n"
        "let Reveal(x: UserId): Int64 = x -> Std.Cast<Int64>;\n",
        "typecheck_std_cast_strong_to_underlying.fleaux", imported_symbols, imported_typed_lets);

    REQUIRE(analyzed.has_value());
  }

  SECTION("Std.Cast rejects underlying to builtin opaque type casts") {
    const std::unordered_set<std::string> imported_symbols = {"Std.Cast"};
    const std::vector<fleaux::frontend::ir::IRLet> imported_typed_lets = {make_imported_std_cast()};
    for (const auto& [type_name, type_text] : builtin_types) {
      INFO("builtin type: " << type_text);
      const auto analyzed = analyze_program(
          std::format("let Accept(x: {}): {} = x;\n(42) -> Std.Cast<{}> -> Accept;\n", type_text, type_text, type_text),
          std::format("typecheck_std_cast_underlying_to_opaque_{}.fleaux", type_name), imported_symbols,
          imported_typed_lets, {make_builtin_opaque_type_decl(type_name)});

      REQUIRE_FALSE(analyzed.has_value());
      REQUIRE(analyzed.error().message.find("Invalid Std.Cast invocation") != std::string::npos);
      REQUIRE(analyzed.error().hint.has_value());
      REQUIRE(analyzed.error().hint->find("builtin types") != std::string::npos);
      REQUIRE(analyzed.error().hint->find(type_text) != std::string::npos);
    }
  }

  SECTION("Std.Cast rejects builtin opaque type to underlying casts") {
    const std::unordered_set<std::string> imported_symbols = {"Std.Cast"};
    const std::vector<fleaux::frontend::ir::IRLet> imported_typed_lets = {make_imported_std_cast()};
    for (const auto& [type_name, type_text] : builtin_types) {
      INFO("builtin type: " << type_text);
      const auto analyzed =
          analyze_program(std::format("let Reveal(x: {}): Any = x -> Std.Cast<Any>;\n", type_text),
                          std::format("typecheck_std_cast_opaque_to_underlying_{}.fleaux", type_name), imported_symbols,
                          imported_typed_lets, {make_builtin_opaque_type_decl(type_name)});

      REQUIRE_FALSE(analyzed.has_value());
      REQUIRE(analyzed.error().message.find("Invalid Std.Cast invocation") != std::string::npos);
      REQUIRE(analyzed.error().hint.has_value());
      REQUIRE(analyzed.error().hint->find("builtin types") != std::string::npos);
      REQUIRE(analyzed.error().hint->find(type_text) != std::string::npos);
    }
  }

  SECTION("Std.Cast rejects unrelated strong type casts") {
    const std::unordered_set<std::string> imported_symbols = {"Std.Cast"};
    const std::vector<fleaux::frontend::ir::IRLet> imported_typed_lets = {make_imported_std_cast()};
    const auto analyzed = analyze_program(
        "type UserId = Int64;\n"
        "type AccountId = Int64;\n"
        "let Recast(x: UserId): AccountId = x -> Std.Cast<AccountId>;\n",
        "typecheck_std_cast_rejects_unrelated_strong.fleaux", imported_symbols, imported_typed_lets);

    REQUIRE_FALSE(analyzed.has_value());
    REQUIRE(analyzed.error().message.find("Invalid Std.Cast invocation") != std::string::npos);
    REQUIRE(analyzed.error().hint.has_value());
    REQUIRE(analyzed.error().hint->find("UserId -> AccountId") != std::string::npos);
  }

  SECTION("Std.Cast rejects first-class references that bypass direct call validation") {
    const std::unordered_set<std::string> imported_symbols = {"Std.Cast"};
    const std::vector<fleaux::frontend::ir::IRLet> imported_typed_lets = {make_imported_std_cast()};
    const auto analyzed = analyze_program(
        "let Std.Apply<T, U>(value: T, func: (T) => U): U :: __builtin__;\n"
        "type UserId = Int64;\n"
        "type AccountId = Int64;\n"
        "let Recast(x: UserId): AccountId = (x, Std.Cast<AccountId>) -> Std.Apply;\n",
        "typecheck_std_cast_rejects_first_class_reference.fleaux", imported_symbols, imported_typed_lets);

    REQUIRE_FALSE(analyzed.has_value());
    REQUIRE(analyzed.error().message.find("Invalid Std.Cast reference") != std::string::npos);
    REQUIRE(analyzed.error().hint.has_value());
    REQUIRE(analyzed.error().hint->find("direct call position") != std::string::npos);
  }
}

TEST_CASE("Type checker rejects invalid builtin opaque random invocations before runtime",
          "[typecheck][builtins][random]") {
  const std::string src = "(\"bad\") -> Std.Random.NextUInt64;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "typecheck_random_generator_arg_mismatch.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower_only(parsed.value());
  REQUIRE(lowered.has_value());

  fleaux::frontend::ir::IRLet imported_random_next;
  imported_random_next.qualifier = std::string{"Std.Random"};
  imported_random_next.name = "NextUInt64";
  imported_random_next.params = {make_ir_param("gen", make_ir_simple_type("RandomGenerator"))};
  imported_random_next.return_type = make_ir_simple_type("Any");

  const std::unordered_set<std::string> imported_symbols = {"Std.Random.NextUInt64"};
  const std::vector<fleaux::frontend::ir::IRLet> imported_typed_lets = {imported_random_next};
  const std::vector<fleaux::frontend::ir::IRTypeDecl> imported_type_decls = {
      make_ir_type_decl("RandomGenerator", make_ir_simple_type("Any")),
  };

  const auto analyzed = fleaux::frontend::type_check::analyze_program(*lowered, imported_symbols, imported_typed_lets,
                                                                      imported_type_decls);
  REQUIRE_FALSE(analyzed.has_value());
  REQUIRE(analyzed.error().message.find("Type mismatch in call target arguments") != std::string::npos);
  REQUIRE(analyzed.error().hint.has_value());
  REQUIRE(analyzed.error().hint->find("Std.Random.NextUInt64 expects argument 0") != std::string::npos);
}

TEST_CASE("Type checker matrix: Stage-3y Std.Random handle ownership", "[typecheck][stage3y][random]") {
  SECTION("Std.Random.Create propagates RandomGenerator from UInt64 seeds") {
    const std::string src =
        "type RandomGenerator = Any;\n"
        "let Std.Random.Create(seed: UInt64): RandomGenerator :: __builtin__;\n"
        "let NeedsGen(gen: RandomGenerator): RandomGenerator = gen;\n"
        "123u64 -> Std.Random.Create -> NeedsGen;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3y_random_create_propagates_generator.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Random.Create rejects negative seeds during type checking") {
    const std::string src =
        "type RandomGenerator = Any;\n"
        "let Std.Random.Create(seed: UInt64): RandomGenerator :: __builtin__;\n"
        "-1 -> Std.Random.Create;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3y_random_create_negative_seed_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Std.Random.Create expects argument 0") != std::string::npos);
  }

  SECTION("Std.Random.NextUInt64 propagates Tuple<UInt64, RandomGenerator>") {
    const std::string src =
        "type RandomGenerator = Any;\n"
        "let Std.Random.NextUInt64(gen: RandomGenerator): Tuple<UInt64, RandomGenerator> :: __builtin__;\n"
        "let MakeGen(): RandomGenerator :: __builtin__;\n"
        "let NeedsStep(step: Tuple<UInt64, RandomGenerator>): Tuple<UInt64, RandomGenerator> = step;\n"
        "() -> MakeGen -> Std.Random.NextUInt64 -> NeedsStep;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3y_random_nextuint64_step_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Random.NextBool propagates Tuple<Bool, RandomGenerator>") {
    const std::string src =
        "type RandomGenerator = Any;\n"
        "let Std.Random.NextBool(gen: RandomGenerator): Tuple<Bool, RandomGenerator> :: __builtin__;\n"
        "let MakeGen(): RandomGenerator :: __builtin__;\n"
        "let NeedsStep(step: Tuple<Bool, RandomGenerator>): Tuple<Bool, RandomGenerator> = step;\n"
        "() -> MakeGen -> Std.Random.NextBool -> NeedsStep;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3y_random_nextbool_step_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Random.Split propagates Tuple<RandomGenerator, RandomGenerator>") {
    const std::string src =
        "type RandomGenerator = Any;\n"
        "let Std.Random.Split(gen: RandomGenerator): Tuple<RandomGenerator, RandomGenerator> :: __builtin__;\n"
        "let MakeGen(): RandomGenerator :: __builtin__;\n"
        "let NeedsSplit(pair: Tuple<RandomGenerator, RandomGenerator>): Tuple<RandomGenerator, RandomGenerator> = "
        "pair;\n"
        "() -> MakeGen -> Std.Random.Split -> NeedsSplit;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3y_random_split_pair_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Random.NextInt64 rejects scalar non-generator inputs during type checking") {
    const std::string src =
        "type RandomGenerator = Any;\n"
        "let Std.Random.NextInt64(gen: RandomGenerator): Tuple<Int64, RandomGenerator> :: __builtin__;\n"
        "1 -> Std.Random.NextInt64;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3y_random_nextint64_scalar_reject.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Random.Split rejects scalar non-generator inputs during type checking") {
    const std::string src =
        "type RandomGenerator = Any;\n"
        "let Std.Random.Split(gen: RandomGenerator): Tuple<RandomGenerator, RandomGenerator> :: __builtin__;\n"
        "1 -> Std.Random.Split;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3y_random_split_scalar_reject.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }
}

TEST_CASE("Type checker rejects invalid bit shift counts before runtime", "[typecheck][builtins][bit]") {
  SECTION("Std.Bit.ShiftLeft accepts UInt64 shift counts") {
    const std::string src =
        "let Std.Bit.ShiftLeft(value: Int64, bits: UInt64): Int64 :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "(3, 2u64) -> Std.Bit.ShiftLeft -> NeedsInt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_bit_shiftleft_uint64_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Bit.ShiftLeft rejects negative shift counts") {
    const std::string src =
        "let Std.Bit.ShiftLeft(value: Int64, bits: UInt64): Int64 :: __builtin__;\n"
        "(1, -1) -> Std.Bit.ShiftLeft;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_bit_shiftleft_negative_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Std.Bit.ShiftLeft expects argument 1") != std::string::npos);
  }

  SECTION("Std.Bit.ShiftRight rejects fractional shift counts") {
    const std::string src =
        "let Std.Bit.ShiftRight(value: Int64, bits: UInt64): Int64 :: __builtin__;\n"
        "(8, 1.5) -> Std.Bit.ShiftRight;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_bit_shiftright_fractional_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Std.Bit.ShiftRight expects argument 1") != std::string::npos);
  }
}

TEST_CASE("Type checker enforces scalar dictionary key contracts before runtime", "[typecheck][builtins][dict]") {
  SECTION("Std.Dict.Create clone preserves dictionary channels") {
    const std::string src =
        "let Std.Dict.Create<K, V>(dict: Dict<K, V>): Dict<K, V> :: __builtin__;\n"
        "let Clone(d: Dict<String, Int64>): Dict<String, Int64> = d -> Std.Dict.Create;\n" "type Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_dict_create_clone_channel_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Dict.Set preserves dictionary channels") {
    const std::string src =
        "let Std.Dict.Set<K, V>(dict: Dict<K, V>, key: K, value: V): Dict<K, V> :: __builtin__;\n"
        "let KeepSet(d: Dict<String, Int64>): Dict<String, Int64> = (d, \"answer\", 42) -> Std.Dict.Set;\n"
        "type Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_dict_set_channel_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Dict.GetDefault propagates value channels") {
    const std::string src =
        "let Std.Dict.GetDefault<K, V>(dict: Dict<K, V>, key: K, default: V): V :: __builtin__;\n"
        "let Lookup(d: Dict<String, Int64>): Int64 = (d, \"missing\", 9) -> Std.Dict.GetDefault;\n" "type Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_dict_getdefault_value_channel_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Dict.Contains returns Bool") {
    const std::string src =
        "let Std.Dict.Contains<K, V>(dict: Dict<K, V>, key: K): Bool :: __builtin__;\n"
        "let HasKey(d: Dict<String, Int64>): Bool = (d, \"answer\") -> Std.Dict.Contains;\n" "type Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_dict_contains_bool_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Dict.Delete preserves dictionary channels") {
    const std::string src =
        "let Std.Dict.Delete<K, V>(dict: Dict<K, V>, key: K): Dict<K, V> :: __builtin__;\n"
        "let DeleteKey(d: Dict<String, Int64>): Dict<String, Int64> = (d, \"answer\") -> Std.Dict.Delete;\n"
        "type Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_dict_delete_channel_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Dict.Merge preserves dictionary channels") {
    const std::string src =
        "let Std.Dict.Merge<K, V>(dict_base: Dict<K, V>, dict_overlay: Dict<K, V>): Dict<K, V> :: __builtin__;\n"
        "let MergeBoth(a: Dict<String, Int64>, b: Dict<String, Int64>): Dict<String, Int64> = (a, b) -> "
        "Std.Dict.Merge;\n" "type Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_dict_merge_channel_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Dict.Set rejects tuple keys even through Dict<Any, Any>") {
    const std::string src =
        "let Std.Dict.Create(): Dict<Never, Never> :: __builtin__;\n"
        "let Std.Dict.Set<K, V>(dict: Dict<K, V>, key: K, value: V): Dict<K, V> :: __builtin__;\n"
        "(() -> Std.Dict.Create, (1, 2), 3) -> Std.Dict.Set;\n" "type Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_dict_set_tuple_key_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("dictionary key types to be scalar values") != std::string::npos);
  }

  SECTION("Std.Dict.Contains rejects tuple keys even through Dict<Any, Any>") {
    const std::string src =
        "let Std.Dict.Create(): Dict<Never, Never> :: __builtin__;\n"
        "let Std.Dict.Contains<K, V>(dict: Dict<K, V>, key: K): Bool :: __builtin__;\n"
        "(() -> Std.Dict.Create, (1, 2)) -> Std.Dict.Contains;\n" "type Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_dict_contains_tuple_key_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("dictionary key types to be scalar values") != std::string::npos);
  }

  SECTION("Std.Dict.Merge rejects non-scalar key channels") {
    const std::string src =
        "let Std.Dict.Merge<K, V>(dict_base: Dict<K, V>, dict_overlay: Dict<K, V>): Dict<K, V> :: __builtin__;\n"
        "let Bad(a: Dict<Tuple<Int64, Int64>, String>, b: Dict<Tuple<Int64, Int64>, String>): Dict<Tuple<Int64, "
        "Int64>, String> = (a, b) -> Std.Dict.Merge;\n" "type Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_dict_merge_tuple_channel_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("dictionary key types to be scalar values") != std::string::npos);
  }

  SECTION("Std.Dict.Keys rejects non-scalar key channels") {
    const std::string src =
        "let Std.Dict.Keys<K, V>(dict: Dict<K, V>): Tuple<K...> :: __builtin__;\n"
        "let Bad(d: Dict<Tuple<Int64, Int64>, String>): Tuple<Tuple<Int64, Int64>...> = d -> Std.Dict.Keys;\n"
        "type Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_dict_keys_tuple_channel_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("dictionary key types to be scalar values") != std::string::npos);
  }

  SECTION("Std.Dict.Keys accepts strong scalar key channels") {
    const std::string src =
        "let Std.Dict.Keys<K, V>(dict: Dict<K, V>): Tuple<K...> :: __builtin__;\n"
        "type UserId = Int64;\n"
        "let KeepKeys(d: Dict<UserId, String>): Tuple<UserId...> = d -> Std.Dict.Keys;\n" "type Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_dict_keys_strong_scalar_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Dict.Values propagates value channels") {
    const std::string src =
        "let Std.Dict.Values<K, V>(dict: Dict<K, V>): Tuple<V...> :: __builtin__;\n"
        "let KeepValues(d: Dict<String, Int64>): Tuple<Int64...> = d -> Std.Dict.Values;\n" "type Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_dict_values_value_channel_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Dict.Entries propagates pair channels") {
    const std::string src =
        "let Std.Dict.Entries<K, V>(dict: Dict<K, V>): Tuple<Tuple<K, V>...> :: __builtin__;\n"
        "let KeepEntries(d: Dict<String, Int64>): Tuple<Tuple<String, Int64>...> = d -> Std.Dict.Entries;\n"
        "type Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_dict_entries_pair_channel_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Dict.Clear preserves dictionary channels") {
    const std::string src =
        "let Std.Dict.Clear<K, V>(dict: Dict<K, V>): Dict<K, V> :: __builtin__;\n"
        "let KeepCleared(d: Dict<String, Int64>): Dict<String, Int64> = d -> Std.Dict.Clear;\n" "type Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_dict_clear_channel_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Dict.Length returns Int64 for strong scalar key channels") {
    const std::string src =
        "let Std.Dict.Length<K, V>(dict: Dict<K, V>): Int64 :: __builtin__;\n"
        "type UserId = Int64;\n"
        "let KeepLength(d: Dict<UserId, String>): Int64 = d -> Std.Dict.Length;\n" "type Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_dict_length_strong_scalar_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Dict.Length rejects non-scalar key channels") {
    const std::string src =
        "let Std.Dict.Length<K, V>(dict: Dict<K, V>): Int64 :: __builtin__;\n"
        "let Bad(d: Dict<Tuple<Int64, Int64>, String>): Int64 = d -> Std.Dict.Length;\n" "type Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_dict_length_tuple_channel_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("dictionary key types to be scalar values") != std::string::npos);
  }
}

TEST_CASE("Type checker validates Parallel.WithOptions options as Dict<String, Any> before runtime",
          "[typecheck][builtins][parallel]") {
  SECTION("Std.Parallel.WithOptions accepts Dict<String, Any>") {
    const std::string src =
        "let Std.Parallel.WithOptions<T, U>(items: Tuple<T...>, func: (T) => U, options: Dict<String, Any>): "
        "Result<Tuple<U...>, Tuple<Int64, String>> :: __builtin__;\n"
        "let ToInt(x: Float64): Int64 = 1;\n"
        "let UseOptions(opts: Dict<String, Any>): Result<Tuple<Int64...>, Tuple<Int64, String>> = "
        "((1.0, 2.0), ToInt, opts) -> Std.Parallel.WithOptions;\n" "type Result<T, E> :: __builtin__;\ntype Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_parallel_with_options_dict_string_any_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Parallel.WithOptions rejects Dict<String, Float64>") {
    const std::string src =
        "let Std.Parallel.WithOptions<T, U>(items: Tuple<T...>, func: (T) => U, options: Dict<String, Any>): "
        "Result<Tuple<U...>, Tuple<Int64, String>> :: __builtin__;\n"
        "let ToInt(x: Float64): Int64 = 1;\n"
        "let MakeOptions(): Dict<String, Float64> :: __builtin__;\n"
        "((1.0, 2.0), ToInt, () -> MakeOptions) -> Std.Parallel.WithOptions;\n"
        "type Result<T, E> :: __builtin__;\ntype Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_parallel_with_options_float64_value_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Dict<String, Any>") != std::string::npos);
  }

  SECTION("Std.Parallel.WithOptions rejects Dict<String, Int64>") {
    const std::string src =
        "let Std.Parallel.WithOptions<T, U>(items: Tuple<T...>, func: (T) => U, options: Dict<String, Any>): "
        "Result<Tuple<U...>, Tuple<Int64, String>> :: __builtin__;\n"
        "let ToInt(x: Float64): Int64 = 1;\n"
        "let MakeOptions(): Dict<String, Int64> :: __builtin__;\n"
        "((1.0, 2.0), ToInt, () -> MakeOptions) -> Std.Parallel.WithOptions;\n"
        "type Result<T, E> :: __builtin__;\ntype Dict<K, V> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_parallel_with_options_int64_value_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Dict<String, Any>") != std::string::npos);
  }
}

TEST_CASE("Type checker rejects negative array indices and dimensions before runtime", "[typecheck][builtins][array]") {
  SECTION("Std.Array.GetAt rejects negative indices") {
    const std::string src =
        "let Std.Array.GetAt<T>(array: Tuple<T...>, index: UInt64): T :: __builtin__;\n"
        "((1, 2, 3), -1) -> Std.Array.GetAt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_array_getat_negative_index_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Std.Array.GetAt expects argument 1") != std::string::npos);
  }

  SECTION("Std.Array.Shape produces UInt64 shape tuples") {
    const std::string src =
        "let Std.Array.Shape(value: Any): Tuple<UInt64...> :: __builtin__;\n"
        "let NeedsShape(s: Tuple<UInt64...>): Tuple<UInt64...> = s;\n"
        "(((1, 2), (3, 4))) -> Std.Array.Shape -> NeedsShape;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_array_shape_uint64_output.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Array.ReshapeND rejects negative shape elements") {
    const std::string src =
        "let Std.Array.ReshapeND(flat_array: Tuple<Any...>, shape: Tuple<UInt64...>): Any :: __builtin__;\n"
        "((1, 2, 3, 4), (2u64, -1)) -> Std.Array.ReshapeND;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_array_reshapend_negative_shape_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }
}

TEST_CASE("Type checker rejects negative core sequence indices and counts before runtime",
          "[typecheck][builtins][sequence]") {
  SECTION("Std.ElementAt rejects negative indices") {
    const std::string src =
        "let Std.ElementAt<T>(tuple: Tuple<T...>, count: UInt64): T :: __builtin__;\n"
        "((1, 2, 3), -1) -> Std.ElementAt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_elementat_negative_index_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Std.ElementAt expects argument 1") != std::string::npos);
  }

  SECTION("Std.Take rejects negative counts") {
    const std::string src =
        "let Std.Take<T>(tuple: Tuple<T...>, count: UInt64): Tuple<T...> :: __builtin__;\n"
        "((1, 2, 3), -1) -> Std.Take;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_take_negative_count_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Std.Take expects argument 1") != std::string::npos);
  }

  SECTION("Std.Drop rejects negative counts") {
    const std::string src =
        "let Std.Drop<T>(tuple: Tuple<T...>, count: UInt64): Tuple<T...> :: __builtin__;\n"
        "((1, 2, 3), -1) -> Std.Drop;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_drop_negative_count_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Std.Drop expects argument 1") != std::string::npos);
  }

  SECTION("Std.Slice rejects negative stops") {
    const std::string src =
        "let Std.Slice<T>(tuple: Tuple<T...>, stop: UInt64): Tuple<T...> :: __builtin__;\n"
        "((1, 2, 3), -1) -> Std.Slice;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_slice_negative_stop_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Std.Slice expects argument 1") != std::string::npos);
  }
}

TEST_CASE("Type checker rejects negative loop and task bounds before runtime", "[typecheck][builtins][control]") {
  SECTION("Std.LoopN rejects negative max_iters") {
    const std::string src =
        "let Std.LoopN<S>(state: S, continue_func: (S) => Bool, step_func: (S) => S, max_iters: UInt64): S :: "
        "__builtin__;\n"
        "let ContinueOk(x: Float64): Bool = True;\n"
        "let StepOk(x: Float64): Float64 = x;\n"
        "(1.0, ContinueOk, StepOk, -1) -> Std.LoopN;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_loopn_negative_max_iters_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Std.LoopN expects argument 3") != std::string::npos);
  }

  SECTION("Std.Task.WithTimeout rejects negative timeout") {
    const std::string src =
        "let Std.Task.WithTimeout(task: TaskHandle<Int64>, timeout_ms: UInt64): Result<Any, String> :: __builtin__;\n"
        "let MakeTask(): TaskHandle<Int64> :: __builtin__;\n"
        "(() -> MakeTask, -1) -> Std.Task.WithTimeout;\n" "type Result<T, E> :: __builtin__;\ntype TaskHandle<T> :: __builtin__;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_task_with_timeout_negative_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Std.Task.WithTimeout expects argument 1") != std::string::npos);
  }
}

TEST_CASE("Type checker returns the bound numeric type from numeric builtins", "[typecheck][builtins][numeric]") {
  SECTION("Std.Add returns Int64 for Int64 arguments") {
    const std::string src =
        "let Std.Add<T: Int64 | UInt64 | Float64>(lhs: T, rhs: T): T :: "
        "__builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "(1, 2) -> Std.Add -> NeedsInt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_numeric_builtin_refine_int64.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Add returns UInt64 for UInt64 arguments") {
    const std::string src =
        "let Std.Add<T: Int64 | UInt64 | Float64>(lhs: T, rhs: T): T :: "
        "__builtin__;\n"
        "let NeedsUInt(x: UInt64): UInt64 = x;\n"
        "(1u64, 2u64) -> Std.Add -> NeedsUInt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_numeric_builtin_refine_uint64.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Add rejects union-typed operands") {
    const std::string src =
        "let Std.Add<T: Int64 | UInt64 | Float64>(lhs: T, rhs: T): T :: "
        "__builtin__;\n"
        "let AddPair(lhs: Float64 | Int64 | UInt64, rhs: Float64 | Int64 | UInt64): Int64 = (lhs, rhs) -> Std.Add;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_numeric_builtin_refine_union_fallback.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type argument does not satisfy its bound") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("A union cannot satisfy a bound") != std::string::npos);
  }
}

TEST_CASE("Type checker matrix: declared return-type enforcement", "[typecheck][return]") {
  SECTION("Primitive mismatch is rejected") {
    const std::string src = "let Bad(): Float64 = \"oops\";\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_return_primitive_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in function return") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("declares return type") != std::string::npos);
  }

  SECTION("Exact Float64 return is accepted") {
    const std::string src = "let Good(): Float64 = 1.0;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_return_numeric_widening.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Tuple shape mismatch is rejected") {
    const std::string src = "let BadTuple(): Tuple<Float64, Float64> = (1.0, 2.0, 3.0);\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_return_tuple_shape_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in function return") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("declares return type") != std::string::npos);
  }

  SECTION("Any return accepts concrete inferred type") {
    const std::string src = "let AnyBox(): Any = \"ok\";\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_return_any_accepts_concrete.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Unresolved target is reported before call-argument mismatch") {
    const std::string src = "let BadCall(): Float64 = (1) -> MissingTarget;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_return_error_precedence.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Unresolved symbol") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("MissingTarget") != std::string::npos);
  }
}

TEST_CASE("Type checker matrix: user function call typing", "[typecheck][stage2]") {
  SECTION("User function call rejects mismatched argument type") {
    const std::string src =
        "let Std.Pi(): Float64 = 3.14159;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "(Std.Pi) -> NeedsInt;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage2_user_call_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Single-parameter callable receives a tuple written with a trailing comma") {
    const std::string src =
        "let AcceptPair(pair: Tuple<Float64, Float64>): Float64 = 0.0;\n"
        "(1.0, 2.0,) -> AcceptPair;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage2_single_param_tuple_shape.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Multi-parameter callable unpacks tuple elements") {
    const std::string src =
        "let Std.Add(lhs: Float64, rhs: Float64): Float64 :: __builtin__;\n"
        "let Sum(a: Float64, b: Float64): Float64 = (a, b) -> Std.Add;\n"
        "(1.0, 2.0) -> Sum;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage2_multi_param_tuple_unpack.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Callable argument enforces declared function signature") {
    const std::string src =
        "let Std.Apply(value: Float64, func: (Float64) => Float64): Float64 :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "(1.0, NeedsInt) -> Std.Apply;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage2_callable_signature_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Closure expression enforces declared return type") {
    const std::string src = "let MakeBad(): Any = (x: Float64): Float64 = \"oops\";\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage2_closure_return_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in function return") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Closure declares return type Float64 but its body has type String") !=
            std::string::npos);
  }

  SECTION("Callable argument accepts matching user function signature") {
    const std::string src =
        "let Std.Add(lhs: Float64, rhs: Float64): Float64 :: __builtin__;\n"
        "let Std.Apply(value: Float64, func: (Float64) => Float64): Float64 :: __builtin__;\n"
        "let Inc(x: Float64): Float64 = (x, 1.0) -> Std.Add;\n"
        "(5.0, Inc) -> Std.Apply;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage2_callable_signature_match.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }
}

TEST_CASE("Type checker matrix: higher-order builtin callable semantics", "[typecheck][stage2b]") {
  SECTION("Std.Apply rejects callable that cannot accept value type") {
    const std::string src =
        "let Std.Apply<T, U>(value: T, func: (T) => U): U :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "(1.0, NeedsInt) -> Std.Apply;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage2b_apply_callable_param_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Branch requires Bool condition") {
    const std::string src =
        "let Std.Branch<T>(condition: Bool, value: T, true_func: (T) => T, false_func: (T) => T): T :: __builtin__;\n"
        "let T(x: Float64): Float64 = x;\n"
        "let F(x: Float64): Float64 = x;\n"
        "(1.0, 2.0, T, F) -> Std.Branch;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage2b_branch_condition_bool.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Branch requires compatible branch return types") {
    const std::string src =
        "let Std.Branch<T>(condition: Bool, value: T, true_func: (T) => T, false_func: (T) => T): T :: __builtin__;\n"
        "let T(x: Float64): Float64 = x;\n"
        "let F(x: Float64): String = \"nope\";\n"
        "(True, 2.0, T, F) -> Std.Branch;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage2b_branch_return_compatibility.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Loop requires continue_func to return Bool") {
    const std::string src =
        "let Std.Loop<S>(state: S, continue_func: (S) => Bool, step_func: (S) => S): S :: __builtin__;\n"
        "let ContinueBad(x: Float64): Float64 = x;\n"
        "let StepOk(x: Float64): Float64 = x;\n"
        "(1.0, ContinueBad, StepOk) -> Std.Loop;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage2b_loop_continue_bool.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.LoopN accepts UInt64 max_iters") {
    const std::string src =
        "let Std.LoopN<S>(state: S, continue_func: (S) => Bool, step_func: (S) => S, max_iters: UInt64): S :: "
        "__builtin__;\n"
        "let ContinueOk(x: Float64): Bool = True;\n"
        "let StepOk(x: Float64): Float64 = x;\n"
        "(1.0, ContinueOk, StepOk, 5u64) -> Std.LoopN;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage2b_loopn_max_iters_uint64.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Tuple.Filter requires predicate to return Bool") {
    const std::string src =
        "let Std.Tuple.Filter<T>(t: Tuple<T...>, pred: (T) => Bool): Tuple<T...> :: __builtin__;\n"
        "let Pred(x: Float64): Float64 = x;\n"
        "((1.0, 2.0), Pred) -> Std.Tuple.Filter;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage2b_tuple_filter_predicate_bool.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Tuple.Filter preserves tuple element type") {
    const std::string src =
        "let Std.Tuple.Filter<T>(t: Tuple<T...>, pred: (T) => Bool): Tuple<T...> :: __builtin__;\n"
        "let Pred(x: Int64): Bool = True;\n"
        "let NeedsInts(values: Tuple<Int64...>): Tuple<Int64...> = values;\n"
        "((1, 2, 3), Pred) -> Std.Tuple.Filter -> NeedsInts;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage2b_tuple_filter_type_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Tuple.All accepts Bool-returning predicate") {
    const std::string src =
        "let Std.Tuple.All(t: Any, pred: Any): Any :: __builtin__;\n"
        "let Pred(x: Float64): Bool = True;\n"
        "((1.0, 2.0), Pred) -> Std.Tuple.All;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage2b_tuple_all_predicate_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Any-based callable bridge does not leak to unrelated builtins") {
    const std::string src =
        "let Std.Tuple.Map(t: Any, func: Any): Any :: __builtin__;\n"
        "let NotBoolPred(x: Float64): Float64 = x;\n"
        "((1.0, 2.0), NotBoolPred) -> Std.Tuple.Map;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage2b_bridge_scope_no_leak.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }
}

TEST_CASE("Type checker matrix: Stage-2c typed generic reduce and try checks", "[typecheck][stage2c]") {
  SECTION("Std.Try rejects callable that cannot accept value type") {
    const std::string src =
        "let Std.Try<T, U>(value: T, func: (T) => U): Result<U, Any> :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "(1.0, NeedsInt) -> Std.Try;\n" "type Result<T, E> :: __builtin__;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage2c_try_callable_param_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Tuple.Reduce rejects reducer with wrong arity") {
    const std::string src =
        "let Std.Tuple.Reduce<T, A>(t: Tuple<T...>, initial: A, func: (A, T) => A): A :: __builtin__;\n"
        "let Bad(acc: Float64): Float64 = acc;\n"
        "((1.0, 2.0), 0.0, Bad) -> Std.Tuple.Reduce;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage2c_tuple_reduce_arity.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Tuple.Reduce rejects accumulator-incompatible reducer return") {
    const std::string src =
        "let Std.Tuple.Reduce<T, A>(t: Tuple<T...>, initial: A, func: (A, T) => A): A :: __builtin__;\n"
        "let Bad(acc: Float64, item: Float64): String = \"bad\";\n"
        "((1.0, 2.0), 0.0, Bad) -> Std.Tuple.Reduce;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage2c_tuple_reduce_return.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Tuple.Reduce accepts accumulator-compatible reducer") {
    const std::string src =
        "let Std.Tuple.Reduce<T, A>(t: Tuple<T...>, initial: A, func: (A, T) => A): A :: __builtin__;\n"
        "let Good(acc: Float64, item: Float64): Float64 = acc;\n"
        "((1.0, 2.0), 0.0, Good) -> Std.Tuple.Reduce;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage2c_tuple_reduce_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Tuple.Reduce propagates accumulator type") {
    const std::string src =
        "let Std.Tuple.Reduce<T, A>(t: Tuple<T...>, initial: A, func: (A, T) => A): A :: __builtin__;\n"
        "let Good(acc: String, item: Int64): String = acc;\n"
        "let NeedsString(value: String): String = value;\n"
        "((1, 2, 3), \"seed\", Good) -> Std.Tuple.Reduce -> NeedsString;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage2c_tuple_reduce_accumulator_type_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Parallel.Reduce rejects reducer item-type mismatch") {
    const std::string src =
        "let Std.Parallel.Reduce<T, A>(items: Tuple<T...>, init: A, func: (A, T) => A): Result<A, Tuple<Int64, "
        "String>> :: __builtin__;\n"
        "let Bad(acc: Float64, item: Int64): Float64 = acc;\n"
        "((\"a\", \"b\"), 0.0, Bad) -> Std.Parallel.Reduce;\n" "type Result<T, E> :: __builtin__;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage2c_parallel_reduce_item_type.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Stage-2c typed checks do not affect Std.Parallel.Map") {
    const std::string src =
        "let Std.Parallel.Map(items: Any, func: Any): Result<Any, Any> :: __builtin__;\n"
        "let Func(x: Float64): Float64 = x;\n"
        "((1.0, 2.0), Func) -> Std.Parallel.Map;\n" "type Result<T, E> :: __builtin__;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage2c_scope_no_leak_parallel_map.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }
}

TEST_CASE("Type checker matrix: Stage-3a structured type consistency", "[typecheck][stage3a]") {
  SECTION("Union parameter accepts member type") {
    const std::string src =
        "let AcceptNum(x: Float64 | Int64 | UInt64): Any = x;\n"
        "(42u64) -> AcceptNum;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3a_union_accepts_member.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Union parameter rejects non-member type") {
    const std::string src =
        "let AcceptNum(x: Float64 | Int64 | UInt64): Any = x;\n"
        "(\"nope\") -> AcceptNum;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3a_union_rejects_non_member.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Applied constructor mismatch is rejected") {
    const std::string src =
        "let MakeResult(): Result<Any, Any> :: __builtin__;\n"
        "let NeedsDict(d: Dict<String, Any>): Any = d;\n"
        "() -> MakeResult -> NeedsDict;\n" "type Result<T, E> :: __builtin__;\ntype Dict<K, V> :: __builtin__;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3a_applied_constructor_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Applied argument mismatch is rejected") {
    const std::string src =
        "let MakeResultString(): Result<String, String> :: __builtin__;\n"
        "let NeedsResultInt(r: Result<Int64, String>): Any = r;\n"
        "() -> MakeResultString -> NeedsResultInt;\n" "type Result<T, E> :: __builtin__;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3a_applied_arg_mismatch.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Applied argument match is accepted") {
    const std::string src =
        "let MakeResultInt(): Result<Int64, String> :: __builtin__;\n"
        "let NeedsResultInt(r: Result<Int64, String>): Any = r;\n"
        "() -> MakeResultInt -> NeedsResultInt;\n" "type Result<T, E> :: __builtin__;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3a_applied_arg_match.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }
}

TEST_CASE("Type checker matrix: Stage-3c temporary bridge removal", "[typecheck][stage3c]") {
  SECTION("Std.Apply Any declaration no longer receives bridge callable constraints") {
    const std::string src =
        "let Std.Apply(value: Any, func: Any): Any :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "(1.0, NeedsInt) -> Std.Apply;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3c_apply_bridge_still_active.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Try Any declaration no longer receives bridge callable constraints") {
    const std::string src =
        "let Std.Try(value: Any, func: Any): Result<Any, Any> :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "(1.0, NeedsInt) -> Std.Try;\n" "type Result<T, E> :: __builtin__;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3c_try_bridge_still_active.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Tuple.All uses typed signature without bridge") {
    const std::string src =
        "let Std.Tuple.All(t: Tuple<Any...>, pred: (Any) => Bool): Bool :: __builtin__;\n"
        "let BadPred(x: Any): Float64 = 1.0;\n"
        "((1.0, 2.0), BadPred) -> Std.Tuple.All;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3c_tuple_all_typed_signature.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Tuple.All Any declaration no longer receives bridge predicate rule") {
    const std::string src =
        "let Std.Tuple.All(t: Any, pred: Any): Any :: __builtin__;\n"
        "let BadPred(x: Any): Float64 = 1.0;\n"
        "((1.0, 2.0), BadPred) -> Std.Tuple.All;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3c_tuple_all_any_no_bridge.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }
}

namespace {

auto lower_and_check(const std::string& src, const std::string& source_name)
    -> fleaux::frontend::lowering::LoweringResult {
  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, source_name);
  REQUIRE(parsed.has_value());
  const fleaux::frontend::lowering::Lowerer lowerer;
  return lowerer.lower(parsed.value());
}

}  // namespace

TEST_CASE("Type checker validates applied type names and argument counts", "[typecheck][generic_types]") {
  const std::string decls =
      "type Result<T, E> :: __builtin__;\n"
      "type FileHandle :: __builtin__;\n";

  SECTION("Unknown applied type names are rejected") {
    const auto lowered = lower_and_check(decls + "let F(x: Banana<Int64>): Int64 = 1;\n", "applied_unknown.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Unknown type") != std::string::npos);
    REQUIRE(lowered.error().hint->find("Banana") != std::string::npos);
  }

  SECTION("Wrong type argument counts are rejected") {
    const auto lowered = lower_and_check(decls + "let F(x: Result<Int64>): Int64 = 1;\n", "applied_wrong_arity.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Wrong number of type arguments") != std::string::npos);
    REQUIRE(lowered.error().hint->find("takes 2 type arguments but got 1") != std::string::npos);
  }

  SECTION("Generic types used without type arguments are rejected") {
    const auto lowered = lower_and_check(decls + "let F(x: Result): Int64 = 1;\n", "applied_missing_args.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Missing type arguments") != std::string::npos);
  }

  SECTION("Non-generic types given type arguments are rejected") {
    const auto lowered =
        lower_and_check(decls + "let F(x: FileHandle<Int64>): Int64 = 1;\n", "applied_non_generic.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Unexpected type arguments") != std::string::npos);
  }

  SECTION("Type arguments are themselves validated") {
    const auto lowered =
        lower_and_check(decls + "let F(x: Result<Banana, String>): Int64 = 1;\n", "applied_bad_arg.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Unknown type") != std::string::npos);
  }
}

TEST_CASE("Type checker validates generic type declarations", "[typecheck][generic_types]") {
  SECTION("Duplicate generic parameters are rejected") {
    const auto lowered = lower_and_check("type Box<T, T> = Tuple<T>;\n", "generic_decl_duplicate_param.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Duplicate generic parameter") != std::string::npos);
  }

  SECTION("Generic parameters may not shadow declared types") {
    const auto lowered = lower_and_check("type User = Null;\ntype Box<User> = Tuple<User>;\n",
                                         "generic_decl_shadowing_param.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Invalid generic parameter") != std::string::npos);
  }

  SECTION("Unknown names in a generic target are rejected") {
    const auto lowered = lower_and_check("type Box<T> = Tuple<U>;\n", "generic_decl_unknown_target.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Unknown type") != std::string::npos);
  }
}

TEST_CASE("Type checker keeps generic strong type instantiations distinct", "[typecheck][generic_types]") {
  const std::string decls =
      "let Std.Cast<T>(value: Any): T :: __builtin__;\n"
      "type User = Null;\n"
      "type Order = Null;\n"
      "type Id<T> = Int64;\n"
      "let LoadUser(id: Id<User>): Int64 = id -> Std.Cast<Int64>;\n";

  SECTION("Matching instantiations are accepted") {
    const auto lowered = lower_and_check(decls + "7 -> Std.Cast<Id<User>> -> LoadUser;\n", "generic_id_match.fleaux");
    REQUIRE(lowered.has_value());
  }

  SECTION("Different type arguments make different types") {
    const auto lowered =
        lower_and_check(decls + "7 -> Std.Cast<Id<Order>> -> LoadUser;\n", "generic_id_mismatch.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
  }

  SECTION("Std.Cast does not convert directly between instantiations") {
    const auto lowered = lower_and_check(decls + "let Swap(id: Id<Order>): Id<User> = id -> Std.Cast<Id<User>>;\n",
                                         "generic_id_cross_cast.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Invalid Std.Cast invocation") != std::string::npos);
  }

  SECTION("Std.Cast instantiates the underlying type of a structural generic strong type") {
    const auto lowered = lower_and_check(
        "let Std.Cast<T>(value: Any): T :: __builtin__;\n"
        "type Pair<A, B> = Tuple<A, B>;\n"
        "let MakePair(a: Int64, b: String): Pair<Int64, String> = (a, b,) -> Std.Cast<Pair<Int64, String>>;\n"
        "let Wrong(a: Int64, b: String): Pair<String, Int64> = (a, b,) -> Std.Cast<Pair<String, Int64>>;\n",
        "generic_pair_cast.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Invalid Std.Cast invocation") != std::string::npos);
    REQUIRE(lowered.error().hint->find("Pair<String, Int64>") != std::string::npos);
  }
}

TEST_CASE("Type checker allows only identity casts for builtin types", "[typecheck][generic_types][builtin_types]") {
  const std::string decls =
      "let Std.Cast<T>(value: Any): T :: __builtin__;\n"
      "type TaskHandle<T> :: __builtin__;\n";

  SECTION("Identity casts are accepted") {
    const auto lowered = lower_and_check(
        decls + "let Same(t: TaskHandle<Int64>): TaskHandle<Int64> = t -> Std.Cast<TaskHandle<Int64>>;\n",
        "builtin_identity_cast.fleaux");
    REQUIRE(lowered.has_value());
  }

  SECTION("Casting to Any is rejected") {
    const auto lowered =
        lower_and_check(decls + "let Reveal(t: TaskHandle<Int64>): Any = t -> Std.Cast<Any>;\n", "builtin_to_any.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().hint->find("builtin types") != std::string::npos);
  }

  SECTION("Changing the type argument is rejected") {
    const auto lowered = lower_and_check(
        decls + "let Retag(t: TaskHandle<Int64>): TaskHandle<String> = t -> Std.Cast<TaskHandle<String>>;\n",
        "builtin_retag.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().hint->find("builtin types") != std::string::npos);
  }
}

TEST_CASE("Type checker carries TaskHandle result types through Std.Task builtins",
          "[typecheck][generic_types][task]") {
  const std::string decls =
      "type Result<T, E> :: __builtin__;\n"
      "type TaskHandle<T> :: __builtin__;\n"
      "let Std.Task.Spawn<T, U>(func: (T) => U, value: T): TaskHandle<U> :: __builtin__;\n"
      "let Std.Task.Await<T>(task: TaskHandle<T>): Result<T, String> :: __builtin__;\n"
      "let ToText(x: Int64): String = \"x\";\n";

  SECTION("Await produces the spawned result type") {
    const auto lowered =
        lower_and_check(decls + "let Run(): Result<String, String> = (ToText, 1) -> Std.Task.Spawn -> Std.Task.Await;\n",
                        "task_result_type.fleaux");
    REQUIRE(lowered.has_value());
  }

  SECTION("A wrong declared result type is rejected") {
    const auto lowered =
        lower_and_check(decls + "let Run(): Result<Int64, String> = (ToText, 1) -> Std.Task.Spawn -> Std.Task.Await;\n",
                        "task_result_type_mismatch.fleaux");
    REQUIRE_FALSE(lowered.has_value());
  }
}

TEST_CASE("Type checker checks operators as calls to their Std builtins", "[typecheck][operators]") {
  SECTION("Operand types are checked against the builtin signature") {
    const auto lowered = lower_and_check("import Std;\n(\"a\", 2) -> +;\n", "operator_string_add.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Operator '+' calls Std.Add.") != std::string::npos);
  }

  SECTION("Builtin contracts apply to operators") {
    const auto lowered = lower_and_check("import Std;\n(1, 2.0) -> +;\n", "operator_float_promotion.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().hint->find("Numbers are not converted implicitly") != std::string::npos);
  }

  SECTION("Operand counts are checked") {
    const auto lowered = lower_and_check("import Std;\n(1, 2, 3) -> -;\n", "operator_minus_arity.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().hint->find("Operator '-' calls Std.Subtract.") != std::string::npos);
  }

  SECTION("Logical operators require Bool operands") {
    const auto lowered = lower_and_check("import Std;\n(1, 2) -> &&;\n", "operator_and_ints.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().hint->find("Operator '&&' calls Std.And.") != std::string::npos);
  }

  SECTION("Operator results carry the builtin return type") {
    REQUIRE(lower_and_check("import Std;\nlet Sum(): Int64 = (1, 2) -> +;\nlet Less(): Bool = (1, 2) -> <;\n",
                            "operator_result_types.fleaux")
                .has_value());

    const auto lowered = lower_and_check("import Std;\nlet Less(): Int64 = (1, 2) -> <;\n",
                                         "operator_result_type_mismatch.fleaux");
    REQUIRE_FALSE(lowered.has_value());
  }

  SECTION("Operators require import Std") {
    const auto lowered = lower_and_check("(1, 2) -> +;\n", "operator_without_std.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Unresolved operator") != std::string::npos);
    REQUIRE(lowered.error().hint->find("Add 'import Std;'") != std::string::npos);
  }
}

TEST_CASE("Type checker selects unary or binary minus from the operand type", "[typecheck][operators]") {
  const auto rhs_of_last_expression = [](const fleaux::frontend::ir::IRProgram& program) {
    REQUIRE_FALSE(program.expressions.empty());
    const auto* flow = std::get_if<fleaux::frontend::ir::IRFlowExpr>(&program.expressions.back().expr.node);
    REQUIRE(flow != nullptr);
    const auto* name_ref = std::get_if<fleaux::frontend::ir::IRNameRef>(&flow->rhs);
    REQUIRE(name_ref != nullptr);
    return *name_ref;
  };

  SECTION("A single Int64 operand is negation") {
    const auto lowered = lower_and_check("import Std;\nlet Neg(): Int64 = 5 -> -;\n5 -> -;\n", "operator_unary_minus.fleaux");
    REQUIRE(lowered.has_value());
    const auto name_ref = rhs_of_last_expression(*lowered);
    REQUIRE(name_ref.qualifier == std::optional<std::string>{"Std"});
    REQUIRE(name_ref.name == "UnaryMinus");
    REQUIRE(name_ref.resolved_symbol_key.has_value());
  }

  SECTION("A spread tuple value is binary subtraction") {
    const auto lowered = lower_and_check(
        "import Std;\nlet P(): Tuple<Int64, Int64> = (5, 2);\nlet Diff(): Int64 = (P...) -> -;\n(P...) -> -;\n",
        "operator_tuple_value_minus.fleaux");
    REQUIRE(lowered.has_value());
    REQUIRE(rhs_of_last_expression(*lowered).name == "Subtract");
  }

  SECTION("Negation keeps the operand type") {
    REQUIRE(lower_and_check("import Std;\nlet Neg(): Float64 = 2.5 -> -;\n", "operator_unary_minus_float.fleaux")
                .has_value());
  }

  SECTION("Unsigned values cannot be negated") {
    const auto lowered = lower_and_check("import Std;\n5u64 -> -;\n", "operator_unary_minus_unsigned.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type argument does not satisfy its bound") != std::string::npos);
    REQUIRE(lowered.error().hint->find("Std.UnaryMinus requires T: Int64 | Float64, but T is UInt64") !=
            std::string::npos);
  }
}

TEST_CASE("Type checker enforces generic bounds at call sites", "[typecheck][generic_bounds]") {
  const std::string decls = "let Std.Add<T: Int64 | UInt64 | Float64>(lhs: T, rhs: T): T :: __builtin__;\n";

  SECTION("A matching type satisfies the bound") {
    REQUIRE(lower_and_check(decls + "let X(): UInt64 = (1u64, 2u64) -> Std.Add;\n", "bound_ok.fleaux").has_value());
  }

  SECTION("A type outside the bound is rejected") {
    const auto lowered = lower_and_check(decls + "(\"a\", \"b\") -> Std.Add;\n", "bound_string.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type argument does not satisfy its bound") != std::string::npos);
    REQUIRE(lowered.error().hint->find("Std.Add requires T: Int64 | UInt64 | Float64, but T is String") !=
            std::string::npos);
  }

  SECTION("Two different numeric types do not bind one parameter") {
    const auto lowered = lower_and_check(decls + "(1, 2u64) -> Std.Add;\n", "bound_mixed.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().hint->find("Numbers are not converted implicitly") != std::string::npos);
  }

  SECTION("A union-typed value cannot satisfy a bound") {
    const auto lowered = lower_and_check(
        decls + "let F(x: Int64 | UInt64): Int64 | UInt64 = (x, x) -> Std.Add;\n", "bound_union.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().hint->find("A union cannot satisfy a bound") != std::string::npos);
  }

  SECTION("Explicit type arguments must satisfy the bound") {
    const auto lowered = lower_and_check(decls + "let F: Any = Std.Add<String>;\n", "bound_explicit.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type argument does not satisfy its bound") != std::string::npos);
  }

  SECTION("Bounds may be aliases") {
    REQUIRE(lower_and_check("alias Num = Int64 | Float64;\n"
                            "let Twice<T: Num>(x: T): T = x;\n"
                            "let X(): Float64 = 1.5 -> Twice;\n",
                            "bound_alias.fleaux")
                .has_value());
  }
}

TEST_CASE("Type checker checks bounded type parameters inside generic bodies", "[typecheck][generic_bounds]") {
  const std::string decls =
      "let Std.Add<T: Int64 | UInt64 | Float64>(lhs: T, rhs: T): T :: __builtin__;\n"
      "let Std.UnaryMinus<T: Int64 | Float64>(val: T): T :: __builtin__;\n";

  SECTION("A parameter whose bound fits the callee's bound is accepted") {
    REQUIRE(lower_and_check(decls + "let Twice<T: Int64 | Float64>(x: T): T = (x, x) -> Std.Add;\n",
                            "body_bound_subset.fleaux")
                .has_value());
  }

  SECTION("Block-local values typed with the parameter keep its bound") {
    REQUIRE(lower_and_check(decls + "let Twice<T: Int64>(x: T): T = { let y: T = x; (y, y) -> Std.Add; };\n",
                            "body_bound_local.fleaux")
                .has_value());
  }

  SECTION("An unbounded parameter is rejected with a hint") {
    const auto lowered = lower_and_check(decls + "let Twice<T>(x: T): T = (x, x) -> Std.Add;\n",
                                         "body_bound_missing.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().hint->find("Type parameter 'T' has no bound") != std::string::npos);
  }

  SECTION("A parameter whose bound is wider than the callee's is rejected") {
    const auto lowered = lower_and_check(
        decls + "let Neg<T: Int64 | UInt64 | Float64>(x: T): T = x -> Std.UnaryMinus;\n", "body_bound_wider.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().hint->find("type parameter 'T' bounded by") != std::string::npos);
  }

  SECTION("Closure bodies see the bounds of their generic parameters") {
    REQUIRE(lower_and_check(decls + "let F: (Int64) => Int64 = <T: Int64>(x: T): T = (x, x) -> Std.Add;\n",
                            "closure_bound.fleaux")
                .has_value());
    const auto lowered =
        lower_and_check(decls + "let F: Any = <T>(x: T): T = (x, x) -> Std.Add;\n", "closure_unbounded.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().hint->find("Type parameter 'T' has no bound") != std::string::npos);
  }
}

TEST_CASE("Type checker validates generic bound declarations", "[typecheck][generic_bounds]") {
  SECTION("Unknown names in a bound are rejected") {
    const auto lowered = lower_and_check("let F<T: Banana>(x: T): T = x;\n", "bound_unknown.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Unknown type") != std::string::npos);
    REQUIRE(lowered.error().hint->find("In the bound of generic parameter 'T'") != std::string::npos);
  }

  SECTION("Bounds cannot refer to other generic parameters") {
    const auto lowered = lower_and_check("let F<T, U: T>(x: T, y: U): T = x;\n", "bound_refers_param.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Unknown type") != std::string::npos);
  }
}

TEST_CASE("Std numeric builtins are bounded and exact", "[typecheck][generic_bounds][std]") {
  SECTION("Arithmetic returns the operand type") {
    REQUIRE(lower_and_check("import Std;\n"
                            "let D(): Int64 = (10, 3) -> /;\n"
                            "let U(): UInt64 = (10u64, 3u64) -> Std.Divide;\n"
                            "let P(): Int64 = (2, 10) -> ^;\n",
                            "std_numeric_returns.fleaux")
                .has_value());
  }

  SECTION("Wrapping and saturating builtins accept integers only") {
    REQUIRE(lower_and_check("import Std;\nlet W(): Int64 = (1, 2) -> Std.WrappingAdd;\n",
                            "std_wrapping_int.fleaux")
                .has_value());
    const auto lowered =
        lower_and_check("import Std;\n(1.0, 2.0) -> Std.SaturatingAdd;\n", "std_saturating_float.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().hint->find("Std.SaturatingAdd requires T: Int64 | UInt64") != std::string::npos);
  }

  SECTION("Sorting requires a comparable element type") {
    REQUIRE(lower_and_check("import Std;\n((\"b\", \"a\")) -> Std.Tuple.Sort;\n", "std_sort_strings.fleaux")
                .has_value());
    const auto lowered =
        lower_and_check("import Std;\n(((1, 2), (3, 4))) -> Std.Tuple.Sort;\n", "std_sort_tuples.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().hint->find("Std.Tuple.Sort requires T:") != std::string::npos);
  }

  SECTION("Comparisons require numbers of the same type") {
    const auto lowered = lower_and_check("import Std;\n(\"a\", \"b\") -> <;\n", "std_compare_strings.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type argument does not satisfy its bound") != std::string::npos);
  }
}

TEST_CASE("Type helpers drop Never from unions", "[typecheck][types]") {
  using fleaux::frontend::type_system::make_type;
  using fleaux::frontend::type_system::normalize_type;
  using fleaux::frontend::type_system::TypeKind;

  Type with_never = make_type(TypeKind::kUnion);
  with_never.union_members = {make_type(TypeKind::kInt64), make_type(TypeKind::kNever)};
  REQUIRE(normalize_type(with_never).kind == TypeKind::kInt64);

  Type only_never = make_type(TypeKind::kUnion);
  only_never.union_members = {make_type(TypeKind::kNever), make_type(TypeKind::kNever)};
  REQUIRE(normalize_type(only_never).kind == TypeKind::kNever);
}

TEST_CASE("Type checker types Result constructors with Never for the unknown side", "[typecheck][result][never]") {
  SECTION("Ok and Err branches join to one Result type") {
    REQUIRE(lower_and_check("import Std;\n"
                            "let Pick(c: Bool): Result<Int64, String> =\n"
                            "  (c, (1) -> Std.Result.Ok, (\"bad\") -> Std.Result.Err) -> Std.Select;\n",
                            "result_join_select.fleaux")
                .has_value());
  }

  SECTION("The joined type is precise, not Any") {
    const auto lowered = lower_and_check("import Std;\n"
                                         "let Pick(c: Bool): Result<Int64, Int64> =\n"
                                         "  (c, (1) -> Std.Result.Ok, (\"bad\") -> Std.Result.Err) -> Std.Select;\n",
                                         "result_join_precise.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in function return") != std::string::npos);
  }

  SECTION("Two different success types still do not join") {
    const auto lowered = lower_and_check("import Std;\n"
                                         "(True, (1) -> Std.Result.Ok, (\"a\") -> Std.Result.Ok) -> Std.Select;\n",
                                         "result_join_conflict.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().hint->find("Std.Select expects argument 2") != std::string::npos);
  }

  SECTION("Payload of an Ok value is the success type") {
    REQUIRE(lower_and_check("import Std;\nlet P(): Int64 = (5) -> Std.Result.Ok -> Std.Result.Payload;\n",
                            "result_ok_payload.fleaux")
                .has_value());
  }

  SECTION("Unwrapping an Err value has type Never and fits any declared type") {
    REQUIRE(lower_and_check("import Std;\n"
                            "let Fails(): Int64 = (\"boom\") -> Std.Result.Err -> Std.Result.Unwrap;\n",
                            "result_err_unwrap_never.fleaux")
                .has_value());
  }

  SECTION("Ok values flow into a declared error type") {
    REQUIRE(lower_and_check("import Std;\n"
                            "let Wrap(x: Int64): Result<Int64, Tuple<Int64, String>> = (x) -> Std.Result.Ok;\n",
                            "result_ok_declared_error.fleaux")
                .has_value());
  }
}

TEST_CASE("Generic binding joins Never inside tuples", "[typecheck][never]") {
  REQUIRE(lower_and_check("let Std.Select<T>(c: Bool, a: T, b: T): T :: __builtin__;\n"
                          "let Left(): Tuple<Int64, Never> :: __builtin__;\n"
                          "let Right(): Tuple<Never, String> :: __builtin__;\n"
                          "let Both(c: Bool): Tuple<Int64, String> = (c, Left, Right) -> Std.Select;\n",
                          "never_join_tuple.fleaux")
              .has_value());
}

TEST_CASE("Type checker passes callbacks whose parameters are wider than expected", "[typecheck][functions]") {
  SECTION("A callback that accepts a union fits a callback slot for one member") {
    REQUIRE(lower_and_check("import Std;\n"
                            "let Describe(x: Int64 | String): String = x -> Std.ToString;\n"
                            "let Run(): Tuple<String...> = ((1, 2), Describe) -> Std.Tuple.Map;\n",
                            "callback_wide_param.fleaux")
                .has_value());
  }

  SECTION("A callback whose result is wider than the declared result is rejected") {
    const auto lowered = lower_and_check("let Apply(f: (Int64) => Int64, x: Int64): Int64 :: __builtin__;\n"
                                         "let Widen(x: Int64): Int64 | String = x;\n"
                                         "(Widen, 1) -> Apply;\n",
                                         "callback_wide_return.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Apply expects argument 0") != std::string::npos);
  }
}

TEST_CASE("Type checker types variadic parameters as tuples inside the body", "[typecheck][variadic]") {
  SECTION("The variadic parameter is a tuple of its declared item type") {
    REQUIRE(lower_and_check("let Collect(items: Int64...): Tuple<Int64...> = items;\n"
                            "let Rest(head: String, rest: Int64...): Tuple<Int64...> = rest;\n",
                            "variadic_param_tuple.fleaux")
                .has_value());
  }

  SECTION("The variadic parameter is not a single item") {
    const auto lowered =
        lower_and_check("let Bad(items: Int64...): Int64 = items;\n", "variadic_param_not_item.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in function return") != std::string::npos);
  }

  SECTION("Closure variadic parameters are tuples too") {
    REQUIRE(lower_and_check("import Std;\n"
                            "let Count: (Int64...) => Int64 = (items: Int64...): Int64 = items -> Std.Length;\n",
                            "variadic_closure_param_tuple.fleaux")
                .has_value());
  }
}

TEST_CASE("Type checker fills in empty dictionary types from the first Set", "[typecheck][dict][never]") {
  SECTION("Key and value types come from the first entry") {
    REQUIRE(lower_and_check("import Std;\n"
                            "let Build(): Dict<String, Int64> =\n"
                            "  () -> Std.Dict.Create -> (_, \"a\", 1) -> Std.Dict.Set -> (_, \"b\", 2) -> Std.Dict.Set;\n",
                            "dict_create_join.fleaux")
                .has_value());
  }

  SECTION("The filled-in types are precise") {
    const auto lowered = lower_and_check("import Std;\n"
                                         "let Build(): Dict<String, String> =\n"
                                         "  () -> Std.Dict.Create -> (_, \"a\", 1) -> Std.Dict.Set;\n",
                                         "dict_create_join_precise.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in function return") != std::string::npos);
  }

  SECTION("Entries of different value types need a declared dictionary type") {
    const auto mixed = lower_and_check("import Std;\n"
                                       "() -> Std.Dict.Create -> (_, \"a\", 1) -> Std.Dict.Set\n"
                                       "  -> (_, \"b\", \"two\") -> Std.Dict.Set;\n",
                                       "dict_create_mixed.fleaux");
    REQUIRE_FALSE(mixed.has_value());

    REQUIRE(lower_and_check("import Std;\n"
                            "let Build(): Dict<String, Any> = {\n"
                            "  let empty: Dict<String, Any> = () -> Std.Dict.Create;\n"
                            "  empty -> (_, \"a\", 1) -> Std.Dict.Set -> (_, \"b\", \"two\") -> Std.Dict.Set;\n"
                            "};\n",
                            "dict_create_declared.fleaux")
                .has_value());
  }
}

TEST_CASE("Std signatures reject mistyped builtin arguments without builtin contracts", "[typecheck][std][signatures]") {
  struct Case {
    const char* name;
    const char* call;
    const char* expected_hint;
  };
  const std::vector<Case> cases = {
      {"String parameter", "(1) -> Std.Path.Basename;", "Std.Path.Basename expects argument 0"},
      {"String parameters of a two-argument builtin", "(\"a\", 1) -> Std.OS.SetEnv;", "Std.OS.SetEnv expects argument 1"},
      {"Variadic String parameters", "(\"a\", \"b\", 3) -> Std.Path.Join;", "Std.Path.Join expects variadic argument 2"},
      {"UInt64 index rejects Int64", "((1, 2, 3), 1) -> Std.Array.GetAt;", "Std.Array.GetAt expects argument 1"},
      {"UInt64 index rejects Float64", "((1, 2, 3), 1.0) -> Std.Take;", "Std.Take expects argument 1"},
      {"UInt64 bit shift count", "(1, 2) -> Std.Bit.ShiftLeft;", "Std.Bit.ShiftLeft expects argument 1"},
      {"UInt64 tuple of ND indices", "(((1, 2), (3, 4)), (0, 1)) -> Std.Array.GetAtND;", "Std.Array.GetAtND expects argument 1"},
      {"UInt64 seed", "(5) -> Std.Random.Create;", "Std.Random.Create expects argument 0"},
      {"Int64 exit code", "(1u64) -> Std.Exit;", "Std.Exit"},
      {"Int64 range bounds", "(0u64, 3u64) -> Std.Tuple.Range;", "Std.Tuple.Range"},
      {"FileHandle parameter", "(1) -> Std.File.ReadLine;", "Std.File.ReadLine expects argument 0"},
      {"RandomGenerator parameter", "(1) -> Std.Random.NextInt64;", "Std.Random.NextInt64 expects argument 0"},
      {"UInt64 loop limit", "(1.0, (x: Float64): Bool = True, (x: Float64): Float64 = x, 5) -> Std.LoopN;", "Std.LoopN expects argument 3"},
      {"Callable arity", "((1, 2), (a: Int64, b: Int64): Int64 = a) -> Std.Tuple.Map;", "Std.Tuple.Map expects argument 1"},
      {"Non-callable callback", "((1, 2), 3) -> Std.Tuple.Filter;", "Std.Tuple.Filter expects argument 1"},
  };

  for (const auto& [name, call, expected_hint] : cases) {
    INFO(name);
    const auto lowered = lower_and_check(std::string{"import Std;\n"} + call + "\n", "std_signature_rejects.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    INFO("message: " << lowered.error().message);
    INFO("hint: " << lowered.error().hint.value_or(""));
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find(expected_hint) != std::string::npos);
  }
}

namespace {

auto find_cast_name_ref(const fleaux::frontend::ir::IRProgram& program, const std::string& let_name)
    -> const fleaux::frontend::ir::IRNameRef* {
  for (const auto& let : program.lets) {
    if (let.name != let_name || !let.body.has_value()) {
      continue;
    }
    if (const auto* flow = std::get_if<fleaux::frontend::ir::IRFlowExpr>(&let.body->node); flow != nullptr) {
      return std::get_if<fleaux::frontend::ir::IRNameRef>(&flow->rhs);
    }
  }
  return nullptr;
}

}  // namespace

TEST_CASE("Type checker attaches runtime checks to casts from Any", "[typecheck][cast]") {
  using fleaux::common::RuntimeTypeCheck;
  using fleaux::common::RuntimeTypeCheckKind;

  const auto lowered = lower_and_check("import Std;\n"
                                       "type UserId :: Int64;\n"
                                       "let FromAny(x: Any): Int64 = x -> Std.Cast<Int64>;\n"
                                       "let Known(x: Int64): UserId = x -> Std.Cast<UserId>;\n"
                                       "let Strong(x: Any): UserId = x -> Std.Cast<UserId>;\n"
                                       "let Try(x: Any): Result<Tuple<String, Int64...>, String> =\n"
                                       "  x -> Std.TryCast<Tuple<String, Int64...>>;\n"
                                       "let Handle(x: Any): FileHandle = x -> Std.Cast<FileHandle>;\n",
                                       "cast_runtime_checks.fleaux");
  REQUIRE(lowered.has_value());

  const auto* from_any = find_cast_name_ref(*lowered, "FromAny");
  REQUIRE(from_any != nullptr);
  REQUIRE(from_any->runtime_type_check == RuntimeTypeCheck{.kind = RuntimeTypeCheckKind::kInt64});

  const auto* known = find_cast_name_ref(*lowered, "Known");
  REQUIRE(known != nullptr);
  REQUIRE_FALSE(known->runtime_type_check.has_value());

  const auto* strong = find_cast_name_ref(*lowered, "Strong");
  REQUIRE(strong != nullptr);
  REQUIRE(strong->runtime_type_check == RuntimeTypeCheck{.kind = RuntimeTypeCheckKind::kInt64});

  const auto* try_cast = find_cast_name_ref(*lowered, "Try");
  REQUIRE(try_cast != nullptr);
  REQUIRE(try_cast->runtime_type_check ==
          RuntimeTypeCheck{.kind = RuntimeTypeCheckKind::kTuple,
                           .items = {RuntimeTypeCheck{.kind = RuntimeTypeCheckKind::kString},
                                     RuntimeTypeCheck{.kind = RuntimeTypeCheckKind::kInt64}},
                           .variadic_tail = true});

  const auto* handle = find_cast_name_ref(*lowered, "Handle");
  REQUIRE(handle != nullptr);
  REQUIRE(handle->runtime_type_check == RuntimeTypeCheck{.kind = RuntimeTypeCheckKind::kFileHandle});
}

TEST_CASE("Type checker rejects casts from Any that cannot be checked at runtime", "[typecheck][cast]") {
  struct Case {
    const char* name;
    const char* body;
    const char* expected_hint;
  };
  const std::vector<Case> cases = {
      {"Function type", "let F(x: Any): (Int64) => Int64 = x -> Std.Cast<(Int64) => Int64>;",
       "function parameter and return types"},
      {"Task result type", "let F(x: Any): TaskHandle<Int64> = x -> Std.Cast<TaskHandle<Int64>>;",
       "cast to TaskHandle<Any>"},
      {"Generic parameter", "let F<T>(x: Any): T = x -> Std.Cast<T>;", "generic parameters"},
      {"Function inside a tuple", "let F(x: Any): Tuple<(Int64) => Int64> = x -> Std.TryCast<Tuple<(Int64) => Int64>> -> Std.Result.Unwrap;",
       "Std.TryCast from Any cannot check"},
  };
  for (const auto& [name, body, expected_hint] : cases) {
    INFO(name);
    const auto lowered = lower_and_check(std::string{"import Std;\n"} + body + "\n", "cast_unsupported.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().hint.has_value());
    INFO("hint: " << *lowered.error().hint);
    REQUIRE(lowered.error().hint->find(expected_hint) != std::string::npos);
  }

  SECTION("TaskHandle<Any> is checked by its tag") {
    REQUIRE(lower_and_check("import Std;\nlet F(x: Any): TaskHandle<Any> = x -> Std.Cast<TaskHandle<Any>>;\n",
                            "cast_task_any.fleaux")
                .has_value());
  }

  SECTION("Std.TryCast is only valid in direct call position") {
    const auto lowered = lower_and_check("import Std;\nlet F: Any = Std.TryCast<Int64>;\n", "trycast_reference.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Invalid Std.TryCast reference") != std::string::npos);
  }
}

TEST_CASE("Type checker treats Any as a strict top type", "[typecheck][strict_any]") {
  const auto rejected = [](const std::string& body, const std::string& source_name) {
    auto lowered = lower_and_check("import Std;\n" + body + "\n", source_name);
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().hint.has_value());
    return lowered.error();
  };

  SECTION("Every value flows into Any") {
    REQUIRE(lower_and_check("import Std;\n"
                            "let Keep(x: Int64): Any = x;\n"
                            "let Show(x: Any): String = x -> Std.ToString;\n"
                            "let Pair(x: Any): Tuple<Any, Any> = (x, x);\n",
                            "strict_any_into_any.fleaux")
                .has_value());
  }

  SECTION("An Any value does not flow into a specific type") {
    const auto error = rejected("let Bad(x: Any): Int64 = x;", "strict_any_return.fleaux");
    REQUIRE(error.message.find("Type mismatch in function return") != std::string::npos);
    REQUIRE(error.hint->find("Bad declares return type Int64 but its body has type Any") != std::string::npos);
    REQUIRE(error.hint->find("check it with '-> Std.Cast<Int64>' first") != std::string::npos);
  }

  SECTION("An Any argument does not match a specific parameter") {
    const auto error = rejected("let Bad(x: Any): String = x -> Std.String.Upper;", "strict_any_argument.fleaux");
    REQUIRE(error.hint->find("Std.String.Upper expects argument 0") != std::string::npos);
    REQUIRE(error.hint->find("Std.Cast<String>") != std::string::npos);
  }

  SECTION("An Any local initializer does not match a specific declared type") {
    const auto error = rejected("let Bad(x: Any): Int64 = { let y: Int64 = x; y; };", "strict_any_local.fleaux");
    REQUIRE(error.message.find("Type mismatch in local let initializer") != std::string::npos);
    REQUIRE(error.hint->find("Std.Cast<Int64>") != std::string::npos);
  }

  SECTION("Any satisfies no generic bound") {
    const auto error = rejected("let Bad(x: Any): Any = (x, 1) -> Std.Add;", "strict_any_bound.fleaux");
    REQUIRE(error.message.find("Type argument does not satisfy its bound") != std::string::npos);
    REQUIRE(error.hint->find("A value of type Any satisfies no bound") != std::string::npos);
  }

  SECTION("An unbounded generic parameter binds to Any and returns Any") {
    REQUIRE(lower_and_check("import Std;\nlet Keep(x: Any): Any = (True, x, x) -> Std.Select;\n",
                            "strict_any_generic_ok.fleaux")
                .has_value());
    const auto error = rejected("let Bad(x: Any): Int64 = (True, x, x) -> Std.Select;", "strict_any_generic_bad.fleaux");
    REQUIRE(error.message.find("Type mismatch in function return") != std::string::npos);
  }

  SECTION("A cast makes an Any value usable") {
    REQUIRE(lower_and_check("import Std;\nlet Good(x: Any): Int64 = (x -> Std.Cast<Int64>, 1) -> Std.Add;\n",
                            "strict_any_cast_ok.fleaux")
                .has_value());
  }
}

TEST_CASE("Generic binding widens to a covering type and checks every argument again", "[typecheck][strict_any][generics]") {
  SECTION("A tuple with an Any tail binds a repeated parameter to Any") {
    REQUIRE(lower_and_check("let Std.Length<T>(tuple: Tuple<T...>): Int64 :: __builtin__;\n"
                            "let Count(xs: Tuple<String, Any...>): Int64 = xs -> Std.Length;\n",
                            "widen_variadic_any.fleaux")
                .has_value());
  }

  SECTION("A value that fits an existing binding keeps it") {
    REQUIRE(lower_and_check("type Dict<K, V> :: __builtin__;\n"
                            "let Std.Dict.Set<K, V>(dict: Dict<K, V>, key: K, value: V): Dict<K, V> :: __builtin__;\n"
                            "let Put(d: Dict<String, Any>): Dict<String, Any> = (d, \"k\", 1) -> Std.Dict.Set;\n",
                            "binding_fits_existing.fleaux")
                .has_value());
  }

  SECTION("Widening is rejected when an earlier callback only accepts the narrower type") {
    const auto lowered = lower_and_check("let Std.Combine<T>(a: T, f: (T) => T, b: T): T :: __builtin__;\n"
                                         "let IntOnly(x: Int64): Int64 = x;\n"
                                         "let Bad(x: Any): Any = (1, IntOnly, x) -> Std.Combine;\n",
                                         "widen_recheck_callback.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("Std.Combine expects argument 1") != std::string::npos);
  }

  SECTION("Explicit type arguments are never widened") {
    const auto lowered = lower_and_check("let Std.Identity<T>(x: T): T :: __builtin__;\n"
                                         "let Bad(x: Int64 | String): Int64 = x -> Std.Identity<Int64>;\n",
                                         "explicit_type_arg_not_widened.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("explicit type argument T = Int64") != std::string::npos);
  }
}

TEST_CASE("Builtin contracts do not let Any through", "[typecheck][strict_any][contracts]") {
  const auto rejected = [](const std::string& body, const std::string& name) {
    auto lowered = lower_and_check("import Std;\n" + body + "\n", name);
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().hint.has_value());
    return *lowered.error().hint;
  };

  SECTION("Numeric conversions take one numeric type") {
    REQUIRE(lower_and_check("import Std;\nlet A(x: Float64): Int64 = x -> Std.ToInt64;\n"
                            "let B(x: Int64): Float64 = x -> Std.ToFloat64;\n",
                            "convert_numeric_ok.fleaux")
                .has_value());
    REQUIRE(rejected("let Bad(x: Any): Int64 = x -> Std.ToInt64;", "convert_any.fleaux")
                .find("A value of type Any satisfies no bound") != std::string::npos);
    REQUIRE(rejected("let Bad(x: String): Int64 = x -> Std.ToInt64;", "convert_string.fleaux")
                .find("Std.ToInt64 requires T") != std::string::npos);
  }

  SECTION("Dictionary keys cannot have type Any") {
    REQUIRE(rejected("let Bad(d: Dict<Any, Int64>, k: Any): Dict<Any, Int64> = (d, k, 1) -> Std.Dict.Set;",
                     "dict_any_key.fleaux")
                .find("requires dictionary key types to be scalar") != std::string::npos);
  }

  SECTION("Std.Match is only valid in direct call position") {
    const auto lowered = lower_and_check("import Std;\nlet F: Any = Std.Match;\n", "match_reference.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Invalid Std.Match reference") != std::string::npos);
  }
}

TEST_CASE("Operators call user operator functions of strong operand types", "[typecheck][operators][user_operators]") {
  const std::string money =
      "import Std;\n"
      "type Money = Int64;\n"
      "let Money.Add(a: Money, b: Money): Money = ((a -> Std.Cast<Int64>, b -> Std.Cast<Int64>) -> +) -> "
      "Std.Cast<Money>;\n"
      "let Money.Multiply(m: Money, k: Int64): Money = ((m -> Std.Cast<Int64>, k) -> *) -> Std.Cast<Money>;\n";

  const auto rhs_of = [](const fleaux::frontend::ir::IRProgram& program, const std::string& let_name) {
    for (const auto& let : program.lets) {
      if (let.name == let_name && let.body.has_value()) {
        const auto& flow = std::get<fleaux::frontend::ir::IRFlowExpr>(let.body->node);
        return std::get<fleaux::frontend::ir::IRNameRef>(flow.rhs);
      }
    }
    FAIL("let not found");
    return fleaux::frontend::ir::IRNameRef{};
  };

  SECTION("The operator is replaced by the user function") {
    const auto lowered = lower_and_check(money + "let Sum(a: Money, b: Money): Money = (a, b) -> +;\n"
                                                 "let Scale(a: Money): Money = (a, 3) -> *;\n"
                                                 "let Plain(a: Int64): Int64 = (a, 1) -> +;\n",
                                         "user_operators_resolve.fleaux");
    REQUIRE(lowered.has_value());
    const auto sum = rhs_of(*lowered, "Sum");
    REQUIRE(sum.qualifier == std::optional<std::string>{"Money"});
    REQUIRE(sum.name == "Add");
    REQUIRE(rhs_of(*lowered, "Scale").name == "Multiply");
    REQUIRE(rhs_of(*lowered, "Plain").qualifier == std::optional<std::string>{"Std"});
  }

  SECTION("Operand order is not swapped") {
    const auto lowered =
        lower_and_check(money + "let Bad(a: Money): Money = (3, a) -> *;\n", "user_operators_order.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().hint->find("tried Money.Multiply") != std::string::npos);
  }

  SECTION("A missing operator function reports the Std builtin") {
    const auto lowered =
        lower_and_check(money + "let Bad(a: Money, b: Money): Money = (a, b) -> /;\n", "user_operators_missing.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().hint->find("Operator '/' calls Std.Divide") != std::string::npos);
  }

  SECTION("Two matching operator functions are ambiguous") {
    const auto lowered = lower_and_check("import Std;\n"
                                         "type A = Int64;\n"
                                         "type B = Int64;\n"
                                         "let A.Add(a: A, b: B): A = a;\n"
                                         "let B.Add(a: A, b: B): B = b;\n"
                                         "let Bad(a: A, b: B): A = (a, b) -> +;\n",
                                         "user_operators_ambiguous.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Ambiguous operator") != std::string::npos);
    REQUIRE(lowered.error().hint->find("A.Add, B.Add") != std::string::npos);
  }

  SECTION("Operator functions that Std already covers are rejected") {
    const auto lowered = lower_and_check(money + "let Money.Equal(a: Money, b: Money): Bool = True;\n",
                                         "user_operators_redefine.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Operator function redefines a builtin operator") != std::string::npos);
    REQUIRE(lowered.error().hint->find("'==' on Money is provided by Std.Equal") != std::string::npos);
  }

  SECTION("Builtin types and aliases do not get user operators") {
    const auto lowered = lower_and_check("import Std;\n"
                                         "alias Cents = Int64;\n"
                                         "let Cents.Add(a: String, b: String): String = a;\n"
                                         "let Bad(a: String, b: String): String = (a, b) -> +;\n",
                                         "user_operators_alias.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().hint->find("tried") == std::string::npos);
  }
}

TEST_CASE("Generic functions passed as values take type arguments from the parameter type",
          "[typecheck][generics][open_functions]") {
  const std::string decls =
      "import Std;\n"
      "let SumWith<T>(xs: Tuple<T...>, zero: T, add: (T, T) => T): T = (xs, zero, add) -> Std.Tuple.Reduce;\n"
      "let Twice<T: Numeric>(x: T): T = (x, x) -> +;\n";

  SECTION("Builtin, user, and closure generics are inferred") {
    REQUIRE(lower_and_check(decls + "let A(): Int64 = ((1, 2), 0, Std.Add) -> SumWith;\n"
                                    "let B(): Float64 = ((1.5, 2.5), 0.0, Std.Add) -> SumWith;\n"
                                    "let C(): Tuple<Int64...> = ((1, 2), Twice) -> Std.Tuple.Map;\n"
                                    "let D(): Tuple<Tuple<String, String>...> =\n"
                                    "  ((\"a\", \"b\"), <T>(x: T): Tuple<T, T> = (x, x)) -> Std.Tuple.Map;\n",
                            "open_functions_inferred.fleaux")
                .has_value());
  }

  SECTION("The inferred return type flows on") {
    const auto lowered =
        lower_and_check(decls + "let Bad(): String = ((1, 2), Twice) -> Std.Tuple.Map;\n", "open_functions_return.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().hint->find("Bad declares return type String but its body has type Tuple<Int64...>") !=
            std::string::npos);
  }

  SECTION("A declared function type supplies the type arguments") {
    REQUIRE(lower_and_check(decls + "let AddInts: (Int64, Int64) => Int64 = Std.Add;\n", "open_functions_let.fleaux")
                .has_value());
    REQUIRE_FALSE(
        lower_and_check(decls + "let AddStrings: (String, String) => String = Std.Add;\n", "open_functions_let_bad.fleaux")
            .has_value());
  }

  SECTION("Inferred type arguments must satisfy the bounds") {
    const auto lowered = lower_and_check(decls + "let Bad(): String = ((\"a\", \"b\"), \"\", Std.Add) -> SumWith;\n",
                                         "open_functions_bound.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().hint->find("SumWith expects argument 2 to be (String, String) => String, but Std.Add "
                                       "cannot take those parameter types") != std::string::npos);
  }

  SECTION("Type parameters of the enclosing generic body can be the type arguments") {
    REQUIRE(lower_and_check(decls + "let Total<T: Numeric>(xs: Tuple<T...>, zero: T): T = (xs, zero, Std.Add) -> SumWith;\n",
                            "open_functions_generic_body.fleaux")
                .has_value());
    const auto unbounded = lower_and_check(
        decls + "let Total<T>(xs: Tuple<T...>, zero: T): T = (xs, zero, Std.Add) -> SumWith;\n",
        "open_functions_generic_body_unbounded.fleaux");
    REQUIRE_FALSE(unbounded.has_value());
  }

  SECTION("Any is not a function type to take type arguments from") {
    const auto lowered = lower_and_check(decls + "let Bad(): Any = Std.Add -> Std.Type;\n", "open_functions_any.fleaux");
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().hint->find("Std.Add could not infer generic callable type variable(s): T") !=
            std::string::npos);
    REQUIRE(lowered.error().hint->find("Any is not a function type") != std::string::npos);
  }
}

namespace {

auto find_let(const fleaux::frontend::ir::IRProgram& program, const std::string_view name)
    -> const fleaux::frontend::ir::IRLet& {
  for (const auto& let : program.lets) {
    if (let.name == name) {
      return let;
    }
  }
  FAIL("let not found: " << name);
  return program.lets.front();
}

const std::string kMoneyOperators =
    "import Std;\n"
    "type Money = Int64;\n"
    "let Money.Add(a: Money, b: Money): Money = ((a -> Std.Cast<Int64>, b -> Std.Cast<Int64>) -> +) -> "
    "Std.Cast<Money>;\n"
    "let M(x: Int64): Money = x -> Std.Cast<Money>;\n"
    "alias Summable = Int64 | Money;\n";

}  // namespace

TEST_CASE("Generic functions take operators of strong-bounded type parameters as hidden arguments",
          "[typecheck][operators][operator_requirements]") {
  using fleaux::frontend::ir::IRFlowExpr;
  using fleaux::frontend::ir::IRNameRef;
  using fleaux::frontend::ir::IRTupleExpr;

  SECTION("The operator becomes a call through a hidden leading parameter") {
    const auto lowered =
        lower_and_check(kMoneyOperators + "let Add2<T: Summable>(a: T, b: T): T = (a, b) -> +;\n", "req_body.fleaux");
    REQUIRE(lowered.has_value());
    const auto& add2 = find_let(*lowered, "Add2");
    REQUIRE(add2.hidden_operator_params == 1U);
    REQUIRE(add2.params.size() == 3U);
    REQUIRE(add2.params[0].name == "'op0");
    const auto& flow = std::get<IRFlowExpr>(add2.body->node);
    REQUIRE(std::get<IRNameRef>(flow.rhs).name == "Apply");
  }

  SECTION("Callers pass the function for the type the parameter binds to") {
    const auto lowered = lower_and_check(kMoneyOperators + "let Add2<T: Summable>(a: T, b: T): T = (a, b) -> +;\n"
                                                           "let OnMoney(a: Money, b: Money): Money = (a, b) -> Add2;\n"
                                                           "let OnInts(a: Int64, b: Int64): Int64 = (a, b) -> Add2;\n",
                                         "req_callers.fleaux");
    REQUIRE(lowered.has_value());
    const auto hidden_arg = [&](const std::string& let_name) {
      const auto& flow = std::get<IRFlowExpr>(find_let(*lowered, let_name).body->node);
      const auto& args = std::get<IRTupleExpr>(flow.lhs->node);
      REQUIRE(args.items.size() == 3U);
      return std::get<IRNameRef>(args.items.front()->node);
    };
    const auto money = hidden_arg("OnMoney");
    REQUIRE(money.qualifier == std::optional<std::string>{"Money"});
    REQUIRE(money.name == "Add");
    const auto ints = hidden_arg("OnInts");
    REQUIRE(ints.qualifier == std::optional<std::string>{"Std"});
    REQUIRE(ints.name == "Add");
  }

  SECTION("Generic callers inherit the requirement and pass their own hidden parameter") {
    const auto lowered = lower_and_check(kMoneyOperators + "let Add2<T: Summable>(a: T, b: T): T = (a, b) -> +;\n"
                                                           "let Twice<T: Summable>(x: T): T = (x, x) -> Add2;\n",
                                         "req_inherit.fleaux");
    REQUIRE(lowered.has_value());
    const auto& twice = find_let(*lowered, "Twice");
    REQUIRE(twice.hidden_operator_params == 1U);
    const auto& flow = std::get<IRFlowExpr>(twice.body->node);
    const auto& args = std::get<IRTupleExpr>(flow.lhs->node);
    REQUIRE(std::get<IRNameRef>(args.items.front()->node).name == "'op0");
  }

  SECTION("Closures in the body capture the hidden parameter") {
    const auto lowered = lower_and_check(
        kMoneyOperators + "let Total<T: Summable>(xs: Tuple<T...>, zero: T): T =\n"
                          "  (xs, zero, (acc: T, x: T): T = (acc, x) -> +) -> Std.Tuple.Reduce;\n",
        "req_closure.fleaux");
    REQUIRE(lowered.has_value());
    const auto& total = find_let(*lowered, "Total");
    const auto& flow = std::get<IRFlowExpr>(total.body->node);
    const auto& args = std::get<IRTupleExpr>(flow.lhs->node);
    const auto& closure = *std::get<fleaux::frontend::ir::IRClosureExprBox>(args.items.back()->node);
    REQUIRE(std::ranges::find(closure.captures, "'op0") != closure.captures.end());
  }

  SECTION("Bounds made only of builtin types need no hidden parameters") {
    const auto lowered =
        lower_and_check("import Std;\nlet Twice<T: Numeric>(x: T): T = (x, x) -> +;\n", "req_builtin_bound.fleaux");
    REQUIRE(lowered.has_value());
    REQUIRE(find_let(*lowered, "Twice").hidden_operator_params == 0U);
  }

  const auto rejected = [](const std::string& body, const std::string& name) {
    auto lowered = lower_and_check(kMoneyOperators + body, name);
    REQUIRE_FALSE(lowered.has_value());
    return lowered.error();
  };

  SECTION("Every alternative of the bound needs a function") {
    const auto error = rejected("let Bad<T: Summable>(a: T, b: T): T = (a, b) -> *;\n", "req_missing.fleaux");
    REQUIRE(error.hint->find("Operator '*' on T needs a function for every alternative of its bound; for Money") !=
            std::string::npos);
  }

  SECTION("A value of a function with requirements needs explicit type arguments") {
    const auto error = rejected("let Add2<T: Summable>(a: T, b: T): T = (a, b) -> +;\n"
                                "let F: (Int64, Int64) => Int64 = Add2;\n",
                                "req_value.fleaux");
    REQUIRE(error.message.find("Missing type arguments") != std::string::npos);
    REQUIRE(error.hint->find("such as 'Add2<T>'") != std::string::npos);

    const auto lowered = lower_and_check(kMoneyOperators + "let Add2<T: Summable>(a: T, b: T): T = (a, b) -> +;\n"
                                                           "let F: (Money, Money) => Money = Add2<Money>;\n",
                                         "req_value_explicit.fleaux");
    REQUIRE(lowered.has_value());
    const auto& value = std::get<IRNameRef>(find_let(*lowered, "F").body->node);
    REQUIRE(value.bound_operator_args.size() == 1U);
    REQUIRE(std::get<IRNameRef>(value.bound_operator_args.front()->node).qualifier == std::optional<std::string>{"Money"});
  }

  SECTION("One operator may use at most one strong-bounded type parameter") {
    const auto error =
        rejected("let Mix<T: Summable, U: Summable>(a: T, b: U): T = (a, b) -> +;\n", "req_two_params.fleaux");
    REQUIRE(error.hint->find("more than one type parameter") != std::string::npos);
  }

  SECTION("Generic closures cannot use operators of strong-bounded type parameters") {
    const auto error =
        rejected("let F(): (Int64) => Int64 = <T: Summable>(x: T): T = (x, x) -> +;\n", "req_closure_param.fleaux");
    REQUIRE(error.hint->find("a type parameter of a generic closure") != std::string::npos);
  }
}

TEST_CASE("Type checker parameter lists report argument shape errors with fixes", "[typecheck][parameter-lists]") {
  const std::string prelude =
      "import Std;\n"
      "let Pair(a: Int64, b: Int64): Int64 = (a, b) -> +;\n"
      "let Whole(t: Tuple<Int64, Int64>): Int64 = t -> Std.Length;\n"
      "let V(xs: Int64...): Int64 = xs -> Std.Length;\n"
      "let P: Tuple<Int64, Int64> = (1, 2);\n"
      "let Many: Tuple<Int64...> = (1, 2, 3);\n"
      "let N: Int64 = 5;\n";

  const auto lower_error = [&](const std::string& body) {
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(prelude + body, "typecheck_parameter_lists.fleaux");
    REQUIRE(parsed.has_value());
    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    return lowered.error();
  };
  const auto full_text = [](const auto& error) -> std::string {
    return error.message + " " + error.hint.value_or("");
  };

  SECTION("A tuple value passed to a two-parameter function suggests a spread") {
    const auto text = full_text(lower_error("P -> Pair;\n"));
    REQUIRE(text.find("expects 2 argument(s) but got 1") != std::string::npos);
    REQUIRE(text.find("spread it: '(x...)'") != std::string::npos);
  }

  SECTION("A parameter list passed to a one-parameter function suggests a trailing comma") {
    const auto text = full_text(lower_error("(1, 2) -> Whole;\n"));
    REQUIRE(text.find("expects 1 argument(s) but got 2") != std::string::npos);
    REQUIRE(text.find("trailing comma") != std::string::npos);
  }

  SECTION("A spread of unknown length can only fill a variadic tail") {
    REQUIRE(full_text(lower_error("(Many...) -> Pair;\n")).find("spread tuple of unknown length") != std::string::npos);
    REQUIRE(full_text(lower_error("(Many..., 9) -> V;\n")).find("can only be spread as the last argument") !=
            std::string::npos);
  }

  SECTION("Only tuples can be spread") {
    const auto error = lower_error("(N...) -> V;\n");
    REQUIRE(error.message.find("Invalid spread") != std::string::npos);
    REQUIRE(full_text(error).find("this value has type Int64") != std::string::npos);
  }

  SECTION("A spread is only valid in a parameter list") {
    REQUIRE(lower_error("let Q: Tuple<Int64, Int64> = (P...);\n").message.find("Invalid spread") != std::string::npos);
    REQUIRE(full_text(lower_error("(P...,) -> Whole;\n")).find("trailing comma") != std::string::npos);
  }

  SECTION("Multi-parameter and tuple-parameter function types stay distinct") {
    REQUIRE(full_text(lower_error("let F: (Tuple<Int64, Int64>) => Int64 = Pair;\n")).find("(Int64, Int64) => Int64") !=
            std::string::npos);
    REQUIRE(full_text(lower_error("let G: (Int64, Int64) => Int64 = Whole;\n"))
                .find("(Tuple<Int64, Int64>) => Int64") != std::string::npos);
  }
}

TEST_CASE("Type checker checks calls of function values", "[typecheck][function-values]") {
  const std::string prelude =
      "import Std;\n"
      "let Inc(x: Int64): Int64 = (x, 1) -> +;\n"
      "let N: Int64 = 5;\n";

  const auto lower_error = [&](const std::string& body) {
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(prelude + body, "typecheck_function_values.fleaux");
    REQUIRE(parsed.has_value());
    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    return lowered.error();
  };
  const auto full_text = [](const auto& error) -> std::string { return error.message + " " + error.hint.value_or(""); };

  SECTION("A local sharing a function's name cannot be a call target") {
    const auto text = full_text(lower_error("let Use(Inc: (Int64) => Int64): Int64 = 5 -> Inc;\n"));
    REQUIRE(text.find("Ambiguous call target") != std::string::npos);
    REQUIRE(text.find("'Inc' names both a local value and a function") != std::string::npos);
  }

  SECTION("Only values of function type can be called") {
    REQUIRE(full_text(lower_error("let Use(x: Int64): Int64 = 3 -> x;\n"))
                .find("'x' is a value of type Int64, not a function") != std::string::npos);
    REQUIRE(full_text(lower_error("3 -> N;\n")).find("'N' is a value of type Int64, not a function") !=
            std::string::npos);
  }

  SECTION("Arguments follow the parameter-list rule") {
    const auto text = full_text(lower_error("let Use(f: (Int64) => Int64): Int64 = (1, 2) -> f;\n"));
    REQUIRE(text.find("f expects 1 argument(s) but got 2") != std::string::npos);
    REQUIRE(text.find("trailing comma") != std::string::npos);
    REQUIRE(full_text(lower_error("let Use(f: (Int64) => Int64): Int64 = \"a\" -> f;\n")).find("Type mismatch") !=
            std::string::npos);
  }

  SECTION("A function value takes no explicit type arguments") {
    REQUIRE(full_text(lower_error("let Use(f: (Int64) => Int64): Int64 = 5 -> f<Int64>;\n"))
                .find("is a function value and does not accept explicit type arguments") != std::string::npos);
  }

  SECTION("The call returns the function type's return type") {
    REQUIRE(lower_error("let Use(f: (Int64) => Int64): String = 5 -> f;\n").message.find("Type mismatch") !=
            std::string::npos);
  }
}

TEST_CASE("Type checker checks closure stage arguments", "[typecheck][function-values][closures]") {
  const auto lower_error = [](const std::string& body) {
    const fleaux::frontend::parse::Parser parser;
    const auto parsed =
        parser.parse_program("import Std;\nlet P: Tuple<Int64, Int64> = (3, 4);\n" + body, "typecheck_closure_stage.fleaux");
    REQUIRE(parsed.has_value());
    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    return lowered.error();
  };
  const auto full_text = [](const auto& error) -> std::string { return error.message + " " + error.hint.value_or(""); };

  SECTION("A parameter list passed to a one-parameter closure suggests a trailing comma") {
    const auto text = full_text(lower_error("(1, 2) -> (a: Int64): Int64 = a;\n"));
    REQUIRE(text.find("closure expects 1 argument(s) but got 2") != std::string::npos);
    REQUIRE(text.find("trailing comma") != std::string::npos);
  }

  SECTION("A tuple value passed to a two-parameter closure suggests a spread") {
    const auto text = full_text(lower_error("P -> (a: Int64, b: Int64): Int64 = a;\n"));
    REQUIRE(text.find("closure expects 2 argument(s) but got 1") != std::string::npos);
    REQUIRE(text.find("spread it") != std::string::npos);
  }

  SECTION("A generic closure's type arguments must satisfy its bounds") {
    REQUIRE(full_text(lower_error("\"a\" -> <T: Numeric>(x: T): T = x;\n"))
                .find("closure cannot take these arguments") != std::string::npos);
  }

  SECTION("Argument types are checked against the closure's parameters") {
    REQUIRE(lower_error("(1, \"a\") -> (a: Int64, b: Int64): Int64 = a;\n").message.find("Type mismatch") !=
            std::string::npos);
  }
}
