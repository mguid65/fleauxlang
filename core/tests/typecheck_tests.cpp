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

TEST_CASE("Type helpers normalize unions and convert IR simple types", "[typecheck][types]") {
  using fleaux::frontend::ir::IRSimpleType;
  using fleaux::frontend::type_system::Type;
  using fleaux::frontend::type_system::TypeKind;
  using fleaux::frontend::type_system::from_ir_type;
  using fleaux::frontend::type_system::is_integer_like;
  using fleaux::frontend::type_system::normalize_type;

  SECTION("normalize_type flattens, sorts, and deduplicates union members") {
    Type union_type{.kind = TypeKind::kUnion,
                    .union_members = {
                        Type{.kind = TypeKind::kFloat64},
                        Type{.kind = TypeKind::kInt64},
                        Type{.kind = TypeKind::kUnion,
                             .union_members = {Type{.kind = TypeKind::kUInt64}, Type{.kind = TypeKind::kInt64}}},
                    }};

    const auto normalized = normalize_type(std::move(union_type));
    REQUIRE(normalized.kind == TypeKind::kUnion);
    REQUIRE(normalized.union_members.size() == 3);
    REQUIRE(normalized.union_members[0].kind == TypeKind::kInt64);
    REQUIRE(normalized.union_members[1].kind == TypeKind::kUInt64);
    REQUIRE(normalized.union_members[2].kind == TypeKind::kFloat64);
  }

  SECTION("normalize_type collapses a single remaining union member") {
    Type union_type{.kind = TypeKind::kUnion,
                    .union_members = {
                        Type{.kind = TypeKind::kInt64},
                        Type{.kind = TypeKind::kUnion, .union_members = {Type{.kind = TypeKind::kInt64}}},
                    }};

    const auto normalized = normalize_type(std::move(union_type));
    REQUIRE(normalized.kind == TypeKind::kInt64);
  }

  SECTION("normalize_type orders mixed structured members through the internal sort keys") {
    Type variadic_item{.kind = TypeKind::kString, .variadic = true};
    Type variadic_param{.kind = TypeKind::kString, .variadic = true};
    Type union_type{.kind = TypeKind::kUnion,
                    .union_members = {
                        Type{.kind = TypeKind::kFunction, .function_params = {variadic_param}},
                        Type{.kind = TypeKind::kTuple, .items = {variadic_item}},
                        Type{.kind = TypeKind::kApplied,
                             .applied_name = "Box",
                             .applied_args = {Type{.kind = TypeKind::kInt64}}},
                        Type{.kind = TypeKind::kTypeVar, .nominal_name = "T"},
                        Type{.kind = TypeKind::kBool},
                        Type{.kind = TypeKind::kUnknown},
                        Type{.kind = TypeKind::kUnion,
                             .union_members = {Type{.kind = TypeKind::kString}, Type{.kind = TypeKind::kNull}}},
                    }};

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
        .param_types = {
            IRSimpleType{.name = "Int64"},
            IRSimpleType{.name = "String", .variadic = true},
        },
        .return_type = fleaux::frontend::make_box<IRSimpleType>(IRSimpleType{
            .name = "Dict",
            .type_args = {
                IRSimpleType{.name = "String"},
                IRSimpleType{.name = "Float64"},
            },
        }),
    };

    IRSimpleType union_type;
    union_type.alternative_types = {
        IRSimpleType{.name = "UInt64"},
        IRSimpleType{.name = "Int64"},
        IRSimpleType{.name = "UInt64"},
    };

    const auto callable = from_ir_type(callable_type);
    const auto structured_union = from_ir_type(union_type);
    const auto nominal = from_ir_type(IRSimpleType{.name = "Widget"});

    REQUIRE(callable.kind == TypeKind::kFunction);
    REQUIRE(callable.function_params.size() == 2);
    REQUIRE(callable.function_params[0].kind == TypeKind::kInt64);
    REQUIRE(callable.function_params[1].kind == TypeKind::kString);
    REQUIRE(callable.function_params[1].variadic);
    REQUIRE(callable.function_return.has_value());
    REQUIRE((**callable.function_return).kind == TypeKind::kApplied);
    REQUIRE((**callable.function_return).applied_name == "Dict");
    REQUIRE((**callable.function_return).applied_args.size() == 2);
    REQUIRE((**callable.function_return).applied_args[0].kind == TypeKind::kString);
    REQUIRE((**callable.function_return).applied_args[1].kind == TypeKind::kFloat64);

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
    REQUIRE(converted.union_members.size() == 6);
    REQUIRE(std::ranges::any_of(converted.union_members,
                                [](const Type& member) { return member.kind == TypeKind::kNever; }));
    REQUIRE(std::ranges::any_of(converted.union_members,
                                [](const Type& member) { return member.kind == TypeKind::kAny; }));
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
    REQUIRE(is_integer_like(Type{.kind = TypeKind::kInt64}));
    REQUIRE(is_integer_like(Type{.kind = TypeKind::kUInt64}));
    REQUIRE_FALSE(is_integer_like(Type{.kind = TypeKind::kFloat64}));
    REQUIRE_FALSE(is_integer_like(Type{.kind = TypeKind::kNominal, .nominal_name = "Int64"}));
  }
}

