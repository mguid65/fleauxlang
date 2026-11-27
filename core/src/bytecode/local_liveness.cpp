#include "fleaux/bytecode/local_liveness.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace fleaux::bytecode {

namespace {

// What one instruction does to local slots.
struct LocalEffect {
  std::array<std::size_t, 2> reads{};
  std::size_t read_count{0};
  std::optional<std::size_t> write;
};

auto slot_of(const Instruction& instruction) -> std::size_t { return static_cast<std::size_t>(instruction.operand); }

// Every opcode is listed, so an opcode added later does not compile until its effect on locals is stated here. A
// negative slot is no slot: the VM refuses the instruction.
auto local_effect(const Instruction& instruction) -> LocalEffect {
  if (instruction.operand < 0) {
    return LocalEffect{};
  }
  switch (instruction.opcode) {
    case Opcode::kLoadLocal:
    case Opcode::kTakeLocal:
      return LocalEffect{.reads = {slot_of(instruction), 0U}, .read_count = 1U, .write = std::nullopt};
    case Opcode::kLoadLocalItem:
    case Opcode::kTakeLocalItem:
      return LocalEffect{.reads = {local_item_slot(instruction.operand), 0U}, .read_count = 1U, .write = std::nullopt};
    case Opcode::kStoreLocal:
    case Opcode::kMarkStack:
      return LocalEffect{.reads = {}, .read_count = 0U, .write = slot_of(instruction)};
    case Opcode::kBuildTupleSinceMark:
      return LocalEffect{.reads = {slot_of(instruction), 0U}, .read_count = 1U, .write = std::nullopt};
    case Opcode::kIterate:
      // The tuple is in the operand slot, the index in the next one, which kIterate reads and advances, and the item in
      // the one after, which it writes. The index's write is not stated: kIterate reads it first, so it stays live.
      return LocalEffect{.reads = {slot_of(instruction), slot_of(instruction) + 1U},
                         .read_count = 2U,
                         .write = slot_of(instruction) + 2U};
    case Opcode::kNoOp:
    case Opcode::kPushConst:
    case Opcode::kPop:
    case Opcode::kDup:
    case Opcode::kBuildTuple:
    case Opcode::kCallBuiltin:
    case Opcode::kCallUserFunc:
    case Opcode::kReturn:
    case Opcode::kMakeUserFuncRef:
    case Opcode::kMakeBuiltinFuncRef:
    case Opcode::kMakeClosureRef:
    case Opcode::kJump:
    case Opcode::kJumpIf:
    case Opcode::kJumpIfNot:
    case Opcode::kAdd:
    case Opcode::kSub:
    case Opcode::kMul:
    case Opcode::kDiv:
    case Opcode::kMod:
    case Opcode::kPow:
    case Opcode::kNeg:
    case Opcode::kCmpEq:
    case Opcode::kCmpNe:
    case Opcode::kCmpLt:
    case Opcode::kCmpGt:
    case Opcode::kCmpLe:
    case Opcode::kCmpGe:
    case Opcode::kAnd:
    case Opcode::kOr:
    case Opcode::kNot:
    case Opcode::kSelect:
    case Opcode::kHalt:
    case Opcode::kCallUserFuncArgs:
    case Opcode::kCheckType:
    case Opcode::kTryCheckType:
    case Opcode::kConcatTuples:
    case Opcode::kCallNative:
    case Opcode::kElementAt:
    case Opcode::kBuildTupleCounted:
    case Opcode::kMatchFailed:
    case Opcode::kBitAnd:
    case Opcode::kBitOr:
    case Opcode::kBitXor:
    case Opcode::kShiftLeft:
    case Opcode::kShiftRight:
    case Opcode::kCallFunctionValue:
    case Opcode::kCallFunctionValueArgs:
    case Opcode::kCallBuiltinArgs:
      return LocalEffect{};
  }
  return LocalEffect{};
}

// The instructions that can run after instruction `index`. A jump's operand is the index of its target in the stream;
// one past the last instruction leaves the stream.
struct Successors {
  std::array<std::size_t, 2> indices{};
  std::size_t count{0};
};

auto successors(const std::span<const Instruction> instructions, const std::size_t index) -> Successors {
  Successors result;
  const auto add = [&](const std::size_t target) {
    if (target < instructions.size()) {
      result.indices[result.count++] = target;
    }
  };
  const Instruction& instruction = instructions[index];
  switch (instruction.opcode) {
    case Opcode::kJump:
      add(static_cast<std::size_t>(instruction.operand));
      break;
    case Opcode::kJumpIf:
    case Opcode::kJumpIfNot:
      add(static_cast<std::size_t>(instruction.operand));
      add(index + 1U);
      break;
    case Opcode::kReturn:
    case Opcode::kHalt:
    case Opcode::kMatchFailed:
      break;
    default:
      add(index + 1U);
      break;
  }
  return result;
}

// A set of slots, one bit each.
class SlotSet {
public:
  explicit SlotSet(const std::size_t slots) : words_((slots + 63U) / 64U, 0U) {}

