#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <tl/expected.hpp>

#include "fleaux/bytecode/module.hpp"
#include "fleaux/frontend/ast.hpp"

namespace fleaux::bytecode {

struct CompileError {
  std::string message{};
};

struct CompileOptions {
  std::optional<std::filesystem::path> source_path{std::nullopt};
  std::optional<std::string> source_text{std::nullopt};
  std::optional<std::string> module_name{std::nullopt};
  std::vector<Module> imported_modules{};
  // Calls of small named functions compile to the function's body in place of the call. Off, every call is a call,
  // which the tests of the VM's calls rely on.
  bool inline_functions{true};
};

using CompileResult = tl::expected<Module, CompileError>;

class BytecodeCompiler {
public:
  [[nodiscard]] auto compile(const frontend::ir::IRProgram& program,
                             const CompileOptions& options = CompileOptions{}) const -> CompileResult;
};

}  // namespace fleaux::bytecode
