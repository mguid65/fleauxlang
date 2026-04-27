#include "fleaux/common/embedded_resource.hpp"

namespace fleaux::common {
namespace {

auto registry_storage() -> std::span<const EmbeddedResourceView>& {
  static std::span<const EmbeddedResourceView> registry;
  return registry;
}

}  // namespace

auto find_embedded_resource(const std::span<const EmbeddedResourceView> resources, const std::string_view name)
	-> std::optional<EmbeddedResourceView> {
  for (const auto& resource : resources) {
	if (resource.name == name) { return resource; }
  }
  return std::nullopt;
}

auto embedded_resource_text(const EmbeddedResourceView& resource) -> std::string_view {
  return {reinterpret_cast<const char*>(resource.bytes.data()), resource.bytes.size()};
}

auto embedded_resource_registry() -> std::span<const EmbeddedResourceView> { return registry_storage(); }

auto set_embedded_resource_registry(const std::span<const EmbeddedResourceView> resources) -> void {
  registry_storage() = resources;
}

auto clear_embedded_resource_registry() -> void {
  auto& registry = registry_storage();
  registry = std::span<const EmbeddedResourceView>{};
}

auto find_embedded_resource(const std::string_view name) -> std::optional<EmbeddedResourceView> {
  return find_embedded_resource(embedded_resource_registry(), name);
}

auto embedded_resource_text(const std::string_view name) -> std::optional<std::string_view> {
  const auto resource = find_embedded_resource(name);
  if (!resource.has_value()) { return std::nullopt; }
  return embedded_resource_text(*resource);
}

}  // namespace fleaux::common


