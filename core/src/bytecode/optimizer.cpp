#include "fleaux/bytecode/optimizer.hpp"

#include "fleaux/bytecode/verifier.hpp"

#include <algorithm>
#include <cmath>
#include <concepts>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>
#include <vector>

#include "fleaux/runtime/integer_arithmetic.hpp"

namespace fleaux::bytecode {

namespace {

// Applies a function to every instruction stream in the module.
template <typename Fn>
void for_each_stream(Module& module, Fn&& fn) {
  fn(module.instructions);
  for (auto& fn_def : module.functions) {
    fn(fn_def.instructions);
  }
}

// NoOpEliminationPass helpers

void erase_noops(std::vector<Instruction>& instrs) {
  std::erase_if(instrs, [](const Instruction& i) -> bool { return i.opcode == Opcode::kNoOp; });
}

// ConstantPoolDeduplicationPass helpers

// Equality comparison for ConstValue. std::variant operator== compares the
// active alternative first, then the held value, which is correct here for all
// pool types (int64, uint64, double, bool, string, monostate).
auto const_values_equal(const ConstValue& a, const ConstValue& b) -> bool { return a.data == b.data; }

// Build a remap table: for each index in the original pool, find the lowest
// equivalent index (the canonical entry). Returns a vector where remap[i] is
// the canonical index for the entry that was at position i.
auto build_const_remap(const std::vector<ConstValue>& pool) -> std::vector<std::uint32_t> {
  std::vector<std::uint32_t> remap(pool.size());
  for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(pool.size()); ++i) {
    // Search for an earlier equal entry.
    std::uint32_t canonical = i;
    for (std::uint32_t j = 0; j < i; ++j) {
      if (const_values_equal(pool[j], pool[i])) {
        canonical = j;
        break;
      }
    }
    remap[i] = canonical;
  }
  return remap;
}

// Rebuild the deduplicated pool and produce a second remap from old index to
// new (compacted) index. The first remap already points every entry to its
// canonical old-index; now we compact out the duplicates.
auto compact_pool(const std::vector<ConstValue>& pool, const std::vector<std::uint32_t>& canonical_remap)
    -> std::pair<std::vector<ConstValue>, std::vector<std::uint32_t>> {
  // new_index_of[old_canonical] = new compacted index; only set for entries
  // that are their own canonical (i.e. canonical_remap[i] == i).
  std::vector<std::uint32_t> new_index_of(pool.size(), 0);
  std::vector<ConstValue> new_pool;
  for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(pool.size()); ++i) {
    if (canonical_remap[i] == i) {
      new_index_of[i] = static_cast<std::uint32_t>(new_pool.size());
      new_pool.push_back(pool[i]);
    }
  }
  // Final remap: old index -> new compacted index.
  std::vector<std::uint32_t> final_remap(pool.size());
  for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(pool.size()); ++i) {
    final_remap[i] = new_index_of[canonical_remap[i]];
  }
  return {std::move(new_pool), std::move(final_remap)};
}

void rewrite_push_const_operands(std::vector<Instruction>& instrs, const std::vector<std::uint32_t>& final_remap) {
  for (auto& [opcode, operand] : instrs) {
    if (opcode == Opcode::kPushConst) {
      operand = static_cast<std::int64_t>(final_remap[static_cast<std::uint32_t>(operand)]);
    }
  }
}

// DeadPushEliminationPass helpers

constexpr auto is_side_effect_free_push(const Opcode op) -> bool {
  return op == Opcode::kPushConst || op == Opcode::kLoadLocal;
}

void eliminate_dead_pushes(std::vector<Instruction>& instrs) {
  // Mark indices to remove, then erase in one pass.
  // We scan forward: if instrs[i] is a side-effect-free push and instrs[i+1]
  // is kPop, both are dead. Skip i+1 after marking both.
  std::vector<bool> dead(instrs.size(), false);
  for (std::size_t i = 0; i + 1 < instrs.size(); ++i) {
    if (dead[i]) {
      continue;
    }
    if (is_side_effect_free_push(instrs[i].opcode) && instrs[i + 1].opcode == Opcode::kPop) {
      dead[i] = true;
      dead[i + 1] = true;
      ++i;  // skip the kPop we just marked
    }
  }
  std::size_t write = 0;
  for (std::size_t read = 0; read < instrs.size(); ++read) {
    if (!dead[read]) {
      instrs[write++] = instrs[read];
    }
  }
  instrs.resize(write);
}

// ConstantFoldingPass helpers

auto try_fold_unary(const Opcode opcode, const ConstValue& input) -> std::optional<ConstValue> {
  if (opcode == Opcode::kNot) {
    if (const auto* value = std::get_if<bool>(&input.data); value != nullptr) {
      return ConstValue{.data = !*value};
    }
    return std::nullopt;
  }

  if (opcode == Opcode::kNeg) {
    if (const auto* value = std::get_if<std::int64_t>(&input.data); value != nullptr) {
      if (*value == std::numeric_limits<std::int64_t>::min()) {
        // Negating INT64_MIN overflows; preserve runtime behavior by skipping fold.
        return std::nullopt;
      }
      return ConstValue{.data = -*value};
    }
    if (const auto* value = std::get_if<double>(&input.data); value != nullptr) {
      return ConstValue{.data = -*value};
    }
    return std::nullopt;
  }

  return std::nullopt;
}

