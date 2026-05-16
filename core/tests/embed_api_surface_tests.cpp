#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/embed/binding_plugin.hpp"
#include "fleaux/embed/dynamic_loader.hpp"
#include "fleaux/embed/fleaux_binding.hpp"
#include "fleaux/embed/native_bindings.hpp"
#include "fleaux/embed/vm_host.hpp"

TEST_CASE("Embedding headers expose draft API types", "[embed]") {
  static_assert(std::is_enum_v<fleaux::embed::HostErrorCategory>);
  static_assert(std::is_enum_v<fleaux::embed::BindingPluginStatus>);
  static_assert(std::is_class_v<fleaux::embed::VmHost>);
  static_assert(std::is_class_v<fleaux::embed::DynamicLoader>);
  static_assert(std::is_class_v<fleaux::embed::NativeBindingRegistry>);
  SUCCEED();
}

