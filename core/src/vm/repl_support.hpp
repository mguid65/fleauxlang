#pragma once

#include <charconv>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <tl/expected.hpp>

#include "fleaux/common/embedded_resource.hpp"
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

inline auto repl_normalize_type_name(const std::string& name,
                                     const std::unordered_map<std::string, std::size_t>& generic_slots) -> std::string {
  if (const auto it = generic_slots.find(name); it != generic_slots.end()) { return "G" + std::to_string(it->second); }
  return name;
}

inline auto repl_type_signature_fragment(const frontend::ir::IRSimpleType& type,
                                         const std::unordered_map<std::string, std::size_t>& generic_slots)
    -> std::string {
  std::string out;
  out += repl_normalize_type_name(type.name, generic_slots);
  out += type.variadic ? "..." : "";

  if (!type.alternative_types.empty()) {
    out += "|U[";
    for (std::size_t index = 0; index < type.alternative_types.size(); ++index) {
      if (index != 0U) { out += ","; }
      out += repl_type_signature_fragment(type.alternative_types[index], generic_slots);
    }
    out += "]";
  } else if (!type.alternatives.empty()) {
    out += "|u[";
    for (std::size_t index = 0; index < type.alternatives.size(); ++index) {
      if (index != 0U) { out += ","; }
      out += repl_normalize_type_name(type.alternatives[index], generic_slots);
    }
    out += "]";
  }

  if (!type.tuple_items.empty()) {
    out += "|T[";
    for (std::size_t index = 0; index < type.tuple_items.size(); ++index) {
      if (index != 0U) { out += ","; }
      out += repl_type_signature_fragment(type.tuple_items[index], generic_slots);
    }
    out += "]";
  }

  if (!type.type_args.empty()) {
    out += "|A[";
    for (std::size_t index = 0; index < type.type_args.size(); ++index) {
      if (index != 0U) { out += ","; }
      out += repl_type_signature_fragment(type.type_args[index], generic_slots);
    }
    out += "]";
  }

  if (type.function_sig.has_value()) {
    out += "|F(";
    for (std::size_t index = 0; index < type.function_sig->param_types.size(); ++index) {
      if (index != 0U) { out += ","; }
      out += repl_type_signature_fragment(type.function_sig->param_types[index], generic_slots);
    }
    out += ")->";
    out += repl_type_signature_fragment(*type.function_sig->return_type, generic_slots);
  }

  return out;
}

inline auto repl_signature_key(const frontend::ir::IRLet& let) -> std::string {
  std::unordered_map<std::string, std::size_t> generic_slots;
  generic_slots.reserve(let.generic_params.size());
  for (std::size_t index = 0; index < let.generic_params.size(); ++index) {
    generic_slots.emplace(let.generic_params[index], index);
  }

  std::string out = "g" + std::to_string(let.generic_params.size()) + "|p[";
  for (std::size_t index = 0; index < let.params.size(); ++index) {
    if (index != 0U) { out += ";"; }
    out += repl_type_signature_fragment(let.params[index].type, generic_slots);
  }
  out += "]";
  return out;
}

