#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <set>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/bytecode/compiler.hpp"
#include "fleaux/bytecode/module_loader.hpp"
#include "fleaux/bytecode/module.hpp"
#include "fleaux/bytecode/opcode.hpp"
#include "fleaux/bytecode/optimizer.hpp"
#include "fleaux/bytecode/serialization.hpp"
#include "fleaux/frontend/lowering.hpp"
#include "fleaux/frontend/parser.hpp"
#include "fleaux/vm/builtin_catalog.hpp"
#include "fleaux/vm/runtime.hpp"

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
let Continue(n: Float64): Bool = (n, 0) -> Std.GreaterThan;
let Step(n: Float64): Float64 = (n, 1) -> Std.Subtract;
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
let Continue(n: Float64): Bool = (n, 0) -> Std.GreaterThan;
let Step(n: Float64): Float64 = (n, 1) -> Std.Subtract;
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
let Inc(x: Float64): Float64 = (x, 1) -> Std.Add;
let Dec(x: Float64): Float64 = (x, 1) -> Std.Subtract;
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
      "(10, (x: Float64): Float64 = (x, 1) -> Std.Add) -> Std.Apply -> Std.Println;\n",
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
      "let MakeAdder(n: Float64): Any = (x: Float64): Float64 = (x, n) -> Std.Add;\n"
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
      "(10, ((head: Float64, tail: Any...): Float64 = head)) -> Std.Apply -> Std.Println;\n",
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
let Inc(x: Float64): Float64 = (x, 1) -> Std.Add;
let Dec(x: Float64): Float64 = (x, 1) -> Std.Subtract;
let ChooseApply(x: Float64, tf: Any, ff: Any): Float64 =
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
let Double(x: Float64): Float64 = (x, x) -> Std.Add;
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

