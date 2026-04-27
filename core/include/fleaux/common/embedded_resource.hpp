#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace fleaux::common {

struct EmbeddedResourceView {
  std::string_view name;
  std::span<const std::byte> bytes;
};

[[nodiscard]] auto find_embedded_resource(std::span<const EmbeddedResourceView> resources, std::string_view name)
    -> std::optional<EmbeddedResourceView>;

[[nodiscard]] auto embedded_resource_text(const EmbeddedResourceView& resource) -> std::string_view;

[[nodiscard]] auto embedded_resource_registry() -> std::span<const EmbeddedResourceView>;

auto set_embedded_resource_registry(std::span<const EmbeddedResourceView> resources) -> void;

auto clear_embedded_resource_registry() -> void;

[[nodiscard]] auto find_embedded_resource(std::string_view name) -> std::optional<EmbeddedResourceView>;

[[nodiscard]] auto embedded_resource_text(std::string_view name) -> std::optional<std::string_view>;

}  // namespace fleaux::common

