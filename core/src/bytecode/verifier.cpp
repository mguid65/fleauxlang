#include "fleaux/bytecode/verifier.hpp"

#include <format>
#include <unordered_set>

namespace fleaux::bytecode {
namespace {

using BuiltinCallsiteList = std::vector<BuiltinCallsiteRecord>;

struct CallableSlotContract {
  std::uint32_t slot_index;
  std::uint32_t expected_arity;
};

auto owner_label(const BuiltinCallsiteRecord& record) -> std::string {
  if (record.owner_kind == BuiltinCallsiteOwnerKind::kModule) {
    return std::string{"module"};
  }
  return std::format("function {}", record.owner_index);
}

auto inferred_static_argument_count(const std::vector<Instruction>& instructions, const std::uint32_t instruction_index,
                                    const vm::BuiltinCallShape call_shape) -> std::uint32_t {
  if (call_shape == vm::BuiltinCallShape::kDirectValue) {
    return 1U;
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

auto builtin_callable_slot_contracts(const vm::BuiltinId builtin_id) -> std::vector<CallableSlotContract> {
  switch (builtin_id) {
    case vm::BuiltinId::Apply:
    case vm::BuiltinId::Try:
    case vm::BuiltinId::TupleMap:
    case vm::BuiltinId::TupleFilter:
    case vm::BuiltinId::TupleFindIndex:
    case vm::BuiltinId::TupleAny:
    case vm::BuiltinId::TupleAll:
    case vm::BuiltinId::ParallelMap:
    case vm::BuiltinId::ParallelForEach:
    case vm::BuiltinId::ParallelWithOptions:
    case vm::BuiltinId::TaskSpawn:
    case vm::BuiltinId::FileWithOpen:
      return {{builtin_id == vm::BuiltinId::TaskSpawn ? 0U : builtin_id == vm::BuiltinId::FileWithOpen ? 2U : 1U, 1U}};
    case vm::BuiltinId::TupleReduce:
    case vm::BuiltinId::ParallelReduce:
      return {{2U, 2U}};
    case vm::BuiltinId::Branch:
      return {{2U, 1U}, {3U, 1U}};
    case vm::BuiltinId::Loop:
    case vm::BuiltinId::LoopN:
      return {{1U, 1U}, {2U, 1U}};
    default:
      return {};
  }
}

auto builtin_exact_numeric_slot_indices(const vm::BuiltinId builtin_id) -> std::vector<std::uint32_t> {
  switch (builtin_id) {
    case vm::BuiltinId::ExitInt64:
    case vm::BuiltinId::RandomCreate:
    case vm::BuiltinId::TupleRangeInt64:
      return {0U};
    case vm::BuiltinId::TupleRangeInt64Int64:
      return {0U, 1U};
    case vm::BuiltinId::TupleRangeInt64Int64Int64:
      return {0U, 1U, 2U};
    case vm::BuiltinId::TaskWithTimeout:
    case vm::BuiltinId::LoopN:
    case vm::BuiltinId::FileReadChunk:
      return builtin_id == vm::BuiltinId::LoopN ? std::vector<std::uint32_t>{3U} : std::vector<std::uint32_t>{1U};
    case vm::BuiltinId::ElementAt:
    case vm::BuiltinId::Take:
    case vm::BuiltinId::Drop:
    case vm::BuiltinId::SliceTupleUInt64:
    case vm::BuiltinId::StringCharAt:
    case vm::BuiltinId::StringSliceStringUInt64:
    case vm::BuiltinId::BitShiftLeft:
    case vm::BuiltinId::BitShiftRight:
    case vm::BuiltinId::ArrayGetAt:
    case vm::BuiltinId::ArraySetAt:
    case vm::BuiltinId::ArrayInsertAt:
    case vm::BuiltinId::ArrayRemoveAt:
      return {1U};
    case vm::BuiltinId::SliceTupleUInt64UInt64:
    case vm::BuiltinId::StringSliceStringUInt64UInt64:
    case vm::BuiltinId::ArraySlice:
    case vm::BuiltinId::ArrayReshape:
      return {1U, 2U};
    case vm::BuiltinId::SliceTupleUInt64UInt64UInt64:
      return {1U, 2U, 3U};
    case vm::BuiltinId::StringFindStringStringUInt64:
      return {2U};
    case vm::BuiltinId::ArraySetAt2D:
    case vm::BuiltinId::ArrayFill:
      return {1U, 2U};
    case vm::BuiltinId::ArraySlice2D:
      return {1U, 2U, 3U, 4U};
    default:
      return {};
  }
}

auto builtin_handle_slot_summary_kind(const vm::BuiltinId builtin_id, const std::uint32_t slot_index)
    -> std::optional<BuiltinValueSlotSummaryKind> {
  if ((builtin_id == vm::BuiltinId::TaskAwait || builtin_id == vm::BuiltinId::TaskCancel ||
       builtin_id == vm::BuiltinId::TaskWithTimeout) &&
      slot_index == 0U) {
    return BuiltinValueSlotSummaryKind::kTaskHandle;
  }
  if ((builtin_id == vm::BuiltinId::RandomNextUInt64 || builtin_id == vm::BuiltinId::RandomNextInt64 ||
       builtin_id == vm::BuiltinId::RandomNextFloat64 || builtin_id == vm::BuiltinId::RandomNextBool ||
       builtin_id == vm::BuiltinId::RandomSplit) &&
      slot_index == 0U) {
    return BuiltinValueSlotSummaryKind::kRandomGenerator;
  }
  if ((builtin_id == vm::BuiltinId::FileReadLine || builtin_id == vm::BuiltinId::FileReadChunk ||
       builtin_id == vm::BuiltinId::FileWriteChunk || builtin_id == vm::BuiltinId::FileFlush ||
       builtin_id == vm::BuiltinId::FileClose) &&
      slot_index == 0U) {
    return BuiltinValueSlotSummaryKind::kFileHandle;
  }
  return std::nullopt;
}

auto function_matches_value_slot_summary(const FunctionDef& function, const BuiltinValueSlotSummaryKind summary_kind) -> bool {
  switch (summary_kind) {
    case BuiltinValueSlotSummaryKind::kDictStringAny:
      return function.return_type_is_dict_string_any;
    case BuiltinValueSlotSummaryKind::kExactUInt64:
      return function.return_type_is_exact_uint64;
    case BuiltinValueSlotSummaryKind::kExactInt64:
      return function.return_type_is_exact_int64;
    case BuiltinValueSlotSummaryKind::kTaskHandle:
      return function.return_type_is_task_handle;
    case BuiltinValueSlotSummaryKind::kRandomGenerator:
      return function.return_type_is_random_generator;
    case BuiltinValueSlotSummaryKind::kFileHandle:
      return function.return_type_is_file_handle;
  }
  return false;
}

void collect_builtin_callsites_for_stream(BuiltinCallsiteList& out, const BuiltinCallsiteOwnerKind owner_kind,
                                         const std::uint32_t owner_index, const std::vector<Instruction>& instructions) {
  for (std::uint32_t instruction_index = 0; instruction_index < static_cast<std::uint32_t>(instructions.size());
       ++instruction_index) {
    const auto& instruction = instructions[instruction_index];
    if (instruction.opcode != Opcode::kCallBuiltin) {
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
    if (instruction.opcode != Opcode::kCallBuiltin) {
      return tl::unexpected(VerificationError{.message =
                                                  std::format("Builtin callsite at {} instruction {} does not target kCallBuiltin.",
                                                              owner_label(record), record.instruction_index)});
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

    const auto callable_contracts = builtin_callable_slot_contracts(record.builtin_id);
    std::unordered_set<std::uint32_t> seen_slots;
    for (const auto& callable_slot : record.callable_slots) {
      if (!seen_slots.insert(callable_slot.slot_index).second) {
        return tl::unexpected(VerificationError{.message = std::format(
                                                    "Builtin callsite at {} instruction {} has duplicate callable metadata for slot {}.",
                                                    owner_label(record), record.instruction_index,
                                                    callable_slot.slot_index)});
      }

      const auto contract_it = std::find_if(callable_contracts.begin(), callable_contracts.end(),
                                            [&](const CallableSlotContract& contract) {
                                              return contract.slot_index == callable_slot.slot_index;
                                            });
      if (contract_it == callable_contracts.end()) {
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

      if (callable_slot.has_variadic_tail || callable_slot.declared_arity != contract_it->expected_arity) {
        return tl::unexpected(VerificationError{.message = std::format(
                                                    "Builtin callsite callable mismatch at {} instruction {} for {} slot {}: expected arity {}.",
                                                    owner_label(record), record.instruction_index,
                                                    vm::builtin_name(record.builtin_id), callable_slot.slot_index,
                                                    contract_it->expected_arity)});
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

      const auto exact_numeric_slots = builtin_exact_numeric_slot_indices(record.builtin_id);
      const auto expected_handle_summary = builtin_handle_slot_summary_kind(record.builtin_id, value_slot.slot_index);
      const bool expects_dict_string_any = record.builtin_id == vm::BuiltinId::ParallelWithOptions && value_slot.slot_index == 2U &&
                                          value_slot.summary_kind == BuiltinValueSlotSummaryKind::kDictStringAny;
      const bool expects_exact_uint64 =
          value_slot.summary_kind == BuiltinValueSlotSummaryKind::kExactUInt64 &&
          record.builtin_id != vm::BuiltinId::ExitInt64 && record.builtin_id != vm::BuiltinId::TupleRangeInt64 &&
          record.builtin_id != vm::BuiltinId::TupleRangeInt64Int64 &&
          record.builtin_id != vm::BuiltinId::TupleRangeInt64Int64Int64 &&
          std::ranges::find(exact_numeric_slots, value_slot.slot_index) != exact_numeric_slots.end();
      const bool expects_exact_int64 =
          value_slot.summary_kind == BuiltinValueSlotSummaryKind::kExactInt64 &&
          (record.builtin_id == vm::BuiltinId::ExitInt64 || record.builtin_id == vm::BuiltinId::TupleRangeInt64 ||
           record.builtin_id == vm::BuiltinId::TupleRangeInt64Int64 ||
           record.builtin_id == vm::BuiltinId::TupleRangeInt64Int64Int64) &&
          std::ranges::find(exact_numeric_slots, value_slot.slot_index) != exact_numeric_slots.end();
      const bool expects_handle = expected_handle_summary.has_value() && value_slot.summary_kind == *expected_handle_summary;
      if ((!expects_dict_string_any && !expects_exact_uint64 && !expects_exact_int64 && !expects_handle) ||
          value_slot.source_kind != BuiltinValueSlotSourceKind::kFunctionReturn) {
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

      if (!function_matches_value_slot_summary(module.functions[value_slot.source_index], value_slot.summary_kind)) {
        return tl::unexpected(VerificationError{.message = std::format(
                                                    "Builtin callsite at {} instruction {} has stale {} value metadata for slot {}.",
                                                    owner_label(record), record.instruction_index,
                                                    value_slot.summary_kind == BuiltinValueSlotSummaryKind::kDictStringAny
                                                        ? "Dict<String, Any>"
                                                        : value_slot.summary_kind == BuiltinValueSlotSummaryKind::kExactUInt64
                                                              ? "exact UInt64"
                                                              : value_slot.summary_kind == BuiltinValueSlotSummaryKind::kExactInt64
                                                                    ? "exact Int64"
                                                                    : value_slot.summary_kind == BuiltinValueSlotSummaryKind::kTaskHandle
                                                                          ? "TaskHandle"
                                                                          : value_slot.summary_kind ==
                                                                                    BuiltinValueSlotSummaryKind::kRandomGenerator
                                                                                ? "RandomGenerator"
                                                                                : "FileHandle",
                                                    value_slot.slot_index)});
      }
    }
  }

  const auto expected_records = collect_builtin_callsites(module);
  if (expected_records.size() != module.builtin_callsites.size()) {
    return tl::unexpected(VerificationError{.message =
                                                std::format("Builtin callsite metadata count mismatch: expected {} record(s) for kCallBuiltin instructions but found {}.",
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

}  // namespace fleaux::bytecode










