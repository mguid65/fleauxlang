#pragma once

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>

#include "fleaux/frontend/source_loader.hpp"
#include "fleaux/runtime/runtime_support.hpp"

namespace fleaux::vm::detail {

[[nodiscard]] inline auto format_help_ir_type(const frontend::ir::IRSimpleType& type) -> std::string {
  const auto append_variadic = [](std::string base, const bool variadic) {
    if (variadic) { base += "..."; }
    return base;
  };

  if (!type.alternatives.empty()) {
    std::ostringstream out;
    for (std::size_t idx = 0; idx < type.alternatives.size(); ++idx) {
      if (idx > 0) { out << " | "; }
      out << type.alternatives[idx];
    }
    return append_variadic(out.str(), type.variadic);
  }

  return append_variadic(type.name, type.variadic);
}

[[nodiscard]] inline auto format_help_signature(const frontend::ir::IRLet& let) -> std::string {
  std::ostringstream out;
  out << "let " << frontend::source_loader::symbol_key(let.qualifier, let.name) << "(";
  for (std::size_t idx = 0; idx < let.params.size(); ++idx) {
    if (idx > 0) { out << ", "; }
    out << let.params[idx].name << ": " << format_help_ir_type(let.params[idx].type);
  }
  out << "): " << format_help_ir_type(let.return_type);
  if (let.is_builtin) { out << " :: __builtin__"; }
  return out.str();
}

inline auto register_help_for_let(const frontend::ir::IRLet& let) -> void {
  fleaux::runtime::register_help_metadata(fleaux::runtime::HelpMetadata{
      .name = frontend::source_loader::symbol_key(let.qualifier, let.name),
      .signature = format_help_signature(let),
      .doc_lines = let.doc_comments,
      .is_builtin = let.is_builtin,
  });
}

[[nodiscard]] inline auto find_std_file() -> std::optional<std::filesystem::path> {
  if (const char* env_path = std::getenv("FLEAUX_STD_PATH"); env_path != nullptr && *env_path != '\0') {
    const std::filesystem::path candidate(env_path);
    if (std::filesystem::exists(candidate)) { return std::filesystem::weakly_canonical(candidate); }
  }

  std::filesystem::path cursor = std::filesystem::current_path();
  while (true) {
    if (const auto candidate = cursor / "Std.fleaux"; std::filesystem::exists(candidate)) {
      return std::filesystem::weakly_canonical(candidate);
    }
    if (cursor == cursor.root_path()) { break; }
    cursor = cursor.parent_path();
  }

  return std::nullopt;
}

inline auto preload_std_help_metadata() -> void {
  const auto std_file = find_std_file();
  if (!std_file.has_value()) { return; }

  const auto parsed_std = frontend::source_loader::parse_file_to_ir<std::string>(
      *std_file,
      [](const std::string& message, const std::optional<std::string>& hint,
         const std::optional<frontend::diag::SourceSpan>&) -> std::string {
        return hint.has_value() ? message + " (" + *hint + ")" : message;
      });
  if (!parsed_std) { return; }

  for (const auto& let : parsed_std->lets) {
    if (!let.qualifier.has_value()) { continue; }
    if (!(let.qualifier.value() == "Std" || let.qualifier->starts_with("Std."))) { continue; }
    register_help_for_let(let);
  }
}

class StdHelpMetadataScope {
public:
  StdHelpMetadataScope() {
    fleaux::runtime::clear_help_metadata_registry();
    preload_std_help_metadata();
  }

  StdHelpMetadataScope(const StdHelpMetadataScope&) = delete;
  auto operator=(const StdHelpMetadataScope&) -> StdHelpMetadataScope& = delete;
  StdHelpMetadataScope(StdHelpMetadataScope&&) = delete;
  auto operator=(StdHelpMetadataScope&&) -> StdHelpMetadataScope& = delete;

  ~StdHelpMetadataScope() { fleaux::runtime::clear_help_metadata_registry(); }
};

}  // namespace fleaux::vm::detail
