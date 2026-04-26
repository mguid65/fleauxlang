#include "fleaux/bytecode/optimizer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace fleaux::bytecode {

namespace {

// Applies a function to every instruction stream in the module.
template <typename Fn>
void for_each_stream(Module& module, Fn&& fn) {
  fn(module.instructions);
  for (auto& fn_def : module.functions) { fn(fn_def.instructions); }
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
    if (dead[i]) { continue; }
    if (is_side_effect_free_push(instrs[i].opcode) && instrs[i + 1].opcode == Opcode::kPop) {
      dead[i] = true;
      dead[i + 1] = true;
      ++i;  // skip the kPop we just marked
    }
  }
  std::size_t write = 0;
  for (std::size_t read = 0; read < instrs.size(); ++read) {
    if (!dead[read]) { instrs[write++] = instrs[read]; }
  }
  instrs.resize(write);
}

// ConstantFoldingPass helpers

auto try_fold_unary(const Opcode opcode, const ConstValue& input) -> std::optional<ConstValue> {
  if (opcode == Opcode::kNot) {
    if (const auto* value = std::get_if<bool>(&input.data); value != nullptr) { return ConstValue{.data = !*value}; }
    return std::nullopt;
  }

  if (opcode == Opcode::kNeg) {
    if (const auto* value = std::get_if<std::int64_t>(&input.data); value != nullptr) {
      if (*value == std::numeric_limits<std::int64_t>::min()) {
        // Negating INT64_MIN overflows; preserve runtime behavior by skipping fold.
        return std::nullopt;
      }
      return ConstValue{.data = static_cast<std::int64_t>(-*value)};
    }
    if (const auto* value = std::get_if<double>(&input.data); value != nullptr) { return ConstValue{.data = -*value}; }
    return std::nullopt;
  }

  return std::nullopt;
}

