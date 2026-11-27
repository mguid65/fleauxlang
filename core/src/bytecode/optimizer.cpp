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

constexpr auto is_jump(const Opcode op) -> bool {
  return op == Opcode::kJump || op == Opcode::kJumpIf || op == Opcode::kJumpIfNot;
}

// Rebuilds one instruction stream while keeping its jumps pointing at the same code. A pass walks the old stream in
// order and, for each position, keeps the instruction, drops a run of instructions, or replaces a run with one new
// instruction. Every old position records where its code now starts, and finish() moves each jump to the new
// position of its target. A run may begin at a jump target but must not contain one after its first instruction,
// since the code a jump lands on would then be gone; replaceable() tells a pass whether that holds.
class StreamRewrite {
public:
  explicit StreamRewrite(const std::vector<Instruction>& old)
      : old_(old), targeted_(old.size() + 1U, false), new_position_(old.size() + 1U, 0U) {
    rewritten_.reserve(old.size());
    for (const Instruction& instruction : old) {
      if (is_jump(instruction.opcode) && instruction.operand >= 0 && static_cast<std::size_t>(instruction.operand) <= old.size()) {
        targeted_[static_cast<std::size_t>(instruction.operand)] = true;
      }
    }
  }

  [[nodiscard]] auto replaceable(const std::size_t first, const std::size_t count) const -> bool {
    for (std::size_t position = first + 1U; position < first + count; ++position) {
      if (targeted_[position]) {
        return false;
      }
    }
    return true;
  }

  void keep(const std::size_t position) {
    new_position_[position] = rewritten_.size();
    rewritten_.push_back(old_[position]);
  }

  void drop(const std::size_t first, const std::size_t count) {
    for (std::size_t position = first; position < first + count; ++position) {
      new_position_[position] = rewritten_.size();
    }
  }

  // The replacement reports the location of the run's last instruction, the operation that can fail (the kElementAt
  // of a fused item read), unless it has one of its own.
  void replace(const std::size_t first, const std::size_t count, Instruction replacement) {
    drop(first, count);
    if (replacement.location == kNoLocation && count != 0U) {
      replacement.location = old_[first + count - 1U].location;
    }
    rewritten_.push_back(replacement);
  }

  [[nodiscard]] auto finish() -> std::vector<Instruction> {
    new_position_[old_.size()] = rewritten_.size();
    for (Instruction& instruction : rewritten_) {
      if (is_jump(instruction.opcode) && instruction.operand >= 0 && static_cast<std::size_t>(instruction.operand) <= old_.size()) {
        instruction.operand = static_cast<std::int64_t>(new_position_[static_cast<std::size_t>(instruction.operand)]);
      }
    }
    return std::move(rewritten_);
  }

private:
  const std::vector<Instruction>& old_;
  std::vector<bool> targeted_;
  std::vector<std::size_t> new_position_;
  std::vector<Instruction> rewritten_;
};

// NoOpEliminationPass helpers

void erase_noops(std::vector<Instruction>& instrs) {
  StreamRewrite rewrite(instrs);
  for (std::size_t position = 0; position < instrs.size(); ++position) {
    if (instrs[position].opcode == Opcode::kNoOp) {
      rewrite.drop(position, 1U);
    } else {
      rewrite.keep(position);
    }
  }
  instrs = rewrite.finish();
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
  for (Instruction& instruction : instrs) {
    if (instruction.opcode == Opcode::kPushConst) {
      instruction.operand = static_cast<std::int64_t>(final_remap[static_cast<std::uint32_t>(instruction.operand)]);
    }
  }
}

// DeadPushEliminationPass helpers

constexpr auto is_side_effect_free_push(const Opcode op) -> bool {
  return op == Opcode::kPushConst || op == Opcode::kLoadLocal || op == Opcode::kTakeLocal;
}

