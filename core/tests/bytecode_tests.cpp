#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/bytecode/compiler.hpp"
#include "fleaux/bytecode/module.hpp"
#include "fleaux/bytecode/opcode.hpp"
#include "fleaux/frontend/lowering.hpp"
#include "fleaux/frontend/parser.hpp"
#include "fleaux/vm/builtin_catalog.hpp"

#ifndef FLEAUX_REPO_ROOT
#error "FLEAUX_REPO_ROOT must be defined by CMake for bytecode tests."
#endif

namespace {

std::filesystem::path repo_root_path() {
  return std::filesystem::path(FLEAUX_REPO_ROOT);
}

fleaux::frontend::ir::IRProgram lower_source_to_ir(const std::string& source_text,
                                                   const std::string& source_name) {
  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(source_text, source_name);
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());
  return lowered.value();
}

std::set<std::string> parse_std_declared_names(const bool builtins_only) {
  std::ifstream in(repo_root_path() / "Std.fleaux");
  REQUIRE(in.good());

  std::set<std::string> names;
  std::string line;
  while (std::getline(in, line)) {
    const auto let_pos = line.find("let Std.");
    if (let_pos == std::string::npos) {
      continue;
    }

    const bool is_builtin = line.find(":: __builtin__;") != std::string::npos;
    if (builtins_only != is_builtin) {
      continue;
    }

    const auto open_paren = line.find('(', let_pos);
    REQUIRE(open_paren != std::string::npos);
    names.insert(line.substr(let_pos + 4, open_paren - (let_pos + 4)));
  }

  return names;
}

std::set<std::string> vm_catalog_builtin_names() {
  std::set<std::string> names;
#define FLEAUX_INSERT_VM_BUILTIN(name_literal, node_type) names.insert(name_literal);
  FLEAUX_VM_BUILTINS(FLEAUX_INSERT_VM_BUILTIN)
#undef FLEAUX_INSERT_VM_BUILTIN
  return names;
}

std::set<std::string> vm_catalog_constant_names() {
  std::set<std::string> names;
#define FLEAUX_INSERT_VM_CONSTANT(name_literal, numeric_value) names.insert(name_literal);
  FLEAUX_VM_CONSTANT_BUILTINS(FLEAUX_INSERT_VM_CONSTANT)
#undef FLEAUX_INSERT_VM_CONSTANT
  return names;
}

}  // namespace

TEST_CASE("VM builtin catalog stays in sync with Std.fleaux", "[bytecode]") {
  REQUIRE(vm_catalog_builtin_names() == parse_std_declared_names(true));
  REQUIRE(vm_catalog_constant_names() == parse_std_declared_names(false));
}

