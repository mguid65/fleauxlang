#include "fleaux/bytecode/verifier.hpp"

#include <format>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace fleaux::bytecode {
namespace {

using BuiltinCallsiteList = std::vector<BuiltinCallsiteRecord>;

auto owner_label(const BuiltinCallsiteRecord& record) -> std::string {
  if (record.owner_kind == BuiltinCallsiteOwnerKind::kModule) {
    return std::string{"module"};
  }
  return std::format("function {}", record.owner_index);
}

// Whether an instruction calls a builtin: with an argument tuple or value (kCallBuiltin), or with its arguments on the
// stack (kCallBuiltinArgs).
auto is_builtin_call(const Opcode opcode) -> bool {
  return opcode == Opcode::kCallBuiltin || opcode == Opcode::kCallBuiltinArgs;
}

auto inferred_static_argument_count(const std::vector<Instruction>& instructions, const std::uint32_t instruction_index,
                                    const vm::BuiltinCallShape call_shape) -> std::uint32_t {
  if (call_shape == vm::BuiltinCallShape::kDirectValue) {
    return 1U;
  }
  if (const auto& call = instructions[instruction_index]; call.opcode == Opcode::kCallBuiltinArgs) {
    return vm::builtin_argument_count_from_operand(call.operand);
  }

  if (instruction_index == 0U) {
    return kUnknownBuiltinStaticArgumentCount;
  }

  const auto& previous = instructions[instruction_index - 1U];
  if (previous.opcode != Opcode::kBuildTuple || previous.operand < 0 ||
      previous.operand > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
    return kUnknownBuiltinStaticArgumentCount;
  }

  return static_cast<std::uint32_t>(previous.operand);
}

void collect_builtin_callsites_for_stream(BuiltinCallsiteList& out, const BuiltinCallsiteOwnerKind owner_kind,
                                         const std::uint32_t owner_index, const std::vector<Instruction>& instructions) {
  for (std::uint32_t instruction_index = 0; instruction_index < static_cast<std::uint32_t>(instructions.size());
       ++instruction_index) {
    const auto& instruction = instructions[instruction_index];
    if (!is_builtin_call(instruction.opcode)) {
      continue;
    }

    const auto builtin_id = vm::builtin_id_from_operand(instruction.operand);
    const auto call_shape = vm::builtin_call_shape_from_operand(instruction.operand);
    if (!builtin_id.has_value() || !call_shape.has_value()) {
      continue;
    }

    out.push_back(BuiltinCallsiteRecord{
        .owner_kind = owner_kind,
        .owner_index = owner_index,
        .instruction_index = instruction_index,
        .builtin_id = *builtin_id,
        .call_shape = *call_shape,
        .static_argument_count = inferred_static_argument_count(instructions, instruction_index, *call_shape),
    });
  }
}

auto collect_builtin_callsites(const Module& module) -> BuiltinCallsiteList {
  BuiltinCallsiteList records;
  for (std::uint32_t function_index = 0; function_index < static_cast<std::uint32_t>(module.functions.size());
       ++function_index) {
    collect_builtin_callsites_for_stream(records, BuiltinCallsiteOwnerKind::kFunction, function_index,
                                         module.functions[function_index].instructions);
  }
  collect_builtin_callsites_for_stream(records, BuiltinCallsiteOwnerKind::kModule, 0U, module.instructions);
  return records;
}

auto same_callsite_identity(const BuiltinCallsiteRecord& lhs, const BuiltinCallsiteRecord& rhs) -> bool {
  return lhs.owner_kind == rhs.owner_kind && lhs.owner_index == rhs.owner_index &&
         lhs.instruction_index == rhs.instruction_index && lhs.builtin_id == rhs.builtin_id &&
         lhs.call_shape == rhs.call_shape;
}

auto exact_callsite_match(const BuiltinCallsiteRecord& lhs, const BuiltinCallsiteRecord& rhs) -> bool {
  return lhs.owner_kind == rhs.owner_kind && lhs.owner_index == rhs.owner_index &&
         lhs.instruction_index == rhs.instruction_index && lhs.builtin_id == rhs.builtin_id &&
         lhs.call_shape == rhs.call_shape && lhs.static_argument_count == rhs.static_argument_count;
}

auto fuzzy_callsite_match(const BuiltinCallsiteRecord& lhs, const BuiltinCallsiteRecord& rhs) -> bool {
  return lhs.owner_kind == rhs.owner_kind && lhs.owner_index == rhs.owner_index && lhs.builtin_id == rhs.builtin_id &&
         lhs.call_shape == rhs.call_shape && lhs.static_argument_count == rhs.static_argument_count;
}

}  // namespace

