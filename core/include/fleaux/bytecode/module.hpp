#pragma once

#include <compare>
#include <cstdint>
#include <limits>
#include <optional>
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
  // Checked against the argument when the host calls this symbol (Runtime::invoke_symbol). Absent only in modules
  // built by hand rather than by the compiler.
  std::optional<common::HostEntryCheck> host_entry_check{};
};

// A location number: 1 + an index into Module::locations, or kNoLocation.
inline constexpr std::uint32_t kNoLocation = 0U;

struct Instruction {
  Opcode opcode{Opcode::kNoOp};
  // Where in the source the instruction came from, read only when a run fails. It sits in the padding after the
  // opcode, so an instruction is no larger for it.
  std::uint32_t location{kNoLocation};
  std::int64_t operand{0};

  friend auto operator==(const Instruction&, const Instruction&) -> bool = default;
};
static_assert(sizeof(Instruction) == 16U);

// A source file that locations refer to, with the hash of the text compiled, so that a stale file is not shown.
struct SourceFileRecord {
  std::string path{};
  std::uint64_t hash{0};

  friend auto operator==(const SourceFileRecord&, const SourceFileRecord&) -> bool = default;
};

// The source span of an expression: lines and columns from 1, the end one past its last character. The instructions of a function's
// body inlined into a call carry locations that name the function and the location of the call, which comes before
// them in Module::locations, so that a chain of inlined calls always ends.
struct SourceLocation {
  std::uint32_t file{0};  // index into Module::source_files
  std::uint32_t line{0};
  std::uint32_t column{0};
  std::uint32_t end_line{0};
  std::uint32_t end_column{0};
  std::optional<std::uint32_t> inlined_function{};  // index into Module::functions
  // For the body of an inline closure that a builtin's in-place form runs: it has no function of its own.
  bool inlined_closure{false};
  std::uint32_t inlined_at{kNoLocation};
  // For a call that a builtin's in-place form makes, the builtin (a vm::BuiltinId): its call of its function argument,
  // or the location a body is inlined at for it.
  std::optional<std::uint32_t> called_by{};

  friend auto operator<=>(const SourceLocation&, const SourceLocation&) = default;
};

// The location a location number names, or null for kNoLocation and a number past the table.
[[nodiscard]] inline auto find_location(const std::vector<SourceLocation>& locations, const std::uint32_t number) -> const SourceLocation* {
  return number != kNoLocation && number <= locations.size() ? &locations[number - 1U] : nullptr;
}

// A typed constant stored in the constant pool.
// All literal constants (including int64_t) are loaded via kPushConst.
struct ConstValue {
    std::variant<std::monostate, std::int64_t, std::uint64_t, double, bool, std::string> data;
};

// An operator a generic function applies to a type parameter bounded by strong types; the function takes the
// operator's implementation as a hidden leading argument. See the type checker's OperatorRequirement.
struct OperatorRequirementRecord {
  std::string op{};
  std::string type_param{};
  // The operator's function type in canonical source syntax, in terms of the function's type parameters.
  std::string signature{};
  // The symbol key of the function serving each alternative of the parameter's bound, in bound order, for host calls.
  std::vector<std::string> alternative_functions{};
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

  // The kind of value slot the function's result fits, if any. A builtin call records a value slot
  // (BuiltinValueSlotRecord) only for an argument a call of such a function makes, and the verifier checks the record
  // against this.
  std::optional<vm::BuiltinValueSlotKind> return_value_slot_kind{std::nullopt};

  // Hidden leading parameters (counted in `arity`) that carry operator implementations, one per requirement.
  std::uint32_t hidden_operator_params{0};
  std::vector<OperatorRequirementRecord> operator_requirements{};

  // True for a value let (`let X: T = ...`), whose name as a call target calls its value.
  bool is_value_binding{false};
};

// A type or alias declaration of the module, kept so that a module importing it from bytecode alone knows its
// types. Types are in canonical source syntax.
struct TypeDeclRecord {
  std::string name{};
  std::vector<std::string> generic_params{};
  bool is_builtin{false};
  std::string target{};  // Empty for a builtin type.
};

struct AliasDeclRecord {
  std::string name{};
  std::string target{};
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

enum class BuiltinValueSlotSourceKind : std::uint8_t {
  kFunctionReturn = 0,
};

struct BuiltinValueSlotRecord {
  std::uint32_t slot_index{0};
  vm::BuiltinValueSlotKind summary_kind{vm::BuiltinValueSlotKind::kDictStringAny};
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

// A function an extension implements, declared '__native__'. The VM binds `symbol` to an extension function before the
// module runs, and checks each value the function returns against `return_check`.
struct NativeFunctionRecord {
  std::string symbol{};
  common::RuntimeTypeCheck return_check{};

  friend auto operator==(const NativeFunctionRecord&, const NativeFunctionRecord&) -> bool = default;
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

  // Native functions: indexed by kCallNative operands.
  std::vector<NativeFunctionRecord> native_functions{};

  // The module's own type and alias declarations, for importers that only have the bytecode.
  std::vector<TypeDeclRecord> type_decls{};
  std::vector<AliasDeclRecord> alias_decls{};

  // One record per emitted builtin call (kCallBuiltin or kCallBuiltinArgs). This metadata is used by
  // loader-side verification and is intentionally kept parallel to the runtime
  // instruction streams rather than embedded into each instruction.
  std::vector<BuiltinCallsiteRecord> builtin_callsites{};

  // The source files and spans the instructions' locations refer to.
  std::vector<SourceFileRecord> source_files{};
  std::vector<SourceLocation> locations{};
};

}  // namespace fleaux::bytecode
