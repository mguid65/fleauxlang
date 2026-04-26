#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace fleaux::frontend::import_resolution {

struct ResolvedModulePaths {
  std::optional<std::filesystem::path> source;
  std::optional<std::filesystem::path> bytecode;
};

[[nodiscard]] inline auto is_symbolic_import(const std::string_view module_name) -> bool {
  return module_name == "Std" || module_name == "StdBuiltins";
}

[[nodiscard]] inline auto path_exists(const std::filesystem::path& path) -> bool {
  return !path.empty() && std::filesystem::exists(path);
}

[[nodiscard]] inline auto canonical_if_exists(const std::filesystem::path& path)
    -> std::optional<std::filesystem::path> {
  if (!path_exists(path)) { return std::nullopt; }
  return std::filesystem::weakly_canonical(path);
}

[[nodiscard]] inline auto normalized_path(const std::filesystem::path& path) -> std::filesystem::path {
  return std::filesystem::absolute(path).lexically_normal();
}

[[nodiscard]] inline auto bytecode_path_for_source(const std::filesystem::path& source_path) -> std::filesystem::path {
  auto bytecode_path = source_path;
  bytecode_path += ".bc";
  return bytecode_path;
}

[[nodiscard]] inline auto source_path_for_bytecode(const std::filesystem::path& bytecode_path)
    -> std::filesystem::path {
  return bytecode_path.parent_path() / bytecode_path.stem();
}

[[nodiscard]] inline auto module_key_for(const ResolvedModulePaths& paths) -> std::string {
  if (paths.source.has_value()) { return paths.source->string(); }
  if (paths.bytecode.has_value()) { return paths.bytecode->string(); }
  return {};
}

[[nodiscard]] inline auto resolve_entry_paths(const std::filesystem::path& entry_path) -> ResolvedModulePaths {
  ResolvedModulePaths paths;
  if (entry_path.extension() == ".bc") {
    paths.bytecode = canonical_if_exists(entry_path);
    const auto source_candidate = source_path_for_bytecode(entry_path);
    paths.source = canonical_if_exists(source_candidate);
    if (!paths.bytecode.has_value() && path_exists(entry_path)) {
      paths.bytecode = std::filesystem::weakly_canonical(entry_path);
    }
    return paths;
  }

  paths.source = canonical_if_exists(entry_path);
  const auto bytecode_candidate = bytecode_path_for_source(entry_path);
  if (paths.source.has_value()) {
    paths.bytecode = canonical_if_exists(bytecode_candidate).value_or(normalized_path(bytecode_candidate));
  } else {
    paths.bytecode = canonical_if_exists(bytecode_candidate);
  }
  return paths;
}

[[nodiscard]] inline auto resolve_import_paths(const std::filesystem::path& current_module_dir,
                                               const std::string& module_name) -> ResolvedModulePaths {
  ResolvedModulePaths paths;
  if (is_symbolic_import(module_name)) { return paths; }

  const auto source_candidate = current_module_dir / (module_name + ".fleaux");
  const auto bytecode_candidate = current_module_dir / (module_name + ".fleaux.bc");
  paths.source = canonical_if_exists(source_candidate);
  if (paths.source.has_value()) {
    paths.bytecode = canonical_if_exists(bytecode_candidate).value_or(normalized_path(bytecode_candidate));
  } else {
    paths.bytecode = canonical_if_exists(bytecode_candidate);
  }
  return paths;
}

}  // namespace fleaux::frontend::import_resolution
