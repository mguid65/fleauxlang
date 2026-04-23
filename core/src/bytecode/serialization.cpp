#include "fleaux/bytecode/serialization.hpp"
#include "fleaux/common/overloaded.hpp"

#include <cstring>
#include <type_traits>

namespace fleaux::bytecode {

namespace {

constexpr std::uint32_t BYTECODE_MAGIC = 0x464C4558;
constexpr std::uint32_t BYTECODE_VERSION = 3;
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
  if (offset + sizeof(T) > buffer.size()) { return false; }
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
  if (!read_pod(buffer, offset, len)) { return false; }
  if (offset + len > buffer.size()) { return false; }
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
  if (!read_pod(buffer, offset, tag)) { return false; }
  switch (tag) {
    case 0: {
      std::int64_t val = 0;
      if (!read_pod(buffer, offset, val)) { return false; }
      out.data = val;
      return true;
    }
    case 1: {
      std::uint64_t val = 0;
      if (!read_pod(buffer, offset, val)) { return false; }
      out.data = val;
      return true;
    }
    case 2: {
      double val = 0;
      if (!read_pod(buffer, offset, val)) { return false; }
      out.data = val;
      return true;
    }
    case 3: {
      bool val = false;
      if (!read_pod(buffer, offset, val)) { return false; }
      out.data = val;
      return true;
    }
    case 4: {
      std::string val;
      if (!read_string(buffer, offset, val)) { return false; }
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
      !read_pod(buffer, offset, kind) ||
      !read_pod(buffer, offset, symbol.index) || !read_string(buffer, offset, symbol.builtin_name)) {
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
  if (!read_pod(buffer, offset, count)) { return false; }
  instructions.reserve(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint32_t opcode_val = 0;
    std::int64_t operand = 0;
    if (!read_pod(buffer, offset, opcode_val) || !read_pod(buffer, offset, operand)) { return false; }
    instructions.push_back({.opcode = static_cast<Opcode>(opcode_val), .operand = operand});
  }
  return true;
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
    for (const auto& dependency : module.dependencies) { write_dependency(buffer, dependency); }
  }

  {
    const auto count = static_cast<std::uint32_t>(module.exports.size());
    write_pod(buffer, count);
    for (const auto& symbol : module.exports) { write_export(buffer, symbol); }
  }

  write_instruction_stream(buffer, module.instructions);

  {
    const auto count = static_cast<std::uint32_t>(module.constants.size());
    write_pod(buffer, count);
    for (const auto& cv : module.constants) { write_const_value(buffer, cv); }
  }

  {
    const auto count = static_cast<std::uint32_t>(module.builtin_names.size());
    write_pod(buffer, count);
    for (const auto& name : module.builtin_names) { write_string(buffer, name); }
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
    }
  }

  {
    const auto count = static_cast<std::uint32_t>(module.closures.size());
    write_pod(buffer, count);
    for (const auto& closure : module.closures) {
      write_pod(buffer, closure.function_index);
      write_pod(buffer, closure.capture_count);
      write_pod(buffer, closure.declared_arity);
      write_pod(buffer, closure.declared_has_variadic_tail);
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
      return tl::unexpected(SerializationError{.message = "Cannot read builtin count"});
    }
    deserialized.builtin_names.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      std::string name;
      if (!read_string(buffer, offset, name)) {
        return tl::unexpected(SerializationError{.message = "Cannot read builtin name"});
      }
      deserialized.builtin_names.push_back(std::move(name));
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
          !read_instruction_stream(buffer, offset, fn.instructions)) {
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

}  // namespace fleaux::bytecode
