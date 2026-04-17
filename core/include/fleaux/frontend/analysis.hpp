#pragma once

#include "fleaux/frontend/lowering.hpp"

namespace fleaux::frontend::analysis {

using AnalysisError = lowering::LoweringError;
using AnalysisResult = lowering::LoweringResult;
using Analyzer = lowering::Lowerer;

}  // namespace fleaux::frontend::analysis