// ---------------------------------------------------------------------------
// NoOpEliminationPass
// ---------------------------------------------------------------------------
TEST_CASE("NoOpEliminationPass removes kNoOp from top-level instructions", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.instructions = {
      {fleaux::bytecode::Opcode::kNoOp, 0},
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kNoOp, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::NoOpEliminationPass{}.run(module);

  REQUIRE(module.instructions.size() == 2);
  REQUIRE(module.instructions[0].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(module.instructions[1].opcode == fleaux::bytecode::Opcode::kHalt);
}

TEST_CASE("NoOpEliminationPass removes kNoOp from function body", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  fleaux::bytecode::FunctionDef fn;
  fn.name = "Foo";
  fn.arity = 0;
  fn.instructions = {
      {fleaux::bytecode::Opcode::kNoOp, 0},
      {fleaux::bytecode::Opcode::kLoadLocal, 0},
      {fleaux::bytecode::Opcode::kNoOp, 0},
      {fleaux::bytecode::Opcode::kReturn, 0},
  };
  module.functions.push_back(std::move(fn));

  fleaux::bytecode::NoOpEliminationPass{}.run(module);

  REQUIRE(module.functions[0].instructions.size() == 2);
  REQUIRE(module.functions[0].instructions[0].opcode == fleaux::bytecode::Opcode::kLoadLocal);
  REQUIRE(module.functions[0].instructions[1].opcode == fleaux::bytecode::Opcode::kReturn);
}

TEST_CASE("NoOpEliminationPass is a no-op when there are no kNoOps", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::NoOpEliminationPass{}.run(module);

  REQUIRE(module.instructions.size() == 2);
}

TEST_CASE("BytecodeOptimizer baseline always runs NoOpEliminationPass", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.instructions = {
      {fleaux::bytecode::Opcode::kNoOp, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  constexpr fleaux::bytecode::BytecodeOptimizer optimizer;
  const auto result = optimizer.optimize(module);  // default = kBaseline

  REQUIRE(result.has_value());
  REQUIRE(module.instructions.size() == 1);
  REQUIRE(module.instructions[0].opcode == fleaux::bytecode::Opcode::kHalt);
}

// ---------------------------------------------------------------------------
// ConstantPoolDeduplicationPass
// ---------------------------------------------------------------------------
TEST_CASE("ConstantPoolDeduplicationPass merges duplicate pool entries", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  // Pool: [42, 99, 42] -- entry 0 and 2 are duplicates.
  module.constants = {
      fleaux::bytecode::ConstValue{std::int64_t{42}},
      fleaux::bytecode::ConstValue{std::int64_t{99}},
      fleaux::bytecode::ConstValue{std::int64_t{42}},
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},  // push 42 (first)
      {fleaux::bytecode::Opcode::kPushConst, 1},  // push 99
      {fleaux::bytecode::Opcode::kPushConst, 2},  // push 42 (duplicate)
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::ConstantPoolDeduplicationPass{}.run(module);

  // Pool should now have 2 entries: 42 and 99.
  REQUIRE(module.constants.size() == 2);
  REQUIRE(std::get<std::int64_t>(module.constants[0].data) == 42);
  REQUIRE(std::get<std::int64_t>(module.constants[1].data) == 99);

  // Both pushes of 42 now point to index 0; the 99 push stays at index 1.
  REQUIRE(module.instructions[0].operand == 0);
  REQUIRE(module.instructions[1].operand == 1);
  REQUIRE(module.instructions[2].operand == 0);
}

TEST_CASE("ConstantPoolDeduplicationPass is a no-op when all entries are unique", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.constants = {
      fleaux::bytecode::ConstValue{std::int64_t{1}},
      fleaux::bytecode::ConstValue{std::int64_t{2}},
      fleaux::bytecode::ConstValue{std::int64_t{3}},
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kPushConst, 1},
      {fleaux::bytecode::Opcode::kPushConst, 2},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::ConstantPoolDeduplicationPass{}.run(module);

  REQUIRE(module.constants.size() == 3);
  REQUIRE(module.instructions[0].operand == 0);
  REQUIRE(module.instructions[1].operand == 1);
  REQUIRE(module.instructions[2].operand == 2);
}

TEST_CASE("ConstantPoolDeduplicationPass deduplicates string constants", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.constants = {
      fleaux::bytecode::ConstValue{std::string{"hello"}},
      fleaux::bytecode::ConstValue{std::string{"hello"}},
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kPushConst, 1},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::ConstantPoolDeduplicationPass{}.run(module);

  REQUIRE(module.constants.size() == 1);
  REQUIRE(module.instructions[0].operand == 0);
  REQUIRE(module.instructions[1].operand == 0);
}

// ---------------------------------------------------------------------------
// DeadPushEliminationPass
// ---------------------------------------------------------------------------
TEST_CASE("DeadPushEliminationPass removes kPushConst followed by kPop", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kPop, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::DeadPushEliminationPass{}.run(module);

  REQUIRE(module.instructions.size() == 1);
  REQUIRE(module.instructions[0].opcode == fleaux::bytecode::Opcode::kHalt);
}

TEST_CASE("DeadPushEliminationPass removes kLoadLocal followed by kPop", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.instructions = {
      {fleaux::bytecode::Opcode::kLoadLocal, 0},
      {fleaux::bytecode::Opcode::kPop, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::DeadPushEliminationPass{}.run(module);

  REQUIRE(module.instructions.size() == 1);
  REQUIRE(module.instructions[0].opcode == fleaux::bytecode::Opcode::kHalt);
}

TEST_CASE("DeadPushEliminationPass does not remove kPop after a non-push", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  // kCallBuiltin has side effects; the kPop after it is live.
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kCallBuiltin, 0},
      {fleaux::bytecode::Opcode::kPop, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::DeadPushEliminationPass{}.run(module);

  REQUIRE(module.instructions.size() == 4);
}

TEST_CASE("DeadPushEliminationPass handles multiple consecutive dead pairs", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kPop, 0},
      {fleaux::bytecode::Opcode::kPushConst, 1},
      {fleaux::bytecode::Opcode::kPop, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::DeadPushEliminationPass{}.run(module);

  REQUIRE(module.instructions.size() == 1);
  REQUIRE(module.instructions[0].opcode == fleaux::bytecode::Opcode::kHalt);
}

TEST_CASE("BytecodeOptimizer extended mode also eliminates kNoOps", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.instructions = {
      {fleaux::bytecode::Opcode::kNoOp, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  constexpr fleaux::bytecode::BytecodeOptimizer optimizer;
  const auto result = optimizer.optimize(
      module, fleaux::bytecode::OptimizerConfig{.mode = fleaux::bytecode::OptimizationMode::kExtended});

  REQUIRE(result.has_value());
  REQUIRE(module.instructions.size() == 1);
  REQUIRE(module.instructions[0].opcode == fleaux::bytecode::Opcode::kHalt);
}

// ---------------------------------------------------------------------------
// Bytecode Serialization
// ---------------------------------------------------------------------------
TEST_CASE("Bytecode serialization and deserialization round-trips", "[bytecode][serialization]") {
  fleaux::bytecode::Module original;
  original.header.module_name = "RoundTrip";
  original.header.source_path = "RoundTrip.fleaux";
  original.header.source_hash = 12345;
  original.header.optimization_mode = 1;
  original.dependencies = {
      fleaux::bytecode::ModuleDependency{.module_name = "Std", .is_symbolic = true},
      fleaux::bytecode::ModuleDependency{.module_name = "20_export", .is_symbolic = false},
  };
  original.exports = {
      fleaux::bytecode::ExportedSymbol{
          .name = "RoundTrip.Double",
          .kind = fleaux::bytecode::ExportKind::kFunction,
          .index = 0,
          .builtin_name = {},
      },
      fleaux::bytecode::ExportedSymbol{
          .name = "RoundTrip.Println",
          .kind = fleaux::bytecode::ExportKind::kBuiltinAlias,
          .index = 0,
          .builtin_name = "RoundTrip.Println",
      },
  };
  original.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kPushConst, 1},
      {fleaux::bytecode::Opcode::kAdd, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };
  original.constants = {
      fleaux::bytecode::ConstValue{std::int64_t{10}},
      fleaux::bytecode::ConstValue{std::int64_t{20}},
  };
  original.builtin_names = {"Std.Println"};

  const auto serialized = fleaux::bytecode::serialize_module(original);
  REQUIRE(serialized.has_value());

  const auto deserialized = fleaux::bytecode::deserialize_module(serialized.value());
  REQUIRE(deserialized.has_value());

  const auto& restored = deserialized.value();
  REQUIRE(restored.header.module_name == original.header.module_name);
  REQUIRE(restored.header.source_path == original.header.source_path);
  REQUIRE(restored.header.source_hash == original.header.source_hash);
  REQUIRE(restored.header.optimization_mode == original.header.optimization_mode);
  REQUIRE(restored.header.payload_checksum != 0);
  REQUIRE(restored.dependencies.size() == original.dependencies.size());
  REQUIRE(restored.exports.size() == original.exports.size());
  REQUIRE(restored.instructions.size() == original.instructions.size());
  REQUIRE(restored.constants.size() == original.constants.size());
  REQUIRE(restored.builtin_names.size() == original.builtin_names.size());

  REQUIRE(restored.dependencies[0].module_name == "Std");
  REQUIRE(restored.dependencies[0].is_symbolic);
  REQUIRE(restored.dependencies[1].module_name == "20_export");
  REQUIRE_FALSE(restored.dependencies[1].is_symbolic);
  REQUIRE(restored.exports[0].name == "RoundTrip.Double");
  REQUIRE(restored.exports[0].kind == fleaux::bytecode::ExportKind::kFunction);
  REQUIRE(restored.exports[0].index == 0);
  REQUIRE(restored.exports[1].kind == fleaux::bytecode::ExportKind::kBuiltinAlias);
  REQUIRE(restored.exports[1].builtin_name == "RoundTrip.Println");

  for (std::size_t i = 0; i < restored.instructions.size(); ++i) {
    REQUIRE(restored.instructions[i].opcode == original.instructions[i].opcode);
    REQUIRE(restored.instructions[i].operand == original.instructions[i].operand);
  }

  REQUIRE(std::get<std::int64_t>(restored.constants[0].data) == 10);
  REQUIRE(std::get<std::int64_t>(restored.constants[1].data) == 20);
  REQUIRE(restored.builtin_names[0] == "Std.Println");
}

TEST_CASE("Bytecode serialization handles all constant types", "[bytecode][serialization]") {
  fleaux::bytecode::Module original;
  original.constants = {
      fleaux::bytecode::ConstValue{std::int64_t{-42}},
      fleaux::bytecode::ConstValue{std::uint64_t{9999}},
      fleaux::bytecode::ConstValue{3.14159},
      fleaux::bytecode::ConstValue{true},
      fleaux::bytecode::ConstValue{std::string{"hello world"}},
      fleaux::bytecode::ConstValue{std::monostate{}},
  };

  const auto serialized = fleaux::bytecode::serialize_module(original);
  REQUIRE(serialized.has_value());

  const auto deserialized = fleaux::bytecode::deserialize_module(serialized.value());
  REQUIRE(deserialized.has_value());

  const auto& restored = deserialized.value();
  REQUIRE(restored.constants.size() == 6);
  REQUIRE(std::get<std::int64_t>(restored.constants[0].data) == -42);
  REQUIRE(std::get<std::uint64_t>(restored.constants[1].data) == 9999);
  REQUIRE(std::abs(std::get<double>(restored.constants[2].data) - 3.14159) < 0.00001);
  REQUIRE(std::get<bool>(restored.constants[3].data) == true);
  REQUIRE(std::get<std::string>(restored.constants[4].data) == "hello world");
  REQUIRE(std::holds_alternative<std::monostate>(restored.constants[5].data));
}

TEST_CASE("Bytecode serialization handles functions", "[bytecode][serialization]") {
  fleaux::bytecode::Module original;
  fleaux::bytecode::FunctionDef fn;
  fn.name = "MyFunc";
  fn.arity = 2;
  fn.has_variadic_tail = false;
  fn.instructions = {
      {fleaux::bytecode::Opcode::kLoadLocal, 0},
      {fleaux::bytecode::Opcode::kLoadLocal, 1},
      {fleaux::bytecode::Opcode::kAdd, 0},
      {fleaux::bytecode::Opcode::kReturn, 0},
  };
  original.functions.push_back(std::move(fn));

  const auto serialized = fleaux::bytecode::serialize_module(original);
  REQUIRE(serialized.has_value());

  const auto deserialized = fleaux::bytecode::deserialize_module(serialized.value());
  REQUIRE(deserialized.has_value());

  const auto& restored = deserialized.value();
  REQUIRE(restored.functions.size() == 1);
  REQUIRE(restored.functions[0].name == "MyFunc");
  REQUIRE(restored.functions[0].arity == 2);
  REQUIRE(restored.functions[0].has_variadic_tail == false);
  REQUIRE(restored.functions[0].instructions.size() == 4);
}

TEST_CASE("Bytecode deserialization rejects invalid magic", "[bytecode][serialization]") {
  std::vector<std::uint8_t> bad_buffer;
  bad_buffer.push_back(0xFF);
  bad_buffer.push_back(0xFF);
  bad_buffer.push_back(0xFF);
  bad_buffer.push_back(0xFF);

  const auto result = fleaux::bytecode::deserialize_module(bad_buffer);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message.find("Invalid magic") != std::string::npos);
}

TEST_CASE("Bytecode deserialization rejects version mismatch", "[bytecode][serialization]") {
  std::vector<std::uint8_t> bad_buffer;
  // Write correct magic.
  std::uint32_t magic = 0x464C4558;
  std::uint32_t bad_version = 999;
  const auto* magic_bytes = reinterpret_cast<const std::uint8_t*>(&magic);
  const auto* version_bytes = reinterpret_cast<const std::uint8_t*>(&bad_version);
  bad_buffer.insert(bad_buffer.end(), magic_bytes, magic_bytes + 4);
  bad_buffer.insert(bad_buffer.end(), version_bytes, version_bytes + 4);

  const auto result = fleaux::bytecode::deserialize_module(bad_buffer);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message.find("Version") != std::string::npos);
}

TEST_CASE("Bytecode deserialization rejects payload checksum mismatch", "[bytecode][serialization]") {
  fleaux::bytecode::Module module;
  module.header.module_name = "Checksum";
  module.instructions = {{fleaux::bytecode::Opcode::kHalt, 0}};

  const auto serialized = fleaux::bytecode::serialize_module(module);
  REQUIRE(serialized.has_value());

  auto corrupted = serialized.value();
  REQUIRE(corrupted.size() > 16);
  corrupted.back() ^= 0x01;

  const auto result = fleaux::bytecode::deserialize_module(corrupted);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message.find("checksum") != std::string::npos);
}

TEST_CASE("Bytecode round-trip: compile -> serialize -> deserialize -> verify", "[bytecode][serialization]") {
  const std::string source_text =
      "import Std;\n"
      "import 20_export;\n"
      "let Local(x: Float64): Any = (y: Float64): Float64 = (x, y) -> Std.Add;\n"
      "(4, 5) -> Std.Add -> Std.Println;\n";
  const auto ir_program = lower_source_to_ir(source_text, "bytecode_roundtrip.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled = compiler.compile(ir_program, fleaux::bytecode::CompileOptions{
                                                        .source_path = std::filesystem::path("bytecode_roundtrip.fleaux"),
                                                        .source_text = source_text,
                                                        .module_name = "bytecode_roundtrip",
                                                    });
  REQUIRE(compiled.has_value());

  const auto& original = compiled.value();
  REQUIRE(original.header.module_name == "bytecode_roundtrip");
  REQUIRE(original.header.source_path == "bytecode_roundtrip.fleaux");
  REQUIRE(original.header.source_hash != 0);
  REQUIRE(original.dependencies.size() == 2);
  REQUIRE(original.dependencies[0].module_name == "Std");
  REQUIRE(original.dependencies[0].is_symbolic);
  REQUIRE(original.dependencies[1].module_name == "20_export");
  REQUIRE_FALSE(original.dependencies[1].is_symbolic);
  REQUIRE(original.exports.size() == 1);
  REQUIRE(original.exports[0].name == "Local");
  REQUIRE(original.exports[0].kind == fleaux::bytecode::ExportKind::kFunction);
  REQUIRE(original.functions.size() >= 2);  // Local + synthetic closure helper.
  const auto serialized = fleaux::bytecode::serialize_module(original);
  REQUIRE(serialized.has_value());

  const auto deserialized = fleaux::bytecode::deserialize_module(serialized.value());
  REQUIRE(deserialized.has_value());

  const auto& restored = deserialized.value();

  REQUIRE(restored.instructions.size() == original.instructions.size());
  REQUIRE(restored.constants.size() == original.constants.size());
  REQUIRE(restored.builtin_names.size() == original.builtin_names.size());
  REQUIRE(restored.functions.size() == original.functions.size());
  REQUIRE(restored.closures.size() == original.closures.size());
  REQUIRE(restored.header.module_name == original.header.module_name);
  REQUIRE(restored.header.source_hash == original.header.source_hash);
  REQUIRE(restored.dependencies.size() == original.dependencies.size());
  REQUIRE(restored.exports.size() == original.exports.size());
  REQUIRE(restored.exports[0].name == "Local");

  for (std::size_t i = 0; i < restored.instructions.size(); ++i) {
    REQUIRE(restored.instructions[i].opcode == original.instructions[i].opcode);
    REQUIRE(restored.instructions[i].operand == original.instructions[i].operand);
  }
}

TEST_CASE("Bytecode module loader imports serialized dependency modules", "[bytecode][serialization][imports]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_bytecode_serialized_imports";
  std::filesystem::create_directories(temp_dir);

  const auto dependency_path = temp_dir / "20_export.fleaux";
  const auto entry_path = temp_dir / "21_import.fleaux";

  {
    std::ofstream out(dependency_path);
    out << "import Std;\n"
           "let Add4(x: Float64): Float64 = (4, x) -> Std.Add;\n";
  }

  {
    std::ofstream out(entry_path);
    out << "import 20_export;\n"
           "(4) -> Add4 -> Std.Println;\n";
  }

  const auto initial_load = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE(initial_load.has_value());
  REQUIRE(std::filesystem::exists(temp_dir / "20_export.fleaux.bc"));
  REQUIRE(std::filesystem::exists(temp_dir / "21_import.fleaux.bc"));

  std::filesystem::remove(dependency_path);

  const auto cached_load = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE(cached_load.has_value());

  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(cached_load.value());
  if (!runtime_result) {
    INFO("vm runtime error: " << runtime_result.error().message);
  }
  REQUIRE(runtime_result.has_value());
}

TEST_CASE("Bytecode module loader can start from a serialized entry module", "[bytecode][serialization][imports]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_bytecode_serialized_entry";
  std::filesystem::create_directories(temp_dir);

  const auto dependency_path = temp_dir / "20_export.fleaux";
  const auto entry_path = temp_dir / "21_import.fleaux";

  {
    std::ofstream out(dependency_path);
    out << "import Std;\n"
           "let Add4(x: Float64): Float64 = (4, x) -> Std.Add;\n";
  }

  {
    std::ofstream out(entry_path);
    out << "import 20_export;\n"
           "(4) -> Add4 -> Std.Println;\n";
  }

  const auto initial_load = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE(initial_load.has_value());

  const auto entry_bytecode_path = temp_dir / "21_import.fleaux.bc";
  REQUIRE(std::filesystem::exists(entry_bytecode_path));

  std::filesystem::remove(entry_path);
  std::filesystem::remove(dependency_path);

  const auto bytecode_only_load = fleaux::bytecode::load_linked_module(entry_bytecode_path);
  REQUIRE(bytecode_only_load.has_value());
  REQUIRE(bytecode_only_load->exports.size() == 0);

  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(bytecode_only_load.value());
  if (!runtime_result) {
    INFO("vm runtime error: " << runtime_result.error().message);
  }
  REQUIRE(runtime_result.has_value());
}

TEST_CASE("Bytecode module loader handles diamond dependencies with serialized fallback",
          "[bytecode][serialization][imports][diamond]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_bytecode_diamond_imports";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto shared_path = temp_dir / "shared.fleaux";
  const auto left_path = temp_dir / "left.fleaux";
  const auto right_path = temp_dir / "right.fleaux";
  const auto root_path = temp_dir / "root.fleaux";

  {
    std::ofstream out(shared_path);
    out << "import Std;\n"
           "(\"shared-init\") -> Std.Println;\n"
           "let Add1(x: Float64): Float64 = (x, 1) -> Std.Add;\n";
  }

  {
    std::ofstream out(left_path);
    out << "import shared;\n"
           "let Left(x: Float64): Float64 = (x) -> Add1;\n";
  }

  {
    std::ofstream out(right_path);
    out << "import shared;\n"
           "let Right(x: Float64): Float64 = (x) -> Add1;\n";
  }

  {
    std::ofstream out(root_path);
    out << "import left;\n"
           "import right;\n"
           "(4) -> Left -> Right -> Std.Println;\n";
  }

  const auto initial_load = fleaux::bytecode::load_linked_module(root_path);
  REQUIRE(initial_load.has_value());

  REQUIRE(std::filesystem::exists(temp_dir / "shared.fleaux.bc"));
  REQUIRE(std::filesystem::exists(temp_dir / "left.fleaux.bc"));
  REQUIRE(std::filesystem::exists(temp_dir / "right.fleaux.bc"));
  REQUIRE(std::filesystem::exists(temp_dir / "root.fleaux.bc"));

  std::filesystem::remove(shared_path);

  const auto cached_load = fleaux::bytecode::load_linked_module(root_path);
  REQUIRE(cached_load.has_value());

  const fleaux::vm::Runtime runtime;
  std::ostringstream output;
  const auto runtime_result = runtime.execute(cached_load.value(), output);
  if (!runtime_result) {
    INFO("vm runtime error: " << runtime_result.error().message);
  }
  REQUIRE(runtime_result.has_value());

  const std::string text = output.str();
  REQUIRE(text.find("shared-init") != std::string::npos);
  const auto first = text.find("shared-init");
  REQUIRE(text.find("shared-init", first + 1) == std::string::npos);
}

TEST_CASE("ConstantFoldingPass folds safe literal unary and binary sequences", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.constants = {
      fleaux::bytecode::ConstValue{true},
      fleaux::bytecode::ConstValue{std::int64_t{2}},
      fleaux::bytecode::ConstValue{std::int64_t{3}},
      fleaux::bytecode::ConstValue{std::int64_t{10}},
      fleaux::bytecode::ConstValue{std::int64_t{4}},
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kNot, 0},
      {fleaux::bytecode::Opcode::kPushConst, 1},
      {fleaux::bytecode::Opcode::kPushConst, 2},
      {fleaux::bytecode::Opcode::kAdd, 0},
      {fleaux::bytecode::Opcode::kPushConst, 3},
      {fleaux::bytecode::Opcode::kPushConst, 4},
      {fleaux::bytecode::Opcode::kCmpGt, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::ConstantFoldingPass{}.run(module);

  REQUIRE(module.instructions.size() == 4);
  REQUIRE(module.instructions[0].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(module.instructions[1].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(module.instructions[2].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(module.instructions[3].opcode == fleaux::bytecode::Opcode::kHalt);

  REQUIRE(std::get<bool>(module.constants[static_cast<std::size_t>(module.instructions[0].operand)].data) == false);
  REQUIRE(std::get<std::int64_t>(module.constants[static_cast<std::size_t>(module.instructions[1].operand)].data) == 5);
  REQUIRE(std::get<bool>(module.constants[static_cast<std::size_t>(module.instructions[2].operand)].data) == true);
}

TEST_CASE("ConstantFoldingPass folds kDiv and preserves floating semantics", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.constants = {
      fleaux::bytecode::ConstValue{std::int64_t{10}},
      fleaux::bytecode::ConstValue{std::int64_t{4}},
      fleaux::bytecode::ConstValue{std::int64_t{0}},
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kPushConst, 1},
      {fleaux::bytecode::Opcode::kDiv, 0},
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kPushConst, 2},
      {fleaux::bytecode::Opcode::kDiv, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::ConstantFoldingPass{}.run(module);

  REQUIRE(module.instructions.size() == 3);
  REQUIRE(module.instructions[0].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(module.instructions[1].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(module.instructions[2].opcode == fleaux::bytecode::Opcode::kHalt);

  REQUIRE(std::holds_alternative<double>(module.constants[static_cast<std::size_t>(module.instructions[0].operand)].data));
  REQUIRE(std::get<double>(module.constants[static_cast<std::size_t>(module.instructions[0].operand)].data) == 2.5);

  REQUIRE(std::holds_alternative<double>(module.constants[static_cast<std::size_t>(module.instructions[1].operand)].data));
  REQUIRE(std::isinf(std::get<double>(module.constants[static_cast<std::size_t>(module.instructions[1].operand)].data)));
}

TEST_CASE("ConstantFoldingPass folds kMod and preserves NaN on mod-by-zero", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.constants = {
      fleaux::bytecode::ConstValue{std::int64_t{10}},
      fleaux::bytecode::ConstValue{std::int64_t{4}},
      fleaux::bytecode::ConstValue{std::int64_t{0}},
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kPushConst, 1},
      {fleaux::bytecode::Opcode::kMod, 0},
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kPushConst, 2},
      {fleaux::bytecode::Opcode::kMod, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::ConstantFoldingPass{}.run(module);

  REQUIRE(module.instructions.size() == 3);
  REQUIRE(module.instructions[0].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(module.instructions[1].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(module.instructions[2].opcode == fleaux::bytecode::Opcode::kHalt);

  REQUIRE(std::holds_alternative<std::int64_t>(module.constants[static_cast<std::size_t>(module.instructions[0].operand)].data));
  REQUIRE(std::get<std::int64_t>(module.constants[static_cast<std::size_t>(module.instructions[0].operand)].data) == 2);

  REQUIRE(std::holds_alternative<double>(module.constants[static_cast<std::size_t>(module.instructions[1].operand)].data));
  REQUIRE(std::isnan(std::get<double>(module.constants[static_cast<std::size_t>(module.instructions[1].operand)].data)));
}

TEST_CASE("ConstantFoldingPass folds kPow with integer and floating outputs", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.constants = {
      fleaux::bytecode::ConstValue{std::int64_t{2}},
      fleaux::bytecode::ConstValue{std::int64_t{8}},
      fleaux::bytecode::ConstValue{std::int64_t{-1}},
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kPushConst, 1},
      {fleaux::bytecode::Opcode::kPow, 0},
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kPushConst, 2},
      {fleaux::bytecode::Opcode::kPow, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::ConstantFoldingPass{}.run(module);

  REQUIRE(module.instructions.size() == 3);
  REQUIRE(module.instructions[0].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(module.instructions[1].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(module.instructions[2].opcode == fleaux::bytecode::Opcode::kHalt);

  REQUIRE(std::holds_alternative<std::int64_t>(module.constants[static_cast<std::size_t>(module.instructions[0].operand)].data));
  REQUIRE(std::get<std::int64_t>(module.constants[static_cast<std::size_t>(module.instructions[0].operand)].data) == 256);

  REQUIRE(std::holds_alternative<double>(module.constants[static_cast<std::size_t>(module.instructions[1].operand)].data));
  REQUIRE(std::get<double>(module.constants[static_cast<std::size_t>(module.instructions[1].operand)].data) == 0.5);
}

TEST_CASE("BytecodeOptimizer baseline does not run ConstantFoldingPass", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.constants = {
      fleaux::bytecode::ConstValue{std::int64_t{2}},
      fleaux::bytecode::ConstValue{std::int64_t{3}},
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kPushConst, 1},
      {fleaux::bytecode::Opcode::kAdd, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  constexpr fleaux::bytecode::BytecodeOptimizer optimizer;
  const auto result = optimizer.optimize(module, fleaux::bytecode::OptimizerConfig{
                                                     .mode = fleaux::bytecode::OptimizationMode::kBaseline,
                                                 });
  REQUIRE(result.has_value());
  REQUIRE(module.instructions.size() == 4);
  REQUIRE(module.instructions[2].opcode == fleaux::bytecode::Opcode::kAdd);
}

TEST_CASE("BytecodeOptimizer extended mode runs ConstantFoldingPass", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.constants = {
      fleaux::bytecode::ConstValue{std::int64_t{2}},
      fleaux::bytecode::ConstValue{std::int64_t{3}},
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kPushConst, 1},
      {fleaux::bytecode::Opcode::kAdd, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  constexpr fleaux::bytecode::BytecodeOptimizer optimizer;
  const auto result = optimizer.optimize(module, fleaux::bytecode::OptimizerConfig{
                                                     .mode = fleaux::bytecode::OptimizationMode::kExtended,
                                                 });
  REQUIRE(result.has_value());
  REQUIRE(module.instructions.size() == 2);
  REQUIRE(module.instructions[0].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(module.instructions[1].opcode == fleaux::bytecode::Opcode::kHalt);
  REQUIRE(std::get<std::int64_t>(module.constants[static_cast<std::size_t>(module.instructions[0].operand)].data) == 5);
}

TEST_CASE("ConstantPropagationSelectPass folds literal kSelect true branch", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.constants = {
      fleaux::bytecode::ConstValue{true},
      fleaux::bytecode::ConstValue{std::int64_t{10}},
      fleaux::bytecode::ConstValue{std::int64_t{20}},
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kPushConst, 1},
      {fleaux::bytecode::Opcode::kPushConst, 2},
      {fleaux::bytecode::Opcode::kSelect, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::ConstantPropagationSelectPass{}.run(module);

  REQUIRE(module.instructions.size() == 2);
  REQUIRE(module.instructions[0].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(module.instructions[0].operand == 1);
  REQUIRE(module.instructions[1].opcode == fleaux::bytecode::Opcode::kHalt);
}

TEST_CASE("ConstantPropagationSelectPass folds literal kSelect false branch", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.constants = {
      fleaux::bytecode::ConstValue{false},
      fleaux::bytecode::ConstValue{std::int64_t{10}},
      fleaux::bytecode::ConstValue{std::int64_t{20}},
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kPushConst, 1},
      {fleaux::bytecode::Opcode::kPushConst, 2},
      {fleaux::bytecode::Opcode::kSelect, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::ConstantPropagationSelectPass{}.run(module);

  REQUIRE(module.instructions.size() == 2);
  REQUIRE(module.instructions[0].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(module.instructions[0].operand == 2);
  REQUIRE(module.instructions[1].opcode == fleaux::bytecode::Opcode::kHalt);
}

TEST_CASE("BytecodeOptimizer extended mode propagates folded kSelect condition", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.constants = {
      fleaux::bytecode::ConstValue{std::int64_t{1}},
      fleaux::bytecode::ConstValue{std::int64_t{1}},
      fleaux::bytecode::ConstValue{std::int64_t{10}},
      fleaux::bytecode::ConstValue{std::int64_t{20}},
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kPushConst, 1},
      {fleaux::bytecode::Opcode::kCmpEq, 0},
      {fleaux::bytecode::Opcode::kPushConst, 2},
      {fleaux::bytecode::Opcode::kPushConst, 3},
      {fleaux::bytecode::Opcode::kSelect, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  constexpr fleaux::bytecode::BytecodeOptimizer optimizer;
  const auto result = optimizer.optimize(module, fleaux::bytecode::OptimizerConfig{
                                                     .mode = fleaux::bytecode::OptimizationMode::kExtended,
                                                 });
  REQUIRE(result.has_value());
  REQUIRE(module.instructions.size() == 2);
  REQUIRE(module.instructions[0].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(module.instructions[1].opcode == fleaux::bytecode::Opcode::kHalt);
  REQUIRE(std::get<std::int64_t>(module.constants[static_cast<std::size_t>(module.instructions[0].operand)].data) == 10);
}

