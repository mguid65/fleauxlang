#include "fleaux/bytecode/serialization.hpp"
#include "fleaux/common/overloaded.hpp"
#include "fleaux/runtime/builtins_core.hpp"
#include "fleaux/vm/builtin_catalog.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <type_traits>
#include <utility>

namespace fleaux::bytecode {

namespace {

constexpr std::uint32_t BYTECODE_MAGIC = 0x464C4558;
// Builtin operands are serialized as numeric BuiltinId values, so changes to
// builtin ordering are bytecode ABI changes and must invalidate cached modules.
// Requiring an explicit `import Std;` before `Std.*` resolution is also a
// bytecode-cache compatibility boundary because older caches may have been
// compiled from source that would now fail semantic analysis.
constexpr std::uint32_t BYTECODE_VERSION = 45;
constexpr std::size_t kChecksumOffset = sizeof(std::uint32_t) * 2;
constexpr std::size_t kPayloadOffset = kChecksumOffset + sizeof(std::uint64_t);
constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

enum class ConstTag : std::uint8_t {
  kInt64 = 0,
  kUInt64 = 1,
  kFloat64 = 2,
  kBool = 3,
  kString = 4,
  kNull = 5,
};

template <typename T>
  requires std::is_trivially_copyable_v<T>
void write_pod(std::vector<std::uint8_t>& buffer, const T& value) {
  const auto bytes = std::bit_cast<std::array<std::uint8_t, sizeof(T)>>(value);
  buffer.insert(buffer.end(), bytes.begin(), bytes.end());
}

template <typename T>
  requires std::is_trivially_copyable_v<T>
auto read_pod(const std::vector<std::uint8_t>& buffer, std::size_t& offset, T& out) -> bool {
  if (offset + sizeof(T) > buffer.size()) {
    return false;
  }
  std::array<std::uint8_t, sizeof(T)> bytes{};
  std::copy_n(buffer.begin() + static_cast<std::ptrdiff_t>(offset), sizeof(T), bytes.begin());
  out = std::bit_cast<T>(bytes);
  offset += sizeof(T);
  return true;
}

void write_string(std::vector<std::uint8_t>& buffer, const std::string& str) {
  const auto len = static_cast<std::uint32_t>(str.length());
  write_pod(buffer, len);
  buffer.insert(buffer.end(), str.begin(), str.end());
}

auto read_string(const std::vector<std::uint8_t>& buffer, std::size_t& offset, std::string& out) -> bool {
  std::uint32_t len = 0;
  if (!read_pod(buffer, offset, len)) {
    return false;
  }
  if (offset + len > buffer.size()) {
    return false;
  }
  out.assign(buffer.begin() + static_cast<std::ptrdiff_t>(offset),
             buffer.begin() + static_cast<std::ptrdiff_t>(offset + len));
  offset += len;
  return true;
}

auto checksum_bytes(const std::vector<std::uint8_t>& buffer, const std::size_t begin) -> std::uint64_t {
  std::uint64_t hash = kFnvOffsetBasis;
  for (std::size_t index = begin; index < buffer.size(); ++index) {
    hash ^= static_cast<std::uint64_t>(buffer[index]);
    hash *= kFnvPrime;
  }
  return hash;
}

void patch_checksum(std::vector<std::uint8_t>& buffer, const std::uint64_t checksum) {
  const auto bytes = std::bit_cast<std::array<std::uint8_t, sizeof(checksum)>>(checksum);
  std::ranges::copy(bytes, buffer.begin() + static_cast<std::ptrdiff_t>(kChecksumOffset));
}

void write_const_value(std::vector<std::uint8_t>& buffer, const ConstValue& cv) {
  std::visit(common::overloaded{
                 [&](const std::int64_t& value) -> void {
                   write_pod(buffer, static_cast<std::uint8_t>(ConstTag::kInt64));
                   write_pod(buffer, value);
                 },
                 [&](const std::uint64_t& value) -> void {
                   write_pod(buffer, static_cast<std::uint8_t>(ConstTag::kUInt64));
                   write_pod(buffer, value);
                 },
                 [&](const double& value) -> void {
                   write_pod(buffer, static_cast<std::uint8_t>(ConstTag::kFloat64));
                   write_pod(buffer, value);
                 },
                 [&](const bool& value) -> void {
                   write_pod(buffer, static_cast<std::uint8_t>(ConstTag::kBool));
                   write_pod(buffer, value);
                 },
                 [&](const std::string& value) -> void {
                   write_pod(buffer, static_cast<std::uint8_t>(ConstTag::kString));
                   write_string(buffer, value);
                 },
                 [&](const std::monostate&) -> void { write_pod(buffer, static_cast<std::uint8_t>(ConstTag::kNull)); }},
             cv.data);
}

auto read_const_value(const std::vector<std::uint8_t>& buffer, std::size_t& offset, ConstValue& out) -> bool {
  std::uint8_t raw_tag = 0;
  if (!read_pod(buffer, offset, raw_tag)) {
    return false;
  }
  switch (static_cast<ConstTag>(raw_tag)) {
    case ConstTag::kInt64: {
      std::int64_t val = 0;
      if (!read_pod(buffer, offset, val)) {
        return false;
      }
      out.data = val;
      return true;
    }
    case ConstTag::kUInt64: {
      std::uint64_t val = 0;
      if (!read_pod(buffer, offset, val)) {
        return false;
      }
      out.data = val;
      return true;
    }
    case ConstTag::kFloat64: {
      double val = 0;
      if (!read_pod(buffer, offset, val)) {
        return false;
      }
      out.data = val;
      return true;
    }
    case ConstTag::kBool: {
      bool val = false;
      if (!read_pod(buffer, offset, val)) {
        return false;
      }
      out.data = val;
      return true;
    }
    case ConstTag::kString: {
      std::string val;
      if (!read_string(buffer, offset, val)) {
        return false;
      }
      out.data = std::move(val);
      return true;
    }
    case ConstTag::kNull: {
      out.data = std::monostate{};
      return true;
    }
    default:
      return false;
  }
}

void write_header(std::vector<std::uint8_t>& buffer, const ModuleHeader& header) {
  write_string(buffer, header.module_name);
  write_string(buffer, header.source_path);
  write_pod(buffer, header.source_hash);
  write_pod(buffer, header.optimization_mode);
}

auto read_header(const std::vector<std::uint8_t>& buffer, std::size_t& offset, ModuleHeader& header) -> bool {
  return read_string(buffer, offset, header.module_name) && read_string(buffer, offset, header.source_path) &&
         read_pod(buffer, offset, header.source_hash) && read_pod(buffer, offset, header.optimization_mode);
}

void write_dependency(std::vector<std::uint8_t>& buffer, const ModuleDependency& dependency) {
  write_string(buffer, dependency.module_name);
  write_pod(buffer, dependency.is_symbolic);
}

auto read_dependency(const std::vector<std::uint8_t>& buffer, std::size_t& offset, ModuleDependency& dependency)
    -> bool {
  return read_string(buffer, offset, dependency.module_name) && read_pod(buffer, offset, dependency.is_symbolic);
}

void write_type_check(std::vector<std::uint8_t>& buffer, const common::RuntimeTypeCheck& check);
void write_string_list(std::vector<std::uint8_t>& buffer, const std::vector<std::string>& list);
auto read_string_list(const std::vector<std::uint8_t>& buffer, std::size_t& offset, std::vector<std::string>& out)
    -> bool;
auto read_type_check(const std::vector<std::uint8_t>& buffer, std::size_t& offset, common::RuntimeTypeCheck& check,
                     std::size_t depth) -> bool;

void write_export(std::vector<std::uint8_t>& buffer, const ExportedSymbol& symbol) {
  write_string(buffer, symbol.name);
  write_string(buffer, symbol.link_name);
  write_pod(buffer, static_cast<std::uint8_t>(symbol.kind));
  write_pod(buffer, symbol.index);
  write_string(buffer, symbol.builtin_name);
  write_pod(buffer, symbol.host_entry_check.has_value());
  if (symbol.host_entry_check.has_value()) {
    const auto& entry = *symbol.host_entry_check;
    write_pod(buffer, entry.variadic_tail);
    write_string_list(buffer, entry.type_params);
    write_pod(buffer, static_cast<std::uint32_t>(entry.params.size()));
    for (const auto& param : entry.params) {
      write_type_check(buffer, param);
    }
  }
}

auto read_export(const std::vector<std::uint8_t>& buffer, std::size_t& offset, ExportedSymbol& symbol) -> bool {
  std::uint8_t kind = 0;
  bool has_entry_check = false;
  if (!read_string(buffer, offset, symbol.name) || !read_string(buffer, offset, symbol.link_name) ||
      !read_pod(buffer, offset, kind) || !read_pod(buffer, offset, symbol.index) ||
      !read_string(buffer, offset, symbol.builtin_name) || !read_pod(buffer, offset, has_entry_check)) {
    return false;
  }
  symbol.kind = static_cast<ExportKind>(kind);
  if (has_entry_check) {
    common::HostEntryCheck entry;
    std::uint32_t count = 0;
    if (!read_pod(buffer, offset, entry.variadic_tail) || !read_string_list(buffer, offset, entry.type_params) ||
        !read_pod(buffer, offset, count) || count > buffer.size() - offset) {
      return false;
    }
    entry.params.resize(count);
    for (auto& param : entry.params) {
      if (!read_type_check(buffer, offset, param, 0U)) {
        return false;
      }
    }
    if (!common::is_well_formed(entry)) {
      return false;
    }
    symbol.host_entry_check = std::move(entry);
  }
  return true;
}

void write_builtin_callable_slot(std::vector<std::uint8_t>& buffer, const BuiltinCallableSlotRecord& record) {
  write_pod(buffer, record.slot_index);
  write_pod(buffer, static_cast<std::uint8_t>(record.kind));
  write_pod(buffer, record.target_index);
  write_pod(buffer, record.declared_arity);
  write_pod(buffer, record.has_variadic_tail);
}

void write_builtin_value_slot(std::vector<std::uint8_t>& buffer, const BuiltinValueSlotRecord& record) {
  write_pod(buffer, record.slot_index);
  write_pod(buffer, static_cast<std::uint8_t>(record.summary_kind));
  write_pod(buffer, static_cast<std::uint8_t>(record.source_kind));
  write_pod(buffer, record.source_index);
}

auto read_builtin_callable_slot(const std::vector<std::uint8_t>& buffer, std::size_t& offset,
                                BuiltinCallableSlotRecord& record) -> bool {
  std::uint8_t kind = 0;
  if (!read_pod(buffer, offset, record.slot_index) || !read_pod(buffer, offset, kind) ||
      !read_pod(buffer, offset, record.target_index) || !read_pod(buffer, offset, record.declared_arity) ||
      !read_pod(buffer, offset, record.has_variadic_tail)) {
    return false;
  }
  if (kind > static_cast<std::uint8_t>(BuiltinCallableSlotKind::kBuiltin)) {
    return false;
  }
  record.kind = static_cast<BuiltinCallableSlotKind>(kind);
  return true;
}

auto read_builtin_value_slot(const std::vector<std::uint8_t>& buffer, std::size_t& offset,
                             BuiltinValueSlotRecord& record) -> bool {
  std::uint8_t summary_kind = 0;
  std::uint8_t source_kind = 0;
  if (!read_pod(buffer, offset, record.slot_index) || !read_pod(buffer, offset, summary_kind) ||
      !read_pod(buffer, offset, source_kind) || !read_pod(buffer, offset, record.source_index)) {
    return false;
  }
  if (summary_kind > static_cast<std::uint8_t>(BuiltinValueSlotSummaryKind::kFileHandle) ||
      source_kind > static_cast<std::uint8_t>(BuiltinValueSlotSourceKind::kFunctionReturn)) {
    return false;
  }
  record.summary_kind = static_cast<BuiltinValueSlotSummaryKind>(summary_kind);
  record.source_kind = static_cast<BuiltinValueSlotSourceKind>(source_kind);
  return true;
}

void write_builtin_callsite(std::vector<std::uint8_t>& buffer, const BuiltinCallsiteRecord& record) {
  write_pod(buffer, static_cast<std::uint8_t>(record.owner_kind));
  write_pod(buffer, record.owner_index);
  write_pod(buffer, record.instruction_index);
  write_pod(buffer, static_cast<std::uint16_t>(record.builtin_id));
  write_pod(buffer, static_cast<std::uint8_t>(record.call_shape));
  write_pod(buffer, record.static_argument_count);
  write_pod(buffer, static_cast<std::uint32_t>(record.callable_slots.size()));
  for (const auto& callable_slot : record.callable_slots) {
    write_builtin_callable_slot(buffer, callable_slot);
  }
  write_pod(buffer, static_cast<std::uint32_t>(record.value_slots.size()));
  for (const auto& value_slot : record.value_slots) {
    write_builtin_value_slot(buffer, value_slot);
  }
}

auto read_builtin_callsite(const std::vector<std::uint8_t>& buffer, std::size_t& offset, BuiltinCallsiteRecord& record)
    -> bool {
  std::uint8_t owner_kind = 0;
  std::uint16_t builtin_id = 0;
  std::uint8_t call_shape = 0;
  if (!read_pod(buffer, offset, owner_kind) || !read_pod(buffer, offset, record.owner_index) ||
      !read_pod(buffer, offset, record.instruction_index) || !read_pod(buffer, offset, builtin_id) ||
      !read_pod(buffer, offset, call_shape) || !read_pod(buffer, offset, record.static_argument_count)) {
    return false;
  }

  if (owner_kind > static_cast<std::uint8_t>(BuiltinCallsiteOwnerKind::kFunction) ||
      std::cmp_greater_equal(builtin_id, fleaux::vm::builtin_count()) ||
      call_shape > static_cast<std::uint8_t>(fleaux::vm::BuiltinCallShape::kDirectValue)) {
    return false;
  }

  record.owner_kind = static_cast<BuiltinCallsiteOwnerKind>(owner_kind);
  record.builtin_id = static_cast<fleaux::vm::BuiltinId>(builtin_id);
  record.call_shape = static_cast<fleaux::vm::BuiltinCallShape>(call_shape);
  std::uint32_t callable_slot_count = 0;
  if (!read_pod(buffer, offset, callable_slot_count)) {
    return false;
  }
  record.callable_slots.clear();
  record.callable_slots.reserve(callable_slot_count);
  for (std::uint32_t i = 0; i < callable_slot_count; ++i) {
    BuiltinCallableSlotRecord callable_slot;
    if (!read_builtin_callable_slot(buffer, offset, callable_slot)) {
      return false;
    }
    record.callable_slots.push_back(callable_slot);
  }
  std::uint32_t value_slot_count = 0;
  if (!read_pod(buffer, offset, value_slot_count)) {
    return false;
  }
  record.value_slots.clear();
  record.value_slots.reserve(value_slot_count);
  for (std::uint32_t i = 0; i < value_slot_count; ++i) {
    BuiltinValueSlotRecord value_slot;
    if (!read_builtin_value_slot(buffer, offset, value_slot)) {
      return false;
    }
    record.value_slots.push_back(value_slot);
  }
  return true;
}

void write_instruction_stream(std::vector<std::uint8_t>& buffer, const std::vector<Instruction>& instructions) {
  const auto count = static_cast<std::uint32_t>(instructions.size());
  write_pod(buffer, count);
  for (const auto& [opcode, operand] : instructions) {
    write_pod(buffer, static_cast<std::uint32_t>(opcode));
    write_pod(buffer, operand);
  }
}

auto read_instruction_stream(const std::vector<std::uint8_t>& buffer, std::size_t& offset,
                             std::vector<Instruction>& instructions) -> std::optional<std::string> {
  std::uint32_t count = 0;
  if (!read_pod(buffer, offset, count)) {
    return "Cannot read instruction count";
  }
  instructions.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint32_t opcode_val = 0;
    std::int64_t operand = 0;
    if (!read_pod(buffer, offset, opcode_val) || !read_pod(buffer, offset, operand)) {
      return std::format("Cannot read instruction {}", i);
    }
    if (!opcode_index_is_valid(static_cast<std::size_t>(opcode_val))) {
      return std::format("Invalid opcode value {} at instruction {}", opcode_val, i);
    }
    instructions.push_back({.opcode = static_cast<Opcode>(opcode_val), .operand = operand});
  }
  return std::nullopt;
}

void write_string_list(std::vector<std::uint8_t>& buffer, const std::vector<std::string>& list) {
  const auto count = static_cast<std::uint32_t>(list.size());
  write_pod(buffer, count);
  for (const auto& s : list) {
    write_string(buffer, s);
  }
}

auto read_string_list(const std::vector<std::uint8_t>& buffer, std::size_t& offset, std::vector<std::string>& out)
    -> bool {
  std::uint32_t count = 0;
  if (!read_pod(buffer, offset, count)) {
    return false;
  }
  out.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::string s;
    if (!read_string(buffer, offset, s)) {
      return false;
    }
    out.push_back(std::move(s));
  }
  return true;
}

