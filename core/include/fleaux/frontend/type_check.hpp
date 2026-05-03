#pragma once

#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <tl/expected.hpp>

#include "fleaux/frontend/ast.hpp"

namespace fleaux::frontend::type_check {

struct AnalysisError {
  std::string message;
  std::optional<std::string> hint;
  std::optional<diag::SourceSpan> span;
};

using AnalysisResult = tl::expected<ir::IRProgram, AnalysisError>;

[[nodiscard]] auto analyze_program(const ir::IRProgram& program,
                                   const std::unordered_set<std::string>& imported_symbols = {},
                                   const std::vector<ir::IRLet>& imported_typed_lets = {},
                                   const std::vector<ir::IRTypeDecl>& imported_type_decls = {},
                                   const std::vector<ir::IRAliasDecl>& imported_alias_decls = {}) -> AnalysisResult;

}  // namespace fleaux::frontend::type_check
