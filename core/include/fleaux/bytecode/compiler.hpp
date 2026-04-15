#pragma once

#include <string>

#include <tl/expected.hpp>

#include "fleaux/bytecode/module.hpp"
#include "fleaux/frontend/ast.hpp"

namespace fleaux::bytecode {

struct CompileError {
  std::string message;
};

using CompileResult = tl::expected<Module, CompileError>;

class BytecodeCompiler {
public:
  [[nodiscard]] auto compile(const frontend::ir::IRProgram& program) const -> CompileResult;
};

}  // namespace fleaux::bytecode
