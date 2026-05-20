#pragma once
// Help and introspection builtins.
// Part of the split runtime support layer; included by fleaux/runtime/runtime_support.hpp.

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fleaux/common/trie.hpp"
#include "fleaux/runtime/value.hpp"

namespace fleaux::runtime {

struct HelpMetadata {
  std::string name;
  std::string signature;
  std::vector<std::string> doc_lines;
  bool is_builtin = false;
};

using HelpMetadataStore = common::TrieMap<HelpMetadata>;

struct HelpMetadataThreadContext {
  std::reference_wrapper<HelpMetadataStore> active;
};

[[nodiscard]] inline auto default_help_metadata_store() -> HelpMetadataStore& {
  static HelpMetadataStore store{};
  return store;
}

[[nodiscard]] inline auto help_metadata_thread_context() -> HelpMetadataThreadContext& {
  thread_local HelpMetadataThreadContext context{default_help_metadata_store()};
  return context;
}

[[nodiscard]] inline auto help_metadata_store() -> HelpMetadataStore& {
  return help_metadata_thread_context().active.get();
}

class ActiveHelpMetadataRegistryScope {
public:
  explicit ActiveHelpMetadataRegistryScope(HelpMetadataStore& store)
      : previous_(help_metadata_thread_context().active) {
    help_metadata_thread_context().active = std::ref(store);
  }

  ActiveHelpMetadataRegistryScope(const ActiveHelpMetadataRegistryScope&) = delete;
  auto operator=(const ActiveHelpMetadataRegistryScope&) -> ActiveHelpMetadataRegistryScope& = delete;