// A side-effect-free push directly followed by kPop does nothing, so both go.
void eliminate_dead_pushes(std::vector<Instruction>& instrs) {
  StreamRewrite rewrite(instrs);
  for (std::size_t position = 0; position < instrs.size();) {
    if (position + 1U < instrs.size() && is_side_effect_free_push(instrs[position].opcode) &&
        instrs[position + 1U].opcode == Opcode::kPop && rewrite.replaceable(position, 2U)) {
      rewrite.drop(position, 2U);
      position += 2U;
      continue;
    }
    rewrite.keep(position);
    ++position;
  }
  instrs = rewrite.finish();
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

auto push_new_constant(std::vector<ConstValue>& constants, const ConstValue& value) -> Instruction {
  const auto const_index = static_cast<std::uint32_t>(constants.size());
  constants.push_back(value);
  return Instruction{.opcode = Opcode::kPushConst, .operand = static_cast<std::int64_t>(const_index)};
}

void fold_constants_in_stream(std::vector<Instruction>& instrs, std::vector<ConstValue>& constants) {
  StreamRewrite rewrite(instrs);
  for (std::size_t index = 0; index < instrs.size();) {
    if (index + 1 < instrs.size() && instrs[index].opcode == Opcode::kPushConst && rewrite.replaceable(index, 2U)) {
      if (const auto unary_fold =
              try_fold_unary(instrs[index + 1].opcode, constants[static_cast<std::size_t>(instrs[index].operand)]);
          unary_fold.has_value()) {
        rewrite.replace(index, 2U, push_new_constant(constants, *unary_fold));
        index += 2;
        continue;
      }
    }

    if (index + 2 < instrs.size() && instrs[index].opcode == Opcode::kPushConst &&
        instrs[index + 1].opcode == Opcode::kPushConst && rewrite.replaceable(index, 3U)) {
      if (const auto binary_fold =
              try_fold_binary(instrs[index + 2].opcode, constants[static_cast<std::size_t>(instrs[index].operand)],
                              constants[static_cast<std::size_t>(instrs[index + 1].operand)]);
          binary_fold.has_value()) {
        rewrite.replace(index, 3U, push_new_constant(constants, *binary_fold));
        index += 3;
        continue;
      }
    }

    rewrite.keep(index);
    ++index;
  }
  instrs = rewrite.finish();
}

void propagate_select_constants_in_stream(std::vector<Instruction>& instrs, const std::vector<ConstValue>& constants) {
  StreamRewrite rewrite(instrs);
  for (std::size_t index = 0; index < instrs.size();) {
    if (index + 3 < instrs.size() && instrs[index].opcode == Opcode::kPushConst &&
        instrs[index + 1].opcode == Opcode::kPushConst && instrs[index + 2].opcode == Opcode::kPushConst &&
        instrs[index + 3].opcode == Opcode::kSelect && rewrite.replaceable(index, 4U)) {
      const auto& [data] = constants[static_cast<std::size_t>(instrs[index].operand)];
      if (const auto* condition = std::get_if<bool>(&data); condition != nullptr) {
        const auto chosen_operand = *condition ? instrs[index + 1].operand : instrs[index + 2].operand;
        rewrite.replace(index, 4U, Instruction{.opcode = Opcode::kPushConst, .operand = chosen_operand});
        index += 4;
        continue;
      }
    }

    rewrite.keep(index);
    ++index;
  }
  instrs = rewrite.finish();
}

// LocalItemReadFusionPass helpers

// The index pushed by `push` when it is an Int64 constant from 0 that fits in the operand of a fused read.
auto local_item_index_constant(const Instruction& push, const std::vector<ConstValue>& constants) -> std::optional<std::uint32_t> {
  if (push.operand < 0 || static_cast<std::size_t>(push.operand) >= constants.size()) {
    return std::nullopt;
  }
  const auto* index = std::get_if<std::int64_t>(&constants[static_cast<std::size_t>(push.operand)].data);
  if (index == nullptr || *index < 0 || *index > std::int64_t{kMaxLocalItemIndex}) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(*index);
}

// The one instruction for the item read that starts at `position`, if one does.
auto fused_local_item_read(const std::vector<Instruction>& instrs, const std::size_t position, const std::vector<ConstValue>& constants)
    -> std::optional<Instruction> {
  if (position + 2U >= instrs.size()) {
    return std::nullopt;
  }
  const Opcode read = instrs[position].opcode;
  const std::int64_t slot = instrs[position].operand;
  if ((read != Opcode::kLoadLocal && read != Opcode::kTakeLocal) || instrs[position + 1U].opcode != Opcode::kPushConst ||
      instrs[position + 2U].opcode != Opcode::kElementAt) {
    return std::nullopt;
  }
  if (slot < 0 || slot > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  const auto item = local_item_index_constant(instrs[position + 1U], constants);
  if (!item.has_value()) {
    return std::nullopt;
  }
  return Instruction{.opcode = read == Opcode::kLoadLocal ? Opcode::kLoadLocalItem : Opcode::kTakeLocalItem,
                     .operand = local_item_operand(static_cast<std::uint32_t>(slot), *item)};
}

void fuse_local_item_reads(std::vector<Instruction>& instrs, const std::vector<ConstValue>& constants) {
  StreamRewrite rewrite(instrs);
  for (std::size_t position = 0; position < instrs.size();) {
    if (const auto fused = fused_local_item_read(instrs, position, constants); fused.has_value() && rewrite.replaceable(position, 3U)) {
      rewrite.replace(position, 3U, *fused);
      position += 3U;
      continue;
    }
    rewrite.keep(position);
    ++position;
  }
  instrs = rewrite.finish();
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

void LocalItemReadFusionPass::run(Module& module) const {
  for_each_stream(module, [&](std::vector<Instruction>& instrs) -> void { fuse_local_item_reads(instrs, module.constants); });
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

  // After the other passes, so that it sees the constants folding made, and before the builtin call sites are rebuilt
  // from the instructions' new positions.
  LocalItemReadFusionPass{}.run(module);
  rebuild_module_builtin_callsites(module);

  return {};
}

}  // namespace fleaux::bytecode
