#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/bytecode/module.hpp"
#include "fleaux/bytecode/opcode.hpp"
#include "fleaux/vm/runtime.hpp"

TEST_CASE("VM executes arithmetic bytecode and prints result", "[vm]") {
  fleaux::bytecode::Module module;
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConstI64, 9},
      {fleaux::bytecode::Opcode::kPushConstI64, 3},
      {fleaux::bytecode::Opcode::kDivI64, 0},
      {fleaux::bytecode::Opcode::kPushConstI64, 7},
      {fleaux::bytecode::Opcode::kAddI64, 0},
      {fleaux::bytecode::Opcode::kPrint, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "10\n");
}

TEST_CASE("VM reports division by zero", "[vm]") {
  fleaux::bytecode::Module module;
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConstI64, 10},
      {fleaux::bytecode::Opcode::kPushConstI64, 0},
      {fleaux::bytecode::Opcode::kDivI64, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message == "division by zero");
}

TEST_CASE("VM reports missing halt", "[vm]") {
  fleaux::bytecode::Module module;
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConstI64, 1},
      {fleaux::bytecode::Opcode::kPushConstI64, 2},
      {fleaux::bytecode::Opcode::kAddI64, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message == "program terminated without halt");
}

