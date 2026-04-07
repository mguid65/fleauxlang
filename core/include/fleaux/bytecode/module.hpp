#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "fleaux/bytecode/opcode.hpp"

namespace fleaux::bytecode {

struct Instruction {
  Opcode opcode = Opcode::kNoOp;
  std::int64_t operand = 0;
};

// A typed constant stored in the constant pool.
// All literal constants (including int64_t) are loaded via kPushConst.
struct ConstValue {
  std::variant<std::int64_t, double, bool, std::string, std::monostate> data;
};

// A compiled user-defined function.
struct FunctionDef {
  std::string name;           // qualified name (e.g. "MyMath.Square")
  std::uint32_t arity = 0;   // number of local parameter slots
  std::vector<Instruction> instructions;
};

struct Module {
  // Top-level instruction stream (executed on program start).
  std::vector<Instruction> instructions;

  // Constant pool: indexed by kPushConst operand.
  std::vector<ConstValue> constants;

  // Indexed stdlib builtin names: indexed by kCallBuiltin operand.
  std::vector<std::string> builtin_names;

  // User-defined functions: indexed by kCallUserFunc operand.
  std::vector<FunctionDef> functions;
};

}  // namespace fleaux::bytecode