// Nesting limit for type checks read from bytecode, so a malformed file cannot exhaust the stack.
constexpr std::size_t kMaxTypeCheckDepth = 128;

void write_type_check(std::vector<std::uint8_t>& buffer, const common::RuntimeTypeCheck& check) {
  write_pod(buffer, static_cast<std::uint8_t>(check.kind));
  write_pod(buffer, check.variadic_tail);
  write_pod(buffer, check.type_param);
  write_pod(buffer, static_cast<std::uint32_t>(check.items.size()));
  for (const auto& item : check.items) {
    write_type_check(buffer, item);
  }
}

auto read_type_check(const std::vector<std::uint8_t>& buffer, std::size_t& offset, common::RuntimeTypeCheck& check,
                     const std::size_t depth) -> bool {
  if (depth > kMaxTypeCheckDepth) {
    return false;
  }
  std::uint8_t kind = 0;
  std::uint32_t count = 0;
  if (!read_pod(buffer, offset, kind) || kind >= common::kRuntimeTypeCheckKindCount ||
      !read_pod(buffer, offset, check.variadic_tail) || !read_pod(buffer, offset, check.type_param) ||
      !read_pod(buffer, offset, count) ||
      count > buffer.size() - offset) {
    return false;
  }
  check.kind = static_cast<common::RuntimeTypeCheckKind>(kind);
  check.items.resize(count);
  for (auto& item : check.items) {
    if (!read_type_check(buffer, offset, item, depth + 1U)) {
      return false;
    }
  }
  return true;
}

