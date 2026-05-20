#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <tl/expected.hpp>

#include "fleaux/embed/binding_plugin.hpp"

namespace fleaux::embed {

struct DynamicLoadError {
  std::string message{};
  std::optional<std::string> hint{std::nullopt};
};

class DynamicLibrary {
public:
  DynamicLibrary() = default;
  DynamicLibrary(const DynamicLibrary&) = delete;
  auto operator=(const DynamicLibrary&) -> DynamicLibrary& = delete;
  DynamicLibrary(DynamicLibrary&&) noexcept = default;
  auto operator=(DynamicLibrary&&) noexcept -> DynamicLibrary& = default;
  virtual ~DynamicLibrary() = default;

  [[nodiscard]] virtual auto symbol(std::string_view symbol_name) const
      -> tl::expected<void*, DynamicLoadError> = 0;
};

class DynamicLoader {
public:
  DynamicLoader() = default;
  DynamicLoader(const DynamicLoader&) = delete;
  auto operator=(const DynamicLoader&) -> DynamicLoader& = delete;
  DynamicLoader(DynamicLoader&&) noexcept = default;
  auto operator=(DynamicLoader&&) noexcept -> DynamicLoader& = default;
  virtual ~DynamicLoader() = default;

  [[nodiscard]] virtual auto open(const std::filesystem::path& library_path) const
      -> tl::expected<std::unique_ptr<DynamicLibrary>, DynamicLoadError> = 0;
};

[[nodiscard]] auto make_system_dynamic_loader() -> std::unique_ptr<DynamicLoader>;

// Resolves the required Fleaux binding-module registration entrypoint from a
// loaded library. The platform-specific symbol-to-function-pointer conversion
// stays isolated at the dynamic-loader boundary.
[[nodiscard]] auto resolve_binding_module_entrypoint(const DynamicLibrary& library)
    -> tl::expected<RegisterBindingModuleFn, DynamicLoadError>;

}  // namespace fleaux::embed