// Folds integer arithmetic exactly like the runtime does. Returns std::nullopt when the runtime would fail (overflow,
// division by zero, negative exponent) so the failure is still reported when the program runs.
template <std::integral T>
auto try_fold_integer_arithmetic(const Opcode opcode, const T lhs, const T rhs) -> std::optional<T> {
  switch (opcode) {
    case Opcode::kAdd:
      return runtime::integer::checked_add(lhs, rhs);
    case Opcode::kSub:
      return runtime::integer::checked_sub(lhs, rhs);
    case Opcode::kMul:
      return runtime::integer::checked_mul(lhs, rhs);
    case Opcode::kDiv:
      if (rhs == 0) {
        return std::nullopt;
      }
      return runtime::integer::checked_div(lhs, rhs);
    case Opcode::kMod:
      if (rhs == 0) {
        return std::nullopt;
      }
      return runtime::integer::remainder(lhs, rhs);
    case Opcode::kPow:
      if (rhs < 0) {
        return std::nullopt;
      }
      return runtime::integer::checked_pow(lhs, rhs);
    default:
      return std::nullopt;
  }
}

auto try_fold_float_arithmetic(const Opcode opcode, const double lhs, const double rhs) -> std::optional<double> {
  switch (opcode) {
    case Opcode::kAdd:
      return lhs + rhs;
    case Opcode::kSub:
      return lhs - rhs;
    case Opcode::kMul:
      return lhs * rhs;
    case Opcode::kDiv:
      return lhs / rhs;
    case Opcode::kMod:
      return std::fmod(lhs, rhs);
    case Opcode::kPow:
      return std::pow(lhs, rhs);
    default:
      return std::nullopt;
  }
}

template <typename T>
auto try_fold_ordering(const Opcode opcode, const T lhs, const T rhs) -> std::optional<ConstValue> {
  switch (opcode) {
    case Opcode::kCmpLt:
      return ConstValue{.data = (lhs < rhs)};
    case Opcode::kCmpGt:
      return ConstValue{.data = (lhs > rhs)};
    case Opcode::kCmpLe:
      return ConstValue{.data = (lhs <= rhs)};
    case Opcode::kCmpGe:
      return ConstValue{.data = (lhs >= rhs)};
    default:
      return std::nullopt;
  }
}

// Folds a binary operation on two constants of the same type. Constants of different numeric types are left alone;
// the type checker rejects them, and the runtime reports them if they arrive through Any.
template <typename T>
auto try_fold_same_type_binary(const Opcode opcode, const T lhs, const T rhs) -> std::optional<ConstValue> {
  if (auto ordering = try_fold_ordering(opcode, lhs, rhs); ordering.has_value()) {
    return ordering;
  }
  if constexpr (std::is_same_v<T, double>) {
    if (const auto folded = try_fold_float_arithmetic(opcode, lhs, rhs); folded.has_value()) {
      return ConstValue{.data = *folded};
    }
  } else {
    if (const auto folded = try_fold_integer_arithmetic(opcode, lhs, rhs); folded.has_value()) {
      return ConstValue{.data = *folded};
    }
  }
  return std::nullopt;
}

auto try_fold_binary(const Opcode opcode, const ConstValue& left, const ConstValue& right)
    -> std::optional<ConstValue> {
  if (opcode == Opcode::kCmpEq) {
    return ConstValue{.data = (left.data == right.data)};
  }
  if (opcode == Opcode::kCmpNe) {
    return ConstValue{.data = (left.data != right.data)};
  }

  if (opcode == Opcode::kAnd || opcode == Opcode::kOr) {
    const auto* lhs = std::get_if<bool>(&left.data);
    const auto* rhs = std::get_if<bool>(&right.data);
    if (lhs == nullptr || rhs == nullptr) {
      return std::nullopt;
    }
    return ConstValue{.data = (opcode == Opcode::kAnd ? (*lhs && *rhs) : (*lhs || *rhs))};
  }

  if (const auto* lhs = std::get_if<std::int64_t>(&left.data), *rhs = std::get_if<std::int64_t>(&right.data);
      lhs != nullptr && rhs != nullptr) {
    return try_fold_same_type_binary(opcode, *lhs, *rhs);
  }
  if (const auto* lhs = std::get_if<std::uint64_t>(&left.data), *rhs = std::get_if<std::uint64_t>(&right.data);
      lhs != nullptr && rhs != nullptr) {
    return try_fold_same_type_binary(opcode, *lhs, *rhs);
  }
  if (const auto* lhs = std::get_if<double>(&left.data), *rhs = std::get_if<double>(&right.data);
      lhs != nullptr && rhs != nullptr) {
    return try_fold_same_type_binary(opcode, *lhs, *rhs);
  }

  return std::nullopt;
}