auto valid_index(const std::int64_t operand, const std::size_t size) -> bool {
  return operand >= 0 && std::cmp_less(operand, size);
}

auto describe_operand(const Module& module, const std::vector<Instruction>& stream, const Instruction& instruction)
    -> std::string {
  const auto index_label = [](const std::int64_t operand, const std::size_t size,
                              const std::string_view kind) -> std::string {
    return std::format("invalid {} index {} (size={})", kind, operand, size);
  };

  switch (instruction.opcode) {
    case Opcode::kPushConst: {
      if (!valid_index(instruction.operand, module.constants.size())) {
        return index_label(instruction.operand, module.constants.size(), "constant");
      }
      return std::format("const[{}]", instruction.operand);
    }
    case Opcode::kCallBuiltin:
    case Opcode::kMakeBuiltinFuncRef: {
      const auto builtin_id = fleaux::vm::builtin_id_from_operand(instruction.operand);
      if (!builtin_id.has_value()) {
        return std::format("invalid builtin id {}", instruction.operand);
      }
      return std::format("builtin={}", fleaux::vm::builtin_name(*builtin_id));
    }
    case Opcode::kCallBuiltinArgs: {
      const auto builtin_id = fleaux::vm::builtin_id_from_operand(instruction.operand);
      if (!builtin_id.has_value()) {
        return std::format("invalid builtin id {}", instruction.operand);
      }
      return std::format("builtin={}, arguments={}", fleaux::vm::builtin_name(*builtin_id),
                         fleaux::vm::builtin_argument_count_from_operand(instruction.operand));
    }
    case Opcode::kCallUserFunc:
    case Opcode::kCallUserFuncArgs:
    case Opcode::kMakeUserFuncRef: {
      if (!valid_index(instruction.operand, module.functions.size())) {
        return index_label(instruction.operand, module.functions.size(), "function");
      }
      const auto& function = module.functions[static_cast<std::size_t>(instruction.operand)];
      return std::format("function[{}]={}", instruction.operand, function.name);
    }
    case Opcode::kMakeClosureRef: {
      if (!valid_index(instruction.operand, module.closures.size())) {
        return index_label(instruction.operand, module.closures.size(), "closure");
      }
      const auto& closure = module.closures[static_cast<std::size_t>(instruction.operand)];
      if (closure.function_index >= module.functions.size()) {
        return std::format("closure[{}] -> invalid function index {} (size={})", instruction.operand,
                           closure.function_index, module.functions.size());
      }
      return std::format("closure[{}] -> function[{}]={}", instruction.operand, closure.function_index,
                         module.functions[closure.function_index].name);
    }
    case Opcode::kCheckType:
    case Opcode::kTryCheckType: {
      if (!valid_index(instruction.operand, module.type_checks.size())) {
        return index_label(instruction.operand, module.type_checks.size(), "type check");
      }
      return std::format("type_check[{}]={}", instruction.operand,
                         common::describe(module.type_checks[static_cast<std::size_t>(instruction.operand)]));
    }
    case Opcode::kCallNative: {
      if (!valid_index(instruction.operand, module.native_functions.size())) {
        return index_label(instruction.operand, module.native_functions.size(), "native function");
      }
      return std::format("native[{}]={}", instruction.operand,
                         module.native_functions[static_cast<std::size_t>(instruction.operand)].symbol);
    }
    case Opcode::kLoadLocal:
    case Opcode::kTakeLocal:
    case Opcode::kMarkStack:
    case Opcode::kBuildTupleSinceMark:
      return std::format("local[{}]", instruction.operand);
    case Opcode::kLoadLocalItem:
    case Opcode::kTakeLocalItem:
      return std::format("local[{}] item {}", local_item_slot(instruction.operand), local_item_index(instruction.operand));
    case Opcode::kBuildTuple:
      return std::format("tuple_size={}", instruction.operand);
    case Opcode::kConcatTuples:
      return std::format("pieces={}", instruction.operand);
    case Opcode::kCallFunctionValueArgs:
      return std::format("arguments={}", instruction.operand);
    case Opcode::kJump:
    case Opcode::kJumpIf:
    case Opcode::kJumpIfNot: {
      if (!valid_index(instruction.operand, stream.size())) {
        return std::format("invalid jump target {} (stream_size={})", instruction.operand, stream.size());
      }
      return std::format("target={}", instruction.operand);
    }
    default:
      return {};
  }
}

