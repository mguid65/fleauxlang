#pragma once

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "fleaux/bytecode/opcode.hpp"

namespace fleaux::bytecode {

struct ModuleHeader {
  std::string module_name;
  std::string source_path;
  std::uint64_t source_hash = 0;
  std::uint64_t payload_checksum = 0;
  std::uint8_t optimization_mode = 0;
};

struct ModuleDependency {
  std::string module_name;
  bool is_symbolic = false;
};

enum class ExportKind : std::uint8_t {
  kFunction = 0,
  kBuiltinAlias = 1,
};

struct ExportedSymbol {
  std::string name;
  std::string link_name;
  ExportKind kind = ExportKind::kFunction;
  std::uint32_t index = 0;
  std::string builtin_name;
};

struct Instruction {
  Opcode opcode = Opcode::kNoOp;
  std::int64_t operand = 0;
};

// A typed constant stored in the constant pool.
// All literal constants (including int64_t) are loaded via kPushConst.
struct ConstValue {
    std::variant<std::int64_t, std::uint64_t, double, bool, std::string, std::monostate> data;
};

// A compiled user-defined function.
struct FunctionDef {
  std::string name;                // qualified name (e.g. "MyMath.Square")
  std::uint32_t arity = 0;         // number of declared parameters
  bool has_variadic_tail = false;  // true when the last declared parameter is variadic
  bool is_import_placeholder = false;
  std::vector<Instruction> instructions;

  // Generic type-parameter names (e.g. {"T", "U"} for `let Foo<T,U>(...)`).
  // Non-empty only for generic functions.  Preserved so consumers that only
  // have the bytecode file (no source) can reconstruct accurate typed-import
  // stubs and validate call sites against the generic signature.
  std::vector<std::string> generic_params;

  // Declared parameter type names, one per parameter (parallel to `arity`).
  // For simple/named types (including type-variable names such as "T") this
  // is the complete type name.  For complex structural types the outer name
  // is stored (e.g. "Function", "Tuple").  Empty when not available.
  std::vector<std::string> param_type_names;

  // Declared return type name.  Same encoding as `param_type_names`.
  std::string return_type_name;
};

struct ClosureDef {
  std::uint32_t function_index = 0;
  std::uint32_t capture_count = 0;
  std::uint32_t declared_arity = 0;
  bool declared_has_variadic_tail = false;
};

struct Module {
  ModuleHeader header;

  // Direct import dependencies for this module.
  std::vector<ModuleDependency> dependencies;

  // Public import surface for this module.
  std::vector<ExportedSymbol> exports;

  // Top-level instruction stream (executed on program start).
  std::vector<Instruction> instructions;

  // Constant pool: indexed by kPushConst operand.
  std::vector<ConstValue> constants;

  // User-defined functions: indexed by kCallUserFunc operand.
  std::vector<FunctionDef> functions;

  // Inline closure templates: indexed by kMakeClosureRef operand.
  std::vector<ClosureDef> closures;
};

}  // namespace fleaux::bytecode