// ---------------------------------------------------------------------------
// Core pipeline: (4, 5) -> Std.Add -> Std.Println
// Expected codegen:
//   [0] kPushConst     idx(4)
//   [1] kPushConst     idx(5)
//   [2] kBuildTuple    2
//   [3] kCallBuiltin   idx(Std.Add)      = 0
//   [4] kCallBuiltin   idx(Std.Println)  = 1
//   [5] kPop
//   [6] kHalt
// ---------------------------------------------------------------------------
TEST_CASE("Bytecode compiler emits pipeline with BuildTuple and CallBuiltin", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(
      "(4, 5) -> Std.Add -> Std.Println;\n",
      "bytecode_pipeline.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  const auto& instr = result->instructions;
  REQUIRE(instr.size() == 7);

  REQUIRE(instr[0].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(std::get<std::int64_t>(result->constants.at(static_cast<std::size_t>(instr[0].operand)).data) == 4);

  REQUIRE(instr[1].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(std::get<std::int64_t>(result->constants.at(static_cast<std::size_t>(instr[1].operand)).data) == 5);

  REQUIRE(instr[2].opcode == fleaux::bytecode::Opcode::kBuildTuple);
  REQUIRE(instr[2].operand == 2);

  REQUIRE(instr[3].opcode == fleaux::bytecode::Opcode::kCallBuiltin);
  REQUIRE(result->builtin_names.at(static_cast<std::size_t>(instr[3].operand)) == "Std.Add");

  REQUIRE(instr[4].opcode == fleaux::bytecode::Opcode::kCallBuiltin);
  REQUIRE(result->builtin_names.at(static_cast<std::size_t>(instr[4].operand)) == "Std.Println");

  REQUIRE(instr[5].opcode == fleaux::bytecode::Opcode::kPop);
  REQUIRE(instr[6].opcode == fleaux::bytecode::Opcode::kHalt);
}

TEST_CASE("Bytecode compiler emits native opcode for binary operator shorthand", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(
      "(4, 5) -> + -> Std.Println;\n",
      "bytecode_native_add.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  const auto& instr = result->instructions;
  REQUIRE(instr.size() == 6);
  REQUIRE(instr[0].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(std::get<std::int64_t>(result->constants.at(static_cast<std::size_t>(instr[0].operand)).data) == 4);
  REQUIRE(instr[1].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(std::get<std::int64_t>(result->constants.at(static_cast<std::size_t>(instr[1].operand)).data) == 5);
  REQUIRE(instr[2].opcode == fleaux::bytecode::Opcode::kAdd);
  REQUIRE(instr[3].opcode == fleaux::bytecode::Opcode::kCallBuiltin);
  REQUIRE(result->builtin_names.at(static_cast<std::size_t>(instr[3].operand)) == "Std.Println");
  REQUIRE(instr[4].opcode == fleaux::bytecode::Opcode::kPop);
  REQUIRE(instr[5].opcode == fleaux::bytecode::Opcode::kHalt);
}

TEST_CASE("Bytecode compiler emits native opcode for unary operator shorthand", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(
      "(True) -> ! -> Std.Println;\n",
      "bytecode_native_not.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  const auto& instr = result->instructions;
  REQUIRE(instr.size() == 5);
  REQUIRE(instr[0].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(instr[1].opcode == fleaux::bytecode::Opcode::kNot);
  REQUIRE(instr[2].opcode == fleaux::bytecode::Opcode::kCallBuiltin);
  REQUIRE(result->builtin_names.at(static_cast<std::size_t>(instr[2].operand)) == "Std.Println");
  REQUIRE(instr[3].opcode == fleaux::bytecode::Opcode::kPop);
  REQUIRE(instr[4].opcode == fleaux::bytecode::Opcode::kHalt);
}

TEST_CASE("Bytecode compiler emits kSelect for Std.Select", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(
      "(True, 10, 20) -> Std.Select -> Std.Println;\n",
      "bytecode_native_select.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  const auto& instr = result->instructions;
  bool found_select = false;
  for (const auto& ins : instr) {
    if (ins.opcode == fleaux::bytecode::Opcode::kSelect) {
      found_select = true;
    }
  }
  REQUIRE(found_select);
}

TEST_CASE("Bytecode compiler emits kLoopCall for Std.Loop", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(R"(
let Continue(n: Number): Bool = (n, 0) -> Std.GreaterThan;
let Step(n: Number): Number = (n, 1) -> Std.Subtract;
(10, Continue, Step) -> Std.Loop -> Std.Println;
)",
      "bytecode_native_loop.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  bool found_loop_call = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kLoopCall) {
      found_loop_call = true;
    }
  }
  REQUIRE(found_loop_call);
}

TEST_CASE("Bytecode compiler emits kLoopNCall for Std.LoopN", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(R"(
let Continue(n: Number): Bool = (n, 0) -> Std.GreaterThan;
let Step(n: Number): Number = (n, 1) -> Std.Subtract;
(10, Continue, Step, 100) -> Std.LoopN -> Std.Println;
)",
      "bytecode_native_loopn.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  bool found_loop_n_call = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kLoopNCall) {
      found_loop_n_call = true;
    }
  }
  REQUIRE(found_loop_n_call);
}

TEST_CASE("Bytecode compiler emits kBranchCall for Std.Branch with user function refs", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(R"(
let Inc(x: Number): Number = (x, 1) -> Std.Add;
let Dec(x: Number): Number = (x, 1) -> Std.Subtract;
(True, 10, Inc, Dec) -> Std.Branch -> Std.Println;
)",
      "bytecode_native_branch_safe.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  bool found_branch_call = false;
  bool found_builtin_branch = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kBranchCall) {
      found_branch_call = true;
    }
    if (ins.opcode == fleaux::bytecode::Opcode::kCallBuiltin) {
      const auto& name = result->builtin_names.at(static_cast<std::size_t>(ins.operand));
      if (name == "Std.Branch") {
        found_builtin_branch = true;
      }
    }
  }

  REQUIRE(found_branch_call);
  REQUIRE_FALSE(found_builtin_branch);
}

TEST_CASE("Bytecode compiler emits builtin callable refs for Std.Apply value call", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(
      "(5, Std.UnaryMinus) -> Std.Apply -> Std.Println;\n",
      "bytecode_builtin_callable_apply.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  bool found_make_builtin_ref = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kMakeBuiltinFuncRef) {
      found_make_builtin_ref = true;
      const auto& name = result->builtin_names.at(static_cast<std::size_t>(ins.operand));
      REQUIRE(name == "Std.UnaryMinus");
    }
  }
  REQUIRE(found_make_builtin_ref);
}

TEST_CASE("Bytecode compiler emits kBranchCall for Std.Branch with builtin refs", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(
      "(True, 10, Std.UnaryMinus, Std.UnaryPlus) -> Std.Branch -> Std.Println;\n",
      "bytecode_builtin_callable_branch.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  bool found_branch_call = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kBranchCall) {
      found_branch_call = true;
    }
  }
  REQUIRE(found_branch_call);
}

TEST_CASE("Bytecode compiler emits closure materialization for inline closure literals", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(
      "(10, (x: Number): Number = (x, 1) -> Std.Add) -> Std.Apply -> Std.Println;\n",
      "bytecode_inline_closure.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());
  REQUIRE(!result->closures.empty());

  bool found_make_closure = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kMakeClosureRef) {
      found_make_closure = true;
    }
  }
  REQUIRE(found_make_closure);
}