void print_instruction(std::ostream& out, const std::size_t instruction_index, const Module& module,
                       const std::vector<Instruction>& stream, const Instruction& instruction,
                       const std::string_view indent) {
  const auto annotation = describe_operand(module, stream, instruction);
  const auto operand_line = annotation.empty()
                                ? std::format("{}operand: {}\n", indent, instruction.operand)
                                : std::format("{}operand: {} ({})\n", indent, instruction.operand, annotation);

  out << std::format("{}Instruction {}:\n", indent.substr(0, indent.size() - 2), instruction_index)
      << std::format("{}opcode: {}\n", indent, stringify_opcode(instruction.opcode)) << operand_line;
}

}  // namespace

auto serialize_module(const Module& module) -> tl::expected<std::vector<std::uint8_t>, SerializationError> {
  std::vector<std::uint8_t> buffer;
  write_pod(buffer, BYTECODE_MAGIC);
  write_pod(buffer, BYTECODE_VERSION);
  write_pod(buffer, static_cast<std::uint64_t>(0));

  write_header(buffer, module.header);

  {
    const auto count = static_cast<std::uint32_t>(module.dependencies.size());
    write_pod(buffer, count);
    for (const auto& dependency : module.dependencies) {
      write_dependency(buffer, dependency);
    }
  }

  {
    const auto count = static_cast<std::uint32_t>(module.exports.size());
    write_pod(buffer, count);
    for (const auto& symbol : module.exports) {
      write_export(buffer, symbol);
    }
  }

  write_instruction_stream(buffer, module.instructions);

  {
    const auto count = static_cast<std::uint32_t>(module.constants.size());
    write_pod(buffer, count);
    for (const auto& cv : module.constants) {
      write_const_value(buffer, cv);
    }
  }

  {
    const auto count = static_cast<std::uint32_t>(module.functions.size());
    write_pod(buffer, count);
    for (const auto& [name, arity, has_variadic_tail, is_import_placeholder, instructions, generic_params,
                      generic_bounds, param_type_names, return_type_name, return_type_is_dict_string_any,
                      return_type_is_exact_uint64, return_type_is_exact_int64, return_type_is_task_handle,
                      return_type_is_random_generator, return_type_is_file_handle, hidden_operator_params,
                      operator_requirements, is_value_binding] : module.functions) {
      write_string(buffer, name);
      write_pod(buffer, arity);
      write_pod(buffer, has_variadic_tail);
      write_pod(buffer, is_import_placeholder);
      write_instruction_stream(buffer, instructions);
      write_string_list(buffer, generic_params);
      write_string_list(buffer, generic_bounds);
      write_string_list(buffer, param_type_names);
      write_string(buffer, return_type_name);
      write_pod(buffer, return_type_is_dict_string_any);
      write_pod(buffer, return_type_is_exact_uint64);
      write_pod(buffer, return_type_is_exact_int64);
      write_pod(buffer, return_type_is_task_handle);
      write_pod(buffer, return_type_is_random_generator);
      write_pod(buffer, return_type_is_file_handle);
      write_pod(buffer, hidden_operator_params);
      write_pod(buffer, static_cast<std::uint32_t>(operator_requirements.size()));
      for (const auto& requirement : operator_requirements) {
        write_string(buffer, requirement.op);
        write_string(buffer, requirement.type_param);
        write_string(buffer, requirement.signature);
        write_string_list(buffer, requirement.alternative_functions);
      }
      write_pod(buffer, is_value_binding);
    }
  }

  {
    const auto count = static_cast<std::uint32_t>(module.closures.size());
    write_pod(buffer, count);
    for (const auto& [function_index, capture_count, declared_arity, declared_has_variadic_tail] : module.closures) {
      write_pod(buffer, function_index);
      write_pod(buffer, capture_count);
      write_pod(buffer, declared_arity);
      write_pod(buffer, declared_has_variadic_tail);
    }
  }

  {
    const auto count = static_cast<std::uint32_t>(module.builtin_callsites.size());
    write_pod(buffer, count);
    for (const auto& record : module.builtin_callsites) {
      write_builtin_callsite(buffer, record);
    }
  }

  {
    const auto count = static_cast<std::uint32_t>(module.type_checks.size());
    write_pod(buffer, count);
    for (const auto& check : module.type_checks) {
      write_type_check(buffer, check);
    }
  }

  write_pod(buffer, static_cast<std::uint32_t>(module.native_functions.size()));
  for (const auto& [symbol, return_check] : module.native_functions) {
    write_string(buffer, symbol);
    write_type_check(buffer, return_check);
  }

  write_pod(buffer, static_cast<std::uint32_t>(module.type_decls.size()));
  for (const auto& [name, generic_params, is_builtin, target] : module.type_decls) {
    write_string(buffer, name);
    write_string_list(buffer, generic_params);
    write_pod(buffer, is_builtin);
    write_string(buffer, target);
  }
  write_pod(buffer, static_cast<std::uint32_t>(module.alias_decls.size()));
  for (const auto& [name, target] : module.alias_decls) {
    write_string(buffer, name);
    write_string(buffer, target);
  }

  const auto checksum = checksum_bytes(buffer, kPayloadOffset);
  patch_checksum(buffer, checksum);
  return buffer;
}

