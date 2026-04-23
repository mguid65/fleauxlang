#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include "fleaux/frontend/ast.hpp"

namespace fleaux::bytecode {

struct AutoValueRefAnalysisOptions {
  bool enabled{false};
  std::size_t byte_cutoff{256};
};

using AutoValueRefParamSlots = std::unordered_map<std::string, std::unordered_set<std::uint32_t>>;

[[nodiscard]] auto analyze_auto_value_ref_params(const frontend::ir::IRProgram& program,
                                                  const AutoValueRefAnalysisOptions& options)
    -> AutoValueRefParamSlots;

}  // namespace fleaux::bytecode

