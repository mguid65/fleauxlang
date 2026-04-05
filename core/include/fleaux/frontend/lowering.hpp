#pragma once

#include <string>

#include <tl/expected.hpp>

#include "fleaux/frontend/ast.hpp"

namespace fleaux::frontend::lowering {

struct LoweringError {
  std::string message;
  std::optional<std::string> hint;
  std::optional<diag::SourceSpan> span;
};

using LoweringResult = tl::expected<ir::IRProgram, LoweringError>;

class Lowerer {
 public:
  LoweringResult lower(const model::Program& program) const;
};

}  // namespace fleaux::frontend::lowering