  [[nodiscard]] auto contains(const std::size_t slot) const -> bool {
    return ((words_[slot / 64U] >> (slot % 64U)) & 1U) != 0U;
  }
  void insert(const std::size_t slot) { words_[slot / 64U] |= std::uint64_t{1} << (slot % 64U); }
  void erase(const std::size_t slot) { words_[slot / 64U] &= ~(std::uint64_t{1} << (slot % 64U)); }
  void clear() { std::ranges::fill(words_, 0U); }

  void insert_all(const SlotSet& other) {
    for (std::size_t word = 0; word < words_.size(); ++word) {
      words_[word] |= other.words_[word];
    }
  }

  friend auto operator==(const SlotSet&, const SlotSet&) -> bool = default;

private:
  std::vector<std::uint64_t> words_;
};

}  // namespace

void move_last_local_reads(std::vector<Instruction>& instructions) {
  std::size_t slot_count = 0;
  for (const Instruction& instruction : instructions) {
    const LocalEffect effect = local_effect(instruction);
    for (std::size_t read = 0; read < effect.read_count; ++read) {
      slot_count = std::max(slot_count, effect.reads[read] + 1U);
    }
    if (effect.write.has_value()) {
      slot_count = std::max(slot_count, *effect.write + 1U);
    }
  }
  if (slot_count == 0U) {
    return;
  }

  // Backward dataflow: the slots read later on some path (live after an instruction) are the union of the slots live
  // before its successors; the slots live before it are those, less the one it writes, plus the ones it reads. Loops
  // need several sweeps; a sweep from the end converges in a few.
  const std::size_t count = instructions.size();
  std::vector<SlotSet> live_before(count, SlotSet(slot_count));
  SlotSet live_after(slot_count);
  bool changed = true;
  while (changed) {
    changed = false;
    for (std::size_t index = count; index-- > 0U;) {
      live_after.clear();
      const Successors next = successors(instructions, index);
      for (std::size_t successor = 0; successor < next.count; ++successor) {
        live_after.insert_all(live_before[next.indices[successor]]);
      }
      const LocalEffect effect = local_effect(instructions[index]);
      if (effect.write.has_value()) {
        live_after.erase(*effect.write);
      }
      for (std::size_t read = 0; read < effect.read_count; ++read) {
        live_after.insert(effect.reads[read]);
      }
      if (!(live_after == live_before[index])) {
        live_before[index] = live_after;
        changed = true;
      }
    }
  }

  for (std::size_t index = 0; index < count; ++index) {
    Instruction& instruction = instructions[index];
    if (instruction.opcode != Opcode::kLoadLocal) {
      continue;
    }
    const std::size_t slot = slot_of(instruction);
    const Successors next = successors(instructions, index);
    const bool read_again = std::ranges::any_of(std::span{next.indices}.first(next.count), [&](const std::size_t target) {
      return live_before[target].contains(slot);
    });
    if (!read_again) {
      instruction.opcode = Opcode::kTakeLocal;
    }
  }
}

void move_last_local_reads(Module& module) {
  move_last_local_reads(module.instructions);
  for (FunctionDef& function : module.functions) {
    move_last_local_reads(function.instructions);
  }
}

}  // namespace fleaux::bytecode