void rebuild_module_builtin_callsites(Module& module) {
  const auto previous_records = module.builtin_callsites;
  auto rebuilt = collect_builtin_callsites(module);

  std::vector<bool> consumed(previous_records.size(), false);
  for (auto& record : rebuilt) {
    auto exact_it = std::find_if(previous_records.begin(), previous_records.end(), [&](const BuiltinCallsiteRecord& candidate) {
      return exact_callsite_match(record, candidate) && !consumed[static_cast<std::size_t>(&candidate - previous_records.data())];
    });
    if (exact_it != previous_records.end()) {
      const auto index = static_cast<std::size_t>(exact_it - previous_records.begin());
      consumed[index] = true;
      record.callable_slots = exact_it->callable_slots;
      record.value_slots = exact_it->value_slots;
      continue;
    }

    auto fuzzy_it = std::find_if(previous_records.begin(), previous_records.end(), [&](const BuiltinCallsiteRecord& candidate) {
      return fuzzy_callsite_match(record, candidate) && !consumed[static_cast<std::size_t>(&candidate - previous_records.data())];
    });
    if (fuzzy_it != previous_records.end()) {
      const auto index = static_cast<std::size_t>(fuzzy_it - previous_records.begin());
      consumed[index] = true;
      record.callable_slots = fuzzy_it->callable_slots;
      record.value_slots = fuzzy_it->value_slots;
    }
  }

  module.builtin_callsites = std::move(rebuilt);
}

