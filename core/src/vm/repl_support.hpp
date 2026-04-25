#pragma once

#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include <tl/expected.hpp>

#include "fleaux/frontend/ast.hpp"
#include "fleaux/frontend/diagnostics.hpp"
#include "fleaux/frontend/source_loader.hpp"
#include "fleaux/frontend/type_check.hpp"

namespace fleaux::vm::detail {

struct ReplSessionError {
  std::string message;
  std::optional<std::string> hint;
  std::optional<frontend::diag::SourceSpan> span;
};

inline auto make_repl_session_error(const std::string& message, const std::optional<std::string>& hint = std::nullopt,
                                    const std::optional<frontend::diag::SourceSpan>& span = std::nullopt)
    -> ReplSessionError {
  return ReplSessionError{
      .message = message,
      .hint = hint,
      .span = span,
  };
}

inline auto repl_let_key(const std::optional<std::string>& qualifier, const std::string& name) -> std::string {
  return frontend::source_loader::symbol_key(qualifier, name);
}

inline auto repl_let_internal_key(const frontend::ir::IRLet& let) -> std::string {
  return let.symbol_key.empty() ? repl_let_key(let.qualifier, let.name) : let.symbol_key;
}

inline auto ensure_repl_imports_supported(const frontend::ir::IRProgram& program)
    -> tl::expected<void, ReplSessionError> {
  for (const auto& [module_name, span] : program.imports) {
    if (module_name == "Std" || module_name == "StdBuiltins") { continue; }
    return tl::unexpected(make_repl_session_error("REPL only supports symbolic imports: Std, StdBuiltins.",
                                                  "Define helper lets inline, or run a file for module imports.",
                                                  span));
  }
  return {};
}

inline auto parse_and_analyze_repl_text(const std::string& source_text, const std::string& source_name,
                                        const std::vector<frontend::ir::IRLet>& prior_session_lets)
    -> tl::expected<frontend::ir::IRProgram, ReplSessionError> {
  auto lowered = frontend::source_loader::parse_text_to_lowered_ir<ReplSessionError>(
      source_text, source_name, make_repl_session_error);
  if (!lowered) { return tl::unexpected(lowered.error()); }

  if (auto imports_ok = ensure_repl_imports_supported(*lowered); !imports_ok) {
    return tl::unexpected(imports_ok.error());
  }

  std::unordered_set<std::string> replaced_keys;
  replaced_keys.reserve(lowered->lets.size());
  for (const auto& let : lowered->lets) { replaced_keys.insert(repl_let_internal_key(let)); }

  std::vector<frontend::ir::IRLet> imported_typed_lets;
  imported_typed_lets.reserve(prior_session_lets.size());
  for (const auto& prior_let : prior_session_lets) {
    if (replaced_keys.contains(repl_let_internal_key(prior_let))) { continue; }
    imported_typed_lets.push_back(prior_let);
  }

  const std::unordered_set<std::string> imported_symbols;
  const auto analyzed = frontend::type_check::analyze_program(*lowered, imported_symbols, imported_typed_lets);
  if (!analyzed) {
    return tl::unexpected(
        make_repl_session_error(analyzed.error().message, analyzed.error().hint, analyzed.error().span));
  }

  return analyzed.value();
}

inline auto merge_repl_session_lets(const std::vector<frontend::ir::IRLet>& prior_session_lets,
                                    const std::vector<frontend::ir::IRLet>& snippet_lets)
    -> std::vector<frontend::ir::IRLet> {
  std::unordered_set<std::string> replaced_keys;
  replaced_keys.reserve(snippet_lets.size());
  for (const auto& let : snippet_lets) { replaced_keys.insert(repl_let_internal_key(let)); }

  std::vector<frontend::ir::IRLet> merged_lets;
  merged_lets.reserve(prior_session_lets.size() + snippet_lets.size());
  for (const auto& prior_let : prior_session_lets) {
    if (replaced_keys.contains(repl_let_internal_key(prior_let))) { continue; }
    merged_lets.push_back(prior_let);
  }
  merged_lets.insert(merged_lets.end(), snippet_lets.begin(), snippet_lets.end());
  return merged_lets;
}

}  // namespace fleaux::vm::detail

