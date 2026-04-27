#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "fleaux/common/embedded_resource.hpp"

TEST_CASE("Embedded Std resource is available in the core test binary", "[common][resources][integration]") {
  const auto std_text = fleaux::common::embedded_resource_text("Std.fleaux");
  REQUIRE(std_text.has_value());
  REQUIRE_THAT(std::string(*std_text), Catch::Matchers::ContainsSubstring("let Std.Println"));
  REQUIRE_THAT(std::string(*std_text), Catch::Matchers::ContainsSubstring("let Std.Help"));
}

