#include <string>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/bytecode/compiler.hpp"
#include "fleaux/bytecode/module.hpp"
#include "fleaux/bytecode/opcode.hpp"
#include "fleaux/frontend/lowering.hpp"
#include "fleaux/frontend/parser.hpp"

namespace {

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

}  // namespace

// ---------------------------------------------------------------------------
// Core pipeline: (4, 5) -> Std.Add -> Std.Println
// Expected codegen:
//   [0] kPushConstI64  4
//   [1] kPushConstI64  5
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

  REQUIRE(instr[0].opcode == fleaux::bytecode::Opcode::kPushConstI64);
  REQUIRE(instr[0].operand == 4);

  REQUIRE(instr[1].opcode == fleaux::bytecode::Opcode::kPushConstI64);
  REQUIRE(instr[1].operand == 5);

  REQUIRE(instr[2].opcode == fleaux::bytecode::Opcode::kBuildTuple);
  REQUIRE(instr[2].operand == 2);

  REQUIRE(instr[3].opcode == fleaux::bytecode::Opcode::kCallBuiltin);
  REQUIRE(result->builtin_names.at(static_cast<std::size_t>(instr[3].operand)) == "Std.Add");

  REQUIRE(instr[4].opcode == fleaux::bytecode::Opcode::kCallBuiltin);
  REQUIRE(result->builtin_names.at(static_cast<std::size_t>(instr[4].operand)) == "Std.Println");

  REQUIRE(instr[5].opcode == fleaux::bytecode::Opcode::kPop);
  REQUIRE(instr[6].opcode == fleaux::bytecode::Opcode::kHalt);
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
// Constant pool: non-integer constants go into Module::constants.
// ---------------------------------------------------------------------------
TEST_CASE("Bytecode compiler stores non-integer constants in pool", "[bytecode]") {
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
