#pragma once

#include <vector>

#include <unordered_set>

#include <tl/expected.hpp>

#include "fleaux/frontend/type_check.hpp"

namespace fleaux::frontend::type_system {

class Checker {
public:
  [[nodiscard]] auto analyze(const ir::IRProgram& program, const std::unordered_set<std::string>& imported_symbols,
                             const std::vector<ir::IRLet>& imported_typed_lets = {}) const
      -> tl::expected<ir::IRProgram, type_check::AnalysisError>;
};

}  // namespace fleaux::frontend::type_system
