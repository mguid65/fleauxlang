#include <catch2/catch_test_macros.hpp>

#include "fleaux/bytecode/compiler.hpp"
#include "fleaux/bytecode/opcode.hpp"
#include "fleaux/frontend/ast.hpp"

TEST_CASE("Bytecode compiler emits halt for placeholder pipeline", "[bytecode]") {
  fleaux::frontend::ir::IRProgram ir_program;

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());
  REQUIRE(result->instructions.size() == 1);
  REQUIRE(result->instructions[0].opcode == fleaux::bytecode::Opcode::kHalt);
}

