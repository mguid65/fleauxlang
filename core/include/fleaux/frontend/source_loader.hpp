#pragma once

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <tl/expected.hpp>

#include "fleaux/common/embedded_resource.hpp"
#include "fleaux/frontend/analysis.hpp"
#include "fleaux/frontend/import_resolution.hpp"
#include "fleaux/frontend/parser.hpp"
#include "fleaux/frontend/type_check.hpp"

namespace fleaux::frontend::source_loader {

[[nodiscard]] inline auto read_text_file(const std::filesystem::path& file) -> std::string {
  std::ifstream in(file);
  if (!in) { return {}; }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

[[nodiscard]] inline auto resolve_import_source(const std::filesystem::path& current_source,
                                                const std::string& module_name) -> std::filesystem::path {
  if (import_resolution::is_symbolic_import(module_name)) { return {}; }
  const auto paths = import_resolution::resolve_import_paths(current_source.parent_path(), module_name);
  return paths.source.value_or(std::filesystem::path{});
}

[[nodiscard]] inline auto symbol_key(const std::optional<std::string>& qualifier, const std::string& name)
    -> std::string {
  return qualifier.has_value() ? (*qualifier + "." + name) : name;
}

[[nodiscard]] inline auto let_identity_key(const ir::IRLet& let) -> std::string {
  if (!let.symbol_key.empty()) { return let.symbol_key; }
  return symbol_key(let.qualifier, let.name);
}

[[nodiscard]] inline auto let_declared_in_source(const ir::IRLet& let, const std::filesystem::path& source_file)
    -> bool {
  if (!let.span.has_value()) { return false; }
  return std::filesystem::path(let.span->source_name) == source_file;
}

template <typename ErrorT, typename ErrorFactory>
[[nodiscard]] auto parse_text_to_ir(const std::string& source_text, const std::string& source_name,
                                    ErrorFactory&& make_error) -> tl::expected<ir::IRProgram, ErrorT> {
  constexpr parse::Parser parser;
  const auto parsed = parser.parse_program(source_text, source_name);
  if (!parsed) { return tl::unexpected(make_error(parsed.error().message, parsed.error().hint, parsed.error().span)); }

  constexpr analysis::Analyzer analyzer;
  const auto analyzed = analyzer.analyze(parsed.value());
  if (!analyzed) {
    return tl::unexpected(make_error(analyzed.error().message, analyzed.error().hint, analyzed.error().span));
  }

  return analyzed.value();
}

template <typename ErrorT, typename ErrorFactory>
[[nodiscard]] auto parse_text_to_lowered_ir(const std::string& source_text, const std::string& source_name,
                                            ErrorFactory&& make_error) -> tl::expected<ir::IRProgram, ErrorT> {
  constexpr parse::Parser parser;
  const auto parsed = parser.parse_program(source_text, source_name);
  if (!parsed) { return tl::unexpected(make_error(parsed.error().message, parsed.error().hint, parsed.error().span)); }

  constexpr analysis::Analyzer analyzer;
  const auto lowered = analyzer.lower_only(parsed.value());
  if (!lowered) {
    return tl::unexpected(make_error(lowered.error().message, lowered.error().hint, lowered.error().span));
  }

  return lowered.value();
}

template <typename ErrorT, typename ErrorFactory>
[[nodiscard]] auto parse_file_to_ir(const std::filesystem::path& source_file, ErrorFactory&& make_error)
    -> tl::expected<ir::IRProgram, ErrorT> {
  const auto source_text = read_text_file(source_file);
  if (source_text.empty()) {
    return tl::unexpected(make_error("Failed to read source file.",
                                     std::optional<std::string>{"Check the file path and ensure it is not empty."},
                                     std::nullopt));
  }

  return parse_text_to_ir<ErrorT>(source_text, source_file.string(), std::forward<ErrorFactory>(make_error));
}

template <typename ErrorT, typename ErrorFactory>
[[nodiscard]] auto parse_file_to_lowered_ir(const std::filesystem::path& source_file, ErrorFactory&& make_error)
    -> tl::expected<ir::IRProgram, ErrorT> {
  const auto source_text = read_text_file(source_file);
  if (source_text.empty()) {
    return tl::unexpected(make_error("Failed to read source file.",
                                     std::optional<std::string>{"Check the file path and ensure it is not empty."},
                                     std::nullopt));
  }

  return parse_text_to_lowered_ir<ErrorT>(source_text, source_file.string(), std::forward<ErrorFactory>(make_error));
}

template <typename ErrorT, typename ErrorFactory>
[[nodiscard]] auto seed_symbolic_imports_for_program(const ir::IRProgram& program, ErrorFactory&& make_error,
                                                     std::unordered_set<std::string>& imported_symbols,
                                                     std::vector<ir::IRLet>& imported_typed_lets)
    -> tl::expected<void, ErrorT> {
  std::unordered_set<std::string> imported_typed_let_keys;
  imported_typed_let_keys.reserve(imported_typed_lets.size());
  for (const auto& imported_let : imported_typed_lets) {
    imported_typed_let_keys.insert(let_identity_key(imported_let));
  }

  for (const auto& [module_name, span] : program.imports) {
    if (!import_resolution::is_symbolic_import(module_name)) { continue; }

    const auto embedded_std = common::embedded_resource_text("Std.fleaux");
    if (!embedded_std.has_value()) {
      return tl::unexpected(make_error("Failed to read source file.",
                                       std::optional<std::string>{"Embedded symbolic module 'Std' is unavailable."},
                                       span));
    }

    const std::filesystem::path std_source_name{"Std.fleaux"};
    const auto std_program = parse_text_to_lowered_ir<ErrorT>(std::string(*embedded_std), std_source_name.string(),
                                                              std::forward<ErrorFactory>(make_error));
    if (!std_program) { return tl::unexpected(std_program.error()); }

    for (const auto& std_let : std_program->lets) {
      if (!let_declared_in_source(std_let, std_source_name)) { continue; }
      imported_symbols.insert(symbol_key(std_let.qualifier, std_let.name));
      if (const auto key = let_identity_key(std_let); imported_typed_let_keys.insert(key).second) {
        imported_typed_lets.push_back(std_let);
      }
    }
  }

  return {};
}

template <typename ErrorT, typename ErrorFactory>
[[nodiscard]] auto analyze_lowered_program_with_imports(
    const ir::IRProgram& current_program, const std::filesystem::path& current_source, ErrorFactory&& make_error,
    const std::unordered_set<std::string>& extra_imported_symbols = {},
    const std::vector<ir::IRLet>& extra_imported_typed_lets = {},
    const std::string_view cycle_message = "Cyclic import detected.",
    const std::optional<std::string>& cycle_hint = std::nullopt) -> tl::expected<ir::IRProgram, ErrorT> {
  using IRProgram = ir::IRProgram;
  using IRExprStatement = ir::IRExprStatement;
  using IRLet = ir::IRLet;

  const auto entry_source = import_resolution::normalized_path(current_source);

  const auto collect_program = [&](const auto& self, const std::filesystem::path& source_path, const bool is_entry,
                                   std::unordered_map<std::string, IRProgram>& cache,
                                   std::unordered_set<std::string>& in_progress) -> tl::expected<IRProgram, ErrorT> {
    const std::string key = is_entry ? entry_source.string() : std::filesystem::weakly_canonical(source_path).string();
    if (cache.contains(key)) { return cache.at(key); }

    if (in_progress.contains(key)) {
      return tl::unexpected(
          make_error(std::string{cycle_message}, cycle_hint, std::optional<diag::SourceSpan>{std::nullopt}));
    }

    in_progress.insert(key);

    tl::expected<IRProgram, ErrorT> current = is_entry ? tl::expected<IRProgram, ErrorT>{current_program}
                                                       : parse_file_to_lowered_ir<ErrorT>(source_path, make_error);
    if (!current) {
      in_progress.erase(key);
      return tl::unexpected(current.error());
    }

    std::unordered_set<std::string> direct_imported_symbols = extra_imported_symbols;
    std::vector<IRLet> direct_imported_typed_lets;
    direct_imported_typed_lets.reserve(extra_imported_typed_lets.size());
    std::unordered_set<std::string> direct_imported_typed_let_keys;
    direct_imported_typed_let_keys.reserve(extra_imported_typed_lets.size());
    for (const auto& imported_let : extra_imported_typed_lets) {
      if (const auto typed_key = let_identity_key(imported_let); direct_imported_typed_let_keys.insert(typed_key).second) {
        direct_imported_typed_lets.push_back(imported_let);
      }
    }

    if (auto symbolic_seed = seed_symbolic_imports_for_program<ErrorT>(current.value(), make_error, direct_imported_symbols,
                                                                       direct_imported_typed_lets);
        !symbolic_seed) {
      in_progress.erase(key);
      return tl::unexpected(symbolic_seed.error());
    }

    IRProgram merged = current.value();
    std::unordered_set<std::string> seen;
    for (const auto& let : merged.lets) { seen.insert(let_identity_key(let)); }

    std::vector<IRLet> imported_lets;
    std::vector<IRExprStatement> imported_exprs;
    for (const auto& [module_name, span] : current->imports) {
      if (import_resolution::is_symbolic_import(module_name)) { continue; }
      const auto import_source = resolve_import_source(source_path, module_name);
      if (import_source.empty()) {
        in_progress.erase(key);
        return tl::unexpected(
            make_error("import-unresolved: Import not found: '" + module_name + "'",
                       std::optional<std::string>{"Checked relative to '" +
                                                  import_resolution::normalized_path(source_path).string() +
                                                  "'. Verify module name and file location."},
                       span));
      }

      auto imported = self(self, import_source, false, cache, in_progress);
      if (!imported) {
        in_progress.erase(key);
        return tl::unexpected(imported.error());
      }

      for (const auto& imported_let : imported->lets) {
        if (!let_declared_in_source(imported_let, import_source)) { continue; }
        direct_imported_symbols.insert(symbol_key(imported_let.qualifier, imported_let.name));
        if (const auto typed_key = let_identity_key(imported_let);
            direct_imported_typed_let_keys.insert(typed_key).second) {
          direct_imported_typed_lets.push_back(imported_let);
        }
      }

      for (const auto& imported_let : imported->lets) {
        if (const auto sym = let_identity_key(imported_let); seen.insert(sym).second) {
          imported_lets.push_back(imported_let);
        }
      }
      imported_exprs.insert(imported_exprs.end(), imported->expressions.begin(), imported->expressions.end());
    }

    auto analyzed_current = type_check::analyze_program(current.value(), direct_imported_symbols, direct_imported_typed_lets);
    if (!analyzed_current) {
      in_progress.erase(key);
      return tl::unexpected(
          make_error(analyzed_current.error().message, analyzed_current.error().hint, analyzed_current.error().span));
    }

    merged = analyzed_current.value();
    merged.lets.insert(merged.lets.begin(), imported_lets.begin(), imported_lets.end());
    merged.expressions.insert(merged.expressions.begin(), imported_exprs.begin(), imported_exprs.end());

    cache[key] = merged;
    in_progress.erase(key);
    return merged;
  };

  std::unordered_map<std::string, IRProgram> cache;
  std::unordered_set<std::string> in_progress;
  return collect_program(collect_program, entry_source, true, cache, in_progress);
}

template <typename ErrorT, typename ErrorFactory>
[[nodiscard]] auto load_ir_program(const std::filesystem::path& source_file, ErrorFactory&& make_error,
                                   const std::string_view cycle_message = "Cyclic import detected.",
                                   const std::optional<std::string>& cycle_hint = std::nullopt)
    -> tl::expected<ir::IRProgram, ErrorT> {
  auto current = parse_file_to_lowered_ir<ErrorT>(source_file, make_error);
  if (!current) { return tl::unexpected(current.error()); }

  return analyze_lowered_program_with_imports<ErrorT>(current.value(), source_file,
                                                      std::forward<ErrorFactory>(make_error), {}, {},
                                                      cycle_message, cycle_hint);
}

}  // namespace fleaux::frontend::source_loader
