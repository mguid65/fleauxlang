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

#include "fleaux/frontend/analysis.hpp"
#include "fleaux/frontend/parser.hpp"

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
  if (module_name == "Std" || module_name == "StdBuiltins") { return {}; }

  if (const auto local = current_source.parent_path() / (module_name + ".fleaux"); std::filesystem::exists(local)) {
    return std::filesystem::weakly_canonical(local);
  }

  return {};
}

[[nodiscard]] inline auto symbol_key(const std::optional<std::string>& qualifier, const std::string& name)
    -> std::string {
  return qualifier.has_value() ? (*qualifier + "." + name) : name;
}

template <typename ErrorT, typename ErrorFactory>
[[nodiscard]] auto parse_text_to_ir(const std::string& source_text, const std::string& source_name,
                                    ErrorFactory&& make_error) -> tl::expected<ir::IRProgram, ErrorT> {
  constexpr parse::Parser parser;
  const auto parsed = parser.parse_program(source_text, source_name);
  if (!parsed) {
    return tl::unexpected(make_error(parsed.error().message, parsed.error().hint, parsed.error().span));
  }

  constexpr analysis::Analyzer analyzer;
  const auto analyzed = analyzer.lower(parsed.value());
  if (!analyzed) {
    return tl::unexpected(make_error(analyzed.error().message, analyzed.error().hint, analyzed.error().span));
  }

  return analyzed.value();
}

template <typename ErrorT, typename ErrorFactory>
[[nodiscard]] auto parse_file_to_ir(const std::filesystem::path& source_file,
                                    ErrorFactory&& make_error) -> tl::expected<ir::IRProgram, ErrorT> {
  const auto source_text = read_text_file(source_file);
  if (source_text.empty()) {
    return tl::unexpected(
        make_error("Failed to read source file.", std::optional<std::string>{"Check the file path and ensure it is not empty."},
                   std::nullopt));
  }

  return parse_text_to_ir<ErrorT>(source_text, source_file.string(), std::forward<ErrorFactory>(make_error));
}

template <typename ErrorT, typename ErrorFactory>
[[nodiscard]] auto load_ir_program(const std::filesystem::path& source_file, ErrorFactory&& make_error,
                                   const std::string_view cycle_message = "Cyclic import detected.",
                                   const std::optional<std::string>& cycle_hint = std::nullopt)
    -> tl::expected<ir::IRProgram, ErrorT> {
  using IRProgram = ir::IRProgram;
  using IRExprStatement = ir::IRExprStatement;
  using IRLet = ir::IRLet;

  const auto collect_program = [&](const auto& self, const std::filesystem::path& current_source,
                                   std::unordered_map<std::string, IRProgram>& cache,
                                   std::unordered_set<std::string>& in_progress) -> tl::expected<IRProgram, ErrorT> {
    const std::string key = std::filesystem::weakly_canonical(current_source).string();
    if (cache.contains(key)) { return cache.at(key); }

    if (in_progress.contains(key)) {
      return tl::unexpected(make_error(std::string{cycle_message}, cycle_hint, std::optional<diag::SourceSpan>{std::nullopt}));
    }

    in_progress.insert(key);
    auto current = parse_file_to_ir<ErrorT>(current_source, make_error);
    if (!current) {
      in_progress.erase(key);
      return tl::unexpected(current.error());
    }

    IRProgram merged = current.value();
    std::unordered_set<std::string> seen;
    for (const auto& let : merged.lets) { seen.insert(symbol_key(let.qualifier, let.name)); }

    std::vector<IRLet> imported_lets;
    std::vector<IRExprStatement> imported_exprs;
    for (const auto& [module_name, _span] : current->imports) {
      const auto import_source = resolve_import_source(current_source, module_name);
      if (import_source.empty()) { continue; }

      auto imported = self(self, import_source, cache, in_progress);
      if (!imported) {
        in_progress.erase(key);
        return tl::unexpected(imported.error());
      }

      for (const auto& imported_let : imported->lets) {
        if (const auto sym = symbol_key(imported_let.qualifier, imported_let.name); seen.insert(sym).second) {
          imported_lets.push_back(imported_let);
        }
      }
      imported_exprs.insert(imported_exprs.end(), imported->expressions.begin(), imported->expressions.end());
    }

    merged.lets.insert(merged.lets.begin(), imported_lets.begin(), imported_lets.end());
    merged.expressions.insert(merged.expressions.begin(), imported_exprs.begin(), imported_exprs.end());
    cache[key] = merged;
    in_progress.erase(key);
    return merged;
  };

  std::unordered_map<std::string, IRProgram> cache;
  std::unordered_set<std::string> in_progress;
  return collect_program(collect_program, source_file, cache, in_progress);
}

}  // namespace fleaux::frontend::source_loader


