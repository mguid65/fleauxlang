#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>

#include "fleaux/bytecode/compiler.hpp"
#include "fleaux/bytecode/local_liveness.hpp"
#include "fleaux/common/match_wildcard.hpp"
#include "fleaux/common/overloaded.hpp"
#include "fleaux/common/symbol_name.hpp"
#include "fleaux/frontend/lowering.hpp"
#include "fleaux/vm/builtin_catalog.hpp"

namespace fleaux::bytecode {
namespace {

using namespace frontend::ir;

// Builtin intrinsic helpers

auto unary_intrinsic_builtin_to_opcode(const std::string& symbol_key) -> std::optional<Opcode> {
  if (symbol_key == "Std.Not")
    return Opcode::kNot;
  if (symbol_key == "Std.UnaryMinus")
    return Opcode::kNeg;
  return std::nullopt;
}

auto binary_intrinsic_builtin_to_opcode(const std::string& symbol_key) -> std::optional<Opcode> {
  if (symbol_key == "Std.Add")
    return Opcode::kAdd;
  if (symbol_key == "Std.Subtract")
    return Opcode::kSub;
  if (symbol_key == "Std.Multiply")
    return Opcode::kMul;
  if (symbol_key == "Std.Divide")
    return Opcode::kDiv;
  if (symbol_key == "Std.Mod")
    return Opcode::kMod;
  if (symbol_key == "Std.Pow")
    return Opcode::kPow;
  if (symbol_key == "Std.Equal")
    return Opcode::kCmpEq;
  if (symbol_key == "Std.NotEqual")
    return Opcode::kCmpNe;
  if (symbol_key == "Std.LessThan")
    return Opcode::kCmpLt;
  if (symbol_key == "Std.GreaterThan")
    return Opcode::kCmpGt;
  if (symbol_key == "Std.LessOrEqual")
    return Opcode::kCmpLe;
  if (symbol_key == "Std.GreaterOrEqual")
    return Opcode::kCmpGe;
  if (symbol_key == "Std.And")
    return Opcode::kAnd;
  if (symbol_key == "Std.Or")
    return Opcode::kOr;
  if (symbol_key == "Std.ElementAt")
    return Opcode::kElementAt;
  if (symbol_key == "Std.Bit.And")
    return Opcode::kBitAnd;
  if (symbol_key == "Std.Bit.Or")
    return Opcode::kBitOr;
  if (symbol_key == "Std.Bit.Xor")
    return Opcode::kBitXor;
  if (symbol_key == "Std.Bit.ShiftLeft")
    return Opcode::kShiftLeft;
  if (symbol_key == "Std.Bit.ShiftRight")
    return Opcode::kShiftRight;
  return std::nullopt;
}

// Compile state

struct CompileState {
  // Exact internal function key -> index into Module::functions.
  std::unordered_map<std::string, std::uint32_t> function_idx;
  std::unordered_set<std::string> ambiguous_function_names;
  // Maps a short/full name to the stdlib builtin it aliases (from is_builtin lets).
  std::unordered_map<std::string, std::string> builtin_alias;
  bool enable_experimental_builtin_reductions{false};
  bool inline_functions{true};
  std::optional<std::uint32_t> current_function_idx;
  // The IR of each function this module compiles, by index into Module::functions, for inlining its calls, and the
  // functions whose bodies are being inlined, innermost last.
  std::unordered_map<std::uint32_t, const IRLet*> function_lets;
  std::vector<std::uint32_t> inlining;

  static auto intern_const(Module& bytecode_module, ConstValue c) -> std::uint32_t {
    const auto idx = static_cast<std::uint32_t>(bytecode_module.constants.size());
    bytecode_module.constants.push_back(std::move(c));
    return idx;
  }