TEST_CASE("Bytecode compiler wires captured closure factory function", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(
      "let MakeAdder(n: Number): Any = (x: Number): Number = (x, n) -> Std.Add;\n"
      "(10, (4) -> MakeAdder) -> Std.Apply -> Std.Println;\n",
      "bytecode_captured_closure.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);
  REQUIRE(result.has_value());

  REQUIRE(result->functions.size() >= 2);
  REQUIRE(!result->closures.empty());

  const auto make_it = std::find_if(
      result->functions.begin(), result->functions.end(),
      [](const fleaux::bytecode::FunctionDef& fn) {
        return fn.name == "MakeAdder";
      });
  REQUIRE(make_it != result->functions.end());
  const auto& make_adder_fn = *make_it;
  bool found_capture_load = false;
  bool found_capture_tuple = false;
  bool found_make_closure = false;
  for (const auto& ins : make_adder_fn.instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kLoadLocal) {
      found_capture_load = true;
    }
    if (ins.opcode == fleaux::bytecode::Opcode::kBuildTuple) {
      found_capture_tuple = true;
    }
    if (ins.opcode == fleaux::bytecode::Opcode::kMakeClosureRef) {
      found_make_closure = true;
    }
  }

  REQUIRE(found_capture_load);
  REQUIRE(found_capture_tuple);
  REQUIRE(found_make_closure);
}

TEST_CASE("Bytecode compiler emits variadic metadata for inline closures", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(
      "(10, ((head: Number, tail: Any...): Number = head)) -> Std.Apply -> Std.Println;\n",
      "bytecode_variadic_inline_closure.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);
  REQUIRE(result.has_value());

  REQUIRE(result->closures.size() == 1);
  const auto& closure = result->closures[0];
  REQUIRE(closure.declared_arity == 2);
  REQUIRE(closure.declared_has_variadic_tail);
}

TEST_CASE("Bytecode compiler emits builtin call for Std.Match", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(
      "(1, (0, (): Any = \"zero\"), (_, (): Any = \"many\")) -> Std.Match -> Std.Println;\n",
      "bytecode_std_match.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);
  REQUIRE(result.has_value());

  bool found_match_call = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kCallBuiltin) {
      const auto& name = result->builtin_names.at(static_cast<std::size_t>(ins.operand));
      if (name == "Std.Match") {
        found_match_call = true;
      }
    }
  }
  REQUIRE(found_match_call);
}

