#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/embed/dynamic_loader.hpp"

TEST_CASE("System dynamic loader can be created", "[embed]") {
  auto loader = fleaux::embed::make_system_dynamic_loader();
  REQUIRE(loader != nullptr);
}

TEST_CASE("System dynamic loader reports missing library", "[embed]") {
  auto loader = fleaux::embed::make_system_dynamic_loader();
  REQUIRE(loader != nullptr);

  const std::filesystem::path missing_path{"__fleaux_missing__/definitely_missing_library"};
  const auto opened = loader->open(missing_path);

  REQUIRE_FALSE(opened.has_value());
  REQUIRE_FALSE(opened.error().message.empty());
}