auto verify_module_builtin_callsites(const Module& module) -> tl::expected<void, VerificationError> {
  for (const auto& record : module.builtin_callsites) {
    const std::vector<Instruction>* instructions = nullptr;

    if (record.owner_kind == BuiltinCallsiteOwnerKind::kModule) {
      instructions = &module.instructions;
    } else {
      if (record.owner_index >= module.functions.size()) {
        return tl::unexpected(VerificationError{.message =
                                                    std::format("Invalid builtin callsite owner function index {}.",
                                                                record.owner_index)});
      }
      instructions = &module.functions[record.owner_index].instructions;
    }

    if (record.instruction_index >= instructions->size()) {
      return tl::unexpected(
          VerificationError{.message = std::format("Builtin callsite points outside {} instruction stream at index {}.",
                                                   owner_label(record), record.instruction_index)});
    }

    const auto& instruction = (*instructions)[record.instruction_index];
    if (!is_builtin_call(instruction.opcode)) {
      return tl::unexpected(VerificationError{.message =
                                                  std::format("Builtin callsite at {} instruction {} does not target a builtin call.",
                                                              owner_label(record), record.instruction_index)});
    }
    if (instruction.opcode == Opcode::kCallBuiltinArgs) {
      const auto called = vm::builtin_id_from_operand(instruction.operand);
      if (!called.has_value() || !vm::builtin_takes_arguments(*called)) {
        return tl::unexpected(VerificationError{.message = std::format(
                                                    "Builtin callsite at {} instruction {} passes stack arguments to a builtin that takes one value.",
                                                    owner_label(record), record.instruction_index)});
      }
    }

    const auto builtin_id = vm::builtin_id_from_operand(instruction.operand);
    const auto call_shape = vm::builtin_call_shape_from_operand(instruction.operand);
    if (!builtin_id.has_value() || !call_shape.has_value()) {
      return tl::unexpected(VerificationError{.message =
                                                  std::format("Builtin callsite at {} instruction {} has undecodable builtin operand {}.",
                                                              owner_label(record), record.instruction_index,
                                                              instruction.operand)});
    }

    if (*builtin_id != record.builtin_id) {
      return tl::unexpected(VerificationError{.message =
                                                  std::format("Builtin callsite mismatch at {} instruction {}: metadata has {} but operand has {}.",
                                                              owner_label(record), record.instruction_index,
                                                              vm::builtin_name(record.builtin_id),
                                                              vm::builtin_name(*builtin_id))});
    }

    if (*call_shape != record.call_shape) {
      return tl::unexpected(VerificationError{.message =
                                                  std::format("Builtin callsite shape mismatch at {} instruction {} for {}.",
                                                              owner_label(record), record.instruction_index,
                                                              vm::builtin_name(record.builtin_id))});
    }

    if (record.call_shape == vm::BuiltinCallShape::kDirectValue &&
        record.static_argument_count != kUnknownBuiltinStaticArgumentCount && record.static_argument_count != 1U) {
      return tl::unexpected(VerificationError{.message =
                                                  std::format("Builtin callsite at {} instruction {} records impossible direct-value arity {} for {}.",
                                                              owner_label(record), record.instruction_index,
                                                              record.static_argument_count,
                                                              vm::builtin_name(record.builtin_id))});
    }

    if (record.static_argument_count != kUnknownBuiltinStaticArgumentCount &&
        !vm::builtin_accepts_arity(record.builtin_id, record.static_argument_count)) {
      return tl::unexpected(VerificationError{.message =
                                                  std::format("Builtin callsite arity mismatch at {} instruction {} for {}: metadata records {} argument(s).",
                                                              owner_label(record), record.instruction_index,
                                                              vm::builtin_name(record.builtin_id),
                                                              record.static_argument_count)});
    }

    std::unordered_set<std::uint32_t> seen_slots;
    for (const auto& callable_slot : record.callable_slots) {
      if (!seen_slots.insert(callable_slot.slot_index).second) {
        return tl::unexpected(VerificationError{.message = std::format(
                                                    "Builtin callsite at {} instruction {} has duplicate callable metadata for slot {}.",
                                                    owner_label(record), record.instruction_index,
                                                    callable_slot.slot_index)});
      }

      const auto expected_arity = vm::builtin_callable_slot_arity(record.builtin_id, callable_slot.slot_index);
      if (!expected_arity.has_value()) {
        return tl::unexpected(VerificationError{.message = std::format(
                                                    "Builtin callsite at {} instruction {} records unsupported callable slot {} for {}.",
                                                    owner_label(record), record.instruction_index,
                                                    callable_slot.slot_index, vm::builtin_name(record.builtin_id))});
      }

      switch (callable_slot.kind) {
        case BuiltinCallableSlotKind::kFunction: {
          if (callable_slot.target_index >= module.functions.size()) {
            return tl::unexpected(VerificationError{.message = std::format(
                                                        "Builtin callsite at {} instruction {} references missing function callable {}.",
                                                        owner_label(record), record.instruction_index,
                                                        callable_slot.target_index)});
          }
          const auto& function = module.functions[callable_slot.target_index];
          if (function.arity != callable_slot.declared_arity || function.has_variadic_tail != callable_slot.has_variadic_tail) {
            return tl::unexpected(VerificationError{.message = std::format(
                                                        "Builtin callsite at {} instruction {} has stale function callable metadata for slot {}.",
                                                        owner_label(record), record.instruction_index,
                                                        callable_slot.slot_index)});
          }
          break;
        }
        case BuiltinCallableSlotKind::kClosure: {
          if (callable_slot.target_index >= module.closures.size()) {
            return tl::unexpected(VerificationError{.message = std::format(
                                                        "Builtin callsite at {} instruction {} references missing closure callable {}.",
                                                        owner_label(record), record.instruction_index,
                                                        callable_slot.target_index)});
          }
          const auto& closure = module.closures[callable_slot.target_index];
          if (closure.declared_arity != callable_slot.declared_arity ||
              closure.declared_has_variadic_tail != callable_slot.has_variadic_tail) {
            return tl::unexpected(VerificationError{.message = std::format(
                                                        "Builtin callsite at {} instruction {} has stale closure callable metadata for slot {}.",
                                                        owner_label(record), record.instruction_index,
                                                        callable_slot.slot_index)});
          }
          break;
        }
        case BuiltinCallableSlotKind::kBuiltin: {
          if (callable_slot.target_index >= vm::builtin_count()) {
            return tl::unexpected(VerificationError{.message = std::format(
                                                        "Builtin callsite at {} instruction {} references missing builtin callable {}.",
                                                        owner_label(record), record.instruction_index,
                                                        callable_slot.target_index)});
          }
          const auto target_builtin = static_cast<vm::BuiltinId>(callable_slot.target_index);
          const auto contract = vm::builtin_arity_contract(target_builtin);
          const auto expected_declared_arity = static_cast<std::uint32_t>(contract.primary);
          const auto expected_variadic = contract.kind == vm::BuiltinArityKind::kVariadicMinimum;
          if (callable_slot.declared_arity != expected_declared_arity ||
              callable_slot.has_variadic_tail != expected_variadic) {
            return tl::unexpected(VerificationError{.message = std::format(
                                                        "Builtin callsite at {} instruction {} has stale builtin callable metadata for slot {}.",
                                                        owner_label(record), record.instruction_index,
                                                        callable_slot.slot_index)});
          }
          break;
        }
      }

      if (callable_slot.has_variadic_tail || callable_slot.declared_arity != *expected_arity) {
        return tl::unexpected(VerificationError{
            .message = std::format("Builtin callsite callable mismatch at {} instruction {} for {} slot {}: expected arity {}.",
                                   owner_label(record), record.instruction_index, vm::builtin_name(record.builtin_id),
                                   callable_slot.slot_index, *expected_arity)});
      }
    }

    std::unordered_set<std::uint32_t> seen_value_slots;
    for (const auto& value_slot : record.value_slots) {
      if (!seen_value_slots.insert(value_slot.slot_index).second) {
        return tl::unexpected(VerificationError{.message = std::format(
                                                    "Builtin callsite at {} instruction {} has duplicate value metadata for slot {}.",
                                                    owner_label(record), record.instruction_index,
                                                    value_slot.slot_index)});
      }

      const auto expected_kind = vm::builtin_value_slot_kind(record.builtin_id, value_slot.slot_index);
      if (expected_kind != value_slot.summary_kind || value_slot.source_kind != BuiltinValueSlotSourceKind::kFunctionReturn) {
        return tl::unexpected(VerificationError{.message = std::format(
                                                    "Builtin callsite at {} instruction {} records unsupported value metadata for {} slot {}.",
                                                    owner_label(record), record.instruction_index,
                                                    vm::builtin_name(record.builtin_id), value_slot.slot_index)});
      }

      if (value_slot.source_index >= module.functions.size()) {
        return tl::unexpected(VerificationError{.message = std::format(
                                                    "Builtin callsite at {} instruction {} references missing value-summary source function {}.",
                                                    owner_label(record), record.instruction_index,
                                                    value_slot.source_index)});
      }

      if (module.functions[value_slot.source_index].return_value_slot_kind != value_slot.summary_kind) {
        return tl::unexpected(
            VerificationError{.message = std::format("Builtin callsite at {} instruction {} has stale {} value metadata for slot {}.",
                                                     owner_label(record), record.instruction_index,
                                                     vm::builtin_value_slot_kind_name(value_slot.summary_kind), value_slot.slot_index)});
      }
    }
  }

  const auto expected_records = collect_builtin_callsites(module);
  if (expected_records.size() != module.builtin_callsites.size()) {
    return tl::unexpected(VerificationError{.message =
                                                std::format("Builtin callsite metadata count mismatch: expected {} record(s) for builtin calls but found {}.",
                                                            expected_records.size(), module.builtin_callsites.size())});
  }

  for (std::size_t index = 0; index < expected_records.size(); ++index) {
    if (!same_callsite_identity(expected_records[index], module.builtin_callsites[index])) {
      return tl::unexpected(VerificationError{.message =
                                                  std::format("Builtin callsite metadata drift at record {}.", index)});
    }
  }

  return {};
}