auto deserialize_module(const std::vector<std::uint8_t>& buffer) -> tl::expected<Module, SerializationError> {
  Module deserialized;
  std::size_t offset = 0;

  if (std::uint32_t magic = 0; !read_pod(buffer, offset, magic) || magic != BYTECODE_MAGIC) {
    return tl::unexpected(SerializationError{.message = "Invalid magic number"});
  }

  if (std::uint32_t version = 0; !read_pod(buffer, offset, version) || version != BYTECODE_VERSION) {
    return tl::unexpected(SerializationError{.message = "Version mismatch"});
  }

  if (!read_pod(buffer, offset, deserialized.header.payload_checksum)) {
    return tl::unexpected(SerializationError{.message = "Cannot read payload checksum"});
  }
  if (deserialized.header.payload_checksum != checksum_bytes(buffer, kPayloadOffset)) {
    return tl::unexpected(SerializationError{.message = "Payload checksum mismatch"});
  }

  if (!read_header(buffer, offset, deserialized.header)) {
    return tl::unexpected(SerializationError{.message = "Cannot read module header"});
  }
  deserialized.header.payload_checksum = checksum_bytes(buffer, kPayloadOffset);

  {
    std::uint32_t count = 0;
    if (!read_pod(buffer, offset, count)) {
      return tl::unexpected(SerializationError{.message = "Cannot read dependency count"});
    }
    deserialized.dependencies.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      ModuleDependency dependency;
      if (!read_dependency(buffer, offset, dependency)) {
        return tl::unexpected(SerializationError{.message = "Cannot read dependency"});
      }
      deserialized.dependencies.push_back(std::move(dependency));
    }
  }

  {
    std::uint32_t count = 0;
    if (!read_pod(buffer, offset, count)) {
      return tl::unexpected(SerializationError{.message = "Cannot read export count"});
    }
    deserialized.exports.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      ExportedSymbol symbol;
      if (!read_export(buffer, offset, symbol)) {
        return tl::unexpected(SerializationError{.message = "Cannot read export"});
      }
      deserialized.exports.push_back(std::move(symbol));
    }
  }

  if (const auto instruction_error = read_instruction_stream(buffer, offset, deserialized.instructions);
      instruction_error.has_value()) {
    return tl::unexpected(SerializationError{.message = *instruction_error});
  }

  {
    std::uint32_t count = 0;
    if (!read_pod(buffer, offset, count)) {
      return tl::unexpected(SerializationError{.message = "Cannot read constant count"});
    }
    deserialized.constants.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      ConstValue cv;
      if (!read_const_value(buffer, offset, cv)) {
        return tl::unexpected(SerializationError{.message = "Cannot read constant"});
      }
      deserialized.constants.push_back(std::move(cv));
    }
  }

  {
    std::uint32_t count = 0;
    if (!read_pod(buffer, offset, count)) {
      return tl::unexpected(SerializationError{.message = "Cannot read function count"});
    }
    deserialized.functions.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      FunctionDef fn;
      if (!read_string(buffer, offset, fn.name) || !read_pod(buffer, offset, fn.arity) ||
          !read_pod(buffer, offset, fn.has_variadic_tail) || !read_pod(buffer, offset, fn.is_import_placeholder)) {
        return tl::unexpected(SerializationError{.message = "Cannot read function"});
      }
      if (const auto instruction_error = read_instruction_stream(buffer, offset, fn.instructions);
          instruction_error.has_value()) {
        return tl::unexpected(
            SerializationError{.message = std::format("Cannot read function '{}': {}", fn.name, *instruction_error)});
      }
      if (!read_string_list(buffer, offset, fn.generic_params) ||
          !read_string_list(buffer, offset, fn.generic_bounds) ||
          !read_string_list(buffer, offset, fn.param_type_names) || !read_string(buffer, offset, fn.return_type_name) ||
          !read_pod(buffer, offset, fn.return_type_is_dict_string_any) ||
          !read_pod(buffer, offset, fn.return_type_is_exact_uint64) ||
          !read_pod(buffer, offset, fn.return_type_is_exact_int64) ||
          !read_pod(buffer, offset, fn.return_type_is_task_handle) ||
          !read_pod(buffer, offset, fn.return_type_is_random_generator) ||
          !read_pod(buffer, offset, fn.return_type_is_file_handle)) {
        return tl::unexpected(SerializationError{.message = "Cannot read function"});
      }
      std::uint32_t requirement_count = 0;
      if (!read_pod(buffer, offset, fn.hidden_operator_params) || !read_pod(buffer, offset, requirement_count) ||
          requirement_count > buffer.size() - offset || fn.hidden_operator_params > fn.arity) {
        return tl::unexpected(SerializationError{.message = "Cannot read function operator requirements"});
      }
      fn.operator_requirements.resize(requirement_count);
      for (auto& requirement : fn.operator_requirements) {
        if (!read_string(buffer, offset, requirement.op) || !read_string(buffer, offset, requirement.type_param) ||
            !read_string(buffer, offset, requirement.signature) ||
            !read_string_list(buffer, offset, requirement.alternative_functions)) {
          return tl::unexpected(SerializationError{.message = "Cannot read function operator requirements"});
        }
      }
      if (!read_pod(buffer, offset, fn.is_value_binding)) {
        return tl::unexpected(SerializationError{.message = "Cannot read function"});
      }
      deserialized.functions.push_back(std::move(fn));
    }
  }

  {
    std::uint32_t count = 0;
    if (!read_pod(buffer, offset, count)) {
      return tl::unexpected(SerializationError{.message = "Cannot read closure count"});
    }
    deserialized.closures.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      ClosureDef closure;
      if (!read_pod(buffer, offset, closure.function_index) || !read_pod(buffer, offset, closure.capture_count) ||
          !read_pod(buffer, offset, closure.declared_arity) ||
          !read_pod(buffer, offset, closure.declared_has_variadic_tail)) {
        return tl::unexpected(SerializationError{.message = "Cannot read closure"});
      }
      deserialized.closures.push_back(closure);
    }
  }

  {
    std::uint32_t count = 0;
    if (!read_pod(buffer, offset, count)) {
      return tl::unexpected(SerializationError{.message = "Cannot read builtin callsite count"});
    }
    deserialized.builtin_callsites.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      BuiltinCallsiteRecord record;
      if (!read_builtin_callsite(buffer, offset, record)) {
        return tl::unexpected(SerializationError{.message = "Cannot read builtin callsite"});
      }
      deserialized.builtin_callsites.push_back(record);
    }
  }

  {
    std::uint32_t count = 0;
    if (!read_pod(buffer, offset, count) || count > buffer.size() - offset) {
      return tl::unexpected(SerializationError{.message = "Cannot read type check count"});
    }
    deserialized.type_checks.resize(count);
    for (auto& check : deserialized.type_checks) {
      if (!read_type_check(buffer, offset, check, 0U) || !common::is_well_formed(check) ||
          !common::is_cast_check(check)) {
        return tl::unexpected(SerializationError{.message = "Cannot read type check"});
      }
    }
  }

  {
    std::uint32_t count = 0;
    if (!read_pod(buffer, offset, count) || count > buffer.size() - offset) {
      return tl::unexpected(SerializationError{.message = "Cannot read native function count"});
    }
    deserialized.native_functions.resize(count);
    for (auto& [symbol, return_check] : deserialized.native_functions) {
      if (!read_string(buffer, offset, symbol) || symbol.empty() || !read_type_check(buffer, offset, return_check, 0U) ||
          !common::is_well_formed(return_check) || !common::is_cast_check(return_check)) {
        return tl::unexpected(SerializationError{.message = "Cannot read native function"});
      }
    }
  }

  {
    std::uint32_t count = 0;
    if (!read_pod(buffer, offset, count) || count > buffer.size() - offset) {
      return tl::unexpected(SerializationError{.message = "Cannot read type declarations"});
    }
    deserialized.type_decls.resize(count);
    for (auto& record : deserialized.type_decls) {
      if (!read_string(buffer, offset, record.name) || !read_string_list(buffer, offset, record.generic_params) ||
          !read_pod(buffer, offset, record.is_builtin) || !read_string(buffer, offset, record.target)) {
        return tl::unexpected(SerializationError{.message = "Cannot read type declarations"});
      }
    }
    if (!read_pod(buffer, offset, count) || count > buffer.size() - offset) {
      return tl::unexpected(SerializationError{.message = "Cannot read alias declarations"});
    }
    deserialized.alias_decls.resize(count);
    for (auto& record : deserialized.alias_decls) {
      if (!read_string(buffer, offset, record.name) || !read_string(buffer, offset, record.target)) {
        return tl::unexpected(SerializationError{.message = "Cannot read alias declarations"});
      }
    }
  }

  return deserialized;
}