void fold_constants_in_stream(std::vector<Instruction>& instrs, std::vector<ConstValue>& constants) {
  std::vector<Instruction> folded;
  folded.reserve(instrs.size());

  for (std::size_t index = 0; index < instrs.size();) {
    if (index + 1 < instrs.size() && instrs[index].opcode == Opcode::kPushConst) {
      const auto unary_fold =
          try_fold_unary(instrs[index + 1].opcode, constants[static_cast<std::size_t>(instrs[index].operand)]);
      if (unary_fold.has_value()) {
        const auto const_index = static_cast<std::uint32_t>(constants.size());
        constants.push_back(*unary_fold);
        folded.push_back(Instruction{.opcode = Opcode::kPushConst, .operand = static_cast<std::int64_t>(const_index)});
        index += 2;
        continue;
      }
    }

    if (index + 2 < instrs.size() && instrs[index].opcode == Opcode::kPushConst &&
        instrs[index + 1].opcode == Opcode::kPushConst) {
      const auto binary_fold =
          try_fold_binary(instrs[index + 2].opcode, constants[static_cast<std::size_t>(instrs[index].operand)],
                          constants[static_cast<std::size_t>(instrs[index + 1].operand)]);
      if (binary_fold.has_value()) {
        const auto const_index = static_cast<std::uint32_t>(constants.size());
        constants.push_back(*binary_fold);
        folded.push_back(Instruction{.opcode = Opcode::kPushConst, .operand = static_cast<std::int64_t>(const_index)});
        index += 3;
        continue;
      }
    }

    folded.push_back(instrs[index]);
    ++index;
  }

  instrs = std::move(folded);
}

void propagate_select_constants_in_stream(std::vector<Instruction>& instrs, const std::vector<ConstValue>& constants) {
  std::vector<Instruction> propagated;
  propagated.reserve(instrs.size());

  for (std::size_t index = 0; index < instrs.size();) {
    if (index + 3 < instrs.size() && instrs[index].opcode == Opcode::kPushConst &&
        instrs[index + 1].opcode == Opcode::kPushConst && instrs[index + 2].opcode == Opcode::kPushConst &&
        instrs[index + 3].opcode == Opcode::kSelect) {
      const auto& [data] = constants[static_cast<std::size_t>(instrs[index].operand)];
      if (const auto* condition = std::get_if<bool>(&data); condition != nullptr) {
        const auto chosen_operand = *condition ? instrs[index + 1].operand : instrs[index + 2].operand;
        propagated.push_back(Instruction{.opcode = Opcode::kPushConst, .operand = chosen_operand});
        index += 4;
        continue;
      }
    }

    propagated.push_back(instrs[index]);
    ++index;
  }

  instrs = std::move(propagated);
}

}  // namespace

// Pass implementations

void NoOpEliminationPass::run(Module& module) const { for_each_stream(module, erase_noops); }

void ConstantPoolDeduplicationPass::run(Module& module) const {
  if (module.constants.size() <= 1) {
    return;
  }

  const auto canonical_remap = build_const_remap(module.constants);

  // Check if anything is actually duplicated before doing extra work.
  bool any_duplicate = false;
  for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(canonical_remap.size()); ++i) {
    if (canonical_remap[i] != i) {
      any_duplicate = true;
      break;
    }
  }
  if (!any_duplicate) {
    return;
  }

  auto [new_pool, final_remap] = compact_pool(module.constants, canonical_remap);
  module.constants = std::move(new_pool);
  for_each_stream(module,
                  [&](std::vector<Instruction>& instrs) -> void { rewrite_push_const_operands(instrs, final_remap); });
}

void DeadPushEliminationPass::run(Module& module) const { for_each_stream(module, eliminate_dead_pushes); }

void ConstantFoldingPass::run(Module& module) const {
  for_each_stream(
      module, [&](std::vector<Instruction>& instrs) -> void { fold_constants_in_stream(instrs, module.constants); });
}

void ConstantPropagationSelectPass::run(Module& module) const {
  for_each_stream(module, [&](std::vector<Instruction>& instrs) -> void {
    propagate_select_constants_in_stream(instrs, module.constants);
  });
}

// BytecodeOptimizer

auto BytecodeOptimizer::run_always_on_passes(Module& module) const -> OptimizeResult {
  NoOpEliminationPass{}.run(module);
  ConstantPoolDeduplicationPass{}.run(module);
  DeadPushEliminationPass{}.run(module);
  return {};
}

auto BytecodeOptimizer::run_extended_passes(Module& module) const -> OptimizeResult {
  ConstantFoldingPass{}.run(module);
  ConstantPropagationSelectPass{}.run(module);
  // Re-run cheap hygiene passes after folding introduces new constants/stack patterns.
  ConstantPoolDeduplicationPass{}.run(module);
  DeadPushEliminationPass{}.run(module);
  return {};
}

auto BytecodeOptimizer::optimize(Module& module, const OptimizerConfig& config) const -> OptimizeResult {
  if (auto result = run_always_on_passes(module); !result) {
    return result;
  }

  if (config.mode == OptimizationMode::kExtended) {
    if (auto result = run_extended_passes(module); !result) {
      return result;
    }
  }

  rebuild_module_builtin_callsites(module);

  return {};
}

}  // namespace fleaux::bytecode
