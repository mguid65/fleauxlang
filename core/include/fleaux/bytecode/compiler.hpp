#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <tl/expected.hpp>

#include "fleaux/bytecode/module.hpp"
#include "fleaux/frontend/ast.hpp"

namespace fleaux::bytecode {

struct CompileError {
  std::string message;
};

struct CompileOptions {
  std::optional<std::filesystem::path> source_path;
  std::optional<std::string> source_text;
  std::optional<std::string> module_name;
  std::vector<const Module*> imported_modules;
};

using CompileResult = tl::expected<Module, CompileError>;

class BytecodeCompiler {
public:
  [[nodiscard]] auto compile(const frontend::ir::IRProgram& program,
                             const CompileOptions& options = CompileOptions{}) const -> CompileResult;
};

}  // namespace fleaux::bytecode