auto disassemble_module(const Module& module, std::ostream& out) -> tl::expected<void, ModuleDumpError> {
  out << std::format(
      "ModuleHeader:\n"
      "  module_name:       {}\n"
      "  source_path:       {}\n"
      "  source_hash:       {}\n"
      "  payload_checksum:  {}\n"
      "  optimization_mode: {}\n",
      module.header.module_name, module.header.source_path, module.header.source_hash, module.header.payload_checksum,
      module.header.optimization_mode);

  out << "Dependencies:\n";
  for (const auto& [module_name, is_symbolic] : module.dependencies) {
    out << std::format(
        "  Dependency:\n"
        "    module_name: {}\n"
        "    is_symbolic: {}\n",
        module_name, is_symbolic);
  }

  out << "Exports:\n";
  for (const auto& [name, link_name, kind, index, builtin_name, host_entry_check] : module.exports) {
    out << std::format(
        "  ExportedSymbol:\n"
        "    name: {}\n"
        "    link_name: {}\n"
        "    kind: {}\n"
        "    index: {}\n"
        "    builtin_name: {}\n",
        name, link_name, static_cast<std::uint8_t>(kind), index, builtin_name);
    if (host_entry_check.has_value()) {
      out << "    host_entry_check:";
      for (const auto& param : host_entry_check->params) {
        out << " " << common::describe(param);
      }
      out << (host_entry_check->variadic_tail ? " (variadic tail)\n" : "\n");
    }
  }

  out << "Instructions:\n";
  size_t instruction_index = 0;
  for (const auto& [opcode, operand] : module.instructions) {
    print_instruction(out, instruction_index, module, module.instructions,
                      Instruction{.opcode = opcode, .operand = operand}, "    ");

    instruction_index++;
  }

  out << "Constants:\n";
  size_t constant_index = 0;
  for (const auto& [data] : module.constants) {
    using VisitRType = std::pair<std::string, std::string>;

    const auto [type, value] = std::visit(
        common::overloaded{
            [](const std::int64_t& val) -> VisitRType { return {"int64_t", std::format("{}({:x})", val, val)}; },
            [](const std::uint64_t& val) -> VisitRType { return {"uint64_t", std::format("{}({:x})", val, val)}; },
            [](const double& val) -> VisitRType {
              return {"double", std::format("{}({:x})", val, std::bit_cast<std::uint64_t>(val))};
            },
            [](const bool& val) -> VisitRType { return {"bool", val ? "true" : "false"}; },
            [](const std::string& val) -> VisitRType { return {"string", val}; },
            [](const std::monostate&) -> VisitRType { return {"null", "null"}; }},
        data);

    out << std::format(
        "  Constant: {}\n"
        "    type: {}\n"
        "    value: {}\n",
        constant_index, type, value);

    constant_index++;
  }

  out << "Functions:\n";
  size_t func_index = 0;
  for (const auto& [name, arity, has_variadic_tail, is_import_placeholder, instructions, generic_params,
                    generic_bounds, param_type_names, return_type_name, return_type_is_dict_string_any,
                    return_type_is_exact_uint64, return_type_is_exact_int64, return_type_is_task_handle,
                    return_type_is_random_generator, return_type_is_file_handle, hidden_operator_params,
                    operator_requirements, is_value_binding] : module.functions) {
    out << std::format(
        "  Function {}:\n"
        "    name: {}\n"
        "    arity: {}\n"
        "    has_variadic_tail: {}\n"
        "    is_import_placeholder: {}\n"
        "    return_type_name: {}\n"
        "    return_type_is_dict_string_any: {}\n"
        "    return_type_is_exact_uint64: {}\n"
        "    return_type_is_exact_int64: {}\n"
        "    return_type_is_task_handle: {}\n"
        "    return_type_is_random_generator: {}\n"
        "    return_type_is_file_handle: {}\n"
        "    is_value_binding: {}\n",
        func_index, name, arity, has_variadic_tail, is_import_placeholder, return_type_name,
        return_type_is_dict_string_any, return_type_is_exact_uint64, return_type_is_exact_int64,
        return_type_is_task_handle, return_type_is_random_generator, return_type_is_file_handle, is_value_binding);

    if (!generic_params.empty()) {
      out << "    generic_params:";
      for (std::size_t index = 0; index < generic_params.size(); ++index) {
        out << " " << generic_params[index];
        if (index < generic_bounds.size() && !generic_bounds[index].empty()) {
          out << ": " << generic_bounds[index];
        }
      }
      out << "\n";
    }
    for (const auto& requirement : operator_requirements) {
      out << std::format("    operator_requirement: '{}' on {} as {}\n", requirement.op, requirement.type_param,
                         requirement.signature);
    }
    if (hidden_operator_params > 0U) {
      out << std::format("    hidden_operator_params: {}\n", hidden_operator_params);
    }
    if (!param_type_names.empty()) {
      out << "    param_type_names:";
      for (const auto& pt : param_type_names) {
        out << " " << pt;
      }
      out << "\n";
    }

    size_t func_instruction_index = 0;
    for (const auto& [opcode, operand] : instructions) {
      print_instruction(out, func_instruction_index, module, instructions,
                        Instruction{.opcode = opcode, .operand = operand}, "      ");

      func_instruction_index++;
    }

    func_index++;
  }

  out << "Closures:\n";
  size_t closure_index = 0;
  for (const auto& [function_index, capture_count, declared_arity, declared_has_variadic_tail] : module.closures) {
    out << std::format(
        "  Closure {}:\n"
        "    function_index: {}\n"
        "    capture_count: {}\n"
        "    declared_arity: {}\n"
        "    declared_has_variadic_tail: {}\n",
        closure_index, function_index, capture_count, declared_arity, declared_has_variadic_tail);

    closure_index++;
  }

  out << "TypeDecls:\n";
  for (const auto& [name, generic_params, is_builtin, target] : module.type_decls) {
    std::string params;
    for (const auto& param : generic_params) {
      params += (params.empty() ? "" : ", ") + param;
    }
    out << std::format("  type {}{} {}\n", name, params.empty() ? "" : "<" + params + ">",
                       is_builtin ? std::string{":: __builtin__"} : "= " + target);
  }
  out << "AliasDecls:\n";
  for (const auto& [name, target] : module.alias_decls) {
    out << std::format("  alias {} = {}\n", name, target);
  }

  out << "TypeChecks:\n";
  for (std::size_t check_index = 0; check_index < module.type_checks.size(); ++check_index) {
    out << std::format("  TypeCheck {}: {}\n", check_index, common::describe(module.type_checks[check_index]));
  }

  out << "NativeFunctions:\n";
  for (std::size_t native_index = 0; native_index < module.native_functions.size(); ++native_index) {
    const auto& [symbol, return_check] = module.native_functions[native_index];
    out << std::format("  NativeFunction {}: {} -> {}\n", native_index, symbol, common::describe(return_check));
  }

  out << "BuiltinCallsites:\n";
  size_t callsite_index = 0;
  for (const auto& record : module.builtin_callsites) {
    out << std::format(
        "  BuiltinCallsite {}:\n"
        "    owner_kind: {}\n"
        "    owner_index: {}\n"
        "    instruction_index: {}\n"
        "    builtin: {}\n"
        "    call_shape: {}\n"
        "    static_argument_count: {}\n"
        "    callable_slots: {}\n"
        "    value_slots: {}\n",
        callsite_index,
        record.owner_kind == BuiltinCallsiteOwnerKind::kModule ? "module" : "function",
        record.owner_index,
        record.instruction_index,
        fleaux::vm::builtin_name(record.builtin_id),
        record.call_shape == fleaux::vm::BuiltinCallShape::kDirectValue ? "direct" : "tuple_expanded",
        record.static_argument_count == kUnknownBuiltinStaticArgumentCount
            ? std::string{"unknown"}
            : std::to_string(record.static_argument_count),
        record.callable_slots.size(), record.value_slots.size());

    for (const auto& callable_slot : record.callable_slots) {
      out << std::format("      slot {}: kind={} target_index={} declared_arity={} variadic={}\n", callable_slot.slot_index,
                         callable_slot.kind == BuiltinCallableSlotKind::kFunction
                             ? "function"
                             : callable_slot.kind == BuiltinCallableSlotKind::kClosure ? "closure" : "builtin",
                         callable_slot.target_index, callable_slot.declared_arity, callable_slot.has_variadic_tail);
    }

    for (const auto& value_slot : record.value_slots) {
      out << std::format("      value slot {}: summary={} source_kind={} source_index={}\n", value_slot.slot_index,
                         value_slot.summary_kind == BuiltinValueSlotSummaryKind::kDictStringAny
                             ? "Dict<String, Any>"
                             : value_slot.summary_kind == BuiltinValueSlotSummaryKind::kExactUInt64
                                   ? "UInt64"
                                   : value_slot.summary_kind == BuiltinValueSlotSummaryKind::kExactInt64 ? "Int64"
                                          : value_slot.summary_kind == BuiltinValueSlotSummaryKind::kTaskHandle
                                                ? "TaskHandle"
                                                : value_slot.summary_kind == BuiltinValueSlotSummaryKind::kRandomGenerator
                                                      ? "RandomGenerator"
                                                      : value_slot.summary_kind == BuiltinValueSlotSummaryKind::kFileHandle
                                                            ? "FileHandle"
                                                            : "unknown",
                         value_slot.source_kind == BuiltinValueSlotSourceKind::kFunctionReturn ? "function_return" : "unknown",
                         value_slot.source_index);
    }

    callsite_index++;
  }

  return {};
}

}  // namespace fleaux::bytecode
