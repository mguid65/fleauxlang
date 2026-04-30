#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include <sstream>

#include "fleaux/bytecode/module.hpp"
#include "fleaux/bytecode/opcode.hpp"
#include "fleaux/bytecode/serialization.hpp"

TEST_CASE("All opcodes can be serialized", "[bytecode][opcodes][stringify]") {
  using namespace fleaux::bytecode;

  REQUIRE(opcode_names.size() == static_cast<std::size_t>(Opcode::kDerefValueRef) + 1U);

  Module module;
  module.instructions.reserve(opcode_names.size());

  for (std::size_t opcode_index = 0; opcode_index < opcode_names.size(); ++opcode_index) {
    const auto opcode = static_cast<Opcode>(opcode_index);
    INFO("opcode index = " << opcode_index);

    const auto name = stringify_opcode(opcode);
    REQUIRE(name == opcode_names[opcode_index]);
    REQUIRE_FALSE(name.empty());

    module.instructions.push_back({.opcode = opcode, .operand = static_cast<std::int64_t>(opcode_index)});
  }

  const auto serialized = serialize_module(module);
  REQUIRE(serialized.has_value());

  const auto deserialized = deserialize_module(*serialized);
  REQUIRE(deserialized.has_value());

  REQUIRE(deserialized->instructions.size() == opcode_names.size());
  for (std::size_t opcode_index = 0; opcode_index < deserialized->instructions.size(); ++opcode_index) {
    INFO("opcode index = " << opcode_index);
    const auto& instruction = deserialized->instructions[opcode_index];
    const auto expected_operand = static_cast<decltype(instruction.operand)>(opcode_index);
    REQUIRE(instruction.opcode == static_cast<Opcode>(opcode_index));
    REQUIRE(instruction.operand == expected_operand);
    REQUIRE(stringify_opcode(instruction.opcode) == opcode_names[opcode_index]);
  }

  std::ostringstream dump;
  const auto disassembly = disassemble_module(*deserialized, dump);
  REQUIRE(disassembly.has_value());
  const auto dumped = dump.str();
  for (const auto expected_name : opcode_names) {
    INFO("expected opcode name = " << expected_name);
    REQUIRE(dumped.find(std::string{expected_name}) != std::string::npos);
  }
}