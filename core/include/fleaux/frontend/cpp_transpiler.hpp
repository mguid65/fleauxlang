#pragma once

#include <filesystem>
#include <string>

#include <tl/expected.hpp>

#include "fleaux/frontend/ast.hpp"

namespace fleaux::frontend::cpp_transpile {

struct TranspileError {
  std::string message;
  std::optional<std::string> hint;
  std::optional<diag::SourceSpan> span;
};

using TranspileResult = tl::expected<std::filesystem::path, TranspileError>;

class FleauxCppTranspiler {
public:
  [[nodiscard]] auto process(const std::filesystem::path& source_file) const -> TranspileResult;

private:
  [[nodiscard]] auto emit_cpp(const ir::IRProgram& program, const std::string& module_name) const -> std::string;
};

}  // namespace fleaux::frontend::cpp_transpile