auto verify_module_locations(const Module& module) -> tl::expected<void, VerificationError> {
  const auto check_stream = [&](const std::vector<Instruction>& instructions,
                                const std::string& owner) -> tl::expected<void, VerificationError> {
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      if (instructions[index].location > module.locations.size()) {
        return tl::unexpected(VerificationError{.message = std::format("{} instruction {} has location {}, but the module has {}.", owner,
                                                                       index, instructions[index].location, module.locations.size())});
      }
    }
    return {};
  };
  if (auto checked = check_stream(module.instructions, "module"); !checked) {
    return checked;
  }
  for (std::size_t function_index = 0; function_index < module.functions.size(); ++function_index) {
    if (auto checked = check_stream(module.functions[function_index].instructions, std::format("function {}", function_index)); !checked) {
      return checked;
    }
  }

  for (std::size_t index = 0; index < module.locations.size(); ++index) {
    const SourceLocation& location = module.locations[index];
    if (location.file >= module.source_files.size()) {
      return tl::unexpected(VerificationError{.message = std::format("Location {} names source file {}, but the module has {}.", index + 1U,
                                                                     location.file, module.source_files.size())});
    }
    if (location.inlined_function.has_value() && *location.inlined_function >= module.functions.size()) {
      return tl::unexpected(VerificationError{.message = std::format("Location {} names function {}, but the module has {}.", index + 1U,
                                                                     *location.inlined_function, module.functions.size())});
    }
    if (location.inlined_closure && (location.inlined_function.has_value() || location.inlined_at == kNoLocation)) {
      return tl::unexpected(VerificationError{
          .message = std::format("Location {} is in an inlined closure, so it names no function and is inlined at a call.", index + 1U)});
    }
    if (location.called_by.has_value() && *location.called_by >= vm::builtin_count()) {
      return tl::unexpected(VerificationError{.message = std::format("Location {} is called by builtin {}, but there are {}.", index + 1U,
                                                                     *location.called_by, vm::builtin_count())});
    }
    // A location's number is index + 1, so the call it was inlined for comes before it.
    if (location.inlined_at > index) {
      return tl::unexpected(
          VerificationError{.message = std::format("Location {} is inlined at location {}, which does not come before it.", index + 1U,
                                                   location.inlined_at)});
    }
  }
  return {};
}

