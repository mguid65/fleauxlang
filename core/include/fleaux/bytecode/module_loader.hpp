#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include <tl/expected.hpp>

#include "fleaux/bytecode/module.hpp"
#include "fleaux/bytecode/optimizer.hpp"
#include "fleaux/frontend/diagnostics.hpp"

namespace fleaux::bytecode {

struct ModuleLoadError {
  std::string message{};
  std::optional<std::string> hint{std::nullopt};
  std::optional<frontend::diag::SourceSpan> span{std::nullopt};
};

struct ModuleLoadOptions {
  OptimizationMode mode{OptimizationMode::kBaseline};
  bool write_bytecode_cache{true};
  // Searched for imports after the importing module's own directory, in order: the directories of loaded extensions.
  std::vector<std::filesystem::path> import_search_paths{};
};

[[nodiscard]] auto load_linked_module(const std::filesystem::path& entry_path, const ModuleLoadOptions& options = ModuleLoadOptions{})
    -> tl::expected<Module, ModuleLoadError>;

}  // namespace fleaux::bytecode
