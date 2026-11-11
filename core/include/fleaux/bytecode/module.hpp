#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <variant>
#include <vector>

#include "fleaux/bytecode/opcode.hpp"
#include "fleaux/common/runtime_type_check.hpp"
#include "fleaux/vm/builtin_catalog.hpp"

namespace fleaux::bytecode {

struct ModuleHeader {
  std::string module_name{};
  std::string source_path{};
  std::uint64_t source_hash{0};
  std::uint64_t payload_checksum{0};
  std::uint8_t optimization_mode{0};
};

struct ModuleDependency {
  std::string module_name{};
  bool is_symbolic{false};
};

enum class ExportKind : std::uint8_t {
  kFunction = 0,
  kBuiltinAlias = 1,
};

struct ExportedSymbol {
  std::string name{};
  std::string link_name{};
  ExportKind kind{ExportKind::kFunction};
  std::uint32_t index{0};
  std::string builtin_name{};
};

struct Instruction {
  Opcode opcode{Opcode::kNoOp};
  std::int64_t operand{0};
};

// A typed constant stored in the constant pool.
// All literal constants (including int64_t) are loaded via kPushConst.
struct ConstValue {
    std::variant<std::monostate, std::int64_t, std::uint64_t, double, bool, std::string> data;
};

// A compiled user-defined function.
struct FunctionDef {
  std::string name{};                // qualified name (e.g. "MyMath.Square")
  std::uint32_t arity{0};         // number of declared parameters
  bool has_variadic_tail{false};  // true when the last declared parameter is variadic
  bool is_import_placeholder{false};
  std::vector<Instruction> instructions{};

  // Generic type-parameter names (e.g. {"T", "U"} for `let Foo<T,U>(...)`).
  // Non-empty only for generic functions.  Preserved so consumers that only
  // have the bytecode file (no source) can reconstruct accurate typed-import
  // stubs and validate call sites against the generic signature.
  std::vector<std::string> generic_params{};

  // Bound of each generic parameter in canonical source syntax, parallel to `generic_params`, e.g.
  // "Int64 | UInt64 | Float64". An empty string means the parameter is unbounded.
  std::vector<std::string> generic_bounds{};

  // Declared parameter types in canonical source syntax, one per parameter
  // (parallel to `arity`), e.g. "Int64", "T", or "Result<Tuple<Int64...>, String>".
  // The variadic marker of a variadic tail parameter is not included; see
  // `has_variadic_tail`.  Empty when not available.
  std::vector<std::string> param_type_names{};

  // Declared return type in canonical source syntax.  Same encoding as `param_type_names`.
  std::string return_type_name{};

  // Narrow verifier-facing summary bit for the selected domain slice.
  bool return_type_is_dict_string_any{false};
  bool return_type_is_exact_uint64{false};
  bool return_type_is_exact_int64{false};
  bool return_type_is_task_handle{false};
  bool return_type_is_random_generator{false};
  bool return_type_is_file_handle{false};
};

struct ClosureDef {
  std::uint32_t function_index{0};
  std::uint32_t capture_count{0};
  std::uint32_t declared_arity{0};
  bool declared_has_variadic_tail{false};
};

enum class BuiltinCallsiteOwnerKind : std::uint8_t {
  kModule = 0,
  kFunction = 1,
};

inline constexpr std::uint32_t kUnknownBuiltinStaticArgumentCount = std::numeric_limits<std::uint32_t>::max();
inline constexpr std::uint32_t kUnknownCallableTargetIndex = std::numeric_limits<std::uint32_t>::max();

enum class BuiltinCallableSlotKind : std::uint8_t {
  kFunction = 0,
  kClosure = 1,
  kBuiltin = 2,
};

struct BuiltinCallableSlotRecord {
  std::uint32_t slot_index{0};
  BuiltinCallableSlotKind kind{BuiltinCallableSlotKind::kFunction};
  std::uint32_t target_index{kUnknownCallableTargetIndex};
  std::uint32_t declared_arity{0};
  bool has_variadic_tail{false};
};

enum class BuiltinValueSlotSummaryKind : std::uint8_t {
  kDictStringAny = 0,
  kExactUInt64 = 1,
  kExactInt64 = 2,
  kTaskHandle = 3,
  kRandomGenerator = 4,
  kFileHandle = 5,
};

enum class BuiltinValueSlotSourceKind : std::uint8_t {
  kFunctionReturn = 0,
};

struct BuiltinValueSlotRecord {
  std::uint32_t slot_index{0};
  BuiltinValueSlotSummaryKind summary_kind{BuiltinValueSlotSummaryKind::kDictStringAny};
  BuiltinValueSlotSourceKind source_kind{BuiltinValueSlotSourceKind::kFunctionReturn};
  std::uint32_t source_index{kUnknownCallableTargetIndex};
};

struct BuiltinCallsiteRecord {
  BuiltinCallsiteOwnerKind owner_kind{BuiltinCallsiteOwnerKind::kModule};
  std::uint32_t owner_index{0};
  std::uint32_t instruction_index{0};
  vm::BuiltinId builtin_id{vm::BuiltinId::UnaryPlus};
  vm::BuiltinCallShape call_shape{vm::BuiltinCallShape::kTupleExpanded};
  std::uint32_t static_argument_count{kUnknownBuiltinStaticArgumentCount};
  std::vector<BuiltinCallableSlotRecord> callable_slots{};
  std::vector<BuiltinValueSlotRecord> value_slots{};
};

struct Module {
  ModuleHeader header{};

  // Direct import dependencies for this module.
  std::vector<ModuleDependency> dependencies{};

  // Public import surface for this module.
  std::vector<ExportedSymbol> exports{};

  // Top-level instruction stream (executed on program start).
  std::vector<Instruction> instructions{};

  // Constant pool: indexed by kPushConst operand.
  std::vector<ConstValue> constants{};

  // User-defined functions: indexed by kCallUserFunc operand.
  std::vector<FunctionDef> functions{};

  // Inline closure templates: indexed by kMakeClosureRef operand.
  std::vector<ClosureDef> closures{};

  // Runtime type checks for casts from Any: indexed by kCheckType and kTryCheckType operands.
  std::vector<common::RuntimeTypeCheck> type_checks{};

  // One record per emitted kCallBuiltin instruction. This metadata is used by
  // loader-side verification and is intentionally kept parallel to the runtime
  // instruction streams rather than embedded into each instruction.
  std::vector<BuiltinCallsiteRecord> builtin_callsites{};
};

}  // namespace fleaux::bytecode