auto verify_module_operands(const Module& module) -> tl::expected<void, VerificationError> {
  const auto check_stream = [&](const std::vector<Instruction>& instructions,
                                const std::string& owner) -> tl::expected<void, VerificationError> {
    for (std::size_t index = 0; index < instructions.size(); ++index) {
      const Instruction& instruction = instructions[index];
      const std::int64_t operand = instruction.operand;
      const auto names = [&](const std::size_t size) -> bool { return operand >= 0 && std::cmp_less(operand, size); };
      const auto refuse = [&](const std::string_view what, const std::size_t size) -> tl::expected<void, VerificationError> {
        return tl::unexpected(VerificationError{.message = std::format("{} instruction {} ({}) names {} {}, but there are {}.", owner,
                                                                       index, stringify_opcode(instruction.opcode), what, operand, size)});
      };
      constexpr auto slots = static_cast<std::size_t>(kMaxLocalSlots);
      switch (instruction.opcode) {
        case Opcode::kPushConst:
          if (!names(module.constants.size())) {
            return refuse("constant", module.constants.size());
          }
          break;
        case Opcode::kCallUserFunc:
        case Opcode::kCallUserFuncArgs:
        case Opcode::kMakeUserFuncRef:
          if (!names(module.functions.size())) {
            return refuse("function", module.functions.size());
          }
          break;
        case Opcode::kMakeClosureRef:
          if (!names(module.closures.size())) {
            return refuse("closure", module.closures.size());
          }
          break;
        case Opcode::kCheckType:
        case Opcode::kTryCheckType:
          if (!names(module.type_checks.size())) {
            return refuse("type check", module.type_checks.size());
          }
          break;
        case Opcode::kCallNative:
          if (!names(module.native_functions.size())) {
            return refuse("native function", module.native_functions.size());
          }
          break;
        case Opcode::kJump:
        case Opcode::kJumpIf:
        case Opcode::kJumpIfNot:
          // A jump may go to the end of the stream.
          if (!names(instructions.size() + 1U)) {
            return refuse("instruction", instructions.size() + 1U);
          }
          break;
        case Opcode::kLoadLocal:
        case Opcode::kStoreLocal:
        case Opcode::kTakeLocal:
        case Opcode::kMarkStack:
        case Opcode::kBuildTupleSinceMark:
          if (!names(slots)) {
            return refuse("local slot", slots);
          }
          break;
        case Opcode::kIterate:
          // The tuple's slot and the index's, after it.
          if (!names(slots - 1U)) {
            return refuse("local slot", slots - 1U);
          }
          break;
        case Opcode::kLoadLocalItem:
        case Opcode::kTakeLocalItem:
          if (operand < 0 || local_item_slot(operand) >= slots) {
            return refuse("local item", slots);
          }
          break;
        default:
          break;
      }
    }
    return {};
  };
  if (auto checked = check_stream(module.instructions, "module"); !checked) {
    return checked;
  }
  for (std::size_t function_index = 0; function_index < module.functions.size(); ++function_index) {
    if (auto checked = check_stream(module.functions[function_index].instructions, std::format("function {}", function_index)); !checked) {
      return checked;
    }
  }
  for (std::size_t index = 0; index < module.closures.size(); ++index) {
    if (module.closures[index].function_index >= module.functions.size()) {
      return tl::unexpected(VerificationError{.message = std::format("Closure {} names function {}, but there are {}.", index,
                                                                     module.closures[index].function_index, module.functions.size())});
    }
  }
  for (const ExportedSymbol& exported : module.exports) {
    if (exported.kind == ExportKind::kFunction && exported.index >= module.functions.size()) {
      return tl::unexpected(VerificationError{.message = std::format("Export '{}' names function {}, but there are {}.", exported.name,
                                                                     exported.index, module.functions.size())});
    }
  }
  return {};
}

}  // namespace fleaux::bytecode