auto try_fold_binary(const Opcode opcode, const ConstValue& left, const ConstValue& right)
    -> std::optional<ConstValue> {
  const auto is_uint = [](const ConstValue& value) -> bool {
    return std::holds_alternative<std::uint64_t>(value.data);
  };
  const auto is_int = [](const ConstValue& value) -> bool { return std::holds_alternative<std::int64_t>(value.data); };
  const auto is_float = [](const ConstValue& value) -> bool { return std::holds_alternative<double>(value.data); };
  const auto is_numeric = [&](const ConstValue& value) -> bool {
    return is_int(value) || is_uint(value) || is_float(value);
  };
  const auto to_double = [](const ConstValue& value) -> std::optional<double> {
    if (const auto* signed_value = std::get_if<std::int64_t>(&value.data); signed_value != nullptr) {
      return static_cast<double>(*signed_value);
    }
    if (const auto* unsigned_value = std::get_if<std::uint64_t>(&value.data); unsigned_value != nullptr) {
      return static_cast<double>(*unsigned_value);
    }
    if (const auto* float_value = std::get_if<double>(&value.data); float_value != nullptr) { return *float_value; }
    return std::nullopt;
  };
  const auto num_result = [](const double value, const bool prefer_unsigned = false,
                             const bool prefer_float = false) -> ConstValue {
    if (prefer_float) { return ConstValue{.data = value}; }
    if (value == std::floor(value) && std::isfinite(value)) {
      if (prefer_unsigned && value >= 0.0 && value <= static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        return ConstValue{.data = static_cast<std::uint64_t>(value)};
      }
      if (value >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
          value <= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return ConstValue{.data = static_cast<std::int64_t>(value)};
      }
      if (!prefer_unsigned && value >= 0.0 && value <= static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
        return ConstValue{.data = static_cast<std::uint64_t>(value)};
      }
    }
    return ConstValue{.data = value};
  };

  if (opcode == Opcode::kCmpEq) { return ConstValue{.data = (left.data == right.data)}; }
  if (opcode == Opcode::kCmpNe) { return ConstValue{.data = (left.data != right.data)}; }

  if (opcode == Opcode::kAnd || opcode == Opcode::kOr) {
    const auto* lhs = std::get_if<bool>(&left.data);
    const auto* rhs = std::get_if<bool>(&right.data);
    if (lhs == nullptr || rhs == nullptr) { return std::nullopt; }
    return ConstValue{.data = (opcode == Opcode::kAnd ? (*lhs && *rhs) : (*lhs || *rhs))};
  }

  if (opcode == Opcode::kDiv || opcode == Opcode::kMod || opcode == Opcode::kPow) {
    if (!is_numeric(left) || !is_numeric(right)) { return std::nullopt; }

    // Runtime rejects mixed Int64/UInt64 for div/mod (but not for pow).
    const bool mixed_signed_unsigned_integer_pair =
        ((is_int(left) && is_uint(right)) || (is_uint(left) && is_int(right)));
    if (mixed_signed_unsigned_integer_pair && (opcode == Opcode::kDiv || opcode == Opcode::kMod)) {
      return std::nullopt;
    }

    const auto lhs = to_double(left);
    const auto rhs = to_double(right);
    if (!lhs.has_value() || !rhs.has_value()) { return std::nullopt; }

    if (opcode == Opcode::kDiv) {
      const bool prefer_unsigned = is_uint(left) && is_uint(right);
      const bool prefer_float = std::holds_alternative<double>(left.data) || std::holds_alternative<double>(right.data);
      return num_result(*lhs / *rhs, prefer_unsigned, prefer_float);
    }
    if (opcode == Opcode::kMod) {
      const bool prefer_unsigned = is_uint(left) && is_uint(right);
      const bool prefer_float = std::holds_alternative<double>(left.data) || std::holds_alternative<double>(right.data);
      return num_result(std::fmod(*lhs, *rhs), prefer_unsigned, prefer_float);
    }
    // pow uses default runtime numeric-result preference.
    return num_result(std::pow(*lhs, *rhs), false,
                      std::holds_alternative<double>(left.data) || std::holds_alternative<double>(right.data));
  }

  const auto* lhs_i64 = std::get_if<std::int64_t>(&left.data);
  const auto* rhs_i64 = std::get_if<std::int64_t>(&right.data);
  if (lhs_i64 != nullptr && rhs_i64 != nullptr) {
    switch (opcode) {
      case Opcode::kAdd: {
        const bool overflow = (*rhs_i64 > 0 && *lhs_i64 > std::numeric_limits<std::int64_t>::max() - *rhs_i64) ||
                              (*rhs_i64 < 0 && *lhs_i64 < std::numeric_limits<std::int64_t>::min() - *rhs_i64);
        if (overflow) { return std::nullopt; }
        return ConstValue{.data = static_cast<std::int64_t>(*lhs_i64 + *rhs_i64)};
      }
      case Opcode::kSub: {
        const bool overflow = (*rhs_i64 < 0 && *lhs_i64 > std::numeric_limits<std::int64_t>::max() + *rhs_i64) ||
                              (*rhs_i64 > 0 && *lhs_i64 < std::numeric_limits<std::int64_t>::min() + *rhs_i64);
        if (overflow) { return std::nullopt; }
        return ConstValue{.data = static_cast<std::int64_t>(*lhs_i64 - *rhs_i64)};
      }
      case Opcode::kMul: {
        if (*lhs_i64 == 0 || *rhs_i64 == 0) { return ConstValue{.data = static_cast<std::int64_t>(0)}; }
        const auto result = static_cast<long double>(*lhs_i64) * static_cast<long double>(*rhs_i64);
        if (result > static_cast<long double>(std::numeric_limits<std::int64_t>::max()) ||
            result < static_cast<long double>(std::numeric_limits<std::int64_t>::min())) {
          return std::nullopt;
        }
        return ConstValue{.data = static_cast<std::int64_t>(*lhs_i64 * *rhs_i64)};
      }
      case Opcode::kCmpLt:
        return ConstValue{.data = (*lhs_i64 < *rhs_i64)};
      case Opcode::kCmpGt:
        return ConstValue{.data = (*lhs_i64 > *rhs_i64)};
      case Opcode::kCmpLe:
        return ConstValue{.data = (*lhs_i64 <= *rhs_i64)};
      case Opcode::kCmpGe:
        return ConstValue{.data = (*lhs_i64 >= *rhs_i64)};
      default:
        break;
    }
  }

  const auto* lhs_double = std::get_if<double>(&left.data);
  if (const auto* rhs_double = std::get_if<double>(&right.data); lhs_double != nullptr && rhs_double != nullptr) {
    switch (opcode) {
      case Opcode::kAdd:
        return ConstValue{.data = (*lhs_double + *rhs_double)};
      case Opcode::kSub:
        return ConstValue{.data = (*lhs_double - *rhs_double)};
      case Opcode::kMul:
        return ConstValue{.data = (*lhs_double * *rhs_double)};
      case Opcode::kCmpLt:
        return ConstValue{.data = (*lhs_double < *rhs_double)};
      case Opcode::kCmpGt:
        return ConstValue{.data = (*lhs_double > *rhs_double)};
      case Opcode::kCmpLe:
        return ConstValue{.data = (*lhs_double <= *rhs_double)};
      case Opcode::kCmpGe:
        return ConstValue{.data = (*lhs_double >= *rhs_double)};
      default:
        break;
    }
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
      const auto& cond = constants[static_cast<std::size_t>(instrs[index].operand)];
      if (const auto* condition = std::get_if<bool>(&cond.data); condition != nullptr) {
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
  if (module.constants.size() <= 1) { return; }

  const auto canonical_remap = build_const_remap(module.constants);

  // Check if anything is actually duplicated before doing extra work.
  bool any_duplicate = false;
  for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(canonical_remap.size()); ++i) {
    if (canonical_remap[i] != i) {
      any_duplicate = true;
      break;
    }
  }
  if (!any_duplicate) { return; }

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
  if (auto result = run_always_on_passes(module); !result) { return result; }

  if (config.mode == OptimizationMode::kExtended) {
    if (auto result = run_extended_passes(module); !result) { return result; }
  }

  return {};
}

}  // namespace fleaux::bytecode
