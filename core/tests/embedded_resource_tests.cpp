#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/common/embedded_resource.hpp"

namespace {

struct RegistryScope {
  explicit RegistryScope(const std::span<const fleaux::common::EmbeddedResourceView> resources) {
    fleaux::common::set_embedded_resource_registry(resources);
  }

  ~RegistryScope() { fleaux::common::clear_embedded_resource_registry(); }
};

auto as_bytes(const std::string_view text) -> std::span<const std::byte> {
  return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

}  // namespace

TEST_CASE("Embedded resources support explicit registry lookup", "[common][resources]") {
  static constexpr std::string_view kStd = "let Std.Add(lhs: Int64, rhs: Int64): Int64 :: __builtin__;\n";
  static constexpr std::string_view kGrammar = "program := statement*;\n";
  static const std::array<fleaux::common::EmbeddedResourceView, 2> kResources = {{
      {.name = "Std.fleaux", .bytes = as_bytes(kStd)},
      {.name = "fleaux_grammar.tx", .bytes = as_bytes(kGrammar)},
  }};

  const auto std_resource = fleaux::common::find_embedded_resource(std::span{kResources}, "Std.fleaux");
  REQUIRE(std_resource.has_value());
  REQUIRE(fleaux::common::embedded_resource_text(*std_resource) == kStd);

  const auto missing_resource = fleaux::common::find_embedded_resource(std::span{kResources}, "missing.txt");
  REQUIRE_FALSE(missing_resource.has_value());
}

TEST_CASE("Embedded resources support global registry lookup", "[common][resources]") {
  static constexpr std::string_view kStd = "// stdlib\n";
  static const std::array<fleaux::common::EmbeddedResourceView, 1> kResources = {{
      {.name = "Std.fleaux", .bytes = as_bytes(kStd)},
  }};

  RegistryScope scope(std::span{kResources});

  const auto std_resource = fleaux::common::find_embedded_resource("Std.fleaux");
  REQUIRE(std_resource.has_value());
  REQUIRE(fleaux::common::embedded_resource_text("Std.fleaux") == std::optional<std::string_view>{kStd});
  REQUIRE_FALSE(fleaux::common::embedded_resource_text("missing.txt").has_value());
}

TEST_CASE("Embedded resource registry is empty by default", "[common][resources]") {
  fleaux::common::clear_embedded_resource_registry();
  REQUIRE(fleaux::common::embedded_resource_registry().empty());
  REQUIRE_FALSE(fleaux::common::find_embedded_resource("Std.fleaux").has_value());
}

