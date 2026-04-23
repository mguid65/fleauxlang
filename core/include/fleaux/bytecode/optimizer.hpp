#pragma once

#include <string>

#include <tl/expected.hpp>

#include "fleaux/bytecode/module.hpp"

namespace fleaux::bytecode {

// kBaseline: always-on, semantics-preserving hygiene passes (run regardless of CLI flags).
// kExtended: baseline + optional performance/aggressive passes (enabled by --optimize).
enum class OptimizationMode {
  kBaseline = 0,
  kExtended = 1,
};

struct OptimizerConfig {
  OptimizationMode mode = OptimizationMode::kBaseline;
};

struct OptimizeError {
  std::string message;
};

using OptimizeResult = tl::expected<void, OptimizeError>;

// Tier A pass: removes all kNoOp instructions from every instruction stream in
// the module. Semantics-preserving because the VM already treats kNoOp as a
// no-op; removing them only shrinks the bytecode.
struct NoOpEliminationPass {
  void run(Module& module) const;
};

// Tier A pass: merges duplicate entries in Module::constants and rewrites all
// kPushConst operands to point to the canonical deduplicated entry. The
// compiler does not deduplicate on emission, so repeated literals (e.g. 0,
// 1.0, "") accumulate multiple pool entries.
struct ConstantPoolDeduplicationPass {
  void run(Module& module) const;
};

// Tier A pass: removes {kPushConst | kLoadLocal} immediately followed by kPop.
// The value is pushed and immediately discarded, so both instructions are dead.
// Semantics-safe because neither push opcode has side effects.
struct DeadPushEliminationPass {
  void run(Module& module) const;
};

// Tier B pass: folds literal constant opcode sequences (e.g. push/push/op)
// into a single kPushConst result when semantics are unambiguous and safe.
struct ConstantFoldingPass {
  void run(Module& module) const;
};

// Tier B pass: when kSelect is fed by a literal bool condition and literal
// candidate values, replace the full sequence with the selected literal push.
struct ConstantPropagationSelectPass {
  void run(Module& module) const;
};

class BytecodeOptimizer {
public:
  [[nodiscard]] auto optimize(Module& module, const OptimizerConfig& config = OptimizerConfig{}) const
      -> OptimizeResult;

private:
  // Always-on passes: low-risk, correctness/simplification only.
  [[nodiscard]] auto run_always_on_passes(Module& module) const -> OptimizeResult;
  // Extended passes: opt-in performance transforms, enabled by kExtended.
  [[nodiscard]] auto run_extended_passes(Module& module) const -> OptimizeResult;
};

}  // namespace fleaux::bytecode