inline auto repl_let_ordinal(const frontend::ir::IRLet& let) -> std::optional<std::size_t> {
  const std::string public_symbol = repl_let_key(let.qualifier, let.name);
  const std::string internal_key = repl_let_internal_key(let);
  if (!internal_key.starts_with(public_symbol + "#")) { return std::nullopt; }

  const std::string_view ordinal_text = std::string_view{internal_key}.substr(public_symbol.size() + 1U);
  std::size_t ordinal = 0;
  const auto* begin = ordinal_text.data();
  const auto* end = begin + ordinal_text.size();
  if (const auto [ptr, error] = std::from_chars(begin, end, ordinal); error != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return ordinal;
}

inline auto assign_repl_stable_symbol_keys(frontend::ir::IRProgram& program,
                                           const std::vector<frontend::ir::IRLet>& prior_session_lets) -> void {
  struct SymbolSlots {
    std::unordered_map<std::string, std::size_t> signature_to_ordinal;
    std::size_t next_ordinal = 0;
  };

  std::unordered_map<std::string, SymbolSlots> slots_by_symbol;

  auto reserve_slot = [&](const frontend::ir::IRLet& let) -> SymbolSlots& {
    const std::string public_symbol = repl_let_key(let.qualifier, let.name);
    return slots_by_symbol[public_symbol];
  };

  for (const auto& prior_let : prior_session_lets) {
    auto& [signature_to_ordinal, next_ordinal] = reserve_slot(prior_let);
    const auto signature_key = repl_signature_key(prior_let);
    const auto ordinal = repl_let_ordinal(prior_let).value_or(next_ordinal);
    signature_to_ordinal[signature_key] = ordinal;
    next_ordinal = std::max(next_ordinal, ordinal + 1U);
  }

  std::unordered_map<std::string, std::size_t> last_definition_index;
  last_definition_index.reserve(program.lets.size());
  std::vector<std::string> let_signatures;
  let_signatures.reserve(program.lets.size());
  for (std::size_t index = 0; index < program.lets.size(); ++index) {
    const auto& let = program.lets[index];
    const std::string signature_key = repl_signature_key(let);
    let_signatures.push_back(signature_key);
    last_definition_index[repl_let_key(let.qualifier, let.name) + "\n" + signature_key] = index;
  }

  std::vector<frontend::ir::IRLet> normalized_lets;
  normalized_lets.reserve(program.lets.size());
  for (std::size_t index = 0; index < program.lets.size(); ++index) {
    auto let = std::move(program.lets[index]);
    const std::string public_symbol = repl_let_key(let.qualifier, let.name);
    if (const std::string dedupe_key = public_symbol + "\n" + let_signatures[index];
        last_definition_index.at(dedupe_key) != index) { continue; }

    auto& [signature_to_ordinal, next_ordinal] = slots_by_symbol[public_symbol];
    const auto existing = signature_to_ordinal.find(let_signatures[index]);
    const std::size_t ordinal =
        existing != signature_to_ordinal.end() ? existing->second : next_ordinal++;
    signature_to_ordinal[let_signatures[index]] = ordinal;
    let.symbol_key = public_symbol + "#" + std::to_string(ordinal);
    normalized_lets.push_back(std::move(let));
  }

  program.lets = std::move(normalized_lets);
}

inline auto parse_and_analyze_repl_text(const std::string& source_text, const std::filesystem::path& source_path,
                                        const std::vector<frontend::ir::IRLet>& prior_session_lets)
    -> tl::expected<frontend::ir::IRProgram, ReplSessionError> {
  auto lowered = frontend::source_loader::parse_text_to_lowered_ir<ReplSessionError>(source_text, source_path.string(),
                                                                                     make_repl_session_error);
  if (!lowered) { return tl::unexpected(lowered.error()); }

  assign_repl_stable_symbol_keys(*lowered, prior_session_lets);

  std::unordered_set<std::string> replaced_keys;
  replaced_keys.reserve(lowered->lets.size());
  for (const auto& let : lowered->lets) { replaced_keys.insert(repl_let_internal_key(let)); }

  std::vector<frontend::ir::IRLet> imported_typed_lets;
  imported_typed_lets.reserve(prior_session_lets.size());
  for (const auto& prior_let : prior_session_lets) {
    if (replaced_keys.contains(repl_let_internal_key(prior_let))) { continue; }
    imported_typed_lets.push_back(prior_let);
  }

  std::unordered_set<std::string> imported_symbols;
  std::vector<frontend::ir::IRLet> symbolic_imported_lets;
  for (const auto& [module_name, span] : lowered->imports) {
    (void)span;
    if (module_name != "Std") { continue; }

    const std::filesystem::path std_source_name{"Std.fleaux"};
    const auto embedded_std = fleaux::common::embedded_resource_text("Std.fleaux");
    if (!embedded_std.has_value()) {
      return tl::unexpected(make_repl_session_error(
          "Failed to read source file.", std::optional<std::string>{"Embedded symbolic module 'Std' is unavailable."},
          std::nullopt));
    }

    const auto std_program = frontend::source_loader::parse_text_to_lowered_ir<ReplSessionError>(
        std::string(*embedded_std), std_source_name.string(), make_repl_session_error);

    if (!std_program) { return tl::unexpected(std_program.error()); }

    std::unordered_set<std::string> imported_typed_keys;
    imported_typed_keys.reserve(imported_typed_lets.size());
    for (const auto& imported_let : imported_typed_lets) {
      imported_typed_keys.insert(frontend::source_loader::let_identity_key(imported_let));
    }

    for (const auto& std_let : std_program->lets) {
      if (!frontend::source_loader::let_declared_in_source(std_let, std_source_name)) { continue; }
      imported_symbols.insert(frontend::source_loader::symbol_key(std_let.qualifier, std_let.name));
      if (const auto key = frontend::source_loader::let_identity_key(std_let); imported_typed_keys.insert(key).second) {
        imported_typed_lets.push_back(std_let);
      }
      symbolic_imported_lets.push_back(std_let);
    }
  }

  const auto analyzed = frontend::source_loader::analyze_lowered_program_with_imports<ReplSessionError>(
      *lowered, source_path, make_repl_session_error, imported_symbols, imported_typed_lets,
      "Cyclic import detected.", std::optional<std::string>{"Break the cycle by moving shared definitions into a third module."});
  if (!analyzed) {
    return tl::unexpected(
        make_repl_session_error(analyzed.error().message, analyzed.error().hint, analyzed.error().span));
  }

  auto result = analyzed.value();
  if (!symbolic_imported_lets.empty()) {
    std::unordered_set<std::string> seen;
    seen.reserve(result.lets.size() + symbolic_imported_lets.size());
    for (const auto& let : result.lets) { seen.insert(frontend::source_loader::let_identity_key(let)); }

    std::vector<frontend::ir::IRLet> merged_lets;
    merged_lets.reserve(result.lets.size() + symbolic_imported_lets.size());
    for (const auto& imported_let : symbolic_imported_lets) {
      if (const auto key = frontend::source_loader::let_identity_key(imported_let); seen.insert(key).second) {
        merged_lets.push_back(imported_let);
      }
    }
    merged_lets.insert(merged_lets.end(), result.lets.begin(), result.lets.end());
    result.lets = std::move(merged_lets);
  }

  return result;
}

inline auto merge_repl_session_lets(const std::vector<frontend::ir::IRLet>& prior_session_lets,
                                    const std::vector<frontend::ir::IRLet>& snippet_lets,
                                    const std::filesystem::path& snippet_source)
    -> std::vector<frontend::ir::IRLet> {
  std::unordered_set<std::string> replaced_keys;
  replaced_keys.reserve(snippet_lets.size());
  for (const auto& let : snippet_lets) {
    if (!frontend::source_loader::let_declared_in_source(let, snippet_source)) { continue; }
    replaced_keys.insert(repl_let_internal_key(let));
  }

  std::vector<frontend::ir::IRLet> merged_lets;
  merged_lets.reserve(prior_session_lets.size() + snippet_lets.size());
  std::unordered_set<std::string> seen_keys;
  seen_keys.reserve(prior_session_lets.size() + snippet_lets.size());
  for (const auto& prior_let : prior_session_lets) {
    if (replaced_keys.contains(repl_let_internal_key(prior_let))) { continue; }
    seen_keys.insert(repl_let_internal_key(prior_let));
    merged_lets.push_back(prior_let);
  }
  for (const auto& snippet_let : snippet_lets) {
    if (const auto key = repl_let_internal_key(snippet_let); seen_keys.insert(key).second) {
      merged_lets.push_back(snippet_let);
    }
  }
  return merged_lets;
}

}  // namespace fleaux::vm::detail