  auto register_public_function_name(const std::string& name, const std::uint32_t fn_idx) -> void {
    if (ambiguous_function_names.contains(name)) {
      return;
    }
    if (const auto existing = function_idx.find(name); existing != function_idx.end()) {
      if (existing->second == fn_idx) {
        return;
      }
      function_idx.erase(existing);
      ambiguous_function_names.insert(name);
      return;
    }
    function_idx[name] = fn_idx;
  }
};

auto make_err(const std::string& msg) -> CompileError { return CompileError{.message = msg}; }

using EmitResult = tl::expected<void, CompileError>;
using LocalSlots = std::unordered_map<std::string, std::uint32_t>;

void record_builtin_callsite(Module& bytecode_module, const CompileState& state, const std::vector<Instruction>& out,
                             const vm::BuiltinId builtin_id, const vm::BuiltinCallShape call_shape,
                             const std::uint32_t static_argument_count = kUnknownBuiltinStaticArgumentCount,
                             std::vector<BuiltinCallableSlotRecord> callable_slots = {},
                             std::vector<BuiltinValueSlotRecord> value_slots = {}) {
  bytecode_module.builtin_callsites.push_back(BuiltinCallsiteRecord{
      .owner_kind = state.current_function_idx.has_value() ? BuiltinCallsiteOwnerKind::kFunction
                                                           : BuiltinCallsiteOwnerKind::kModule,
      .owner_index = state.current_function_idx.value_or(0U),
      .instruction_index = static_cast<std::uint32_t>(out.size()),
      .builtin_id = builtin_id,
      .call_shape = call_shape,
      .static_argument_count = static_argument_count,
      .callable_slots = std::move(callable_slots),
      .value_slots = std::move(value_slots),
  });
}

// Whether a jump in `out` lands on instruction `target`.
auto jumps_to(const std::vector<Instruction>& out, const std::size_t target) -> bool {
  return std::ranges::any_of(out, [target](const Instruction& instruction) {
    return (instruction.opcode == Opcode::kJump || instruction.opcode == Opcode::kJumpIf ||
            instruction.opcode == Opcode::kJumpIfNot) &&
           instruction.operand == static_cast<std::int64_t>(target);
  });
}

// Emits the call of a builtin whose arguments were just emitted. A builtin that takes runtime::Arguments
// (vm::builtin_takes_arguments) is called on the argument values when the last instruction built their tuple: the
// kCallBuiltinArgs takes the BuildTuple's place, so a jump that lands there still finds the arguments on the stack.
// Every other call passes the tuple (kCallBuiltin): an argument list with spreads, a builtin of one parameter.
void emit_builtin_call(Module& bytecode_module, const CompileState& state, std::vector<Instruction>& out,
                       const vm::BuiltinId builtin_id, const vm::BuiltinCallShape call_shape,
                       const std::uint32_t static_argument_count,
                       std::vector<BuiltinCallableSlotRecord> callable_slots = {},
                       std::vector<BuiltinValueSlotRecord> value_slots = {}) {
  if (call_shape == vm::BuiltinCallShape::kTupleExpanded && vm::builtin_takes_arguments(builtin_id) && !out.empty() &&
      out.back().opcode == Opcode::kBuildTuple && out.back().operand >= 0 &&
      std::cmp_less_equal(out.back().operand, vm::kBuiltinOperandArgumentCountMask) &&
      (static_argument_count == kUnknownBuiltinStaticArgumentCount ||
       std::cmp_equal(static_argument_count, out.back().operand)) &&
      !jumps_to(out, out.size())) {
    const auto count = static_cast<std::uint16_t>(out.back().operand);
    out.pop_back();
    record_builtin_callsite(bytecode_module, state, out, builtin_id, call_shape, count, std::move(callable_slots),
                            std::move(value_slots));
    out.push_back(
        Instruction{.opcode = Opcode::kCallBuiltinArgs, .operand = vm::builtin_operand_with_arguments(builtin_id, count)});
    return;
  }
  record_builtin_callsite(bytecode_module, state, out, builtin_id, call_shape, static_argument_count,
                          std::move(callable_slots), std::move(value_slots));
  out.push_back(Instruction{.opcode = Opcode::kCallBuiltin, .operand = vm::builtin_operand(builtin_id, call_shape)});
}

auto builtin_callable_slot_indices(const vm::BuiltinId builtin_id) -> std::vector<std::uint32_t> {
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
      return {1U};
    case vm::BuiltinId::TaskSpawn:
      return {0U};
    case vm::BuiltinId::TupleReduce:
    case vm::BuiltinId::ParallelReduce:
    case vm::BuiltinId::FileWithOpen:
      return {2U};
    case vm::BuiltinId::Branch:
      return {2U, 3U};
    case vm::BuiltinId::Loop:
    case vm::BuiltinId::LoopN:
      return {1U, 2U};
    default:
      return {};
  }
}

auto builtin_value_slot_indices(const vm::BuiltinId builtin_id) -> std::vector<std::uint32_t> {
  switch (builtin_id) {
    case vm::BuiltinId::ParallelWithOptions:
      return {2U};
    case vm::BuiltinId::TaskAwait:
    case vm::BuiltinId::TaskCancel:
    case vm::BuiltinId::RandomNextUInt64:
    case vm::BuiltinId::RandomNextInt64:
    case vm::BuiltinId::RandomNextFloat64:
    case vm::BuiltinId::RandomNextBool:
    case vm::BuiltinId::RandomSplit:
    case vm::BuiltinId::FileReadLine:
    case vm::BuiltinId::FileWriteChunk:
    case vm::BuiltinId::FileFlush:
    case vm::BuiltinId::FileClose:
      return {0U};
    case vm::BuiltinId::ExitInt64:
      return {0U};
    case vm::BuiltinId::RandomCreate:
      return {0U};
    case vm::BuiltinId::TupleRangeInt64:
      return {0U};
    case vm::BuiltinId::TupleRangeInt64Int64:
      return {0U, 1U};
    case vm::BuiltinId::TupleRangeInt64Int64Int64:
      return {0U, 1U, 2U};
    case vm::BuiltinId::TaskWithTimeout:
      return {1U};
    case vm::BuiltinId::LoopN:
      return {3U};
    case vm::BuiltinId::FileReadChunk:
      return {0U, 1U};
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

auto is_dict_string_any_ir_type(const frontend::ir::IRSimpleType& type) -> bool {
  return type.name == "Dict" && type.type_args.size() == 2U && type.type_args[0].name == "String" &&
         type.type_args[1].name == "Any";
}

auto is_exact_uint64_ir_type(const frontend::ir::IRSimpleType& type) -> bool {
  return type.name == "UInt64" && !type.variadic && type.alternatives.empty() && type.alternative_types.empty() &&
         type.tuple_items.empty() && type.type_args.empty() && !type.function_sig.has_value();
}

auto is_exact_int64_ir_type(const frontend::ir::IRSimpleType& type) -> bool {
  return type.name == "Int64" && !type.variadic && type.alternatives.empty() && type.alternative_types.empty() &&
         type.tuple_items.empty() && type.type_args.empty() && !type.function_sig.has_value();
}

// Matches a declared nominal type by name, with or without type arguments (for example FileHandle or
// TaskHandle<Int64>).
auto is_nominal_ir_type(const frontend::ir::IRSimpleType& type, const std::string_view name) -> bool {
  const bool nominal_shape = !type.variadic && type.alternatives.empty() && type.alternative_types.empty() &&
                             type.tuple_items.empty() && !type.function_sig.has_value();
  if (!nominal_shape) {
    return false;
  }
  if (type.name == name) {
    return true;
  }
  return type.bridge_type_node.has_value() &&
         (type.bridge_type_node->kind == frontend::types::TypeNodeKind::kNamed ||
          type.bridge_type_node->kind == frontend::types::TypeNodeKind::kApplied) &&
         type.bridge_type_node->name == name;
}

auto builtin_value_slot_summary_kind(const vm::BuiltinId builtin_id, const std::uint32_t slot_index)
    -> std::optional<BuiltinValueSlotSummaryKind> {
  if (builtin_id == vm::BuiltinId::ParallelWithOptions && slot_index == 2U) {
    return BuiltinValueSlotSummaryKind::kDictStringAny;
  }

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

  const auto exact_uint64_slots = builtin_value_slot_indices(builtin_id);
  if ((builtin_id == vm::BuiltinId::ExitInt64 || builtin_id == vm::BuiltinId::TupleRangeInt64 ||
       builtin_id == vm::BuiltinId::TupleRangeInt64Int64 || builtin_id == vm::BuiltinId::TupleRangeInt64Int64Int64) &&
      std::ranges::find(exact_uint64_slots, slot_index) != exact_uint64_slots.end()) {
    return BuiltinValueSlotSummaryKind::kExactInt64;
  }

  if (std::ranges::find(exact_uint64_slots, slot_index) != exact_uint64_slots.end() &&
      builtin_id != vm::BuiltinId::ParallelWithOptions && builtin_id != vm::BuiltinId::ExitInt64 &&
      builtin_id != vm::BuiltinId::TupleRangeInt64 && builtin_id != vm::BuiltinId::TupleRangeInt64Int64 &&
      builtin_id != vm::BuiltinId::TupleRangeInt64Int64Int64) {
    return BuiltinValueSlotSummaryKind::kExactUInt64;
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

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

auto hash_text(const std::string& text) -> std::uint64_t {
  std::uint64_t hash = kFnvOffsetBasis;
  for (const char ch : text) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
    hash *= kFnvPrime;
  }
  return hash;
}

auto target_symbol_name(const IRNameRef& name_ref) -> std::string;

auto builtin_symbol_key_from_name_ref(const IRNameRef& name_ref, const CompileState& state)
    -> std::optional<std::string> {
  const std::string full_name = common::full_symbol_name(name_ref.qualifier, name_ref.name);
  std::string target_name = target_symbol_name(name_ref);

  if (name_ref.qualifier.has_value() && (*name_ref.qualifier == "Std" || name_ref.qualifier->starts_with("Std."))) {
    return target_name;
  }

  if (const auto alias_it = state.builtin_alias.find(full_name); alias_it != state.builtin_alias.end()) {
    return alias_it->second;
  }

  return std::nullopt;
}

auto internal_symbol_name(const IRLet& let) -> std::string {
  if (!let.symbol_key.empty()) {
    return let.symbol_key;
  }
  return common::full_symbol_name(let.qualifier, let.name);
}

auto target_symbol_name(const IRNameRef& name_ref) -> std::string {
  if (name_ref.resolved_symbol_key.has_value()) {
    return *name_ref.resolved_symbol_key;
  }
  return common::full_symbol_name(name_ref.qualifier, name_ref.name);
}

struct AuditedHandleReturnSummary {
  bool is_task_handle{false};
  bool is_random_generator{false};
  bool is_file_handle{false};
};

auto infer_audited_handle_return_summary(const IRExprBox& body) -> AuditedHandleReturnSummary {
  if (!body.has_value()) {
    return {};
  }

  const auto* flow = std::get_if<IRFlowExpr>(&body->node);
  if (flow == nullptr) {
    return {};
  }

  const auto* target = std::get_if<IRNameRef>(&flow->rhs);
  if (target == nullptr) {
    return {};
  }

  const auto builtin_id = vm::builtin_id_from_symbol_key(target_symbol_name(*target));
  if (!builtin_id.has_value()) {
    return {};
  }

  switch (*builtin_id) {
    case vm::BuiltinId::TaskSpawn:
      return {.is_task_handle = true};
    case vm::BuiltinId::RandomCreate:
      return {.is_random_generator = true};
    case vm::BuiltinId::FileOpen:
    case vm::BuiltinId::FileWriteChunk:
    case vm::BuiltinId::FileFlush:
      return {.is_file_handle = true};
    default:
      return {};
  }
}

auto is_symbolic_import(const std::string& module_name) -> bool { return module_name == "Std"; }

auto let_belongs_to_module(const IRLet& let, const CompileOptions& options) -> bool {
  if (!options.source_path.has_value()) {
    return true;
  }
  if (!let.span.has_value()) {
    return true;
  }
  return let.span->source_name() == options.source_path->string();
}

void register_builtin_alias_export(Module& bytecode_module, const IRLet& let, const CompileOptions& options,
                                   const std::string& full_name) {
  if (!let_belongs_to_module(let, options)) {
    return;
  }
  bytecode_module.exports.push_back(ExportedSymbol{
      .name = full_name,
      .link_name = full_name,
      .kind = ExportKind::kBuiltinAlias,
      .index = 0,
      .builtin_name = full_name,
      .host_entry_check = let.host_entry_check,
  });
}

void register_builtin_alias_definition(Module& bytecode_module, CompileState& state, const IRLet& let,
                                       const CompileOptions& options, const std::string& full_name) {
  state.builtin_alias[full_name] = full_name;
  register_builtin_alias_export(bytecode_module, let, options, full_name);
}

auto make_function_def(const IRLet& let, const std::string& link_name) -> FunctionDef {
  FunctionDef fn_def;
  fn_def.name = link_name;
  fn_def.arity = static_cast<std::uint32_t>(let.params.size());
  fn_def.has_variadic_tail = !let.params.empty() && let.params.back().type.variadic;
  fn_def.is_value_binding = let.is_value_binding;
  fn_def.generic_params = let.generic_params;
  fn_def.generic_bounds.reserve(let.generic_params.size());
  for (std::size_t index = 0; index < let.generic_params.size(); ++index) {
    const bool has_bound = index < let.generic_bounds.size() && let.generic_bounds[index].has_value();
    fn_def.generic_bounds.push_back(has_bound ? frontend::lowering::format_type_text(*let.generic_bounds[index])
                                              : std::string{});
  }
  fn_def.return_type_name = frontend::lowering::format_type_text(let.return_type);
  fn_def.return_type_is_dict_string_any = is_dict_string_any_ir_type(let.return_type);
  fn_def.return_type_is_exact_uint64 = is_exact_uint64_ir_type(let.return_type);
  fn_def.return_type_is_exact_int64 = is_exact_int64_ir_type(let.return_type);
  fn_def.return_type_is_task_handle = is_nominal_ir_type(let.return_type, "TaskHandle");
  fn_def.return_type_is_random_generator = is_nominal_ir_type(let.return_type, "RandomGenerator");
  fn_def.return_type_is_file_handle = is_nominal_ir_type(let.return_type, "FileHandle");
  if (!fn_def.return_type_is_task_handle || !fn_def.return_type_is_random_generator || !fn_def.return_type_is_file_handle) {
    const auto body_summary = infer_audited_handle_return_summary(let.body);
    fn_def.return_type_is_task_handle = fn_def.return_type_is_task_handle || body_summary.is_task_handle;
    fn_def.return_type_is_random_generator =
        fn_def.return_type_is_random_generator || body_summary.is_random_generator;
    fn_def.return_type_is_file_handle = fn_def.return_type_is_file_handle || body_summary.is_file_handle;
  }
  fn_def.param_type_names.reserve(let.params.size());
  for (const auto& param : let.params) {
    fn_def.param_type_names.push_back(frontend::lowering::format_type_text(param.type));
  }
  fn_def.hidden_operator_params = static_cast<std::uint32_t>(let.hidden_operator_params);
  for (const auto& requirement : let.operator_requirements) {
    fn_def.operator_requirements.push_back(OperatorRequirementRecord{
        .op = requirement.op,
        .type_param = requirement.type_param,
        .signature = frontend::lowering::format_type_text(requirement.signature),
        .alternative_functions = requirement.alternative_functions,
    });
  }
  return fn_def;
}

void register_user_function_export(Module& bytecode_module, const IRLet& let, const CompileOptions& options,
                                   const std::string& full_name, const std::string& link_name,
                                   const std::uint32_t function_index) {
  if (!let_belongs_to_module(let, options)) {
    return;
  }
  bytecode_module.exports.push_back(ExportedSymbol{
      .name = full_name,
      .link_name = link_name,
      .kind = ExportKind::kFunction,
      .index = function_index,
      .builtin_name = {},
      .host_entry_check = let.host_entry_check,
  });
}

void register_user_function_definition(Module& bytecode_module, CompileState& state, const IRLet& let,
                                       const CompileOptions& options, const std::string& full_name,
                                       const std::string& link_name) {
  const auto fn_idx = static_cast<std::uint32_t>(bytecode_module.functions.size());
  bytecode_module.functions.push_back(make_function_def(let, link_name));

  state.function_idx[link_name] = fn_idx;
  state.register_public_function_name(full_name, fn_idx);
  register_user_function_export(bytecode_module, let, options, full_name, link_name, fn_idx);
}

void seed_imported_exports(Module& bytecode_module, CompileState& state, const CompileOptions& options) {
  for (const Module& imported_module : options.imported_modules) {
    for (const auto& [name, actual_link_name, kind, index, builtin_name, host_entry_check] : imported_module.exports) {
      if (kind == ExportKind::kBuiltinAlias) {
        state.builtin_alias.emplace(name, builtin_name);
        continue;
      }

      const auto link_name = actual_link_name.empty() ? name : actual_link_name;
      if (state.function_idx.contains(link_name)) {
        continue;
      }
      if (index >= imported_module.functions.size()) {
        continue;
      }

      const auto& imported_fn = imported_module.functions[index];
      const auto fn_idx = static_cast<std::uint32_t>(bytecode_module.functions.size());
      FunctionDef imported_placeholder{};
      imported_placeholder.name = link_name;
      imported_placeholder.arity = imported_fn.arity;
      imported_placeholder.has_variadic_tail = imported_fn.has_variadic_tail;
      imported_placeholder.is_import_placeholder = true;
      imported_placeholder.generic_params = imported_fn.generic_params;
      imported_placeholder.generic_bounds = imported_fn.generic_bounds;
      imported_placeholder.param_type_names = imported_fn.param_type_names;
      imported_placeholder.return_type_name = imported_fn.return_type_name;
      imported_placeholder.return_type_is_dict_string_any = imported_fn.return_type_is_dict_string_any;
      imported_placeholder.return_type_is_exact_uint64 = imported_fn.return_type_is_exact_uint64;
      imported_placeholder.return_type_is_exact_int64 = imported_fn.return_type_is_exact_int64;
      imported_placeholder.return_type_is_task_handle = imported_fn.return_type_is_task_handle;
      imported_placeholder.return_type_is_random_generator = imported_fn.return_type_is_random_generator;
      imported_placeholder.return_type_is_file_handle = imported_fn.return_type_is_file_handle;
      imported_placeholder.hidden_operator_params = imported_fn.hidden_operator_params;
      imported_placeholder.operator_requirements = imported_fn.operator_requirements;
      imported_placeholder.is_value_binding = imported_fn.is_value_binding;
      bytecode_module.functions.push_back(std::move(imported_placeholder));
      state.function_idx[link_name] = fn_idx;
      state.register_public_function_name(name, fn_idx);
    }
  }
}

// Forward declaration.
auto emit_expr(const IRExpr& expr, std::vector<Instruction>& out, const LocalSlots& locals, CompileState& state,
               Module& bytecode_module) -> EmitResult;
auto emit_tuple_items(const IRTupleExpr& tuple_expr, std::vector<Instruction>& out, const LocalSlots& locals,
                      CompileState& state, Module& bytecode_module) -> EmitResult;
auto count_inline_closures(const IRExpr& expr) -> std::size_t;

void initialize_module_header(Module& bytecode_module, CompileState& state, const CompileOptions& options) {
  state.enable_experimental_builtin_reductions = options.enable_experimental_builtin_reductions;
  state.inline_functions = options.inline_functions;
  bytecode_module.header.module_name = options.module_name.value_or(
      options.source_path.has_value() ? options.source_path->stem().string() : std::string{});
  bytecode_module.header.source_path = options.source_path.has_value() ? options.source_path->string() : std::string{};
  bytecode_module.header.source_hash = options.source_text.has_value() ? hash_text(*options.source_text) : 0;
}

void seed_program_dependencies(Module& bytecode_module, const IRProgram& program) {
  std::unordered_set<std::string> seen_dependencies;
  for (const auto& [module_name, _span] : program.imports) {
    if (!seen_dependencies.insert(module_name).second) {
      continue;
    }
    bytecode_module.dependencies.push_back(ModuleDependency{
        .module_name = module_name,
        .is_symbolic = is_symbolic_import(module_name),
    });
  }
}

// True when a declaration was written in the module being compiled rather than seeded from an import.
auto declared_in_module(const std::optional<frontend::diag::SourceSpan>& span, const CompileOptions& options) -> bool {
  return !options.source_path.has_value() || !span.has_value() ||
         span->source_name() == options.source_path->string();
}

void record_type_declarations(Module& bytecode_module, const IRProgram& program, const CompileOptions& options) {
  for (const auto& type_decl : program.type_decls) {
    if (!declared_in_module(type_decl.span, options)) {
      continue;
    }
    bytecode_module.type_decls.push_back(TypeDeclRecord{
        .name = type_decl.name,
        .generic_params = type_decl.generic_params,
        .is_builtin = type_decl.is_builtin,
        .target = type_decl.is_builtin ? std::string{} : frontend::lowering::format_type_text(type_decl.target),
    });
  }
  for (const auto& alias_decl : program.alias_decls) {
    if (!declared_in_module(alias_decl.span, options)) {
      continue;
    }
    bytecode_module.alias_decls.push_back(AliasDeclRecord{
        .name = alias_decl.name,
        .target = frontend::lowering::format_type_text(alias_decl.target),
    });
  }
}

void register_let_definitions(Module& bytecode_module, CompileState& state, const IRProgram& program,
                              const CompileOptions& options) {
  for (const auto& let : program.lets) {
    const std::string full_name = common::full_symbol_name(let.qualifier, let.name);
    const std::string link_name = internal_symbol_name(let);

    if (let.is_builtin) {
      register_builtin_alias_definition(bytecode_module, state, let, options, full_name);
      continue;
    }

    register_user_function_definition(bytecode_module, state, let, options, full_name, link_name);
  }
}

void reserve_inline_closure_capacity(Module& bytecode_module, const IRProgram& program) {
  std::size_t closure_count = 0;
  for (const auto& let : program.lets) {
    if (!let.is_builtin && let.body.has_value()) {
      closure_count += count_inline_closures(*let.body);
    }
  }
  for (const auto& [expr, span] : program.expressions) {
    (void)span;
    closure_count += count_inline_closures(expr);
  }
  bytecode_module.functions.reserve(bytecode_module.functions.size() + closure_count);
  bytecode_module.closures.reserve(closure_count);
}

auto make_parameter_local_slots(const IRLet& let) -> LocalSlots {
  LocalSlots local_slots;
  for (std::uint32_t param_index = 0; param_index < static_cast<std::uint32_t>(let.params.size()); ++param_index) {
    local_slots[let.params[param_index].name] = param_index;
  }
  return local_slots;
}

auto next_local_slot(const LocalSlots& locals) -> std::uint32_t {
  std::uint32_t next_slot = 0;
  for (const auto& [name, slot] : locals) {
    (void)name;
    next_slot = std::max(next_slot, slot + 1U);
  }
  return next_slot;
}

auto compile_single_function_body(Module& bytecode_module, CompileState& state, const IRLet& let) -> EmitResult {
  const std::string full_name = internal_symbol_name(let);
  const LocalSlots local_slots = make_parameter_local_slots(let);

  const auto current_fn_idx = state.function_idx.at(full_name);
  const auto previous_fn_idx = state.current_function_idx;
  state.current_function_idx = current_fn_idx;
  if (auto emit_result = emit_expr(*let.body, bytecode_module.functions[current_fn_idx].instructions, local_slots,
                                   state, bytecode_module);
      !emit_result) {
    state.current_function_idx = previous_fn_idx;
    return emit_result;
  }
  state.current_function_idx = previous_fn_idx;
  bytecode_module.functions[current_fn_idx].instructions.push_back(
      Instruction{.opcode = Opcode::kReturn, .operand = 0});
  return {};
}

auto compile_single_top_level_expression(Module& bytecode_module, CompileState& state, const IRExpr& expr)
    -> EmitResult {
  if (auto emit_result = emit_expr(expr, bytecode_module.instructions, {}, state, bytecode_module); !emit_result) {
    return emit_result;
  }
  bytecode_module.instructions.push_back(Instruction{.opcode = Opcode::kPop, .operand = 0});
  return {};
}

// A native function's body passes its parameters, as one tuple, to the extension function and returns the result.
auto compile_native_function_body(Module& bytecode_module, CompileState& state, const IRLet& let) -> EmitResult {
  if (!let.native_return_check.has_value()) {
    return tl::unexpected(CompileError{.message = "native function '" + common::full_symbol_name(let.qualifier, let.name) +
                                                  "' has no return check; the type checker did not run"});
  }
  auto& instructions = bytecode_module.functions[state.function_idx.at(internal_symbol_name(let))].instructions;
  const auto param_count = static_cast<std::int64_t>(let.params.size());
  for (std::int64_t slot = 0; slot < param_count; ++slot) {
    instructions.push_back(Instruction{.opcode = Opcode::kLoadLocal, .operand = slot});
  }
  instructions.push_back(Instruction{.opcode = Opcode::kBuildTuple, .operand = param_count});
  const auto native_index = static_cast<std::int64_t>(bytecode_module.native_functions.size());
  bytecode_module.native_functions.push_back(NativeFunctionRecord{
      .symbol = common::full_symbol_name(let.qualifier, let.name),
      .return_check = *let.native_return_check,
  });
  instructions.push_back(Instruction{.opcode = Opcode::kCallNative, .operand = native_index});
  instructions.push_back(Instruction{.opcode = Opcode::kReturn, .operand = 0});
  return {};
}

auto compile_function_bodies(Module& bytecode_module, CompileState& state, const IRProgram& program) -> EmitResult {
  // Every function's IR is known before any body is compiled, so a call can inline a function defined after it.
  for (const auto& let : program.lets) {
    if (!let.is_builtin && !let.is_native && !let.is_value_binding && let.body.has_value()) {
      state.function_lets[state.function_idx.at(internal_symbol_name(let))] = &let;
    }
  }
  for (const auto& let : program.lets) {
    if (let.is_builtin)
      continue;

    auto emit_result = let.is_native ? compile_native_function_body(bytecode_module, state, let)
                                     : compile_single_function_body(bytecode_module, state, let);
    if (!emit_result) {
      return emit_result;
    }
  }
  return {};
}

auto compile_top_level_expressions(Module& bytecode_module, CompileState& state, const IRProgram& program)
    -> EmitResult {
  for (const auto& [expr, span] : program.expressions) {
    (void)span;
    if (auto emit_result = compile_single_top_level_expression(bytecode_module, state, expr); !emit_result) {
      return emit_result;
    }
  }
  return {};
}

auto count_inline_closures(const IRExpr& expr) -> std::size_t {
  return std::visit(
      common::overloaded{[](const IRFlowExpr& flow) -> std::size_t { return count_inline_closures(*flow.lhs); },
                         [](const IRTupleExpr& tuple) -> std::size_t {
                           std::size_t total = 0;
                           for (const auto& item : tuple.items) {
                             total += count_inline_closures(*item);
                           }
                           return total;
                         },
                         [](const IRClosureExprBox& closure_ptr) -> std::size_t {
                           return 1U + count_inline_closures(*closure_ptr->body);
                         },
                         [](const IRBlockExprBox& block_ptr) -> std::size_t {
                           std::size_t total = 0;
                           for (const auto& item : block_ptr->items) {
                             std::visit(common::overloaded{[&](const IRLocalLet& local_let) -> void {
                                                             total += count_inline_closures(*local_let.expr);
                                                           },
                                                           [&](const IRBlockExprStatement& stmt) -> void {
                                                             total += count_inline_closures(*stmt.expr);
                                                           }},
                                        item);
                           }
                           total += count_inline_closures(*block_ptr->result);
                           return total;
                         },
                         [](auto&&) -> std::size_t { return 0; }},
      expr.node);
}

auto expr_is_known_user_function_ref(const IRExpr& expr, const CompileState& state) -> bool {
  return std::visit(common::overloaded{[&](const IRNameRef& name_ref) -> bool {
                                         return !name_ref.materialize_as_value &&
                                                state.function_idx.contains(target_symbol_name(name_ref));
                                       },
                                       [](const auto&) -> bool { return false; }},
                    expr.node);
}

auto expr_is_known_builtin_function_ref(const IRExpr& expr, const CompileState& state) -> bool {
  return std::visit(
      common::overloaded{[&](const IRNameRef& name_ref) -> bool {
                           if (name_ref.materialize_as_value) {
                             return false;
                           }
                           const std::string full_name = common::full_symbol_name(name_ref.qualifier, name_ref.name);

                           if (name_ref.qualifier.has_value() &&
                               (*name_ref.qualifier == "Std" || name_ref.qualifier->starts_with("Std."))) {
                             return true;
                           }
                           return state.builtin_alias.contains(full_name);
                         },
                         [](const auto&) -> bool { return false; }},
      expr.node);
}

auto expr_is_known_callable_ref(const IRExpr& expr, const CompileState& state) -> bool {
  return expr_is_known_user_function_ref(expr, state) || expr_is_known_builtin_function_ref(expr, state);
}

auto user_function_index_from_target(const IRCallTarget& target, const CompileState& state)
    -> std::optional<std::uint32_t> {
  return std::visit(
      common::overloaded{[&](const IRNameRef& name_ref) -> std::optional<std::uint32_t> {
                           if (const auto it = state.function_idx.find(target_symbol_name(name_ref));
                               it != state.function_idx.end()) {
                             return it->second;
                           }
                           return std::nullopt;
                         },
                         [](const IROperatorRef&) -> std::optional<std::uint32_t> { return std::nullopt; }},
      target);
}

auto can_emit_native_branch_call(const std::string& full_name, const IRTupleExpr& tuple, const CompileState& state)
    -> bool {
  if (full_name != "Std.Branch" || tuple.items.size() != 4) {
    return false;
  }
  // Keep this strict: only lower when both function positions are known
  // callable symbols. Callable locals still use builtin dispatch.
  return expr_is_known_callable_ref(*tuple.items[2], state) && expr_is_known_callable_ref(*tuple.items[3], state);
}

// Call-target emission

auto emit_call_target(const IRCallTarget& target, std::vector<Instruction>& out, const LocalSlots& locals,
                      CompileState& state, Module& bytecode_module,
                      const std::uint32_t builtin_static_argument_count = kUnknownBuiltinStaticArgumentCount,
                      const std::vector<BuiltinCallableSlotRecord>& builtin_callable_slots = {}) -> EmitResult {
  (void)locals;
  // Helper: emit the call of a resolved BuiltinId.
  auto emit_builtin = [&](const std::string& symbol_key) -> EmitResult {
    const auto builtin_id = vm::builtin_id_from_symbol_key(symbol_key);
    if (!builtin_id.has_value()) {
      return tl::unexpected(make_err("Unknown builtin in bytecode compiler: '" + symbol_key + "'."));
    }
    emit_builtin_call(bytecode_module, state, out, *builtin_id, vm::BuiltinCallShape::kTupleExpanded,
                      builtin_static_argument_count, builtin_callable_slots);
    return {};
  };

  return std::visit(
      common::overloaded{
          [&](const IROperatorRef& op_ref) -> EmitResult {
            // The type checker replaces every operator target with a reference to its Std builtin.
            return tl::unexpected(make_err("Operator '" + op_ref.op +
                                           "' reached the bytecode compiler without being resolved by the type checker."));
          },
          [&](const IRNameRef& name_ref) -> EmitResult {
            const std::string full_name = common::full_symbol_name(name_ref.qualifier, name_ref.name);
            const std::string target_name = target_symbol_name(name_ref);

            // Qualified Std.* builtin (e.g. Std.Add, Std.Math.Floor).
            if (name_ref.qualifier.has_value() &&
                (*name_ref.qualifier == "Std" || name_ref.qualifier->starts_with("Std."))) {
              return emit_builtin(target_name);
            }

            // User-defined function (check by full name first, then short name).
            if (const auto fn_it = state.function_idx.find(target_name); fn_it != state.function_idx.end()) {
              out.push_back(
                  Instruction{.opcode = Opcode::kCallUserFunc, .operand = static_cast<std::int64_t>(fn_it->second)});
              return {};
            }

            // Builtin alias introduced by an is_builtin let (unqualified import).
            if (const auto alias_it = state.builtin_alias.find(full_name); alias_it != state.builtin_alias.end()) {
              return emit_builtin(alias_it->second);
            }

            return tl::unexpected(make_err("Unsupported call target in bytecode compiler: '" + full_name + "'."));
          }},
      target);
}

auto emit_tuple_items(const IRTupleExpr& tuple_expr, std::vector<Instruction>& out, const LocalSlots& locals,
                      CompileState& state, Module& bytecode_module) -> EmitResult {
  for (const auto& item : tuple_expr.items) {
    if (auto emit_result = emit_expr(*item, out, locals, state, bytecode_module); !emit_result) {
      return emit_result;
    }
  }
  return {};
}

// A direct call passes the item of a one-item parameter list; a tuple-expanded call passes its parameter list as the
// argument tuple.
auto adapted_user_flow_lhs_expr(const IRFlowExpr& flow) -> const IRExpr& {
  if (flow.call_shape == IRFlowCallShape::kDirectValue) {
    if (const auto* item = single_parameter_list_item(*flow.lhs); item != nullptr) {
      return **item;
    }
  }
  return *flow.lhs;
}

// A call of a non-variadic user function of two or more parameters, written with one argument per parameter: the
// arguments go on the stack and kCallUserFuncArgs binds them, without an argument tuple.
auto try_emit_user_args_call_fast_path(const IRFlowExpr& flow, std::vector<Instruction>& out,
                                       const LocalSlots& locals, CompileState& state, Module& bytecode_module)
    -> tl::expected<bool, CompileError> {
  const auto user_fn_idx = user_function_index_from_target(flow.rhs, state);
  if (!user_fn_idx.has_value() || *user_fn_idx >= bytecode_module.functions.size()) {
    return false;
  }

  const auto& function = bytecode_module.functions[*user_fn_idx];
  if (function.has_variadic_tail || function.arity < 2U) {
    return false;
  }

  const auto& [node, span] = adapted_user_flow_lhs_expr(flow);
  const auto* tuple_expr = std::get_if<IRTupleExpr>(&node);
  if (tuple_expr == nullptr || tuple_expr->items.size() != function.arity || tuple_expr->has_spread()) {
    return false;
  }

  if (auto emit_result = emit_tuple_items(*tuple_expr, out, locals, state, bytecode_module); !emit_result) {
    return tl::unexpected(emit_result.error());
  }
  out.push_back(Instruction{.opcode = Opcode::kCallUserFuncArgs, .operand = static_cast<std::int64_t>(*user_fn_idx)});
  return true;
}

auto builtin_symbol_key_from_call_target(const IRCallTarget& target, const CompileState& state)
    -> std::optional<std::string> {
  return std::visit(common::overloaded{[&](const IRNameRef& name_ref) -> std::optional<std::string> {
                                         return builtin_symbol_key_from_name_ref(name_ref, state);
                                       },
                                       [](const IROperatorRef&) -> std::optional<std::string> { return std::nullopt; }},
                    target);
}

[[nodiscard]] auto builtin_symbol_matches(const std::string_view symbol_key, const std::string_view builtin_name)
    -> bool {
  if (const auto builtin_id = vm::builtin_id_from_symbol_key(symbol_key); builtin_id.has_value()) {
    return vm::builtin_name(*builtin_id) == builtin_name;
  }
  return symbol_key == builtin_name;
}

auto format_builtin_arity_expectation(const vm::BuiltinArityContract contract) -> std::string {
  switch (contract.kind) {
    case vm::BuiltinArityKind::kExact:
      return std::to_string(contract.primary);
    case vm::BuiltinArityKind::kVariadicMinimum:
      return std::to_string(contract.primary) + "+";
    case vm::BuiltinArityKind::kUnchecked:
      return "unknown";
  }
  return "unknown";
}

// A builtin that takes one argument receives the argument value itself, on every path: a direct call, a call through a
// function value, and a host call. A call written with a one-item parameter list passes that item.
auto builtin_takes_one_argument(const vm::BuiltinId builtin_id) -> bool {
  const auto contract = vm::builtin_arity_contract(builtin_id);
  return contract.kind == vm::BuiltinArityKind::kExact && contract.primary == 1U;
}

// Builtin pipeline adaptation must stay syntax-aware: each pipeline expression may unwrap only
// its own outermost singleton lhs layer once. Do not replace this with generic runtime probing or
// recursive singleton flattening, because genuine singleton tuple results from earlier stages are
// real values and must remain intact.
auto adapted_builtin_flow_lhs_expr(const fleaux::vm::BuiltinId builtin_id, const IRFlowExpr& flow) -> const IRExpr& {
  if (flow.call_shape == IRFlowCallShape::kDirectValue && builtin_takes_one_argument(builtin_id)) {
    if (const auto* item = single_parameter_list_item(*flow.lhs); item != nullptr) {
      return **item;
    }
  }
  return *flow.lhs;
}

auto validate_builtin_known_arity(const vm::BuiltinId builtin_id, const std::size_t arity) -> EmitResult {
  if (!fleaux::vm::builtin_has_arity_contract(builtin_id) || fleaux::vm::builtin_accepts_arity(builtin_id, arity)) {
    return {};
  }

  const auto contract = fleaux::vm::builtin_arity_contract(builtin_id);
  const std::string expectation = format_builtin_arity_expectation(contract);
  const bool plural_expected = contract.kind != vm::BuiltinArityKind::kExact || contract.primary != 1U;
  const std::string suffix = plural_expected ? " arguments" : " argument";
  return tl::unexpected(make_err("Builtin arity mismatch in bytecode compiler: '" +
                                 std::string(fleaux::vm::builtin_name(builtin_id)) + "' expects " + expectation +
                                 suffix + " but got " + std::to_string(arity) + "."));
}

auto known_builtin_call_arity(const vm::BuiltinId builtin_id, const IRFlowExpr& flow) -> std::optional<std::size_t> {
  const auto& [node, span] = adapted_builtin_flow_lhs_expr(builtin_id, flow);
  if (flow.call_shape == IRFlowCallShape::kDirectValue) {
    return 1U;
  }

  if (flow.call_shape == IRFlowCallShape::kTupleExpanded) {
    if (const auto* tuple = std::get_if<IRTupleExpr>(&node); tuple != nullptr) {
      return tuple->items.size();
    }
    return std::nullopt;
  }

  return std::visit(
      common::overloaded{[](const IRTupleExpr& tuple) -> std::optional<std::size_t> { return tuple.items.size(); },
                         [](const IRConstant&) -> std::optional<std::size_t> { return 1U; },
                         [](const IRClosureExprBox&) -> std::optional<std::size_t> { return 1U; },
                         [](const IRNameRef&) -> std::optional<std::size_t> { return std::nullopt; },
                         [](const IRFlowExpr&) -> std::optional<std::size_t> { return std::nullopt; },
                         [](const IRBlockExprBox&) -> std::optional<std::size_t> { return std::nullopt; }},
      flow.lhs->node);
}

auto recorded_builtin_static_argument_count(const std::optional<std::size_t> known_arity) -> std::uint32_t {
  if (!known_arity.has_value()) {
    return kUnknownBuiltinStaticArgumentCount;
  }
  return static_cast<std::uint32_t>(*known_arity);
}

auto builtin_call_shape_for_flow(const IRFlowExpr& flow) -> vm::BuiltinCallShape;

auto callable_slot_record_from_emitted_fragment(const std::uint32_t slot_index, const std::size_t fragment_begin,
                                                const std::vector<Instruction>& out, const Module& bytecode_module)
    -> std::optional<BuiltinCallableSlotRecord> {
  if (fragment_begin >= out.size()) {
    return std::nullopt;
  }

  const auto& tail = out.back();
  switch (tail.opcode) {
    case Opcode::kMakeUserFuncRef: {
      if (tail.operand < 0 || static_cast<std::size_t>(tail.operand) >= bytecode_module.functions.size()) {
        return std::nullopt;
      }
      const auto function_index = static_cast<std::uint32_t>(tail.operand);
      const auto& function = bytecode_module.functions[function_index];
      return BuiltinCallableSlotRecord{
          .slot_index = slot_index,
          .kind = BuiltinCallableSlotKind::kFunction,
          .target_index = function_index,
          .declared_arity = function.arity,
          .has_variadic_tail = function.has_variadic_tail,
      };
    }
    case Opcode::kMakeClosureRef: {
      if (tail.operand < 0 || static_cast<std::size_t>(tail.operand) >= bytecode_module.closures.size()) {
        return std::nullopt;
      }
      const auto closure_index = static_cast<std::uint32_t>(tail.operand);
      const auto& closure = bytecode_module.closures[closure_index];
      return BuiltinCallableSlotRecord{
          .slot_index = slot_index,
          .kind = BuiltinCallableSlotKind::kClosure,
          .target_index = closure_index,
          .declared_arity = closure.declared_arity,
          .has_variadic_tail = closure.declared_has_variadic_tail,
      };
    }
    case Opcode::kMakeBuiltinFuncRef: {
      const auto builtin_id = vm::builtin_id_from_operand(tail.operand);
      if (!builtin_id.has_value()) {
        return std::nullopt;
      }
      const auto contract = vm::builtin_arity_contract(*builtin_id);
      return BuiltinCallableSlotRecord{
          .slot_index = slot_index,
          .kind = BuiltinCallableSlotKind::kBuiltin,
          .target_index = static_cast<std::uint32_t>(*builtin_id),
          .declared_arity = contract.primary,
          .has_variadic_tail = contract.kind == vm::BuiltinArityKind::kVariadicMinimum,
      };
    }
    default:
      return std::nullopt;
  }
}

auto emit_expr_for_builtin_callable_slot(const IRExpr& expr, const std::uint32_t slot_index, std::vector<Instruction>& out,
                                         const LocalSlots& locals, CompileState& state, Module& bytecode_module,
                                         std::vector<BuiltinCallableSlotRecord>& callable_slots) -> EmitResult {
  const auto fragment_begin = out.size();
  if (auto emit_result = emit_expr(expr, out, locals, state, bytecode_module); !emit_result) {
    return emit_result;
  }
  if (auto record = callable_slot_record_from_emitted_fragment(slot_index, fragment_begin, out, bytecode_module);
      record.has_value()) {
    callable_slots.push_back(std::move(*record));
  }
  return {};
}

auto builtin_call_shape_for_expr(const IRExpr& expr) -> vm::BuiltinCallShape {
  if (const auto* tuple = std::get_if<IRTupleExpr>(&expr.node); tuple != nullptr) {
    return tuple->items.size() == 1U ? vm::BuiltinCallShape::kDirectValue : vm::BuiltinCallShape::kTupleExpanded;
  }
  return vm::BuiltinCallShape::kDirectValue;
}

auto builtin_call_shape_for_flow(const IRFlowExpr& flow) -> vm::BuiltinCallShape {
  switch (flow.call_shape) {
    case IRFlowCallShape::kDirectValue:
      return vm::BuiltinCallShape::kDirectValue;
    case IRFlowCallShape::kTupleExpanded:
      return vm::BuiltinCallShape::kTupleExpanded;
    case IRFlowCallShape::kImplicit:
      return builtin_call_shape_for_expr(*flow.lhs);
  }
  return builtin_call_shape_for_expr(*flow.lhs);
}

auto emit_builtin_symbol_flow(const std::string& symbol_key, const IRFlowExpr& flow, std::vector<Instruction>& out,
                              const LocalSlots& locals, CompileState& state, Module& bytecode_module) -> EmitResult {
  const auto builtin_id = vm::builtin_id_from_symbol_key(symbol_key);
  if (!builtin_id.has_value()) {
    return tl::unexpected(make_err("Unknown builtin in bytecode compiler: '" + symbol_key + "'."));
  }
  const IRExpr& adapted_lhs = adapted_builtin_flow_lhs_expr(*builtin_id, flow);
  const auto known_arity = known_builtin_call_arity(*builtin_id, flow);
  if (known_arity.has_value()) {
    if (auto validated = validate_builtin_known_arity(*builtin_id, *known_arity); !validated) {
      return validated;
    }
  }

  std::vector<BuiltinCallableSlotRecord> callable_slots;
  std::vector<BuiltinValueSlotRecord> value_slots;
  const auto callable_slot_indices = builtin_callable_slot_indices(*builtin_id);
  const auto value_slot_indices = builtin_value_slot_indices(*builtin_id);
  const auto call_shape = builtin_call_shape_for_flow(flow);
  const auto* tuple = std::get_if<IRTupleExpr>(&adapted_lhs.node);
  const auto try_record_value_slot = [&](const IRExpr& slot_expr, const std::uint32_t slot_index) {
    const auto summary_kind = builtin_value_slot_summary_kind(*builtin_id, slot_index);
    if (!summary_kind.has_value()) {
      return;
    }

    const auto* slot_flow = std::get_if<IRFlowExpr>(&slot_expr.node);
    if (slot_flow == nullptr) {
      return;
    }
    const auto* slot_target = std::get_if<IRNameRef>(&slot_flow->rhs);
    if (slot_target == nullptr) {
      return;
    }

    const auto target_name = target_symbol_name(*slot_target);
    const auto fn_it = state.function_idx.find(target_name);
    if (fn_it == state.function_idx.end() || fn_it->second >= bytecode_module.functions.size()) {
      return;
    }

    const auto function_index = fn_it->second;
    if (!function_matches_value_slot_summary(bytecode_module.functions[function_index], *summary_kind)) {
      return;
    }

    value_slots.push_back(BuiltinValueSlotRecord{
        .slot_index = slot_index,
        .summary_kind = *summary_kind,
        .source_kind = BuiltinValueSlotSourceKind::kFunctionReturn,
        .source_index = function_index,
    });
  };

  if (!value_slot_indices.empty()) {
    if (call_shape == vm::BuiltinCallShape::kDirectValue) {
      if (std::ranges::find(value_slot_indices, 0U) != value_slot_indices.end()) {
        try_record_value_slot(adapted_lhs, 0U);
      }
    } else if (tuple != nullptr) {
      for (const auto value_slot_index : value_slot_indices) {
        if (value_slot_index >= tuple->items.size()) {
          continue;
        }
        try_record_value_slot(*tuple->items[value_slot_index], value_slot_index);
      }
    }
  }
  if (!callable_slot_indices.empty()) {
    if (call_shape == vm::BuiltinCallShape::kDirectValue) {
      if (std::ranges::find(callable_slot_indices, 0U) != callable_slot_indices.end()) {
        if (auto emit_result = emit_expr_for_builtin_callable_slot(adapted_lhs, 0U, out, locals, state, bytecode_module,
                                                                   callable_slots);
            !emit_result) {
          return emit_result;
        }
      } else {
        if (auto emit_result = emit_expr(adapted_lhs, out, locals, state, bytecode_module); !emit_result) {
          return emit_result;
        }
      }
    } else if (tuple != nullptr) {
      for (std::uint32_t item_index = 0; item_index < static_cast<std::uint32_t>(tuple->items.size()); ++item_index) {
        if (std::ranges::find(callable_slot_indices, item_index) != callable_slot_indices.end()) {
          if (auto emit_result = emit_expr_for_builtin_callable_slot(*tuple->items[item_index], item_index, out, locals,
                                                                     state, bytecode_module, callable_slots);
              !emit_result) {
            return emit_result;
          }
        } else {
          if (auto emit_result = emit_expr(*tuple->items[item_index], out, locals, state, bytecode_module); !emit_result) {
            return emit_result;
          }
        }
      }
    }

    if (call_shape == vm::BuiltinCallShape::kTupleExpanded && tuple != nullptr) {
      out.push_back(Instruction{.opcode = Opcode::kBuildTuple,
                                .operand = static_cast<std::int64_t>(tuple->items.size())});
    }
  }

  if (tuple != nullptr) {
    if (tuple->items.size() == 2) {
      if (const auto opcode = binary_intrinsic_builtin_to_opcode(symbol_key)) {
        if (!callable_slot_indices.empty()) {
          return tl::unexpected(make_err("Callable-slot metadata path unexpectedly reached intrinsic binary lowering."));
        }
        if (auto emit_result = emit_tuple_items(*tuple, out, locals, state, bytecode_module); !emit_result) {
          return emit_result;
        }
        out.push_back(Instruction{.opcode = *opcode, .operand = 0});
        return {};
      }
    }

    if (tuple->items.size() == 1) {
      if (const auto opcode = unary_intrinsic_builtin_to_opcode(symbol_key)) {
        if (!callable_slot_indices.empty()) {
          return tl::unexpected(make_err("Callable-slot metadata path unexpectedly reached intrinsic unary lowering."));
        }
        if (auto emit_result = emit_expr(*tuple->items[0], out, locals, state, bytecode_module); !emit_result) {
          return emit_result;
        }
        out.push_back(Instruction{.opcode = *opcode, .operand = 0});
        return {};
      }
    }
  } else if (const auto opcode = unary_intrinsic_builtin_to_opcode(symbol_key)) {
    if (!callable_slot_indices.empty()) {
      return tl::unexpected(make_err("Callable-slot metadata path unexpectedly reached direct intrinsic unary lowering."));
    }
    if (auto emit_result = emit_expr(adapted_lhs, out, locals, state, bytecode_module); !emit_result) {
      return emit_result;
    }
    out.push_back(Instruction{.opcode = *opcode, .operand = 0});
    return {};
  }

  if (callable_slot_indices.empty()) {
    if (auto emit_result = emit_expr(adapted_lhs, out, locals, state, bytecode_module); !emit_result) {
      return emit_result;
    }
  }
  emit_builtin_call(bytecode_module, state, out, *builtin_id, call_shape,
                    recorded_builtin_static_argument_count(known_arity), callable_slots, value_slots);
  return {};
}

auto int64_constant_value(const IRExpr& expr) -> std::optional<std::int64_t> {
  const auto* constant = std::get_if<IRConstant>(&expr.node);
  if (constant == nullptr) {
    return std::nullopt;
  }
  if (const auto* int_value = std::get_if<std::int64_t>(&constant->val); int_value != nullptr) {
    return *int_value;
  }
  return std::nullopt;
}

auto is_zero_integer_constant(const IRExpr& expr) -> bool {
  const auto* constant = std::get_if<IRConstant>(&expr.node);
  if (constant == nullptr) {
    return false;
  }
  if (const auto* int_value = std::get_if<std::int64_t>(&constant->val); int_value != nullptr) {
    return *int_value == 0;
  }
  if (const auto* uint_value = std::get_if<std::uint64_t>(&constant->val); uint_value != nullptr) {
    return *uint_value == 0U;
  }
  return false;
}

auto is_null_constant(const IRExpr& expr) -> bool {
  const auto* constant = std::get_if<IRConstant>(&expr.node);
  return constant != nullptr && std::holds_alternative<std::monostate>(constant->val);
}

auto emit_builtin_symbol_flow_for_tuple_prefix(
    const std::string& symbol_key, const IRTupleExpr& tuple,
    // NOLINTNEXTLINE We don't care that item_count is always 2 due to the current usage.
    const std::size_t item_count, std::vector<Instruction>& out, const LocalSlots& locals, CompileState& state,
    Module& bytecode_module) -> EmitResult {
  if (tuple.items.size() < item_count) {
    return tl::unexpected(make_err("Builtin reduction requires at least " + std::to_string(item_count) +
                                   " tuple items for '" + symbol_key + "'."));
  }
  for (std::size_t item_index = 0; item_index < item_count; ++item_index) {
    if (auto emit_result = emit_expr(*tuple.items[item_index], out, locals, state, bytecode_module); !emit_result) {
      return emit_result;
    }
  }

  // This is always 2 only because of the current usages, so the linter is right, but we don't care.
  // NOLINTNEXTLINE
  if (item_count == 2) {
    if (const auto opcode = binary_intrinsic_builtin_to_opcode(symbol_key)) {
      out.push_back(Instruction{.opcode = *opcode, .operand = 0});
      return {};
    }
  }

  const auto builtin_id = vm::builtin_id_from_symbol_key(symbol_key);
  if (!builtin_id.has_value()) {
    return tl::unexpected(make_err("Unknown builtin in bytecode compiler: '" + symbol_key + "'."));
  }
  out.push_back(Instruction{.opcode = Opcode::kBuildTuple, .operand = static_cast<std::int64_t>(item_count)});
  emit_builtin_call(bytecode_module, state, out, *builtin_id, vm::BuiltinCallShape::kTupleExpanded,
                    static_cast<std::uint32_t>(item_count));
  return {};
}

auto try_emit_experimental_builtin_reduction(const IRFlowExpr& flow, const std::string& outer_symbol_key,
                                             std::vector<Instruction>& out, const LocalSlots& locals,
                                             CompileState& state, Module& bytecode_module)
    -> tl::expected<bool, CompileError> {
  if (!state.enable_experimental_builtin_reductions) {
    return false;
  }

  const auto* inner_flow = std::get_if<IRFlowExpr>(&flow.lhs->node);
  const auto inner_symbol_key =
      inner_flow != nullptr ? builtin_symbol_key_from_call_target(inner_flow->rhs, state) : std::nullopt;

  const auto emit_reduced = [&](const std::string& reduced_symbol_key) -> tl::expected<bool, CompileError> {
    if (inner_flow == nullptr) {
      return false;
    }
    if (auto emit_result =
            emit_builtin_symbol_flow(reduced_symbol_key, *inner_flow, out, locals, state, bytecode_module);
        !emit_result) {
      return tl::unexpected(emit_result.error());
    }
    return true;
  };

  if (inner_symbol_key.has_value()) {
    const bool is_trim_chain =
        (outer_symbol_key == "Std.String.TrimStart" && *inner_symbol_key == "Std.String.TrimEnd") ||
        (outer_symbol_key == "Std.String.TrimEnd" && *inner_symbol_key == "Std.String.TrimStart");
    if (is_trim_chain) {
      return emit_reduced("Std.String.Trim");
    }

    if (outer_symbol_key == "Std.Length") {
      if (*inner_symbol_key == "Std.Array.Shape") {
        return emit_reduced("Std.Array.Rank");
      }
      if (*inner_symbol_key == "Std.Dict.Keys" || *inner_symbol_key == "Std.Dict.Values" ||
          *inner_symbol_key == "Std.Dict.Entries") {
        return emit_reduced("Std.Dict.Length");
      }
    }

    if (outer_symbol_key == "Std.Result.Tag") {
      if (*inner_symbol_key == "Std.Result.Ok" || *inner_symbol_key == "Std.Result.Err") {
        return emit_reduced("Std.Result.IsOk");
      }
    }

    if (outer_symbol_key == "Std.Not") {
      if (*inner_symbol_key == "Std.Result.Tag" || *inner_symbol_key == "Std.Result.IsOk") {
        return emit_reduced("Std.Result.IsErr");
      }
    }
  }

  if (outer_symbol_key == "Std.NotEqual") {
    const auto* compare_tuple = std::get_if<IRTupleExpr>(&flow.lhs->node);
    if (compare_tuple == nullptr || compare_tuple->items.size() != 2) {
      return false;
    }

    const IRExpr& compare_lhs = *compare_tuple->items[0];
    const IRExpr& compare_rhs = *compare_tuple->items[1];
    std::optional<std::string> compared_symbol_key;

    const auto bind_compared_flow = [&](const IRExpr& candidate_expr, const IRExpr& candidate_constant) -> bool {
      const auto* candidate_flow = std::get_if<IRFlowExpr>(&candidate_expr.node);
      if (candidate_flow == nullptr) {
        return false;
      }
      const auto candidate_symbol_key = builtin_symbol_key_from_call_target(candidate_flow->rhs, state);
      if (!candidate_symbol_key.has_value()) {
        return false;
      }
      if ((builtin_symbol_matches(*candidate_symbol_key, "Std.String.Find") ||
           builtin_symbol_matches(*candidate_symbol_key, "Std.String.Regex.Find")) &&
          int64_constant_value(candidate_constant) == -1) {
        inner_flow = candidate_flow;
        compared_symbol_key = candidate_symbol_key;
        return true;
      }
      if (builtin_symbol_matches(*candidate_symbol_key, "Std.OS.Env") && is_null_constant(candidate_constant)) {
        inner_flow = candidate_flow;
        compared_symbol_key = candidate_symbol_key;
        return true;
      }
      return false;
    };

    if (!bind_compared_flow(compare_lhs, compare_rhs) && !bind_compared_flow(compare_rhs, compare_lhs)) {
      return false;
    }

    if (builtin_symbol_matches(*compared_symbol_key, "Std.String.Find")) {
      const auto* find_tuple = std::get_if<IRTupleExpr>(&inner_flow->lhs->node);
      if (find_tuple == nullptr || find_tuple->items.size() != 3 || !is_zero_integer_constant(*find_tuple->items[2])) {
        return false;
      }
      if (auto emit_result = emit_builtin_symbol_flow_for_tuple_prefix("Std.String.Contains", *find_tuple, 2, out,
                                                                       locals, state, bytecode_module);
          !emit_result) {
        return tl::unexpected(emit_result.error());
      }
      return true;
    }

    if (builtin_symbol_matches(*compared_symbol_key, "Std.String.Regex.Find")) {
      return emit_reduced("Std.String.Regex.IsMatch");
    }

    if (builtin_symbol_matches(*compared_symbol_key, "Std.OS.Env")) {
      return emit_reduced("Std.OS.HasEnv");
    }
  }

  return false;
}

auto emit_name_ref_expr(const IRNameRef& name_ref, std::vector<Instruction>& out, const LocalSlots& locals,
                        CompileState& state, Module& bytecode_module) -> EmitResult {
  const std::string full_name = common::full_symbol_name(name_ref.qualifier, name_ref.name);
  const std::string target_name = target_symbol_name(name_ref);

  if (!name_ref.qualifier.has_value()) {
    if (const auto it = locals.find(name_ref.name); it != locals.end()) {
      out.push_back(Instruction{.opcode = Opcode::kLoadLocal, .operand = static_cast<std::int64_t>(it->second)});
      return {};
    }
  }

  if (name_ref.materialize_as_value) {
    out.push_back(Instruction{.opcode = Opcode::kBuildTuple, .operand = 0});

    if (const auto fn_it = state.function_idx.find(target_name); fn_it != state.function_idx.end()) {
      out.push_back(Instruction{.opcode = Opcode::kCallUserFunc, .operand = static_cast<std::int64_t>(fn_it->second)});
      return {};
    }

    if (name_ref.qualifier.has_value() && (*name_ref.qualifier == "Std" || name_ref.qualifier->starts_with("Std."))) {
      const auto builtin_id = fleaux::vm::builtin_id_from_symbol_key(target_name);
      if (!builtin_id.has_value()) {
        return tl::unexpected(make_err("Unknown builtin in bytecode compiler: '" + target_name + "'."));
      }
      if (auto validated = validate_builtin_known_arity(*builtin_id, 0U); !validated) {
        return validated;
      }
      emit_builtin_call(bytecode_module, state, out, *builtin_id, vm::BuiltinCallShape::kTupleExpanded, 0U);
      return {};
    }

    if (const auto alias_it = state.builtin_alias.find(full_name); alias_it != state.builtin_alias.end()) {
      const auto builtin_id = fleaux::vm::builtin_id_from_name(alias_it->second);
      if (!builtin_id.has_value()) {
        return tl::unexpected(make_err("Unknown builtin in bytecode compiler: '" + alias_it->second + "'."));
      }
      if (auto validated = validate_builtin_known_arity(*builtin_id, 0U); !validated) {
        return validated;
      }
      emit_builtin_call(bytecode_module, state, out, *builtin_id, vm::BuiltinCallShape::kTupleExpanded, 0U);
      return {};
    }
  }

  if (const auto fn_it = state.function_idx.find(target_name); fn_it != state.function_idx.end()) {
    if (!name_ref.bound_operator_args.empty()) {
      // A value of a function with hidden operator parameters binds them in front, like a closure's captures.
      for (const auto& bound : name_ref.bound_operator_args) {
        if (auto emitted = emit_expr(*bound, out, locals, state, bytecode_module); !emitted) {
          return emitted;
        }
      }
      const auto bound_count = static_cast<std::uint32_t>(name_ref.bound_operator_args.size());
      const auto& function = bytecode_module.functions[fn_it->second];
      const auto closure_idx = static_cast<std::int64_t>(bytecode_module.closures.size());
      bytecode_module.closures.push_back(ClosureDef{.function_index = fn_it->second,
                                                    .capture_count = bound_count,
                                                    .declared_arity = function.arity - bound_count,
                                                    .declared_has_variadic_tail = function.has_variadic_tail});
      out.push_back(Instruction{.opcode = Opcode::kBuildTuple, .operand = static_cast<std::int64_t>(bound_count)});
      out.push_back(Instruction{.opcode = Opcode::kMakeClosureRef, .operand = closure_idx});
      return {};
    }
    out.push_back(Instruction{.opcode = Opcode::kMakeUserFuncRef, .operand = static_cast<std::int64_t>(fn_it->second)});
    return {};
  }

  if (name_ref.qualifier.has_value() && (*name_ref.qualifier == "Std" || name_ref.qualifier->starts_with("Std."))) {
    const auto builtin_id = fleaux::vm::builtin_id_from_symbol_key(target_name);
    if (!builtin_id.has_value()) {
      return tl::unexpected(make_err("Unknown builtin in bytecode compiler: '" + target_name + "'."));
    }
    out.push_back(Instruction{.opcode = Opcode::kMakeBuiltinFuncRef,
                              .operand = fleaux::vm::builtin_operand(*builtin_id,
                                                                      fleaux::vm::builtin_callable_call_shape(*builtin_id))});
    return {};
  }

  if (const auto alias_it = state.builtin_alias.find(full_name); alias_it != state.builtin_alias.end()) {
    const auto builtin_id = fleaux::vm::builtin_id_from_name(alias_it->second);
    if (!builtin_id.has_value()) {
      return tl::unexpected(make_err("Unknown builtin in bytecode compiler: '" + alias_it->second + "'."));
    }
    out.push_back(Instruction{.opcode = Opcode::kMakeBuiltinFuncRef,
                              .operand = fleaux::vm::builtin_operand(*builtin_id,
                                                                      fleaux::vm::builtin_callable_call_shape(*builtin_id))});
    return {};
  }

  return tl::unexpected(
      make_err("Name '" + full_name + "' used as a value is not supported in the bytecode compiler."));
}

auto make_closure_local_slots(const IRClosureExpr& closure) -> LocalSlots {
  LocalSlots closure_locals;
  std::uint32_t slot = 0;
  for (const auto& capture_name : closure.captures) {
    closure_locals[capture_name] = slot++;
  }
  for (const auto& param : closure.params) {
    closure_locals[param.name] = slot++;
  }
  return closure_locals;
}

auto emit_closure_expr(const IRClosureExpr& closure, std::vector<Instruction>& out, const LocalSlots& locals,
                       CompileState& state, Module& bytecode_module) -> EmitResult {
  const auto fn_idx = static_cast<std::uint32_t>(bytecode_module.functions.size());
  FunctionDef fn_def;
  fn_def.name = "__closure_" + std::to_string(fn_idx);
  fn_def.arity = static_cast<std::uint32_t>(closure.captures.size() + closure.params.size());
  fn_def.has_variadic_tail = !closure.params.empty() && closure.params.back().type.variadic;
  fn_def.return_type_name = frontend::lowering::format_type_text(closure.return_type);
  fn_def.return_type_is_dict_string_any = is_dict_string_any_ir_type(closure.return_type);
  fn_def.return_type_is_exact_uint64 = is_exact_uint64_ir_type(closure.return_type);
  fn_def.return_type_is_exact_int64 = is_exact_int64_ir_type(closure.return_type);
  fn_def.return_type_is_task_handle = is_nominal_ir_type(closure.return_type, "TaskHandle");
  fn_def.return_type_is_random_generator = is_nominal_ir_type(closure.return_type, "RandomGenerator");
  fn_def.return_type_is_file_handle = is_nominal_ir_type(closure.return_type, "FileHandle");
  if (!fn_def.return_type_is_task_handle || !fn_def.return_type_is_random_generator || !fn_def.return_type_is_file_handle) {
    const auto body_summary = infer_audited_handle_return_summary(closure.body);
    fn_def.return_type_is_task_handle = fn_def.return_type_is_task_handle || body_summary.is_task_handle;
    fn_def.return_type_is_random_generator =
        fn_def.return_type_is_random_generator || body_summary.is_random_generator;
    fn_def.return_type_is_file_handle = fn_def.return_type_is_file_handle || body_summary.is_file_handle;
  }

  const LocalSlots closure_locals = make_closure_local_slots(closure);

  const auto previous_fn_idx = state.current_function_idx;
  state.current_function_idx = fn_idx;
  if (auto emit_result = emit_expr(*closure.body, fn_def.instructions, closure_locals, state, bytecode_module);
      !emit_result) {
    state.current_function_idx = previous_fn_idx;
    return tl::unexpected(emit_result.error());
  }
  state.current_function_idx = previous_fn_idx;
  fn_def.instructions.push_back(Instruction{.opcode = Opcode::kReturn, .operand = 0});
  bytecode_module.functions.push_back(std::move(fn_def));

  const auto closure_idx = static_cast<std::uint32_t>(bytecode_module.closures.size());
  bytecode_module.closures.push_back(
      ClosureDef{.function_index = fn_idx,
                 .capture_count = static_cast<std::uint32_t>(closure.captures.size()),
                 .declared_arity = static_cast<std::uint32_t>(closure.params.size()),
                 .declared_has_variadic_tail = !closure.params.empty() && closure.params.back().type.variadic});

  for (const auto& capture_name : closure.captures) {
    const auto local_it = locals.find(capture_name);
    if (local_it == locals.end()) {
      return tl::unexpected(make_err("Unsupported closure capture in bytecode compiler: '" + capture_name + "'."));
    }
    out.push_back(Instruction{.opcode = Opcode::kLoadLocal, .operand = static_cast<std::int64_t>(local_it->second)});
  }
  out.push_back(
      Instruction{.opcode = Opcode::kBuildTuple, .operand = static_cast<std::int64_t>(closure.captures.size())});
  out.push_back(Instruction{.opcode = Opcode::kMakeClosureRef, .operand = static_cast<std::int64_t>(closure_idx)});
  return {};
}

auto emit_block_expr(const IRBlockExpr& block, std::vector<Instruction>& out, const LocalSlots& locals,
                     CompileState& state, Module& bytecode_module) -> EmitResult {
  LocalSlots block_locals = locals;
  std::uint32_t next_slot_index = next_local_slot(block_locals);

  for (const auto& item : block.items) {
    auto emit_item = std::visit(
        common::overloaded{
            [&](const IRLocalLet& local_let) -> EmitResult {
              if (auto emit_result = emit_expr(*local_let.expr, out, block_locals, state, bytecode_module);
                  !emit_result) {
                return emit_result;
              }
              const auto slot = next_slot_index++;
              out.push_back(Instruction{.opcode = Opcode::kStoreLocal, .operand = static_cast<std::int64_t>(slot)});
              block_locals[local_let.name] = slot;
              return {};
            },
            [&](const IRBlockExprStatement& stmt) -> EmitResult {
              if (auto emit_result = emit_expr(*stmt.expr, out, block_locals, state, bytecode_module); !emit_result) {
                return emit_result;
              }
              out.push_back(Instruction{.opcode = Opcode::kPop, .operand = 0});
              return {};
            }},
        item);
    if (!emit_item) {
      return emit_item;
    }
  }

  return emit_expr(*block.result, out, block_locals, state, bytecode_module);
}

// Branch, Loop, Map, Filter, Reduce, Match, and Apply in place
//
// When the functions passed to Std.Branch, Std.Loop, Std.Tuple.Map, Std.Match, or Std.Apply are written inline, or
// name a user function, the compiler runs them in the current function instead of making function values for the
// builtin to call: the argument goes into a local slot, and the code jumps between the bodies. Calls then stay in one
// interpreter loop. Std.Tuple.Filter and Std.Tuple.Reduce run in place with an inline closure only.

// Named functions inlined
//
// A call of a small named function of this module emits the function's body in place of kCallUserFunc: its arguments
// go into slots above the caller's, as an inline closure's do in place, and the body sees only its parameters. A call
// costs about 9 ns, most of it its own three instructions (kCallUserFunc, the read of the parameter, kReturn). Each
// function is still compiled as a function too, for function values, host calls, exports, imports, and recursion.

// The largest body inlined, in IR nodes, and the deepest inlining. With 32, the falling-sand program's bytecode grows
// by 61%, and the Std.Tuple.Map stress program's loop step, which a limit of 16 leaves a call, is inlined.
constexpr std::size_t kInlineBodyNodeLimit = 32U;
constexpr std::size_t kInlineDepthLimit = 3U;

// The number of IR nodes in an expression.
auto ir_node_count(const IRExpr& expr) -> std::size_t {
  return std::visit(
      common::overloaded{[](const IRFlowExpr& flow) -> std::size_t { return 1U + ir_node_count(*flow.lhs); },
                         [](const IRTupleExpr& tuple) -> std::size_t {
                           std::size_t total = 1U;
                           for (const auto& item : tuple.items) {
                             total += ir_node_count(*item);
                           }
                           return total;
                         },
                         [](const IRClosureExprBox& closure_ptr) -> std::size_t {
                           return 1U + (closure_ptr->body.has_value() ? ir_node_count(*closure_ptr->body) : 0U);
                         },
                         [](const IRBlockExprBox& block_ptr) -> std::size_t {
                           std::size_t total = 1U;
                           for (const auto& item : block_ptr->items) {
                             std::visit(
                                 common::overloaded{[&](const IRLocalLet& local_let) -> void { total += ir_node_count(*local_let.expr); },
                                                    [&](const IRBlockExprStatement& stmt) -> void { total += ir_node_count(*stmt.expr); }},
                                 item);
                           }
                           return total + ir_node_count(*block_ptr->result);
                         },
                         [](auto&&) -> std::size_t { return 1U; }},
      expr.node);
}

// The IR of the function at `index` when a call of it can be inlined: a function of this module with one or more
// parameters, none variadic, no hidden operator parameters, and a body of at most kInlineBodyNodeLimit nodes. Never
// the function being compiled, nor one whose body is being inlined, so no recursion is ever inlined.
auto inlinable_function(const std::uint32_t index, const CompileState& state) -> const IRLet* {
  if (!state.inline_functions) {
    return nullptr;
  }
  const auto found = state.function_lets.find(index);
  if (found == state.function_lets.end()) {
    return nullptr;
  }
  const IRLet& let = *found->second;
  if (let.params.empty() || let.hidden_operator_params != 0U || !let.operator_requirements.empty() || !let.body.has_value() ||
      std::ranges::any_of(let.params, [](const IRParam& param) -> bool { return param.type.variadic; }) ||
      ir_node_count(*let.body) > kInlineBodyNodeLimit) {
    return nullptr;
  }
  if (state.current_function_idx == index || std::ranges::find(state.inlining, index) != state.inlining.end() ||
      state.inlining.size() >= kInlineDepthLimit) {
    return nullptr;
  }
  return &let;
}

// Emits the body of the function at `index` with its parameters in `slots`, leaving its result on the stack.
auto emit_inlined_body(const IRLet& let, const std::uint32_t index, const std::span<const std::uint32_t> slots,
                       std::vector<Instruction>& out, CompileState& state, Module& bytecode_module) -> EmitResult {
  LocalSlots parameters;
  for (std::size_t position = 0; position < let.params.size(); ++position) {
    parameters[let.params[position].name] = slots[position];
  }
  state.inlining.push_back(index);
  auto emitted = emit_expr(*let.body, out, parameters, state, bytecode_module);
  state.inlining.pop_back();
  return emitted;
}

// A function that can run in place: an inline closure with one parameter, or a named user function. A Std.Match
// handler may also be an inline closure with no parameter.
struct InPlaceFunction {
  const IRClosureExpr* closure{nullptr};
  std::uint32_t function_index{0};
};

auto in_place_function(const IRExpr& expr, const LocalSlots& locals, const CompileState& state)
    -> std::optional<InPlaceFunction> {
  if (const auto* closure_box = std::get_if<IRClosureExprBox>(&expr.node); closure_box != nullptr) {
    const IRClosureExpr& closure = **closure_box;
    if (!closure.generic_params.empty() || closure.params.size() != 1U || closure.params.front().type.variadic ||
        !closure.body.has_value()) {
      return std::nullopt;
    }
    return InPlaceFunction{.closure = &closure};
  }
  if (const auto* name_ref = std::get_if<IRNameRef>(&expr.node); name_ref != nullptr) {
    // A local of the same name is a function value, not this function.
    if (name_ref->materialize_as_value || !name_ref->bound_operator_args.empty() ||
        (!name_ref->qualifier.has_value() && locals.contains(name_ref->name))) {
      return std::nullopt;
    }
    if (const auto it = state.function_idx.find(target_symbol_name(*name_ref)); it != state.function_idx.end()) {
      return InPlaceFunction{.function_index = it->second};
    }
  }
  return std::nullopt;
}

// Runs an inline closure in place on the values in `argument_slots`: one slot per parameter, or for a closure with no
// parameter the one slot its argument went into. The body sees what it would see as a function value: its captures,
// in the enclosing slots they come from, and its parameters. It leaves its result on the stack.
auto emit_closure_in_place(const IRClosureExpr& closure, const std::span<const std::uint32_t> argument_slots,
                           std::vector<Instruction>& out, const LocalSlots& locals, CompileState& state,
                           Module& bytecode_module) -> EmitResult {
  LocalSlots closure_locals;
  for (const auto& capture_name : closure.captures) {
    const auto local_it = locals.find(capture_name);
    if (local_it == locals.end()) {
      return tl::unexpected(make_err("Unsupported closure capture in bytecode compiler: '" + capture_name + "'."));
    }
    closure_locals.emplace(capture_name, local_it->second);
  }
  if (closure.params.empty()) {
    // The body's own locals take the slots above the highest one named here, so name the argument's slot, which is
    // above every slot the enclosing code uses. No name written in source is empty.
    closure_locals.emplace("", argument_slots.front());
  } else {
    for (std::size_t index = 0; index < closure.params.size(); ++index) {
      closure_locals[closure.params[index].name] = argument_slots[index];
    }
  }
  return emit_expr(*closure.body, out, closure_locals, state, bytecode_module);
}

// Runs a function in place on the value in `argument_slot`, leaving its result on the stack. A named function's body
// is inlined when it can be, with its parameter in that slot.
auto emit_in_place_call(const InPlaceFunction& function, const std::uint32_t argument_slot,
                        std::vector<Instruction>& out, const LocalSlots& locals, CompileState& state,
                        Module& bytecode_module) -> EmitResult {
  if (function.closure == nullptr) {
    if (const IRLet* let = inlinable_function(function.function_index, state); let != nullptr && let->params.size() == 1U) {
      return emit_inlined_body(*let, function.function_index, std::span{&argument_slot, 1U}, out, state, bytecode_module);
    }
    out.push_back(Instruction{.opcode = Opcode::kLoadLocal, .operand = static_cast<std::int64_t>(argument_slot)});
    out.push_back(
        Instruction{.opcode = Opcode::kCallUserFunc, .operand = static_cast<std::int64_t>(function.function_index)});
    return {};
  }
  return emit_closure_in_place(*function.closure, std::span{&argument_slot, 1U}, out, locals, state, bytecode_module);
}

// A Std.Match handler that can run in place: an inline closure with no parameter, or any in-place function.
auto in_place_match_handler(const IRExpr& expr, const LocalSlots& locals, const CompileState& state)
    -> std::optional<InPlaceFunction> {
  if (const auto* closure_box = std::get_if<IRClosureExprBox>(&expr.node); closure_box != nullptr) {
    if (const IRClosureExpr& closure = **closure_box;
        closure.generic_params.empty() && closure.params.empty() && closure.body.has_value()) {
      return InPlaceFunction{.closure = &closure};
    }
  }
  return in_place_function(expr, locals, state);
}

auto has_spread_item(const IRTupleExpr& tuple) -> bool {
  return std::ranges::any_of(tuple.spread_items, [](const bool spread) -> bool { return spread; });
}

auto jump_to_here(std::vector<Instruction>& out, const std::size_t jump_position) -> void {
  out[jump_position].operand = static_cast<std::int64_t>(out.size());
}

// (condition, value, when_true, when_false) -> Std.Branch:
//   <condition> <value> StoreLocal v  JumpIfNot F  <when_true v>  Jump E  F: <when_false v>  E:
auto try_emit_branch_in_place(const IRTupleExpr& tuple, std::vector<Instruction>& out, const LocalSlots& locals,
                              CompileState& state, Module& bytecode_module) -> tl::expected<bool, CompileError> {
  if (tuple.items.size() != 4U || has_spread_item(tuple)) {
    return false;
  }
  const auto when_true = in_place_function(*tuple.items[2], locals, state);
  const auto when_false = in_place_function(*tuple.items[3], locals, state);
  if (!when_true.has_value() || !when_false.has_value()) {
    return false;
  }

  for (std::size_t item = 0; item < 2U; ++item) {
    if (auto emit_result = emit_expr(*tuple.items[item], out, locals, state, bytecode_module); !emit_result) {
      return tl::unexpected(emit_result.error());
    }
  }
  // Above every slot the enclosing code uses; the bodies take their own slots above this one.
  const auto value_slot = next_local_slot(locals);
  out.push_back(Instruction{.opcode = Opcode::kStoreLocal, .operand = static_cast<std::int64_t>(value_slot)});
  const auto jump_to_false = out.size();
  out.push_back(Instruction{.opcode = Opcode::kJumpIfNot, .operand = 0});
  if (auto emit_result = emit_in_place_call(*when_true, value_slot, out, locals, state, bytecode_module);
      !emit_result) {
    return tl::unexpected(emit_result.error());
  }
  const auto jump_to_end = out.size();
  out.push_back(Instruction{.opcode = Opcode::kJump, .operand = 0});
  jump_to_here(out, jump_to_false);
  if (auto emit_result = emit_in_place_call(*when_false, value_slot, out, locals, state, bytecode_module);
      !emit_result) {
    return tl::unexpected(emit_result.error());
  }
  jump_to_here(out, jump_to_end);
  return true;
}

// (state, continue_func, step_func) -> Std.Loop:
//   <state> StoreLocal s  T: <continue_func s>  JumpIfNot E  <step_func s>  StoreLocal s  Jump T  E: LoadLocal s
auto try_emit_loop_in_place(const IRTupleExpr& tuple, std::vector<Instruction>& out, const LocalSlots& locals,
                            CompileState& state, Module& bytecode_module) -> tl::expected<bool, CompileError> {
  if (tuple.items.size() != 3U || has_spread_item(tuple)) {
    return false;
  }
  const auto continue_func = in_place_function(*tuple.items[1], locals, state);
  const auto step_func = in_place_function(*tuple.items[2], locals, state);
  if (!continue_func.has_value() || !step_func.has_value()) {
    return false;
  }

  if (auto emit_result = emit_expr(*tuple.items[0], out, locals, state, bytecode_module); !emit_result) {
    return tl::unexpected(emit_result.error());
  }
  const auto state_slot = next_local_slot(locals);
  out.push_back(Instruction{.opcode = Opcode::kStoreLocal, .operand = static_cast<std::int64_t>(state_slot)});
  const auto loop_top = out.size();
  if (auto emit_result = emit_in_place_call(*continue_func, state_slot, out, locals, state, bytecode_module);
      !emit_result) {
    return tl::unexpected(emit_result.error());
  }
  const auto jump_to_end = out.size();
  out.push_back(Instruction{.opcode = Opcode::kJumpIfNot, .operand = 0});
  if (auto emit_result = emit_in_place_call(*step_func, state_slot, out, locals, state, bytecode_module);
      !emit_result) {
    return tl::unexpected(emit_result.error());
  }
  out.push_back(Instruction{.opcode = Opcode::kStoreLocal, .operand = static_cast<std::int64_t>(state_slot)});
  out.push_back(Instruction{.opcode = Opcode::kJump, .operand = static_cast<std::int64_t>(loop_top)});
  jump_to_here(out, jump_to_end);
  out.push_back(Instruction{.opcode = Opcode::kLoadLocal, .operand = static_cast<std::int64_t>(state_slot)});
  return true;
}

// (items, func) -> Std.Tuple.Map, as a loop over the items whose results collect on the stack, tested at the bottom.
// kIterate stores the next item in x and pushes true, or pushes false when the items have run out, so the loop runs
// two instructions per item besides the function's. When the loop ends, the index is the number of items:
//   <items> StoreLocal s  PushConst 0u64 StoreLocal i  Jump C
//   B: <func x>
//   C: Iterate s  JumpIf B
//      LoadLocal i  BuildTupleCounted
auto try_emit_map_in_place(const IRTupleExpr& tuple, std::vector<Instruction>& out, const LocalSlots& locals,
                           CompileState& state, Module& bytecode_module) -> tl::expected<bool, CompileError> {
  if (tuple.items.size() != 2U || has_spread_item(tuple)) {
    return false;
  }
  const auto function = in_place_function(*tuple.items[1], locals, state);
  if (!function.has_value()) {
    return false;
  }

  if (auto emit_result = emit_expr(*tuple.items[0], out, locals, state, bytecode_module); !emit_result) {
    return tl::unexpected(emit_result.error());
  }
  // Above every slot the enclosing code uses. kIterate wants the index right after the items and the item after the
  // index; the item slot is the highest, so the function's body takes its own slots above all three.
  const auto items_slot = next_local_slot(locals);
  const auto index_slot = items_slot + 1U;
  const auto item_slot = items_slot + 2U;
  out.push_back(Instruction{.opcode = Opcode::kStoreLocal, .operand = static_cast<std::int64_t>(items_slot)});
  const auto zero = CompileState::intern_const(bytecode_module, ConstValue{std::uint64_t{0}});
  out.push_back(Instruction{.opcode = Opcode::kPushConst, .operand = static_cast<std::int64_t>(zero)});
  out.push_back(Instruction{.opcode = Opcode::kStoreLocal, .operand = static_cast<std::int64_t>(index_slot)});

  const auto jump_to_test = out.size();
  out.push_back(Instruction{.opcode = Opcode::kJump, .operand = 0});
  const auto body = out.size();
  if (auto emit_result = emit_in_place_call(*function, item_slot, out, locals, state, bytecode_module); !emit_result) {
    return tl::unexpected(emit_result.error());
  }
  jump_to_here(out, jump_to_test);
  out.push_back(Instruction{.opcode = Opcode::kIterate, .operand = static_cast<std::int64_t>(items_slot)});
  out.push_back(Instruction{.opcode = Opcode::kJumpIf, .operand = static_cast<std::int64_t>(body)});
  out.push_back(Instruction{.opcode = Opcode::kLoadLocal, .operand = static_cast<std::int64_t>(index_slot)});
  out.push_back(Instruction{.opcode = Opcode::kBuildTupleCounted, .operand = 0});
  return true;
}

// Filter and Reduce run in place only with an inline closure. Run in place, a named function is called with
// kCallUserFunc, which costs as much as the builtin's call of it, and the loop's own instructions come on top.

// The inline closure of `expr` when it has `param_count` parameters, none variadic, and no generic parameters.
auto inline_closure_with_params(const IRExpr& expr, const std::size_t param_count) -> const IRClosureExpr* {
  const auto* closure_box = std::get_if<IRClosureExprBox>(&expr.node);
  if (closure_box == nullptr) {
    return nullptr;
  }
  const IRClosureExpr& closure = **closure_box;
  if (!closure.generic_params.empty() || closure.params.size() != param_count || !closure.body.has_value() ||
      std::ranges::any_of(closure.params, [](const IRParam& param) -> bool { return param.type.variadic; })) {
    return nullptr;
  }
  return &closure;
}

// (items, pred) -> Std.Tuple.Filter, as the loop of an in-place Std.Tuple.Map that pushes the items the predicate
// keeps. They are counted by the stack's height, marked in m before the loop:
//   <items> StoreLocal s  PushConst 0u64 StoreLocal i  MarkStack m  Jump C
//   B: <pred x>  JumpIfNot C  LoadLocal x
//   C: Iterate s  JumpIf B
//      BuildTupleSinceMark m
auto try_emit_filter_in_place(const IRTupleExpr& tuple, std::vector<Instruction>& out, const LocalSlots& locals, CompileState& state,
                              Module& bytecode_module) -> tl::expected<bool, CompileError> {
  if (tuple.items.size() != 2U || has_spread_item(tuple)) {
    return false;
  }
  const IRClosureExpr* predicate = inline_closure_with_params(*tuple.items[1], 1U);
  if (predicate == nullptr) {
    return false;
  }

  if (auto emit_result = emit_expr(*tuple.items[0], out, locals, state, bytecode_module); !emit_result) {
    return tl::unexpected(emit_result.error());
  }
  // The mark goes below the loop's three slots, so that the item slot is the highest and the predicate's own locals,
  // which take the slots above it, cannot overwrite the mark.
  const auto mark_slot = next_local_slot(locals);
  const auto items_slot = mark_slot + 1U;
  const auto index_slot = items_slot + 1U;
  const auto item_slot = items_slot + 2U;
  out.push_back(Instruction{.opcode = Opcode::kStoreLocal, .operand = static_cast<std::int64_t>(items_slot)});
  const auto zero = CompileState::intern_const(bytecode_module, ConstValue{std::uint64_t{0}});
  out.push_back(Instruction{.opcode = Opcode::kPushConst, .operand = static_cast<std::int64_t>(zero)});
  out.push_back(Instruction{.opcode = Opcode::kStoreLocal, .operand = static_cast<std::int64_t>(index_slot)});
  out.push_back(Instruction{.opcode = Opcode::kMarkStack, .operand = static_cast<std::int64_t>(mark_slot)});

  const auto jump_to_test = out.size();
  out.push_back(Instruction{.opcode = Opcode::kJump, .operand = 0});
  const auto body = out.size();
  if (auto emit_result = emit_closure_in_place(*predicate, std::span{&item_slot, 1U}, out, locals, state, bytecode_module); !emit_result) {
    return tl::unexpected(emit_result.error());
  }
  const auto jump_past_keep = out.size();
  out.push_back(Instruction{.opcode = Opcode::kJumpIfNot, .operand = 0});
  out.push_back(Instruction{.opcode = Opcode::kLoadLocal, .operand = static_cast<std::int64_t>(item_slot)});
  jump_to_here(out, jump_to_test);
  jump_to_here(out, jump_past_keep);
  out.push_back(Instruction{.opcode = Opcode::kIterate, .operand = static_cast<std::int64_t>(items_slot)});
  out.push_back(Instruction{.opcode = Opcode::kJumpIf, .operand = static_cast<std::int64_t>(body)});
  out.push_back(Instruction{.opcode = Opcode::kBuildTupleSinceMark, .operand = static_cast<std::int64_t>(mark_slot)});
  return true;
}

// (items, initial, func) -> Std.Tuple.Reduce, as the loop of an in-place Std.Tuple.Map that stores each result of the
// function in the accumulator a:
//   <items> <initial> StoreLocal a  StoreLocal s  PushConst 0u64 StoreLocal i  Jump C
//   B: <func a x>  StoreLocal a
//   C: Iterate s  JumpIf B
//      LoadLocal a
auto try_emit_reduce_in_place(const IRTupleExpr& tuple, std::vector<Instruction>& out, const LocalSlots& locals, CompileState& state,
                              Module& bytecode_module) -> tl::expected<bool, CompileError> {
  if (tuple.items.size() != 3U || has_spread_item(tuple)) {
    return false;
  }
  const IRClosureExpr* function = inline_closure_with_params(*tuple.items[2], 2U);
  if (function == nullptr) {
    return false;
  }

  // The items and the initial value are both evaluated before either is stored: the initial value's own code may use
  // the slots above the enclosing ones.
  for (std::size_t item = 0; item < 2U; ++item) {
    if (auto emit_result = emit_expr(*tuple.items[item], out, locals, state, bytecode_module); !emit_result) {
      return tl::unexpected(emit_result.error());
    }
  }
  // The accumulator goes below the loop's three slots, as Filter's mark does.
  const auto accumulator_slot = next_local_slot(locals);
  const auto items_slot = accumulator_slot + 1U;
  const auto index_slot = items_slot + 1U;
  const auto item_slot = items_slot + 2U;
  out.push_back(Instruction{.opcode = Opcode::kStoreLocal, .operand = static_cast<std::int64_t>(accumulator_slot)});
  out.push_back(Instruction{.opcode = Opcode::kStoreLocal, .operand = static_cast<std::int64_t>(items_slot)});
  const auto zero = CompileState::intern_const(bytecode_module, ConstValue{std::uint64_t{0}});
  out.push_back(Instruction{.opcode = Opcode::kPushConst, .operand = static_cast<std::int64_t>(zero)});
  out.push_back(Instruction{.opcode = Opcode::kStoreLocal, .operand = static_cast<std::int64_t>(index_slot)});

  const auto jump_to_test = out.size();
  out.push_back(Instruction{.opcode = Opcode::kJump, .operand = 0});
  const auto body = out.size();
  const std::array<std::uint32_t, 2> argument_slots{accumulator_slot, item_slot};
  if (auto emit_result = emit_closure_in_place(*function, argument_slots, out, locals, state, bytecode_module); !emit_result) {
    return tl::unexpected(emit_result.error());
  }
  out.push_back(Instruction{.opcode = Opcode::kStoreLocal, .operand = static_cast<std::int64_t>(accumulator_slot)});
  jump_to_here(out, jump_to_test);
  out.push_back(Instruction{.opcode = Opcode::kIterate, .operand = static_cast<std::int64_t>(items_slot)});
  out.push_back(Instruction{.opcode = Opcode::kJumpIf, .operand = static_cast<std::int64_t>(body)});
  out.push_back(Instruction{.opcode = Opcode::kLoadLocal, .operand = static_cast<std::int64_t>(accumulator_slot)});
  return true;
}

auto is_match_wildcard_pattern(const IRConstant& constant) -> bool {
  const auto* text = std::get_if<std::string>(&constant.val);
  return text != nullptr && *text == common::kMatchWildcardSentinel;
}

// (subject, (pattern, handler), ...) -> Std.Match, as a chain of tests on the subject. It runs in place when every
// pattern is `_`, a constant, or a function that can run in place, and every handler can run in place. A constant
// pattern matches a subject equal to it, by the `==` the builtin uses; a function pattern is called on the subject.
// The patterns are tested in order, and none after the matching case is evaluated:
//   <subject> StoreLocal s
//   constant:  LoadLocal s  <pattern>  CmpEq  JumpIfNot N  <handler s>  Jump E  N:
//   function:  <pattern s>  JumpIfNot N  <handler s>  Jump E  N:
//   `_`:       <handler s>
//   no `_`:    MatchFailed
//   E:
auto try_emit_match_in_place(const IRTupleExpr& tuple, std::vector<Instruction>& out, const LocalSlots& locals,
                             CompileState& state, Module& bytecode_module) -> tl::expected<bool, CompileError> {
  if (tuple.items.size() < 2U || has_spread_item(tuple)) {
    return false;
  }
  struct InPlaceCase {
    // The pattern when it is a constant, and the function when it is one; neither for `_`.
    const IRExpr* constant{nullptr};
    std::optional<InPlaceFunction> predicate{};
    InPlaceFunction handler{};
  };
  std::vector<InPlaceCase> cases;
  cases.reserve(tuple.items.size() - 1U);
  for (std::size_t item = 1; item < tuple.items.size(); ++item) {
    const auto* case_tuple = std::get_if<IRTupleExpr>(&tuple.items[item]->node);
    if (case_tuple == nullptr || case_tuple->items.size() != 2U || has_spread_item(*case_tuple)) {
      return false;
    }
    const auto handler = in_place_match_handler(*case_tuple->items[1], locals, state);
    if (!handler.has_value()) {
      return false;
    }
    InPlaceCase match_case{.handler = *handler};
    const IRExpr& pattern = *case_tuple->items[0];
    if (const auto* constant = std::get_if<IRConstant>(&pattern.node); constant != nullptr) {
      if (!is_match_wildcard_pattern(*constant)) {
        match_case.constant = &pattern;
      }
    } else if (auto predicate = in_place_function(pattern, locals, state); predicate.has_value()) {
      match_case.predicate = predicate;
    } else {
      // A local, a value let, or a computed pattern may hold a value or a function: only the builtin can tell which.
      return false;
    }
    cases.push_back(match_case);
  }

  if (auto emit_result = emit_expr(*tuple.items[0], out, locals, state, bytecode_module); !emit_result) {
    return tl::unexpected(emit_result.error());
  }
  // Above every slot the enclosing code uses; the patterns and handlers take their own slots above this one.
  const auto subject_slot = next_local_slot(locals);
  out.push_back(Instruction{.opcode = Opcode::kStoreLocal, .operand = static_cast<std::int64_t>(subject_slot)});
  std::vector<std::size_t> jumps_to_end;
  bool has_wildcard = false;
  for (const auto& match_case : cases) {
    if (match_case.constant == nullptr && !match_case.predicate.has_value()) {
      // Lowering keeps `_` the last case.
      if (auto emit_result = emit_in_place_call(match_case.handler, subject_slot, out, locals, state, bytecode_module);
          !emit_result) {
        return tl::unexpected(emit_result.error());
      }
      has_wildcard = true;
      break;
    }
    if (match_case.constant != nullptr) {
      out.push_back(Instruction{.opcode = Opcode::kLoadLocal, .operand = static_cast<std::int64_t>(subject_slot)});
      if (auto emit_result = emit_expr(*match_case.constant, out, locals, state, bytecode_module); !emit_result) {
        return tl::unexpected(emit_result.error());
      }
      out.push_back(Instruction{.opcode = Opcode::kCmpEq, .operand = 0});
    } else if (auto emit_result =
                   emit_in_place_call(*match_case.predicate, subject_slot, out, locals, state, bytecode_module);
               !emit_result) {
      return tl::unexpected(emit_result.error());
    }
    const auto jump_to_next = out.size();
    out.push_back(Instruction{.opcode = Opcode::kJumpIfNot, .operand = 0});
    if (auto emit_result = emit_in_place_call(match_case.handler, subject_slot, out, locals, state, bytecode_module);
        !emit_result) {
      return tl::unexpected(emit_result.error());
    }
    jumps_to_end.push_back(out.size());
    out.push_back(Instruction{.opcode = Opcode::kJump, .operand = 0});
    jump_to_here(out, jump_to_next);
  }
  if (!has_wildcard) {
    out.push_back(Instruction{.opcode = Opcode::kMatchFailed, .operand = 0});
  }
  for (const auto jump : jumps_to_end) {
    jump_to_here(out, jump);
  }
  return true;
}

// The arguments of a call written as a list of two or more, with no spread: a call passes them as separate values,
// which is the same call as with their tuple. Null for any other argument expression.
auto apply_argument_list(const IRExpr& arguments) -> const IRTupleExpr* {
  const auto* list = std::get_if<IRTupleExpr>(&arguments.node);
  return list != nullptr && list->items.size() >= 2U && !list->has_spread() ? list : nullptr;
}

// (arguments, function) -> Std.Apply, a call of a function value. `arguments` is the argument itself for a function of
// one parameter, and the tuple of the arguments otherwise. A function written right there runs in place, and no
// function value is made:
//   named function:  <arguments> CallUserFunc F,  or  <a1 .. an> CallUserFuncArgs F  with one argument per parameter
//   inline closure:  <arguments> StoreLocal s  <body>,  or  <a1 .. an> StoreLocal s+n-1 .. StoreLocal s  <body>
// Any other function is called as a value:
//   <arguments> <function> CallFunctionValue,  or  <a1 .. an> <function> CallFunctionValueArgs n
// The arguments are evaluated before the function, as the tuple (arguments, function) evaluates them.
auto try_emit_apply(const IRTupleExpr& tuple, std::vector<Instruction>& out, const LocalSlots& locals,
                    CompileState& state, Module& bytecode_module) -> tl::expected<bool, CompileError> {
  if (tuple.items.size() != 2U || tuple.has_spread()) {
    return false;
  }
  const IRExpr& arguments = *tuple.items[0];
  const IRExpr& function = *tuple.items[1];
  const IRTupleExpr* argument_list = apply_argument_list(arguments);

  if (const auto* closure_box = std::get_if<IRClosureExprBox>(&function.node); closure_box != nullptr) {
    const IRClosureExpr& closure = **closure_box;
    const std::size_t param_count = closure.params.size();
    if (closure.generic_params.empty() && closure.body.has_value() &&
        std::ranges::none_of(closure.params, [](const IRParam& param) -> bool { return param.type.variadic; }) &&
        (param_count < 2U || (argument_list != nullptr && argument_list->items.size() == param_count))) {
      const std::uint32_t first_slot = next_local_slot(locals);
      std::vector<std::uint32_t> slots;
      if (param_count < 2U) {
        if (auto emitted = emit_expr(arguments, out, locals, state, bytecode_module); !emitted) {
          return tl::unexpected(emitted.error());
        }
        slots.push_back(first_slot);
      } else {
        // Every argument is evaluated before any is stored: an argument's own code may use the slots above the
        // enclosing ones.
        if (auto emitted = emit_tuple_items(*argument_list, out, locals, state, bytecode_module); !emitted) {
          return tl::unexpected(emitted.error());
        }
        for (std::size_t index = 0; index < param_count; ++index) {
          slots.push_back(first_slot + static_cast<std::uint32_t>(index));
        }
      }
      for (auto slot = slots.rbegin(); slot != slots.rend(); ++slot) {
        out.push_back(Instruction{.opcode = Opcode::kStoreLocal, .operand = static_cast<std::int64_t>(*slot)});
      }
      if (auto emitted = emit_closure_in_place(closure, slots, out, locals, state, bytecode_module); !emitted) {
        return tl::unexpected(emitted.error());
      }
      return true;
    }
  }

  if (const auto named = in_place_function(function, locals, state);
      named.has_value() && named->closure == nullptr) {
    const auto index = named->function_index;
    if (argument_list != nullptr && index < bytecode_module.functions.size()) {
      const auto& callee = bytecode_module.functions[index];
      if (!callee.has_variadic_tail && callee.arity == argument_list->items.size()) {
        if (auto emitted = emit_tuple_items(*argument_list, out, locals, state, bytecode_module); !emitted) {
          return tl::unexpected(emitted.error());
        }
        out.push_back(Instruction{.opcode = Opcode::kCallUserFuncArgs, .operand = static_cast<std::int64_t>(index)});
        return true;
      }
    }
    if (auto emitted = emit_expr(arguments, out, locals, state, bytecode_module); !emitted) {
      return tl::unexpected(emitted.error());
    }
    out.push_back(Instruction{.opcode = Opcode::kCallUserFunc, .operand = static_cast<std::int64_t>(index)});
    return true;
  }

  if (auto emitted = argument_list != nullptr
                         ? emit_tuple_items(*argument_list, out, locals, state, bytecode_module)
                         : emit_expr(arguments, out, locals, state, bytecode_module);
      !emitted) {
    return tl::unexpected(emitted.error());
  }
  if (auto emitted = emit_expr(function, out, locals, state, bytecode_module); !emitted) {
    return tl::unexpected(emitted.error());
  }
  out.push_back(argument_list != nullptr
                    ? Instruction{.opcode = Opcode::kCallFunctionValueArgs,
                                  .operand = static_cast<std::int64_t>(argument_list->items.size())}
                    : Instruction{.opcode = Opcode::kCallFunctionValue, .operand = 0});
  return true;
}

auto try_emit_named_flow_fast_path(const IRFlowExpr& flow, const IRNameRef& name_ref, std::vector<Instruction>& out,
                                   const LocalSlots& locals, CompileState& state, Module& bytecode_module)
    -> tl::expected<bool, CompileError> {
  if (const auto builtin_symbol_key = builtin_symbol_key_from_name_ref(name_ref, state);
      builtin_symbol_key.has_value()) {
    if (auto reduced =
            try_emit_experimental_builtin_reduction(flow, *builtin_symbol_key, out, locals, state, bytecode_module);
        !reduced) {
      return tl::unexpected(reduced.error());
    } else if (*reduced) {
      return true;
    }
  }

  const auto* tuple = std::get_if<IRTupleExpr>(&flow.lhs->node);
  if (tuple == nullptr) {
    if (const auto builtin_symbol_key = builtin_symbol_key_from_name_ref(name_ref, state);
        builtin_symbol_key.has_value()) {
      if (auto emit_result = emit_builtin_symbol_flow(*builtin_symbol_key, flow, out, locals, state, bytecode_module);
          !emit_result) {
        return tl::unexpected(emit_result.error());
      }
      return true;
    }
    return false;
  }

  const std::string full_name = common::full_symbol_name(name_ref.qualifier, name_ref.name);

  if (full_name == "Std.Branch" || full_name == "Std.Loop" || full_name == "Std.Tuple.Map" || full_name == "Std.Tuple.Filter" ||
      full_name == "Std.Tuple.Reduce" || full_name == "Std.Match" || full_name == "Std.Apply") {
    auto in_place = full_name == "Std.Branch"         ? try_emit_branch_in_place(*tuple, out, locals, state, bytecode_module)
                    : full_name == "Std.Loop"         ? try_emit_loop_in_place(*tuple, out, locals, state, bytecode_module)
                    : full_name == "Std.Tuple.Map"    ? try_emit_map_in_place(*tuple, out, locals, state, bytecode_module)
                    : full_name == "Std.Tuple.Filter" ? try_emit_filter_in_place(*tuple, out, locals, state, bytecode_module)
                    : full_name == "Std.Tuple.Reduce" ? try_emit_reduce_in_place(*tuple, out, locals, state, bytecode_module)
                    : full_name == "Std.Match"        ? try_emit_match_in_place(*tuple, out, locals, state, bytecode_module)
                                                      : try_emit_apply(*tuple, out, locals, state, bytecode_module);
    if (!in_place) {
      return tl::unexpected(in_place.error());
    }
    if (*in_place) {
      return true;
    }
  }

  const auto emit_fast_path = [&](const bool matched, const Opcode opcode) -> tl::expected<bool, CompileError> {
    if (!matched) {
      return false;
    }
    if (auto emit_result = emit_tuple_items(*tuple, out, locals, state, bytecode_module); !emit_result) {
      return tl::unexpected(emit_result.error());
    }
    out.push_back(Instruction{.opcode = opcode, .operand = 0});
    return true;
  };

  if (auto fast_path = emit_fast_path(full_name == "Std.Select" && tuple->items.size() == 3 &&
                                          !expr_is_known_user_function_ref(*tuple->items[1], state) &&
                                          !expr_is_known_user_function_ref(*tuple->items[2], state),
                                      Opcode::kSelect);
      !fast_path) {
    return tl::unexpected(fast_path.error());
  } else if (*fast_path) {
    return true;
  }

  if (auto fast_path = emit_fast_path(can_emit_native_branch_call(full_name, *tuple, state), Opcode::kBranchCall);
      !fast_path) {
    return tl::unexpected(fast_path.error());
  } else if (*fast_path) {
    return true;
  }

  if (auto fast_path = emit_fast_path(full_name == "Std.Loop" && tuple->items.size() == 3, Opcode::kLoopCall);
      !fast_path) {
    return tl::unexpected(fast_path.error());
  } else if (*fast_path) {
    return true;
  }

  if (auto fast_path = emit_fast_path(full_name == "Std.LoopN" && tuple->items.size() == 4, Opcode::kLoopNCall);
      !fast_path) {
    return tl::unexpected(fast_path.error());
  } else if (*fast_path) {
    return true;
  }

  if (const auto builtin_symbol_key = builtin_symbol_key_from_name_ref(name_ref, state);
      builtin_symbol_key.has_value()) {
    if (auto emit_result =
            emit_builtin_symbol_flow(*builtin_symbol_key, flow, out, locals, state, bytecode_module);
        !emit_result) {
      return tl::unexpected(emit_result.error());
    }
    return true;
  }

  return false;
}

auto emit_flow_expr(const IRFlowExpr& flow, std::vector<Instruction>& out, const LocalSlots& locals,
                    CompileState& state, Module& bytecode_module) -> EmitResult;

// A Std.Cast or Std.TryCast whose argument has static type Any compiles to the Std.Cast builtin, which only unwraps
// the argument, followed by a runtime check of the value.
auto emit_checked_cast_flow(const IRFlowExpr& flow, const IRNameRef& name_ref, std::vector<Instruction>& out,
                            const LocalSlots& locals, CompileState& state, Module& bytecode_module) -> EmitResult {
  const bool is_try = common::full_symbol_name(name_ref.qualifier, name_ref.name) == "Std.TryCast";
  const IRFlowExpr cast_flow{
      .lhs = flow.lhs,
      .rhs = IRNameRef{.qualifier = std::string{"Std"},
                       .name = "Cast",
                       .resolved_symbol_key = std::string{"Std.Cast"},
                       .span = name_ref.span},
      .call_shape = flow.call_shape,
      .span = flow.span,
  };
  if (auto emitted = emit_flow_expr(cast_flow, out, locals, state, bytecode_module); !emitted) {
    return emitted;
  }

  const auto check_index = static_cast<std::int64_t>(bytecode_module.type_checks.size());
  bytecode_module.type_checks.push_back(*name_ref.runtime_type_check);
  out.push_back(Instruction{.opcode = is_try ? Opcode::kTryCheckType : Opcode::kCheckType, .operand = check_index});
  return {};
}

// A parameter list with spreads, as the tuple of its arguments: each plain item becomes a one-item tuple, each spread
// contributes its tuple, and kConcatTuples joins them.
auto emit_spread_tuple(const IRTupleExpr& arguments, std::vector<Instruction>& out, const LocalSlots& locals,
                       CompileState& state, Module& bytecode_module) -> EmitResult {
  for (std::size_t position = 0; position < arguments.items.size(); ++position) {
    if (auto emitted = emit_expr(*arguments.items[position], out, locals, state, bytecode_module); !emitted) {
      return emitted;
    }
    if (!arguments.is_spread(position)) {
      out.push_back(Instruction{.opcode = Opcode::kBuildTuple, .operand = 1});
    }
  }
  out.push_back(
      Instruction{.opcode = Opcode::kConcatTuples, .operand = static_cast<std::int64_t>(arguments.items.size())});
  return {};
}

// A direct call of a named function, `args -> F`, with F's body in place of the call when F can be inlined. A function
// of several parameters is inlined only when the call writes one argument per parameter, as kCallUserFuncArgs needs.
// The arguments are evaluated in order and stored, last first, before the body:
//   <a1 .. an> StoreLocal s+n-1 .. StoreLocal s  <body>
auto try_emit_inlined_call(const IRFlowExpr& flow, std::vector<Instruction>& out, const LocalSlots& locals, CompileState& state,
                           Module& bytecode_module) -> tl::expected<bool, CompileError> {
  // A local of the same name is a function value, not this function.
  const auto* name_ref = std::get_if<IRNameRef>(&flow.rhs);
  if (name_ref == nullptr || name_ref->materialize_as_value || !name_ref->bound_operator_args.empty() ||
      (!name_ref->qualifier.has_value() && locals.contains(name_ref->name))) {
    return false;
  }
  const auto index = user_function_index_from_target(flow.rhs, state);
  if (!index.has_value()) {
    return false;
  }
  const IRLet* let = inlinable_function(*index, state);
  if (let == nullptr) {
    return false;
  }

  const IRExpr& arguments = adapted_user_flow_lhs_expr(flow);
  const auto first_slot = next_local_slot(locals);
  std::vector<std::uint32_t> slots;
  if (let->params.size() == 1U) {
    if (auto emitted = emit_expr(arguments, out, locals, state, bytecode_module); !emitted) {
      return tl::unexpected(emitted.error());
    }
    slots.push_back(first_slot);
  } else {
    const auto* argument_list = std::get_if<IRTupleExpr>(&arguments.node);
    if (argument_list == nullptr || argument_list->items.size() != let->params.size() || argument_list->has_spread()) {
      return false;
    }
    // Every argument is evaluated before any is stored: an argument's own code may use the slots above the caller's.
    if (auto emitted = emit_tuple_items(*argument_list, out, locals, state, bytecode_module); !emitted) {
      return tl::unexpected(emitted.error());
    }
    for (std::size_t position = 0; position < let->params.size(); ++position) {
      slots.push_back(first_slot + static_cast<std::uint32_t>(position));
    }
  }
  for (auto slot = slots.rbegin(); slot != slots.rend(); ++slot) {
    out.push_back(Instruction{.opcode = Opcode::kStoreLocal, .operand = static_cast<std::int64_t>(*slot)});
  }
  if (auto emitted = emit_inlined_body(*let, *index, slots, out, state, bytecode_module); !emitted) {
    return tl::unexpected(emitted.error());
  }
  return true;
}

auto emit_flow_expr(const IRFlowExpr& flow, std::vector<Instruction>& out, const LocalSlots& locals,
                    CompileState& state, Module& bytecode_module) -> EmitResult {
  if (const auto* name_ref = std::get_if<IRNameRef>(&flow.rhs);
      name_ref != nullptr && name_ref->runtime_type_check.has_value()) {
    return emit_checked_cast_flow(flow, *name_ref, out, locals, state, bytecode_module);
  }

  if (const auto* arguments = std::get_if<IRTupleExpr>(&flow.lhs->node);
      arguments != nullptr && !arguments->trailing_comma && arguments->has_spread()) {
    if (auto emitted = emit_spread_tuple(*arguments, out, locals, state, bytecode_module); !emitted) {
      return emitted;
    }
    return emit_call_target(flow.rhs, out, locals, state, bytecode_module);
  }

  auto handled = std::visit(
      common::overloaded{[&](const IROperatorRef& op_ref) -> tl::expected<bool, CompileError> {
                           // The type checker replaces every operator target with a reference to its Std builtin.
                           return tl::unexpected(make_err(
                               "Operator '" + op_ref.op +
                               "' reached the bytecode compiler without being resolved by the type checker."));
                         },
                         [&](const IRNameRef& name_ref) -> tl::expected<bool, CompileError> {
                           return try_emit_named_flow_fast_path(flow, name_ref, out, locals, state, bytecode_module);
                         }},
      flow.rhs);

  if (!handled) {
    return tl::unexpected(handled.error());
  }
  if (*handled) {
    return {};
  }

  if (auto inlined = try_emit_inlined_call(flow, out, locals, state, bytecode_module); !inlined) {
    return tl::unexpected(inlined.error());
  } else if (*inlined) {
    return {};
  }

  if (auto emitted_args_call = try_emit_user_args_call_fast_path(flow, out, locals, state, bytecode_module);
      !emitted_args_call) {
    return tl::unexpected(emitted_args_call.error());
  } else if (*emitted_args_call) {
    return {};
  }

  const bool calls_user_function = user_function_index_from_target(flow.rhs, state).has_value();
  if (auto emit_result = emit_expr(calls_user_function ? adapted_user_flow_lhs_expr(flow) : *flow.lhs, out, locals,
                                   state, bytecode_module);
      !emit_result) {
    return emit_result;
  }
  std::uint32_t builtin_static_argument_count = kUnknownBuiltinStaticArgumentCount;
  std::vector<BuiltinCallableSlotRecord> builtin_callable_slots;
  if (const auto builtin_symbol_key = builtin_symbol_key_from_call_target(flow.rhs, state); builtin_symbol_key.has_value()) {
    if (const auto builtin_id = vm::builtin_id_from_symbol_key(*builtin_symbol_key); builtin_id.has_value()) {
      builtin_static_argument_count = recorded_builtin_static_argument_count(known_builtin_call_arity(*builtin_id, flow));
    }
  }

  return emit_call_target(flow.rhs, out, locals, state, bytecode_module, builtin_static_argument_count,
                          builtin_callable_slots);
}

// Expression emission

auto emit_expr(const IRExpr& expr, std::vector<Instruction>& out, const LocalSlots& locals, CompileState& state,
               Module& bytecode_module) -> EmitResult {
  return std::visit(
      common::overloaded{
          [&](const IRConstant& constant) -> EmitResult {
            const auto idx = CompileState::intern_const(bytecode_module, ConstValue{constant.val});
            out.push_back(Instruction{.opcode = Opcode::kPushConst, .operand = static_cast<std::int64_t>(idx)});
            return {};
          },
          [&](const IRNameRef& name_ref) -> EmitResult {
            return emit_name_ref_expr(name_ref, out, locals, state, bytecode_module);
          },
          [&](const IRClosureExprBox& closure_ptr) -> EmitResult {
            return emit_closure_expr(*closure_ptr, out, locals, state, bytecode_module);
          },
          [&](const IRBlockExprBox& block_ptr) -> EmitResult {
            return emit_block_expr(*block_ptr, out, locals, state, bytecode_module);
          },
          [&](const IRTupleExpr& tuple_expr) -> EmitResult {
            if (tuple_expr.has_spread()) {
              return emit_spread_tuple(tuple_expr, out, locals, state, bytecode_module);
            }
            if (auto emit_result = emit_tuple_items(tuple_expr, out, locals, state, bytecode_module); !emit_result) {
              return emit_result;
            }
            out.push_back(Instruction{.opcode = Opcode::kBuildTuple,
                                      .operand = static_cast<std::int64_t>(tuple_expr.items.size())});
            return {};
          },
          [&](const IRFlowExpr& flow) -> EmitResult {
            return emit_flow_expr(flow, out, locals, state, bytecode_module);
          },
          [&](auto&&) -> EmitResult {
            return tl::unexpected(make_err("Unsupported IR expression type in bytecode compiler."));
          }},
      expr.node);
}

}  // namespace

// BytecodeCompiler::compile

auto BytecodeCompiler::compile(const IRProgram& program, const CompileOptions& options) const -> CompileResult {
  Module bytecode_module;
  CompileState state;
  initialize_module_header(bytecode_module, state, options);
  seed_program_dependencies(bytecode_module, program);
  record_type_declarations(bytecode_module, program, options);

  seed_imported_exports(bytecode_module, state, options);
  register_let_definitions(bytecode_module, state, program, options);
  reserve_inline_closure_capacity(bytecode_module, program);

  if (auto emit_result = compile_function_bodies(bytecode_module, state, program); !emit_result) {
    return tl::unexpected(emit_result.error());
  }

  if (auto emit_result = compile_top_level_expressions(bytecode_module, state, program); !emit_result) {
    return tl::unexpected(emit_result.error());
  }

  bytecode_module.instructions.push_back(Instruction{.opcode = Opcode::kHalt, .operand = 0});
  move_last_local_reads(bytecode_module);
  return bytecode_module;
}

}  // namespace fleaux::bytecode
