#pragma once

#include <cstddef>
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
  bool enable_value_ref_gate{false};
  bool enable_auto_value_ref{false};
  std::size_t value_ref_byte_cutoff{256};
};

using CompileResult = tl::expected<Module, CompileError>;

class BytecodeCompiler {
public:
  [[nodiscard]] auto compile(const frontend::ir::IRProgram& program,
                             const CompileOptions& options = CompileOptions{}) const -> CompileResult;
};

}  // namespace fleaux::bytecode
