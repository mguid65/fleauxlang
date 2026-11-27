#include <catch2/catch_test_macros.hpp>

#include <sstream>

#include "fleaux/bytecode/module.hpp"
#include "fleaux/bytecode/opcode.hpp"
#include "fleaux/bytecode/serialization.hpp"

TEST_CASE("All opcodes can be serialized", "[bytecode][opcodes][stringify]") {
  using namespace fleaux::bytecode;

  REQUIRE(opcode_names.size() == static_cast<std::size_t>(Opcode::kBuildTupleSinceMark) + 1U);

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

TEST_CASE("Item reads of a local keep their slot and index in the operand", "[bytecode][opcodes][stringify]") {
  using namespace fleaux::bytecode;

  constexpr auto kLargest = local_item_operand(0xFFFF'FFFFU, kMaxLocalItemIndex);
  STATIC_REQUIRE(kLargest >= 0);
  STATIC_REQUIRE(local_item_slot(kLargest) == 0xFFFF'FFFFU);
  STATIC_REQUIRE(local_item_index(kLargest) == kMaxLocalItemIndex);
  STATIC_REQUIRE(local_item_slot(local_item_operand(2U, 3U)) == 2U);
  STATIC_REQUIRE(local_item_index(local_item_operand(2U, 3U)) == 3U);

  Module module;
  module.instructions = {{.opcode = Opcode::kLoadLocalItem, .operand = local_item_operand(2U, 3U)},
                         {.opcode = Opcode::kTakeLocalItem, .operand = local_item_operand(0U, kMaxLocalItemIndex)},
                         {.opcode = Opcode::kHalt, .operand = 0}};
  std::ostringstream dump;
  REQUIRE(disassemble_module(module, dump).has_value());
  const auto dumped = dump.str();
  REQUIRE(dumped.find("(local[2] item 3)") != std::string::npos);
  REQUIRE(dumped.find("(local[0] item 2147483647)") != std::string::npos);
}

TEST_CASE("Invalid opcodes stringify safely and disassemble without throwing", "[bytecode][opcodes][stringify]") {
  using namespace fleaux::bytecode;

  constexpr auto kInvalidOpcode = static_cast<Opcode>(9999);
  REQUIRE(stringify_opcode(kInvalidOpcode) == "<invalid-opcode>");

  Module module;
  module.instructions = {{.opcode = kInvalidOpcode, .operand = 0}};

  std::ostringstream dump;
  const auto disassembly = disassemble_module(module, dump);
  REQUIRE(disassembly.has_value());
  REQUIRE(dump.str().find("<invalid-opcode>") != std::string::npos);
}