TEST_CASE("Type consistency handles unions tuples applied names and function signatures", "[typecheck][types]") {
  using fleaux::frontend::type_system::Type;
  using fleaux::frontend::type_system::TypeKind;
  using fleaux::frontend::type_system::is_consistent;

  const auto mk_function = [](std::vector<Type> params, std::optional<Type> return_type) -> Type {
    Type out{.kind = TypeKind::kFunction, .function_params = std::move(params)};
    if (return_type.has_value()) { out.function_return = fleaux::frontend::make_box<Type>(std::move(*return_type)); }
    return out;
  };

  SECTION("Any and Never follow the top and bottom consistency rules") {
    REQUIRE(is_consistent(Type{.kind = TypeKind::kAny}, Type{.kind = TypeKind::kString}));
    REQUIRE(is_consistent(Type{.kind = TypeKind::kString}, Type{.kind = TypeKind::kAny}));
    REQUIRE(is_consistent(Type{.kind = TypeKind::kString}, Type{.kind = TypeKind::kNever}));
    REQUIRE_FALSE(is_consistent(Type{.kind = TypeKind::kNever}, Type{.kind = TypeKind::kString}));
  }

  SECTION("union consistency handles subsets and empty unions") {
    const Type expected_union{.kind = TypeKind::kUnion,
                              .union_members = {Type{.kind = TypeKind::kInt64}, Type{.kind = TypeKind::kString}}};
    const Type actual_union{.kind = TypeKind::kUnion,
                            .union_members = {Type{.kind = TypeKind::kString}, Type{.kind = TypeKind::kInt64}}};
    const Type smaller_union{.kind = TypeKind::kUnion, .union_members = {Type{.kind = TypeKind::kInt64}}};
    const Type empty_union{.kind = TypeKind::kUnion};

    REQUIRE(is_consistent(expected_union, actual_union));
    REQUIRE(is_consistent(expected_union, smaller_union));
    REQUIRE(is_consistent(expected_union, Type{.kind = TypeKind::kInt64}));
    REQUIRE(is_consistent(Type{.kind = TypeKind::kInt64}, smaller_union));
    REQUIRE_FALSE(is_consistent(expected_union, empty_union));
    REQUIRE_FALSE(is_consistent(empty_union, Type{.kind = TypeKind::kInt64}));
  }

  SECTION("tuple consistency enforces variadic placement and repeated item matching") {
    Type repeated_string{.kind = TypeKind::kString, .variadic = true};
    const Type variadic_tuple{.kind = TypeKind::kTuple,
                              .items = {Type{.kind = TypeKind::kInt64}, repeated_string}};
    const Type matching_actual{.kind = TypeKind::kTuple,
                               .items = {Type{.kind = TypeKind::kInt64},
                                         Type{.kind = TypeKind::kString},
                                         Type{.kind = TypeKind::kString}}};
    const Type too_short_actual{.kind = TypeKind::kTuple};

    Type invalid_variadic_head{.kind = TypeKind::kString, .variadic = true};
    const Type invalid_expected{.kind = TypeKind::kTuple,
                                .items = {invalid_variadic_head, Type{.kind = TypeKind::kInt64}}};

    REQUIRE(is_consistent(variadic_tuple, matching_actual));
    REQUIRE_FALSE(is_consistent(variadic_tuple, too_short_actual));
    REQUIRE_FALSE(is_consistent(invalid_expected, matching_actual));
    REQUIRE_FALSE(is_consistent(variadic_tuple, Type{.kind = TypeKind::kInt64}));
  }

  SECTION("applied, type-variable, and nominal consistency require matching identities") {
    const Type dict_int_string{.kind = TypeKind::kApplied,
                               .applied_name = "Dict",
                               .applied_args = {Type{.kind = TypeKind::kInt64}, Type{.kind = TypeKind::kString}}};
    const Type dict_int_float{.kind = TypeKind::kApplied,
                              .applied_name = "Dict",
                              .applied_args = {Type{.kind = TypeKind::kInt64}, Type{.kind = TypeKind::kFloat64}}};
    const Type maybe_int{.kind = TypeKind::kApplied,
                         .applied_name = "Maybe",
                         .applied_args = {Type{.kind = TypeKind::kInt64}}};

    REQUIRE(is_consistent(dict_int_string, dict_int_string));
    REQUIRE_FALSE(is_consistent(dict_int_string, dict_int_float));
    REQUIRE_FALSE(is_consistent(dict_int_string, maybe_int));
    REQUIRE_FALSE(is_consistent(dict_int_string, Type{.kind = TypeKind::kInt64}));

    REQUIRE(is_consistent(Type{.kind = TypeKind::kTypeVar, .nominal_name = "T"},
                          Type{.kind = TypeKind::kTypeVar, .nominal_name = "T"}));
    REQUIRE_FALSE(is_consistent(Type{.kind = TypeKind::kTypeVar, .nominal_name = "T"},
                                Type{.kind = TypeKind::kTypeVar, .nominal_name = "U"}));

    REQUIRE(is_consistent(Type{.kind = TypeKind::kNominal, .nominal_name = "Widget"},
                          Type{.kind = TypeKind::kNominal, .nominal_name = "Widget"}));
    REQUIRE_FALSE(is_consistent(Type{.kind = TypeKind::kNominal, .nominal_name = "Widget"},
                                Type{.kind = TypeKind::kNominal, .nominal_name = "Gadget"}));
  }

  SECTION("function consistency handles missing returns and exact symmetric signatures") {
    const Type missing_return_expected =
        mk_function({Type{.kind = TypeKind::kInt64}}, std::nullopt);
    const Type mismatched_actual =
        mk_function({Type{.kind = TypeKind::kString}, Type{.kind = TypeKind::kString}}, Type{.kind = TypeKind::kBool});

    Type variadic_string{.kind = TypeKind::kString, .variadic = true};
    const Type variadic_fn = mk_function({variadic_string}, Type{.kind = TypeKind::kInt64});
    const Type non_variadic_fn = mk_function({Type{.kind = TypeKind::kString}}, Type{.kind = TypeKind::kInt64});
    const Type short_fn = mk_function({}, Type{.kind = TypeKind::kInt64});

    const Type named_widget_fn = mk_function({Type{.kind = TypeKind::kNominal, .nominal_name = "Widget"}},
                                             Type{.kind = TypeKind::kInt64});
    const Type named_gadget_fn = mk_function({Type{.kind = TypeKind::kNominal, .nominal_name = "Gadget"}},
                                             Type{.kind = TypeKind::kInt64});

    REQUIRE(is_consistent(missing_return_expected, mismatched_actual));
    REQUIRE_FALSE(is_consistent(variadic_fn, non_variadic_fn));
    REQUIRE_FALSE(is_consistent(named_widget_fn, short_fn));
    REQUIRE(is_consistent(named_widget_fn, named_widget_fn));
    REQUIRE_FALSE(is_consistent(named_widget_fn, named_gadget_fn));
    REQUIRE_FALSE(is_consistent(named_widget_fn, Type{.kind = TypeKind::kInt64}));
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

TEST_CASE("Type checker rejects user generic call with conflicting argument bindings", "[typecheck][generics][stage_g3a]") {
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

TEST_CASE("Type checker rejects user generic let body that violates declared type variable", "[typecheck][generics][stage_g3b]") {
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
  const std::string src =
      "let MakeInlineGeneric(): Any = <T>(x: T): T = x;\n";

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
        "((), (): String = \"fleaux\") -> Std.Apply<Tuple(), String>;\n";

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

TEST_CASE("Type checker rejects user generic variadic tail with conflicting bindings", "[typecheck][generics][stage_g3c]") {
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
  const auto parsed = parser.parse_program(src, "typecheck_stage_g3f_callable_return_transitive_unbound_generic.fleaux");
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
  const auto parsed = parser.parse_program(src, "typecheck_stage_g3f_callable_return_alias_into_enclosing_generic_ok.fleaux");
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
      "let Std.Tuple.Map<T, U>(t: Tuple(T...), func: (T) => U): Tuple(U...) :: __builtin__;\n"
      "let ToInt(x: Float64): Int64 = 1;\n"
      "let NeedsInts(xs: Tuple(Int64...)): Tuple(Int64...) = xs;\n"
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
      "let Std.Tuple.Map<T, U>(t: Tuple(T...), func: (T) => U): Tuple(U...) :: __builtin__;\n"
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
  SECTION("Std.Parallel.Map infers Result(Tuple(U...), Tuple(Int64, String)) output") {
    const std::string src =
        "let Std.Parallel.Map<T, U>(items: Tuple(T...), func: (T) => U): Result(Tuple(U...), Tuple(Int64, String)) "
        ":: __builtin__;\n"
        "let ToInt(x: Float64): Int64 = 1;\n"
        "let NeedsMapped(r: Result(Tuple(Int64...), Tuple(Int64, String))): Result(Tuple(Int64...), Tuple(Int64, "
        "String)) = r;\n"
        "((1.0, 2.0), ToInt) -> Std.Parallel.Map -> NeedsMapped;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3e_parallel_map_result_inference.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Parallel.Map rejects callable input mismatch") {
    const std::string src =
        "let Std.Parallel.Map<T, U>(items: Tuple(T...), func: (T) => U): Result(Tuple(U...), Tuple(Int64, String)) "
        ":: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "((1.0, 2.0), NeedsInt) -> Std.Parallel.Map;\n";

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

  SECTION("Std.Parallel.WithOptions infers Result(Tuple(U...), Tuple(Int64, String)) output") {
    const std::string src =
        "let Std.Dict.Create(): Dict(Any, Any) :: __builtin__;\n"
        "let Std.Dict.Set<K, V>(dict: Dict(K, V), key: K, value: V): Dict(K, V) :: __builtin__;\n"
        "let Std.Parallel.WithOptions<T, U>(items: Tuple(T...), func: (T) => U, options: Dict(String, Any)): "
        "Result(Tuple(U...), Tuple(Int64, String)) :: __builtin__;\n"
        "let ToInt(x: Float64): Int64 = 1;\n"
        "let NeedsMapped(r: Result(Tuple(Int64...), Tuple(Int64, String))): Result(Tuple(Int64...), Tuple(Int64, "
        "String)) = r;\n"
        "let BuildOptions(): Dict(String, Any) = (() -> Std.Dict.Create, \"max_workers\", 2) -> Std.Dict.Set;\n"
        "((1.0, 2.0), ToInt, () -> BuildOptions) -> Std.Parallel.WithOptions -> NeedsMapped;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3e_parallel_with_options_result_inference.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Parallel.WithOptions rejects callable input mismatch") {
    const std::string src =
        "let Std.Dict.Create(): Dict(Any, Any) :: __builtin__;\n"
        "let Std.Dict.Set<K, V>(dict: Dict(K, V), key: K, value: V): Dict(K, V) :: __builtin__;\n"
        "let Std.Parallel.WithOptions<T, U>(items: Tuple(T...), func: (T) => U, options: Dict(String, Any)): "
        "Result(Tuple(U...), Tuple(Int64, String)) :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "let BuildOptions(): Dict(String, Any) = (() -> Std.Dict.Create, \"max_workers\", 2) -> Std.Dict.Set;\n"
        "((1.0, 2.0), NeedsInt, () -> BuildOptions) -> Std.Parallel.WithOptions;\n";

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
        "let Std.Task.Spawn<T, U>(func: (T) => U, value: T): TaskHandle :: __builtin__;\n"
        "let ToInt(x: Float64): Int64 = 1;\n"
        "let NeedsHandle(task: TaskHandle): TaskHandle = task;\n"
        "(ToInt, 1.0) -> Std.Task.Spawn -> NeedsHandle;\n";

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
        "(\"/tmp/a.txt\", \"r\", (h: FileHandle): Int64 = 1) -> Std.File.WithOpen -> NeedsInt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3e_file_withopen_return_inference.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Tuple.Any and Std.Tuple.FindIndex enforce generic predicate parameter type") {
    const std::string src =
        "let Std.Tuple.Any<T>(t: Tuple(T...), pred: (T) => Bool): Bool :: __builtin__;\n"
        "let Std.Tuple.FindIndex<T>(t: Tuple(T...), pred: (T) => Bool): Int64 :: __builtin__;\n"
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
        "let Std.Dict.Create(): Dict(Any, Any) :: __builtin__;\n"
        "let Std.Dict.Get<K, V>(dict: Dict(K, V), key: K): V :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "let Lookup(d: Dict(String, Int64)): Int64 = (d, \"a\") -> Std.Dict.Get;\n"
        "(() -> Std.Dict.Create) -> Lookup -> NeedsInt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3f_dict_get_value_type.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Dict.Get rejects key-type mismatch") {
    const std::string src =
        "let Std.Dict.Create(): Dict(Any, Any) :: __builtin__;\n"
        "let Std.Dict.Get<K, V>(dict: Dict(K, V), key: K): V :: __builtin__;\n"
        "let Lookup(d: Dict(String, Int64)): Int64 = (d, 1) -> Std.Dict.Get;\n"
        "(() -> Std.Dict.Create) -> Lookup;\n";

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
        "let Std.Dict.Create(): Dict(Any, Any) :: __builtin__;\n"
        "let Std.Dict.GetDefault<K, V>(dict: Dict(K, V), key: K, default: V): V :: __builtin__;\n"
        "let Lookup(d: Dict(String, Int64)): Int64 = (d, \"a\", \"oops\") -> Std.Dict.GetDefault;\n"
        "(() -> Std.Dict.Create) -> Lookup;\n";

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
        "let Std.Array.GetAtND(value: Any, indices: Tuple(Int64...)): Any :: __builtin__;\n"
        "(((1, 2), (3, 4)), (1, 0.5)) -> Std.Array.GetAtND;\n";

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

  SECTION("Std.Array.Shape produces Tuple(Int64...) compatible output") {
    const std::string src =
        "let Std.Array.Shape(value: Any): Tuple(Int64...) :: __builtin__;\n"
        "let NeedsShape(s: Tuple(Int64...)): Tuple(Int64...) = s;\n"
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
  SECTION("Std.String.Split output is typed as Tuple(String...)") {
    const std::string src =
        "let Std.String.Split(s: String, sep: String): Tuple(String...) :: __builtin__;\n"
        "let NeedsStrings(parts: Tuple(String...)): Tuple(String...) = parts;\n"
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
        "let Std.GetArgs(): Tuple(String...) :: __builtin__;\n"
        "let NeedsInts(args: Tuple(Int64...)): Tuple(Int64...) = args;\n"
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

  SECTION("Std.Tuple.Range output is typed as Tuple(Int64...)") {
    const std::string src =
        "let Std.Tuple.Range(stop: Int64): Tuple(Int64...) :: __builtin__;\n"
        "let NeedsInts(values: Tuple(Int64...)): Tuple(Int64...) = values;\n"
        "(5) -> Std.Tuple.Range -> NeedsInts;\n";

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
        "let Std.Array.GetAt<T>(array: Tuple(T...), index: Int64): T :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "((1, 2, 3), 1) -> Std.Array.GetAt -> NeedsInt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3h_array_getat_type_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Array.GetAt accepts integer indices through builtin contract") {
    const std::string src =
        "let Std.Array.GetAt<T>(array: Tuple(T...), index: Any): T :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "((1, 2, 3), 1) -> Std.Array.GetAt -> NeedsInt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3h_array_getat_integer_contract_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Array.GetAt rejects fractional index with builtin contract hint") {
    const std::string src =
        "let Std.Array.GetAt<T>(array: Tuple(T...), index: Any): T :: __builtin__;\n"
        "((1, 2, 3), 1.5) -> Std.Array.GetAt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3h_array_getat_fractional_contract_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects Int64 or UInt64 for integer arguments") != std::string::npos);
  }

  SECTION("Std.Array.SetAt rejects replacement value type mismatch") {
    const std::string src =
        "let Std.Array.SetAt<T>(array: Tuple(T...), index: Int64, value: T): Tuple(T...) :: __builtin__;\n"
        "((1, 2, 3), 1, \"oops\") -> Std.Array.SetAt;\n";

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

  SECTION("Std.Array.Concat rejects mismatched tuple element types") {
    const std::string src =
        "let Std.Array.Concat<T>(lhs: Tuple(T...), rhs: Tuple(T...)): Tuple(T...) :: __builtin__;\n"
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

  SECTION("Std.Array.Slice preserves tuple element type") {
    const std::string src =
        "let Std.Array.Slice<T>(array: Tuple(T...), start: Int64, stop: Int64): Tuple(T...) :: __builtin__;\n"
        "let NeedsInts(values: Tuple(Int64...)): Tuple(Int64...) = values;\n"
        "((1, 2, 3, 4), 1, 3) -> Std.Array.Slice -> NeedsInts;\n";

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
        "let Std.Array.Fill<T>(tuple: Tuple(T...), start_index: Int64, length: Int64, value: T): Tuple(T...) :: "
        "__builtin__;\n"
        "((1, 2, 3, 4), 1, 2, 9) -> Std.Array.Fill;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3i_array_fill_accepts_matching_type.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Array.Fill rejects mismatched replacement element type") {
    const std::string src =
        "let Std.Array.Fill<T>(tuple: Tuple(T...), start_index: Int64, length: Int64, value: T): Tuple(T...) :: "
        "__builtin__;\n"
        "((1, 2, 3, 4), 1, 2, \"oops\") -> Std.Array.Fill;\n";

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
        "let Std.Array.Fill<T>(tuple: Tuple(T...), start_index: Int64, length: Int64, value: T): Tuple(T...) :: "
        "__builtin__;\n"
        "let NeedsInts(values: Tuple(Int64...)): Tuple(Int64...) = values;\n"
        "((1, 2, 3, 4), 1, 2, 9) -> Std.Array.Fill -> NeedsInts;\n";

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
        "let Std.ElementAt<T>(tuple: Tuple(T...), count: Int64): T :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "((1, 2, 3), 1) -> Std.ElementAt -> NeedsInt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3j_elementat_type_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.ElementAt rejects downstream element-type mismatch") {
    const std::string src =
        "let Std.ElementAt<T>(tuple: Tuple(T...), count: Int64): T :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "((1.0, 2.0, 3.0), 1) -> Std.ElementAt -> NeedsInt;\n";

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
        "let Std.Take<T>(tuple: Tuple(T...), count: Int64): Tuple(T...) :: __builtin__;\n"
        "let NeedsInts(values: Tuple(Int64...)): Tuple(Int64...) = values;\n"
        "((1, 2, 3, 4), 2) -> Std.Take -> NeedsInts;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3j_take_type_preservation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Drop preserves tuple element type") {
    const std::string src =
        "let Std.Drop<T>(tuple: Tuple(T...), count: Int64): Tuple(T...) :: __builtin__;\n"
        "let NeedsInts(values: Tuple(Int64...)): Tuple(Int64...) = values;\n"
        "((1, 2, 3, 4), 2) -> Std.Drop -> NeedsInts;\n";

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
        "let Std.Slice<T>(tuple: Tuple(T...), stop: Int64): Tuple(T...) :: __builtin__;\n"
        "let NeedsInts(values: Tuple(Int64...)): Tuple(Int64...) = values;\n"
        "((1, 2, 3, 4), 2) -> Std.Slice -> NeedsInts;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3k_slice_type_preservation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Slice rejects downstream tuple element mismatch") {
    const std::string src =
        "let Std.Slice<T>(tuple: Tuple(T...), stop: Int64): Tuple(T...) :: __builtin__;\n"
        "let NeedsInts(values: Tuple(Int64...)): Tuple(Int64...) = values;\n"
        "((1.0, 2.0, 3.0, 4.0), 2) -> Std.Slice -> NeedsInts;\n";

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
        "let Std.Length<T>(tuple: Tuple(T...)): Int64 :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "((1.0, 2.0, 3.0)) -> Std.Length -> NeedsInt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3k_length_typed_input_returns_int64.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }
}

TEST_CASE("Type checker matrix: Stage-3l Std.Wrap/Unwrap generic tightening", "[typecheck][stage3l][generics]") {
  SECTION("Std.Wrap propagates wrapped element type") {
    const std::string src =
        "let Std.Wrap<T>(val: T): Tuple(T) :: __builtin__;\n"
        "let NeedsIntTuple(x: Tuple(Int64)): Tuple(Int64) = x;\n"
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
        "let Std.Wrap<T>(val: T): Tuple(T) :: __builtin__;\n"
        "let NeedsIntTuple(x: Tuple(Int64)): Tuple(Int64) = x;\n"
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
        "let Std.Wrap<T>(val: T): Tuple(T) :: __builtin__;\n"
        "let Std.Unwrap<T>(tuple: Tuple(T)): T :: __builtin__;\n"
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
        "let Std.Unwrap<T>(tuple: Tuple(T)): T :: __builtin__;\n"
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
        "let Std.Tuple.Reverse<T>(t: Tuple(T...)): Tuple(T...) :: __builtin__;\n"
        "let NeedsInts(values: Tuple(Int64...)): Tuple(Int64...) = values;\n"
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
        "let Std.Tuple.Unique<T>(t: Tuple(T...)): Tuple(T...) :: __builtin__;\n"
        "let NeedsInts(values: Tuple(Int64...)): Tuple(Int64...) = values;\n"
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
        "let Std.Tuple.Min<T>(t: Tuple(T...)): T :: __builtin__;\n"
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
        "let Std.Tuple.Max<T>(t: Tuple(T...)): T :: __builtin__;\n"
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
        "let Std.Tuple.Sort<T>(t: Tuple(T...)): Tuple(T...) :: __builtin__;\n"
        "let NeedsInts(values: Tuple(Int64...)): Tuple(Int64...) = values;\n"
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
        "let Std.Tuple.Sort<T>(t: Tuple(T...)): Tuple(T...) :: __builtin__;\n"
        "let NeedsInts(values: Tuple(Int64...)): Tuple(Int64...) = values;\n"
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
        "let Std.Tuple.Append<T>(t: Tuple(T...), item: T): Tuple(T...) :: __builtin__;\n"
        "let NeedsInts(values: Tuple(Int64...)): Tuple(Int64...) = values;\n"
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
        "let Std.Tuple.Append<T>(t: Tuple(T...), item: T): Tuple(T...) :: __builtin__;\n"
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
        "let Std.Tuple.Prepend<T>(t: Tuple(T...), item: T): Tuple(T...) :: __builtin__;\n"
        "let NeedsInts(values: Tuple(Int64...)): Tuple(Int64...) = values;\n"
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
        "let Std.Tuple.Contains<T>(t: Tuple(T...), item: T): Bool :: __builtin__;\n"
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
        "let Std.Tuple.Zip<A, B>(a: Tuple(A...), b: Tuple(B...)): Tuple(Any...) :: __builtin__;\n"
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
        "let Std.Tuple.Zip<A, B>(a: Tuple(A...), b: Tuple(B...)): Tuple(Tuple(A, B)...) :: __builtin__;\n"
        "let NeedsPairs(values: Tuple(Tuple(Int64, String)...)): Tuple(Tuple(Int64, String)...) = values;\n"
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
        "let Std.Tuple.Zip<A, B>(a: Tuple(A...), b: Tuple(B...)): Tuple(Tuple(A, B)...) :: __builtin__;\n"
        "let NeedsPairs(values: Tuple(Tuple(String, Int64)...)): Tuple(Tuple(String, Int64)...) = values;\n"
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
        "let Std.Tuple.Zip<A, B>(a: Tuple(A...), b: Tuple(B...)): Tuple(Tuple(A, B)...) :: __builtin__;\n"
        "let NeedsInts(values: Tuple(Int64...)): Tuple(Int64...) = values;\n"
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
        "let Std.Result.Unwrap<T, E>(result: Result(T, E)): T :: __builtin__;\n"
        "let MakeOk(): Result(Int64, String) :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "() -> MakeOk -> Std.Result.Unwrap -> NeedsInt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3q_result_unwrap_type_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Result.UnwrapErr rejects downstream error-type mismatch") {
    const std::string src =
        "let Std.Result.UnwrapErr<T, E>(result: Result(T, E)): E :: __builtin__;\n"
        "let MakeErr(): Result(Int64, String) :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "() -> MakeErr -> Std.Result.UnwrapErr -> NeedsInt;\n";

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

TEST_CASE("Type checker matrix: Stage-4c Std.Result constructor policy", "[typecheck][stage4c][generics]") {
  SECTION("Std.Result.Ok propagates known success payload type") {
    const std::string src =
        "let Std.Result.Ok<T>(value: T): Result(T, Any) :: __builtin__;\n"
        "let NeedsOk(r: Result(Int64, Any)): Result(Int64, Any) = r;\n"
        "(1) -> Std.Result.Ok -> NeedsOk;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_result_ok_payload_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Result.Ok rejects downstream success-payload mismatch") {
    const std::string src =
        "let Std.Result.Ok<T>(value: T): Result(T, Any) :: __builtin__;\n"
        "let NeedsWrongOk(r: Result(String, Any)): Result(String, Any) = r;\n"
        "(1) -> Std.Result.Ok -> NeedsWrongOk;\n";

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
        "let Std.Result.Err<E>(error: E): Result(Any, E) :: __builtin__;\n"
        "let NeedsErr(r: Result(Any, String)): Result(Any, String) = r;\n"
        "(\"oops\") -> Std.Result.Err -> NeedsErr;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_result_err_payload_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Result.Err rejects downstream error-payload mismatch") {
    const std::string src =
        "let Std.Result.Err<E>(error: E): Result(Any, E) :: __builtin__;\n"
        "let NeedsWrongErr(r: Result(Any, Int64)): Result(Any, Int64) = r;\n"
        "(\"oops\") -> Std.Result.Err -> NeedsWrongErr;\n";

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
  SECTION("Std.Dict.Create propagates nullary Dict(Any, Any) output") {
    const std::string src =
        "let Std.Dict.Create(): Dict(Any, Any) :: __builtin__;\n"
        "let Std.Dict.Create<K, V>(dict: Dict(K, V)): Dict(K, V) :: __builtin__;\n"
        "let NeedsDict(d: Dict(Any, Any)): Dict(Any, Any) = d;\n"
        "() -> Std.Dict.Create -> NeedsDict;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_dict_create_nullary_surface.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Dict.Create preserves channels when cloning an existing dictionary") {
    const std::string src =
        "let Std.Dict.Create(): Dict(Any, Any) :: __builtin__;\n"
        "let Std.Dict.Create<K, V>(dict: Dict(K, V)): Dict(K, V) :: __builtin__;\n"
        "let Std.Dict.Set<K, V>(dict: Dict(K, V), key: K, value: V): Dict(K, V) :: __builtin__;\n"
        "let NeedsStringInt(d: Dict(String, Int64)): Dict(String, Int64) = d;\n"
        "let MakeDict(): Dict(String, Int64) = (() -> Std.Dict.Create, \"a\", 1) -> Std.Dict.Set;\n"
        "() -> MakeDict -> Std.Dict.Create -> NeedsStringInt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_dict_create_clone_surface_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Annotated analysis records the resolved builtin overload symbol key") {
    const std::string src =
        "let Std.Dict.Create(): Dict(Any, Any) :: __builtin__;\n"
        "let Std.Dict.Create<K, V>(dict: Dict(K, V)): Dict(K, V) :: __builtin__;\n"
        "let Std.Dict.Set<K, V>(dict: Dict(K, V), key: K, value: V): Dict(K, V) :: __builtin__;\n"
        "let MakeDict(): Dict(String, Int64) = (() -> Std.Dict.Create, \"a\", 1) -> Std.Dict.Set;\n"
        "() -> Std.Dict.Create;\n"
        "() -> MakeDict -> Std.Dict.Create;\n";

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
        "let Std.Dict.Create(): Dict(Any, Any) :: __builtin__;\n"
        "let Std.Dict.Create<K, V>(dict: Dict(K, V)): Dict(K, V) :: __builtin__;\n"
        "let Keep(value: Any): Any = value;\n"
        "Std.Dict.Create -> Keep;\n";

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

TEST_CASE("Type checker rejects builtin overloads that rely on type-only dispatch", "[typecheck][stage4c][builtins][overload]") {
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

TEST_CASE("Type checker matrix: Stage-4d Std.Match semantics", "[typecheck][stage4d][match]") {
  SECTION("Std.Match accepts literal, predicate, wildcard, and mixed handler forms") {
    const std::string src =
        "let Std.Match(value: Any, cases: Any...): Any :: __builtin__;\n"
        "let Std.Mod(lhs: Float64 | Int64 | UInt64, rhs: Float64 | Int64 | UInt64): Float64 | Int64 | UInt64 :: __builtin__;\n"
        "let Std.Equal<T>(lhs: T, rhs: T): Bool :: __builtin__;\n"
        "let Std.ToString(value: Any): String :: __builtin__;\n"
        "let Std.String.Join(sep: String, items: Tuple(String...)): String :: __builtin__;\n"
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
        "let PassResult<T>(value: Result(Null | T, String)): Result(Null | T, String) :: __builtin__;\n"
        "let GetNullResult(): Result(Null, String) :: __builtin__;\n"
        "let NeedsNullResult(value: Result(Null, String)): Result(Null, String) = value;\n"
        "() -> GetNullResult -> PassResult -> NeedsNullResult;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4e_applied_nullable_union_fixed_first_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Applied generic passthrough preserves a multi-member nullable payload channel") {
    const std::string src =
        "let PassResult<T>(value: Result(T | Null, String)): Result(T | Null, String) :: __builtin__;\n"
        "let GetMaybeMixedResult(): Result(String | Int64 | Null, String) :: __builtin__;\n"
        "let NeedsMaybeMixedResult(value: Result(String | Int64 | Null, String)): Result(String | Int64 | Null, String) = value;\n"
        "() -> GetMaybeMixedResult -> PassResult -> NeedsMaybeMixedResult;\n";

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
        "let GetUnknown(): Any :: __builtin__;\n"
        "let Echo(x: Int64): Int64 = x;\n"
        "let Echo(x: String): String = x;\n"
        "() -> GetUnknown -> Echo;\n";

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
        "let Make<T>(x: Tuple(T)): T :: __builtin__;\n"
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
    REQUIRE(lowered.error().hint->find("No overload of Make matches the provided argument types") !=
            std::string::npos);
    REQUIRE(lowered.error().hint->find("Make(T): T") != std::string::npos);
    REQUIRE(lowered.error().hint->find("Make(Tuple(T)): T") != std::string::npos);
    REQUIRE(lowered.error().hint->find("Make(Int64): Int64") == std::string::npos);
  }

  SECTION("Builtin contract diagnostics preserve specific index-tuple hints") {
    const std::string src =
        "let Std.Array.GetAtND(value: Any, indices: Any): Any :: __builtin__;\n"
        "(((1, 2), (3, 4)), (1, 0.5)) -> Std.Array.GetAtND;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4h_builtin_contract_tuple_hint.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects all index tuple elements to be Int64 or UInt64") !=
            std::string::npos);
  }

  SECTION("Builtin contract diagnostics preserve tuple-index shape hints") {
    const std::string src =
        "let Std.Array.GetAtND(value: Any, indices: Any): Any :: __builtin__;\n"
        "(((1, 2), (3, 4)), 1) -> Std.Array.GetAtND;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4h_builtin_contract_tuple_shape_hint.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects tuple indices") != std::string::npos);
  }

  SECTION("Builtin numeric contracts reject implicit Float64 promotion") {
    const std::string src =
        "let Std.Add(lhs: Any, rhs: Any): Any :: __builtin__;\n"
        "(1.0, 2) -> Std.Add;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4h_builtin_contract_float_promotion_rejects.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("does not implicitly cast Int64 or UInt64 to Float64") != std::string::npos);
    REQUIRE(lowered.error().hint->find("Std.ToFloat64") != std::string::npos);
  }

  SECTION("Builtin numeric return refinement preserves Float64 flow") {
    const std::string src =
        "let Std.Add(lhs: Any, rhs: Any): Any :: __builtin__;\n"
        "let NeedsFloat(x: Float64): Float64 = x;\n"
        "(1.0, 2.0) -> Std.Add -> NeedsFloat;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4h_builtin_contract_float64_refine_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
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
  SECTION("Std.Array.ReshapeND accepts structural Tuple(Int64...) shape") {
    const std::string src =
        "let Std.Array.ReshapeND(flat_array: Tuple(Any...), shape: Tuple(Int64...)): Any :: __builtin__;\n"
        "let NeedsAny(x: Any): Any = x;\n"
        "((1, 2, 3, 4), (2, 2)) -> Std.Array.ReshapeND -> NeedsAny;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_array_reshapend_structural_shape_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Array.ReshapeND rejects fractional structural shape elements") {
    const std::string src =
        "let Std.Array.ReshapeND(flat_array: Tuple(Any...), shape: Tuple(Int64...)): Any :: __builtin__;\n"
        "((1, 2, 3, 4), (2, 2.5)) -> Std.Array.ReshapeND;\n";

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

  SECTION("Std.Array.ReshapeND preserves tuple-shape contract hint for non-tuple shapes") {
    const std::string src =
        "let Std.Array.ReshapeND(flat_array: Tuple(Any...), shape: Any): Any :: __builtin__;\n"
        "((1, 2, 3, 4), 2) -> Std.Array.ReshapeND;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage4c_array_reshapend_tuple_shape_hint.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects tuple indices") != std::string::npos);
  }
}

TEST_CASE("Type checker matrix: Stage-3r Std.Task result-shape tightening", "[typecheck][stage3r][generics]") {
  SECTION("Std.Task.Await propagates Result(Any, String)") {
    const std::string src =
        "let Std.Task.Await(task: TaskHandle): Result(Any, String) :: __builtin__;\n"
        "let MakeTask(): TaskHandle :: __builtin__;\n"
        "let NeedsAwaitResult(r: Result(Any, String)): Result(Any, String) = r;\n"
        "() -> MakeTask -> Std.Task.Await -> NeedsAwaitResult;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3r_task_await_result_shape.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Task.WithTimeout propagates Result(Any, String)") {
    const std::string src =
        "let Std.Task.WithTimeout(task: TaskHandle, timeout_ms: Int64): Result(Any, String) :: __builtin__;\n"
        "let MakeTask(): TaskHandle :: __builtin__;\n"
        "let NeedsTimeoutResult(r: Result(Any, String)): Result(Any, String) = r;\n"
        "(() -> MakeTask, 50) -> Std.Task.WithTimeout -> NeedsTimeoutResult;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3r_task_timeout_result_shape.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Task.AwaitAll propagates Result(Tuple(Any...), Tuple(Int64, String))") {
    const std::string src =
        "let Std.Task.AwaitAll(tasks: Tuple(TaskHandle...)): Result(Tuple(Any...), Tuple(Int64, String)) :: "
        "__builtin__;\n"
        "let MakeTasks(): Tuple(TaskHandle...) :: __builtin__;\n"
        "let NeedsTasks(r: Result(Tuple(Any...), Tuple(Int64, String))): Result(Tuple(Any...), Tuple(Int64, "
        "String)) = r;\n"
        "() -> MakeTasks -> Std.Task.AwaitAll -> NeedsTasks;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3r_task_awaitall_result_shape.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Task.AwaitAll rejects downstream error-shape mismatch") {
    const std::string src =
        "let Std.Task.AwaitAll(tasks: Tuple(TaskHandle...)): Result(Tuple(Any...), Tuple(Int64, String)) :: "
        "__builtin__;\n"
        "let MakeTasks(): Tuple(TaskHandle...) :: __builtin__;\n"
        "let NeedsWrongShape(r: Result(Tuple(Any...), Tuple(String, Int64))): Result(Tuple(Any...), Tuple(String, "
        "Int64)) = r;\n"
        "() -> MakeTasks -> Std.Task.AwaitAll -> NeedsWrongShape;\n";

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
        "let Std.Array.SetAt2D<T>(grid: Tuple(Tuple(T...)...), row: Int64, col: Int64, value: T): "
        "Tuple(Tuple(T...)...) :: __builtin__;\n"
        "let NeedsGrid(g: Tuple(Tuple(Int64...)...)): Tuple(Tuple(Int64...)...) = g;\n"
        "(((1, 2), (3, 4)), 0, 1, 9) -> Std.Array.SetAt2D -> NeedsGrid;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3s_array_setat2d_grid_type_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Array.SetAt2D rejects replacement value type mismatch") {
    const std::string src =
        "let Std.Array.SetAt2D<T>(grid: Tuple(Tuple(T...)...), row: Int64, col: Int64, value: T): "
        "Tuple(Tuple(T...)...) :: __builtin__;\n"
        "(((1, 2), (3, 4)), 0, 1, \"oops\") -> Std.Array.SetAt2D;\n";

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

  SECTION("Std.Array.Transpose2D rejects downstream nested element mismatch") {
    const std::string src =
        "let Std.Array.Transpose2D<T>(grid: Tuple(Tuple(T...)...)): Tuple(Tuple(T...)...) :: __builtin__;\n"
        "let NeedsIntGrid(g: Tuple(Tuple(Int64...)...)): Tuple(Tuple(Int64...)...) = g;\n"
        "((\"a\", \"b\"), (\"c\", \"d\")) -> Std.Array.Transpose2D -> NeedsIntGrid;\n";

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
        "let Std.Try<T, U>(value: T, func: (T) => U): Result(U, String) :: __builtin__;\n"
        "let EchoMaybe(value: String | Null): String | Null = value;\n"
        "let NeedsTryResult(r: Result(String | Null, String)): Result(String | Null, String) = r;\n"
        "(\"HOME\") -> Std.OS.Env -> (_, EchoMaybe) -> Std.Try -> NeedsTryResult;\n";

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
        "let Std.Try<T, U>(value: T, func: (T) => U): Result(U, String) :: __builtin__;\n"
        "let NeedsString(value: String): String = value;\n"
        "(\"HOME\") -> Std.OS.Env -> (_, NeedsString) -> Std.Try;\n";

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
        "let Std.Try<T, U>(value: T, func: (T) => U): Result(U, String) :: __builtin__;\n"
        "let Id(x: Int64): Int64 = x;\n"
        "let NeedsTryResult(r: Result(Int64, String)): Result(Int64, String) = r;\n"
        "(1, Id) -> Std.Try -> NeedsTryResult;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3t_try_string_error_channel.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Parallel.Map propagates tuple index+message error shape") {
    const std::string src =
        "let Std.Parallel.Map<T, U>(items: Tuple(T...), func: (T) => U): Result(Tuple(U...), Tuple(Int64, String)) :: "
        "__builtin__;\n"
        "let ToInt(x: Int64): Int64 = x;\n"
        "let NeedsMapped(r: Result(Tuple(Int64...), Tuple(Int64, String))): Result(Tuple(Int64...), Tuple(Int64, "
        "String)) = r;\n"
        "((1, 2), ToInt) -> Std.Parallel.Map -> NeedsMapped;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3t_parallel_map_error_shape_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Parallel.WithOptions propagates tuple index+message error shape") {
    const std::string src =
        "let Std.Dict.Create(): Dict(Any, Any) :: __builtin__;\n"
        "let Std.Dict.Set<K, V>(dict: Dict(K, V), key: K, value: V): Dict(K, V) :: __builtin__;\n"
        "let Std.Parallel.WithOptions<T, U>(items: Tuple(T...), func: (T) => U, options: Dict(String, Any)): "
        "Result(Tuple(U...), Tuple(Int64, String)) :: __builtin__;\n"
        "let ToInt(x: Int64): Int64 = x;\n"
        "let NeedsMapped(r: Result(Tuple(Int64...), Tuple(Int64, String))): Result(Tuple(Int64...), Tuple(Int64, "
        "String)) = r;\n"
        "let BuildOptions(): Dict(String, Any) = (() -> Std.Dict.Create, \"max_workers\", 2) -> Std.Dict.Set;\n"
        "((1, 2), ToInt, () -> BuildOptions) -> Std.Parallel.WithOptions -> NeedsMapped;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3t_parallel_with_options_error_shape_propagation.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Parallel.Reduce rejects downstream error-shape mismatch") {
    const std::string src =
        "let Std.Add(lhs: Float64 | Int64 | UInt64, rhs: Float64 | Int64 | UInt64): Float64 | Int64 | UInt64 :: __builtin__;\n"
        "let Std.Parallel.Reduce<T, A>(items: Tuple(T...), init: A, func: (A, T) => A): Result(A, Tuple(Int64, "
        "String)) :: __builtin__;\n"
        "let ReduceFn(acc: Int64, item: Int64): Int64 = (acc, item) -> Std.Add;\n"
        "let NeedsWrongErr(r: Result(Int64, Tuple(String, Int64))): Result(Int64, Tuple(String, Int64)) = r;\n"
        "((1, 2, 3), 0, ReduceFn) -> Std.Parallel.Reduce -> NeedsWrongErr;\n";

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
        "let Std.Parallel.ForEach<T, U>(items: Tuple(T...), func: (T) => U): Result(Tuple(), Tuple(Int64, String)) :: "
        "__builtin__;\n"
        "let Probe(x: Int64): String = \"ok\";\n"
        "let NeedsResult(r: Result(Tuple(), Tuple(Int64, String))): Result(Tuple(), Tuple(Int64, String)) = r;\n"
        "((1, 2, 3), Probe) -> Std.Parallel.ForEach -> NeedsResult;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3u_parallel_foreach_return_generic.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Parallel.ForEach rejects callback parameter mismatch") {
    const std::string src =
        "let Std.Parallel.ForEach<T, U>(items: Tuple(T...), func: (T) => U): Result(Tuple(), Tuple(Int64, String)) :: "
        "__builtin__;\n"
        "let Probe(x: String): String = x;\n"
        "((1, 2, 3), Probe) -> Std.Parallel.ForEach;\n";

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
  SECTION("Matching imported qualified symbol resolves") {
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
    REQUIRE(analyzed.has_value());
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
    imported_add.params = {{.name = "lhs", .type = {.name = "Int64"}},
                           {.name = "rhs", .type = {.name = "Int64"}}};
    imported_add.return_type = {.name = "Int64"};

    const std::unordered_set<std::string> imported_symbols = {"Std.Add"};
    const std::vector<fleaux::frontend::ir::IRLet> imported_typed_lets = {imported_add};
    const auto analyzed = fleaux::frontend::type_check::analyze_program(*lowered, imported_symbols, imported_typed_lets);
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
    imported_add4.params = {{.name = "x", .type = {.name = "String"}}};
    imported_add4.return_type = {.name = "Int64"};

    const std::unordered_set<std::string> imported_symbols = {"Foo.Add4"};
    const std::vector<fleaux::frontend::ir::IRLet> imported_typed_lets = {imported_add4};
    const auto analyzed = fleaux::frontend::type_check::analyze_program(*lowered, imported_symbols, imported_typed_lets);
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
    imported_add4.params = {{.name = "x", .type = {.name = "Int64"}}};
    imported_add4.return_type = {.name = "Int64"};

    const std::unordered_set<std::string> imported_symbols = {"Foo.Add4"};
    const std::vector<fleaux::frontend::ir::IRLet> imported_typed_lets = {imported_add4};
    const auto analyzed = fleaux::frontend::type_check::analyze_program(*lowered, imported_symbols, imported_typed_lets);
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
        fleaux::frontend::ir::IRTypeDecl{.name = "UserId", .target = {.name = "Int64"}},
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
        fleaux::frontend::ir::IRTypeDecl{.name = "UserId", .target = {.name = "Int64"}},
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
    imported_cast.params = {{.name = "x", .type = {.name = "Any"}}};
    imported_cast.return_type = {.name = "T"};

    const std::unordered_set<std::string> imported_symbols = {"Std.Cast"};
    const std::vector<fleaux::frontend::ir::IRLet> imported_typed_lets = {imported_cast};
    const std::vector<fleaux::frontend::ir::IRTypeDecl> imported_type_decls = {
        fleaux::frontend::ir::IRTypeDecl{.name = "UserId", .target = {.name = "Int64"}},
    };

    const auto analyzed =
        fleaux::frontend::type_check::analyze_program(*lowered, imported_symbols, imported_typed_lets,
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
        "alias Pair = Tuple(Int64, Int64);\n"
        "alias CounterPair = Pair;\n"
        "let Echo(x: CounterPair): Pair = x;\n"
        "let NeedsPair(x: Pair): Pair = x;\n"
        "(1, 2) -> Echo -> NeedsPair;\n";

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
    imported_echo.params = {{.name = "x", .type = {.name = "Distance"}}};
    imported_echo.return_type = {.name = "Distance"};

    const std::unordered_set<std::string> imported_symbols = {"ImportedEcho"};
    const std::vector<fleaux::frontend::ir::IRLet> imported_typed_lets = {imported_echo};
    const std::vector<fleaux::frontend::ir::IRAliasDecl> imported_alias_decls = {
        fleaux::frontend::ir::IRAliasDecl{.name = "Distance", .target = {.name = "Int64"}},
    };

    const auto analyzed = fleaux::frontend::type_check::analyze_program(*lowered, imported_symbols,
                                                                        imported_typed_lets, {}, imported_alias_decls);
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
    imported_cast.params = {{.name = "value", .type = {.name = "Any"}}};
    imported_cast.return_type = {.name = "T"};
    return imported_cast;
  };

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

  SECTION("Value-position explicit type arguments report generic and non-generic expectations across mixed overload sets") {
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
    const auto parsed = parser.parse_program(src, "typecheck_explicit_type_args_value_generic_mixed_arity_summary.fleaux");
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
    const auto analyzed = analyze_program("let Bad(x: Int64): Int64 = x<Int64>;\n",
                                          "typecheck_explicit_type_args_local_value.fleaux");

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

TEST_CASE("Type checker narrows numeric builtin returns to concrete argument kinds", "[typecheck][builtins][numeric]") {
  SECTION("Std.Add narrows to Int64 for concrete Int64 arguments") {
    const std::string src =
        "let Std.Add(lhs: Float64 | Int64 | UInt64, rhs: Float64 | Int64 | UInt64): Float64 | Int64 | UInt64 :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "(1, 2) -> Std.Add -> NeedsInt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_numeric_builtin_refine_int64.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Add narrows to UInt64 for concrete UInt64 arguments") {
    const std::string src =
        "let Std.Add(lhs: Float64 | Int64 | UInt64, rhs: Float64 | Int64 | UInt64): Float64 | Int64 | UInt64 :: __builtin__;\n"
        "let NeedsUInt(x: UInt64): UInt64 = x;\n"
        "(1u64, 2u64) -> Std.Add -> NeedsUInt;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_numeric_builtin_refine_uint64.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Add does not narrow union-typed operands to Int64") {
    const std::string src =
        "let Std.Add(lhs: Float64 | Int64 | UInt64, rhs: Float64 | Int64 | UInt64): Float64 | Int64 | UInt64 :: __builtin__;\n"
        "let AddPair(lhs: Float64 | Int64 | UInt64, rhs: Float64 | Int64 | UInt64): Int64 = (lhs, rhs) -> Std.Add;\n";

    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_numeric_builtin_refine_union_fallback.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("does not implicitly cast Int64 or UInt64 to Float64") != std::string::npos);
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
    const std::string src = "let BadTuple(): Tuple(Float64, Float64) = (1.0, 2.0, 3.0);\n";
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

  SECTION("Single-parameter callable receives whole tuple value") {
    const std::string src =
        "let AcceptPair(pair: Tuple(Float64, Float64)): Float64 = 0.0;\n"
        "(1.0, 2.0) -> AcceptPair;\n";
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
    REQUIRE(lowered.error().hint->find("Closure body type") != std::string::npos);
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

  SECTION("Std.LoopN requires Int64 max_iters") {
    const std::string src =
        "let Std.LoopN<S>(state: S, continue_func: (S) => Bool, step_func: (S) => S, max_iters: Int64): S :: "
        "__builtin__;\n"
        "let ContinueOk(x: Float64): Bool = True;\n"
        "let StepOk(x: Float64): Float64 = x;\n"
        "(1.0, ContinueOk, StepOk, 5u64) -> Std.LoopN;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage2b_loopn_max_iters_int64.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE_FALSE(lowered.has_value());
    REQUIRE(lowered.error().message.find("Type mismatch in call target arguments") != std::string::npos);
    REQUIRE(lowered.error().hint.has_value());
    REQUIRE(lowered.error().hint->find("expects argument") != std::string::npos);
  }

  SECTION("Std.Tuple.Filter requires predicate to return Bool") {
    const std::string src =
        "let Std.Tuple.Filter<T>(t: Tuple(T...), pred: (T) => Bool): Tuple(T...) :: __builtin__;\n"
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
        "let Std.Try<T, U>(value: T, func: (T) => U): Result(U, Any) :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "(1.0, NeedsInt) -> Std.Try;\n";
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
        "let Std.Tuple.Reduce<T, A>(t: Tuple(T...), initial: A, func: (A, T) => A): A :: __builtin__;\n"
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
        "let Std.Tuple.Reduce<T, A>(t: Tuple(T...), initial: A, func: (A, T) => A): A :: __builtin__;\n"
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
        "let Std.Tuple.Reduce<T, A>(t: Tuple(T...), initial: A, func: (A, T) => A): A :: __builtin__;\n"
        "let Good(acc: Float64, item: Float64): Float64 = acc;\n"
        "((1.0, 2.0), 0.0, Good) -> Std.Tuple.Reduce;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage2c_tuple_reduce_ok.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Parallel.Reduce rejects reducer item-type mismatch") {
    const std::string src =
        "let Std.Parallel.Reduce<T, A>(items: Tuple(T...), init: A, func: (A, T) => A): Result(A, Tuple(Int64, "
        "String)) :: __builtin__;\n"
        "let Bad(acc: Float64, item: Int64): Float64 = acc;\n"
        "((\"a\", \"b\"), 0.0, Bad) -> Std.Parallel.Reduce;\n";
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
        "let Std.Parallel.Map(items: Any, func: Any): Result(Any, Any) :: __builtin__;\n"
        "let Func(x: Float64): Float64 = x;\n"
        "((1.0, 2.0), Func) -> Std.Parallel.Map;\n";
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
        "let MakeResult(): Result(Any, Any) :: __builtin__;\n"
        "let NeedsDict(d: Dict(String, Any)): Any = d;\n"
        "() -> MakeResult -> NeedsDict;\n";
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
        "let MakeResultString(): Result(String, String) :: __builtin__;\n"
        "let NeedsResultInt(r: Result(Int64, String)): Any = r;\n"
        "() -> MakeResultString -> NeedsResultInt;\n";
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
        "let MakeResultInt(): Result(Int64, String) :: __builtin__;\n"
        "let NeedsResultInt(r: Result(Int64, String)): Any = r;\n"
        "() -> MakeResultInt -> NeedsResultInt;\n";
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
        "let Std.Try(value: Any, func: Any): Result(Any, Any) :: __builtin__;\n"
        "let NeedsInt(x: Int64): Int64 = x;\n"
        "(1.0, NeedsInt) -> Std.Try;\n";
    const fleaux::frontend::parse::Parser parser;
    const auto parsed = parser.parse_program(src, "typecheck_stage3c_try_bridge_still_active.fleaux");
    REQUIRE(parsed.has_value());

    const fleaux::frontend::lowering::Lowerer lowerer;
    const auto lowered = lowerer.lower(parsed.value());
    REQUIRE(lowered.has_value());
  }

  SECTION("Std.Tuple.All uses typed signature without bridge") {
    const std::string src =
        "let Std.Tuple.All(t: Tuple(Any...), pred: (Any) => Bool): Bool :: __builtin__;\n"
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
