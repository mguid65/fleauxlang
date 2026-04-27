#include "fleaux/bytecode/serialization.hpp"
#include "fleaux/common/overloaded.hpp"
#include "fleaux/runtime/builtins_core.hpp"
#include "fleaux/vm/builtin_catalog.hpp"

#include <bit>
#include <cstring>
#include <format>
#include <iostream>
#include <type_traits>

namespace fleaux::bytecode {

namespace {

constexpr std::uint32_t BYTECODE_MAGIC = 0x464C4558;
constexpr std::uint32_t BYTECODE_VERSION = 6;
constexpr std::size_t kChecksumOffset = sizeof(std::uint32_t) * 2;
constexpr std::size_t kPayloadOffset = kChecksumOffset + sizeof(std::uint64_t);
constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

template <typename T>
void write_pod(std::vector<std::uint8_t>& buffer, const T& value) {
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
  buffer.insert(buffer.end(), bytes, bytes + sizeof(T));
}

template <typename T>
auto read_pod(const std::vector<std::uint8_t>& buffer, std::size_t& offset, T& out) -> bool {
  if (offset + sizeof(T) > buffer.size()) {
    return false;
  }
  std::memcpy(&out, &buffer[offset], sizeof(T));
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
  out = std::string(reinterpret_cast<const char*>(&buffer[offset]), len);
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
  std::memcpy(buffer.data() + kChecksumOffset, &checksum, sizeof(checksum));
}

void write_const_value(std::vector<std::uint8_t>& buffer, const ConstValue& cv) {
  std::visit(
      common::overloaded{[&](const std::int64_t& value) -> void {
                           write_pod(buffer, static_cast<std::uint8_t>(0));
                           write_pod(buffer, value);
                         },
                         [&](const std::uint64_t& value) -> void {
                           write_pod(buffer, static_cast<std::uint8_t>(1));
                           write_pod(buffer, value);
                         },
                         [&](const double& value) -> void {
                           write_pod(buffer, static_cast<std::uint8_t>(2));
                           write_pod(buffer, value);
                         },
                         [&](const bool& value) -> void {
                           write_pod(buffer, static_cast<std::uint8_t>(3));
                           write_pod(buffer, value);
                         },
                         [&](const std::string& value) -> void {
                           write_pod(buffer, static_cast<std::uint8_t>(4));
                           write_string(buffer, value);
                         },
                         [&](const std::monostate&) -> void { write_pod(buffer, static_cast<std::uint8_t>(5)); }},
      cv.data);
}

auto read_const_value(const std::vector<std::uint8_t>& buffer, std::size_t& offset, ConstValue& out) -> bool {
  std::uint8_t tag = 0;
  if (!read_pod(buffer, offset, tag)) {
    return false;
  }
  switch (tag) {
    case 0: {
      std::int64_t val = 0;
      if (!read_pod(buffer, offset, val)) {
        return false;
      }
      out.data = val;
      return true;
    }
    case 1: {
      std::uint64_t val = 0;
      if (!read_pod(buffer, offset, val)) {
        return false;
      }
      out.data = val;
      return true;
    }
    case 2: {
      double val = 0;
      if (!read_pod(buffer, offset, val)) {
        return false;
      }
      out.data = val;
      return true;
    }
    case 3: {
      bool val = false;
      if (!read_pod(buffer, offset, val)) {
        return false;
      }
      out.data = val;
      return true;
    }
    case 4: {
      std::string val;
      if (!read_string(buffer, offset, val)) {
        return false;
      }
      out.data = std::move(val);
      return true;
    }
    case 5: {
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

void write_export(std::vector<std::uint8_t>& buffer, const ExportedSymbol& symbol) {
  write_string(buffer, symbol.name);
  write_string(buffer, symbol.link_name);
  write_pod(buffer, static_cast<std::uint8_t>(symbol.kind));
  write_pod(buffer, symbol.index);
  write_string(buffer, symbol.builtin_name);
}

auto read_export(const std::vector<std::uint8_t>& buffer, std::size_t& offset, ExportedSymbol& symbol) -> bool {
  std::uint8_t kind = 0;
  if (!read_string(buffer, offset, symbol.name) || !read_string(buffer, offset, symbol.link_name) ||
      !read_pod(buffer, offset, kind) || !read_pod(buffer, offset, symbol.index) ||
      !read_string(buffer, offset, symbol.builtin_name)) {
    return false;
  }
  symbol.kind = static_cast<ExportKind>(kind);
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
                             std::vector<Instruction>& instructions) -> bool {
  std::uint32_t count = 0;
  if (!read_pod(buffer, offset, count)) {
    return false;
  }
  instructions.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint32_t opcode_val = 0;
    std::int64_t operand = 0;
    if (!read_pod(buffer, offset, opcode_val) || !read_pod(buffer, offset, operand)) {
      return false;
    }
    instructions.push_back({.opcode = static_cast<Opcode>(opcode_val), .operand = operand});
  }
  return true;
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

auto valid_index(const std::int64_t operand, const std::size_t size) -> bool {
  return operand >= 0 && static_cast<std::size_t>(operand) < size;
}

auto describe_operand(const Module& module, const std::vector<Instruction>& stream, const Instruction& instruction)
    -> std::string {
  const auto index_label = [](const std::int64_t operand, const std::size_t size, const std::string_view kind) -> std::string {
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
    case Opcode::kCallUserFunc:
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
    case Opcode::kLoadLocal:
      return std::format("local[{}]", instruction.operand);
    case Opcode::kBuildTuple:
      return std::format("tuple_size={}", instruction.operand);
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
  const auto operand_line = annotation.empty() ? std::format("{}operand: {}\n", indent, instruction.operand)
                                               : std::format("{}operand: {} ({})\n", indent, instruction.operand,
                                                             annotation);

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
    for (const auto& fn : module.functions) {
      write_string(buffer, fn.name);
      write_pod(buffer, fn.arity);
      write_pod(buffer, fn.has_variadic_tail);
      write_pod(buffer, fn.is_import_placeholder);
      write_instruction_stream(buffer, fn.instructions);
      write_string_list(buffer, fn.generic_params);
      write_string_list(buffer, fn.param_type_names);
      write_string(buffer, fn.return_type_name);
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

  if (!read_instruction_stream(buffer, offset, deserialized.instructions)) {
    return tl::unexpected(SerializationError{.message = "Cannot read instruction stream"});
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
          !read_pod(buffer, offset, fn.has_variadic_tail) || !read_pod(buffer, offset, fn.is_import_placeholder) ||
          !read_instruction_stream(buffer, offset, fn.instructions) ||
          !read_string_list(buffer, offset, fn.generic_params) ||
          !read_string_list(buffer, offset, fn.param_type_names) ||
          !read_string(buffer, offset, fn.return_type_name)) {
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
  for (const auto& [name, link_name, kind, index, builtin_name] : module.exports) {
    out << std::format(
        "  ExportedSymbol:\n"
        "    name: {}\n"
        "    link_name: {}\n"
        "    kind: {}\n"
        "    index: {}\n"
        "    builtin_name: {}\n",
        name, link_name, static_cast<std::uint8_t>(kind), index, builtin_name);
  }

  out << "Instructions:\n";
  size_t instruction_index = 0;
  for (const auto& [opcode, operand] : module.instructions) {
    print_instruction(out, instruction_index, module, module.instructions, Instruction{.opcode = opcode, .operand = operand},
                      "    ");

    instruction_index++;
  }

  out << "Constants:\n";
  size_t constant_index = 0;
  for (const auto& [data] : module.constants) {
    using VisitRType = std::pair<std::string, std::string>;

    const auto [type, value] = std::visit(
        common::overloaded{
            [](const std::int64_t& val) -> VisitRType {
              return {"int64_t", std::format("{}({:x})", val, val)};
            },
            [](const std::uint64_t& val) -> VisitRType {
              return {"uint64_t", std::format("{}({:x})", val, val)};
            },
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
  for (const auto& fn : module.functions) {
    out << std::format(
        "  Function {}:\n"
        "    name: {}\n"
        "    arity: {}\n"
        "    has_variadic_tail: {}\n"
        "    is_import_placeholder: {}\n"
        "    return_type_name: {}\n",
        func_index, fn.name, fn.arity, fn.has_variadic_tail, fn.is_import_placeholder, fn.return_type_name);

    if (!fn.generic_params.empty()) {
      out << "    generic_params:";
      for (const auto& gp : fn.generic_params) { out << " " << gp; }
      out << "\n";
    }
    if (!fn.param_type_names.empty()) {
      out << "    param_type_names:";
      for (const auto& pt : fn.param_type_names) { out << " " << pt; }
      out << "\n";
    }

    size_t func_instruction_index = 0;
    for (const auto& [opcode, operand] : fn.instructions) {
      print_instruction(out, func_instruction_index, module, fn.instructions,
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

  return {};
}

}  // namespace fleaux::bytecode