  ~ActiveHelpMetadataRegistryScope() { help_metadata_thread_context().active = previous_; }

private:
  std::reference_wrapper<HelpMetadataStore> previous_;
};

inline auto clear_help_metadata_registry() -> void { help_metadata_store().clear(); }

inline auto register_help_metadata(HelpMetadataStore& store, HelpMetadata metadata) -> void {
  const auto key = metadata.name;
  store.insert_or_assign(key, std::move(metadata));
}

inline auto register_help_metadata(HelpMetadata metadata) -> void {
  register_help_metadata(help_metadata_store(), std::move(metadata));
}

namespace detail {

[[nodiscard]] inline auto trim_copy(const std::string_view text) -> std::string {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(" \t\r\n");
  return std::string{text.substr(first, last - first + 1)};
}

[[nodiscard]] inline auto starts_with_token(const std::string& line, const std::string_view token) -> bool {
  if (line.size() < token.size()) {
    return false;
  }
  return std::string_view(line).starts_with(token);
}

struct ParsedDoc {
  std::string brief{};
  std::unordered_map<std::string, std::string> params{};
  std::unordered_map<std::string, std::string> tparams{};
  std::string returns{};
  std::vector<std::string> notes{};
};

[[nodiscard]] inline auto parse_doc_lines(const std::vector<std::string>& doc_lines) -> ParsedDoc {
  ParsedDoc parsed;

  for (const auto& raw_line : doc_lines) {
    const std::string line = trim_copy(raw_line);
    if (line.empty()) {
      continue;
    }

    if (starts_with_token(line, "@brief")) {
      parsed.brief = trim_copy(std::string_view(line).substr(6));
      continue;
    }

    if (starts_with_token(line, "@return")) {
      parsed.returns = trim_copy(std::string_view(line).substr(7));
      continue;
    }

    if (starts_with_token(line, "@returns")) {
      parsed.returns = trim_copy(std::string_view(line).substr(8));
      continue;
    }

    if (starts_with_token(line, "@tparam")) {
      std::string rest = trim_copy(std::string_view(line.substr(7)));
      if (rest.empty()) {
        continue;
      }
      const auto first_space = rest.find_first_of(" \t:");
      std::string param_name = rest.substr(0, first_space);
      std::string param_doc = trim_copy(rest.substr(first_space));
      if (!param_doc.empty() && param_doc.front() == ':') {
        param_doc.erase(param_doc.begin());
        param_doc = trim_copy(param_doc);
      }
      parsed.tparams[param_name] = std::move(param_doc);
      continue;
    }

    if (starts_with_token(line, "@param")) {
      std::string rest = trim_copy(std::string_view(line).substr(6));
      if (rest.empty()) {
        continue;
      }
      const auto first_space = rest.find_first_of(" \t:");
      if (first_space == std::string::npos) {
        parsed.params[rest] = {};
        continue;
      }
      std::string param_name = rest.substr(0, first_space);
      std::string param_doc = trim_copy(rest.substr(first_space));
      if (!param_doc.empty() && param_doc.front() == ':') {
        param_doc.erase(param_doc.begin());
        param_doc = trim_copy(param_doc);
      }
      parsed.params[param_name] = std::move(param_doc);
      continue;
    }

    parsed.notes.push_back(line);
  }

  if (parsed.brief.empty() && !parsed.notes.empty()) {
    parsed.brief = parsed.notes.front();
    parsed.notes.erase(parsed.notes.begin());
  }

  return parsed;
}

}  // namespace detail

[[nodiscard]] inline auto Help(Value arg) -> Value {
  std::string name = detail::trim_copy(to_string(unwrap_singleton_arg(std::move(arg))));

  const auto& store = help_metadata_store();
  if (name.empty()) {
    if (store.empty()) {
      return make_string("Help: no symbols available.");
    }

    const auto names = store.completions("");

    std::string result = "Available symbols:\n";
    for (const auto& symbol_name : names) {
      const auto metadata = store.find(symbol_name);
      if (!metadata.has_value()) {
        continue;
      }
      const auto parsed = detail::parse_doc_lines(metadata->get().doc_lines);
      result += std::format("- {}", symbol_name);
      if (!parsed.brief.empty()) {
        result += std::format(" - {}", parsed.brief);
      }
      result += "\n";
    }
    return make_string(result);
  }

  std::optional<std::reference_wrapper<const HelpMetadata>> metadata = store.find(name);
  if (!metadata.has_value() && name.find('.') == std::string::npos) {
    metadata = store.find("Std." + name);
  }

  if (!metadata.has_value()) {
    if (store.empty()) {
      return make_string("Help: no symbols available.");
    }

    const auto names = store.completions(name);
    // if we dont find an exact match, try to find a partial match like Std.Tuple.*
    if (names.empty()) {
      return make_string(
          std::format("Help: unknown symbol '{}'\n\nUse (\"Symbol\") -> Std.Help for symbol help, or (\"\") -> "
                      "Std.Help() to list symbols.",
                      name));
    }

    std::string result = std::format("Available symbols({}*):\n", name);
    for (const auto& symbol_name : names) {
      const auto matched_metadata = store.find(symbol_name);
      if (!matched_metadata.has_value()) {
        continue;
      }
      const auto parsed = detail::parse_doc_lines(matched_metadata->get().doc_lines);
      result += std::format("- {}", symbol_name);
      if (!parsed.brief.empty()) {
        result += std::format(" - {}", parsed.brief);
      }
      result += "\n";
    }
    return make_string(result);
  }

  const auto [brief, params, tparams, returns, notes] = detail::parse_doc_lines(metadata->get().doc_lines);

  std::string result = std::format("Help on function {} \n\n  {}\n", metadata->get().name, metadata->get().signature);

  if (!brief.empty()) {
    result += std::format("\n{}\n", brief);
  }

  if (!tparams.empty()) {
    result += "\nGenerics:\n";
    for (const auto& [param_name, desc] : tparams) {
      result += std::format("  - {}: {}\n", param_name, desc);
    }
  }

  if (!params.empty()) {
    result += "\nParameters:\n";
    for (const auto& [param_name, desc] : params) {
      result += std::format("  - {}: {}\n", param_name, desc);
    }
  }

  if (!returns.empty()) {
    result += std::format("\nReturns:\n  {}\n", returns);
  }

  if (!notes.empty()) {
    result += "\nNotes:\n";
    for (const auto& note : notes) {
      result += std::format("  - {}\n", note);
    }
  }

  return make_string(result);
}

}  // namespace fleaux::runtime
