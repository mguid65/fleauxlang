#include "fleaux/bytecode/compiler.hpp"

namespace fleaux::bytecode {

CompileResult BytecodeCompiler::compile(const frontend::ir::IRProgram& program) const {
  (void)program;

  Module module;
  module.instructions.push_back(Instruction{Opcode::kHalt, 0});
  return module;
}

}  // namespace fleaux::bytecode

