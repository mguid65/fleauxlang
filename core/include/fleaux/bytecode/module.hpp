#pragma once

#include <cstdint>
#include <vector>

#include "fleaux/bytecode/opcode.hpp"

namespace fleaux::bytecode {

struct Instruction {
  Opcode opcode = Opcode::kNoOp;
  std::int64_t operand = 0;
};

struct Module {
  std::vector<Instruction> instructions;
};

}  // namespace fleaux::bytecode

