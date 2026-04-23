#pragma once

#include "fleaux/frontend/lowering.hpp"
#include "fleaux/frontend/type_check.hpp"

namespace fleaux::frontend::analysis {

using AnalysisError = type_check::AnalysisError;
using AnalysisResult = type_check::AnalysisResult;

class Analyzer {
public:
  [[nodiscard]] auto lower_only(const model::Program& program) const -> lowering::LoweringResult;
  [[nodiscard]] auto analyze(const model::Program& program) const -> AnalysisResult;
};

}  // namespace fleaux::frontend::analysis

