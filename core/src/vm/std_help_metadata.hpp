#pragma once

#include <filesystem>
#include <format>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <variant>

#include "fleaux/common/embedded_resource.hpp"
#include "fleaux/common/overloaded.hpp"
#include "fleaux/frontend/lowering.hpp"
#include "fleaux/frontend/source_loader.hpp"
#include "fleaux/runtime/runtime_support.hpp"

namespace fleaux::vm::detail {

// Formats a declared type in canonical source syntax, keeping the variadic marker of a parameter.
[[nodiscard]] inline auto format_help_ir_type(const frontend::ir::IRSimpleType& type) -> std::string {
  std::string out = frontend::lowering::format_type_text(type);
  if (type.variadic) {
    out += "...";
  }
  return out;
}

[[nodiscard]] inline auto format_help_signature(const frontend::ir::IRLet& let) -> std::string {
  std::ostringstream out;
  out << "let " << frontend::source_loader::symbol_key(let.qualifier, let.name);
  if (let.is_value_binding) {
    out << ": " << format_help_ir_type(let.return_type);
    if (const frontend::ir::IRConstant* literal = frontend::ir::value_let_literal(let); literal != nullptr) {
      out << " = "
          << std::visit(common::overloaded{[](const std::monostate&) -> std::string { return "null"; },
                                           [](const std::string& text) -> std::string { return "\"" + text + "\""; },
                                           [](const bool value) -> std::string { return value ? "True" : "False"; },
                                           [](const std::uint64_t value) -> std::string { return std::format("{}u64", value); },
                                           [](const auto& value) -> std::string { return std::format("{}", value); }},
                        literal->val);
    }
    return out.str();
  }
  if (!let.generic_params.empty()) {
    out << "<";
    for (std::size_t idx = 0; idx < let.generic_params.size(); ++idx) {
      if (idx > 0) {
        out << ", ";
      }
      out << let.generic_params[idx];
      if (idx < let.generic_bounds.size() && let.generic_bounds[idx].has_value()) {
        out << ": " << frontend::lowering::format_type_text(*let.generic_bounds[idx]);
      }
    }
    out << ">";
  }
  out << "(";
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

inline auto register_help_for_let(runtime::HelpMetadataStore& store, const frontend::ir::IRLet& let) -> void {
  fleaux::runtime::register_help_metadata(store, runtime::HelpMetadata{
                                                     .name = frontend::source_loader::symbol_key(let.qualifier, let.name),
                                                     .signature = format_help_signature(let),
                                                     .doc_lines = let.doc_comments,
                                                     .is_builtin = let.is_builtin,
                                                     .is_value = let.is_value_binding,
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
    const std::string trimmed = runtime::detail::trim_copy(line);
    if (trimmed.empty()) {
      clear_pending();
      continue;
    }

    if (std::string_view(trimmed).starts_with("//")) {
      pending_comments.push_back(runtime::detail::trim_copy(std::string_view(trimmed).substr(2)));
      continue;
    }

    if (std::string_view(trimmed).starts_with("let ")) {
      const std::string rest = runtime::detail::trim_copy(std::string_view(trimmed).substr(4));
      const auto generic_pos = rest.find('<');
      const auto params_pos = rest.find('(');
      const auto end_pos = std::min(generic_pos == std::string::npos ? rest.size() : generic_pos,
                                    params_pos == std::string::npos ? rest.size() : params_pos);
      if (const std::string symbol_name = runtime::detail::trim_copy(rest.substr(0, end_pos));
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

[[nodiscard]] inline auto build_std_help_metadata_store() -> runtime::HelpMetadataStore {
  runtime::HelpMetadataStore store;
  const auto embedded_std = common::embedded_resource_text("Std.fleaux");
  if (!embedded_std.has_value()) {
    return store;
  }

  const std::string source_text{*embedded_std};
  const auto parsed_std = frontend::source_loader::parse_text_to_lowered_ir<std::string>(
      source_text, "Std.fleaux",
      [](const std::string& message, const std::optional<std::string>& hint,
         const std::optional<frontend::diag::SourceSpan>&) -> std::string {
        return hint.has_value() ? message + " (" + *hint + ")" : message;
      });

  if (!parsed_std) {
    return store;
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
    register_help_for_let(store, let_with_docs);
  }

  return store;
}

[[nodiscard]] inline auto cached_std_help_metadata_store() -> const runtime::HelpMetadataStore& {
  static const runtime::HelpMetadataStore store = build_std_help_metadata_store();
  return store;
}

// Points Std.Help at the Std metadata for as long as it lives. The metadata is built once and shared, never copied.
class StdHelpMetadataScope {
public:
  StdHelpMetadataScope() : registry_scope_(cached_std_help_metadata_store()) {}

  StdHelpMetadataScope(const StdHelpMetadataScope&) = delete;
  auto operator=(const StdHelpMetadataScope&) -> StdHelpMetadataScope& = delete;
  StdHelpMetadataScope(StdHelpMetadataScope&&) = delete;
  auto operator=(StdHelpMetadataScope&&) -> StdHelpMetadataScope& = delete;

private:
  runtime::ActiveHelpMetadataRegistryScope registry_scope_;
};

}  // namespace fleaux::vm::detail
