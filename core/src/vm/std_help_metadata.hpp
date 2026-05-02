#pragma once

#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>

#include "fleaux/common/embedded_resource.hpp"
#include "fleaux/frontend/source_loader.hpp"
#include "fleaux/runtime/runtime_support.hpp"

namespace fleaux::vm::detail {

[[nodiscard]] inline auto format_help_ir_type(const frontend::ir::IRSimpleType& type) -> std::string {
  const auto append_variadic = [](std::string base, const bool variadic) -> std::string {
    if (variadic) {
      base += "...";
    }
    return base;
  };

  if (!type.alternatives.empty()) {
    std::ostringstream out;
    for (std::size_t idx = 0; idx < type.alternatives.size(); ++idx) {
      if (idx > 0) {
        out << " | ";
      }
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
    if (idx > 0) {
      out << ", ";
    }
    out << let.params[idx].name << ": " << format_help_ir_type(let.params[idx].type);
  }
  out << "): " << format_help_ir_type(let.return_type);
  if (let.is_builtin) {
    out << " :: __builtin__";
  }
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

[[nodiscard]] inline auto load_std_doc_comments_by_symbol(const std::string_view source_text)
    -> std::unordered_map<std::string, std::vector<std::string>> {
  std::unordered_map<std::string, std::vector<std::string>> docs_by_symbol;
  if (source_text.empty()) {
    return docs_by_symbol;
  }

  std::istringstream input{std::string(source_text)};
  std::string line;
  std::vector<std::string> pending_comments;

  const auto clear_pending = [&]() -> void { pending_comments.clear(); };
  while (std::getline(input, line)) {
    const std::string trimmed = fleaux::runtime::detail::trim_copy(line);
    if (trimmed.empty()) {
      clear_pending();
      continue;
    }

    if (std::string_view(trimmed).starts_with("//")) {
      pending_comments.push_back(fleaux::runtime::detail::trim_copy(std::string_view(trimmed).substr(2)));
      continue;
    }

    if (std::string_view(trimmed).starts_with("let ")) {
      const std::string rest = fleaux::runtime::detail::trim_copy(std::string_view(trimmed).substr(4));
      const auto generic_pos = rest.find('<');
      const auto params_pos = rest.find('(');
      const auto end_pos = std::min(generic_pos == std::string::npos ? rest.size() : generic_pos,
                                    params_pos == std::string::npos ? rest.size() : params_pos);
      if (const std::string symbol_name = fleaux::runtime::detail::trim_copy(rest.substr(0, end_pos));
          !symbol_name.empty() && !pending_comments.empty()) {
        docs_by_symbol[symbol_name] = pending_comments;
      }
      clear_pending();
      continue;
    }

    clear_pending();
  }

  return docs_by_symbol;
}

inline auto preload_std_help_metadata() -> void {
  const auto embedded_std = fleaux::common::embedded_resource_text("Std.fleaux");
  if (!embedded_std.has_value()) {
    return;
  }

  const std::string source_text{*embedded_std};
  const auto parsed_std = frontend::source_loader::parse_text_to_lowered_ir<std::string>(
      source_text, "Std.fleaux",
      [](const std::string& message, const std::optional<std::string>& hint,
         const std::optional<frontend::diag::SourceSpan>&) -> std::string {
        return hint.has_value() ? message + " (" + *hint + ")" : message;
      });

  if (!parsed_std) {
    return;
  }

  const auto docs_by_symbol = load_std_doc_comments_by_symbol(source_text);

  for (const auto& let : parsed_std->lets) {
    if (!let.qualifier.has_value()) {
      continue;
    }
    if (!(let.qualifier.value() == "Std" || let.qualifier->starts_with("Std."))) {
      continue;
    }

    auto let_with_docs = let;
    if (const auto it = docs_by_symbol.find(frontend::source_loader::symbol_key(let.qualifier, let.name));
        it != docs_by_symbol.end()) {
      let_with_docs.doc_comments = it->second;
    }
    register_help_for_let(let_with_docs);
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
