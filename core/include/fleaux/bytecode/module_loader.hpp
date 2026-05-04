#pragma once

#include <filesystem>
#include <string>

#include <tl/expected.hpp>

#include "fleaux/bytecode/module.hpp"
#include "fleaux/bytecode/optimizer.hpp"

namespace fleaux::bytecode {

struct ModuleLoadError {
  std::string message;
};

struct ModuleLoadOptions {
  OptimizationMode mode = OptimizationMode::kBaseline;
  bool write_bytecode_cache = true;
  bool enable_value_ref_gate = false;
  bool enable_auto_value_ref = false;
  std::size_t value_ref_byte_cutoff = 256;
};

[[nodiscard]] auto load_linked_module(const std::filesystem::path& entry_path,
                                      const ModuleLoadOptions& options = ModuleLoadOptions{})
    -> tl::expected<Module, ModuleLoadError>;

}  // namespace fleaux::bytecode