TEST_CASE("Bytecode compiler keeps Std.Branch as builtin for callable locals", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(R"(
let Inc(x: Number): Number = (x, 1) -> Std.Add;
let Dec(x: Number): Number = (x, 1) -> Std.Subtract;
let ChooseApply(x: Number, tf: Any, ff: Any): Number =
    ((x, 0) -> Std.GreaterThan, x, tf, ff) -> Std.Branch;
(10, Inc, Dec) -> ChooseApply -> Std.Println;
)",
      "bytecode_branch_callable_locals.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  const auto choose_it = std::find_if(
      result->functions.begin(), result->functions.end(),
      [](const fleaux::bytecode::FunctionDef& fn) {
        return fn.name == "ChooseApply";
      });
  REQUIRE(choose_it != result->functions.end());

  bool found_branch_call = false;
  bool found_builtin_branch = false;
  for (const auto& ins : choose_it->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kBranchCall) {
      found_branch_call = true;
    }
    if (ins.opcode == fleaux::bytecode::Opcode::kCallBuiltin) {
      const auto& name = result->builtin_names.at(static_cast<std::size_t>(ins.operand));
      if (name == "Std.Branch") {
        found_builtin_branch = true;
      }
    }
  }

  REQUIRE_FALSE(found_branch_call);
  REQUIRE(found_builtin_branch);
}

// ---------------------------------------------------------------------------
// Stdlib ToString now compiles successfully via kCallBuiltin.
// ---------------------------------------------------------------------------
TEST_CASE("Bytecode compiler supports Std.ToString via kCallBuiltin", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(
      "(123) -> Std.ToString -> Std.Println;\n",
      "bytecode_tostring.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  // Both Std.ToString and Std.Println are compiled as kCallBuiltin.
  bool found_to_string = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kCallBuiltin) {
      const auto& name =
          result->builtin_names.at(static_cast<std::size_t>(ins.operand));
      if (name == "Std.ToString") {
        found_to_string = true;
      }
    }
  }
  REQUIRE(found_to_string);
}

// ---------------------------------------------------------------------------
// Truly unsupported call target: an unknown unqualified name that is neither a
// stdlib builtin nor a user-defined function.
// ---------------------------------------------------------------------------
TEST_CASE("Bytecode compiler returns error for unknown call target", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(
      "(42) -> UnknownFunction;\n",
      "bytecode_unknown_target.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message.find("Unsupported call target") != std::string::npos);
}

// ---------------------------------------------------------------------------
// User-defined function: let and call.
// ---------------------------------------------------------------------------
TEST_CASE("Bytecode compiler emits user function and kCallUserFunc", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(R"(
let Double(x: Number): Number = (x, x) -> Std.Add;
(7) -> Double -> Std.Println;
)",
      "bytecode_user_func.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());
  // One function should be compiled.
  REQUIRE(result->functions.size() == 1);
  REQUIRE(result->functions[0].name == "Double");
  REQUIRE(result->functions[0].arity == 1);

  // The top-level code should include a kCallUserFunc.
  bool found_call_user_func = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kCallUserFunc) {
      REQUIRE(ins.operand == 0);  // function index 0
      found_call_user_func = true;
    }
  }
  REQUIRE(found_call_user_func);

  // The function body should end with kReturn.
  const auto& fn_instrs = result->functions[0].instructions;
  REQUIRE(!fn_instrs.empty());
  REQUIRE(fn_instrs.back().opcode == fleaux::bytecode::Opcode::kReturn);
}

// ---------------------------------------------------------------------------
// Constant pool: constants go into Module::constants.
// ---------------------------------------------------------------------------
TEST_CASE("Bytecode compiler stores constants in pool", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(
      "(\"hello\") -> Std.Println;\n",
      "bytecode_string_const.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());
  REQUIRE(!result->constants.empty());

  // The first constant should be the string "hello".
  const auto& c = result->constants[0];
  REQUIRE(std::holds_alternative<std::string>(c.data));
  REQUIRE(std::get<std::string>(c.data) == "hello");
}
