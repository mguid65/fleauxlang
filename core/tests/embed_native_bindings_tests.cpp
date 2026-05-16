#include <string>
#include <catch2/catch_test_macros.hpp>
#include "fleaux/embed/native_bindings.hpp"
#include "fleaux/runtime/value.hpp"
TEST_CASE("NativeBindingRegistry manages callable lifecycle", "[embed]") {
  fleaux::embed::NativeBindingRegistry registry;
  const auto callable = [](const fleaux::embed::BindingContext&,
                           const fleaux::embed::VmValue& args) -> fleaux::embed::NativeInvokeResult {
    return args;
  };
  auto first = registry.register_callable(fleaux::embed::NativeBinding{
      .symbol = "Host.Echo",
      .signature = fleaux::embed::BindingSignature{.arity = fleaux::embed::ArityRange{.min = 1, .max = 1},
                                                   .description = "Echo value"},
      .callable = callable,
  });
  REQUIRE(first.has_value());
  REQUIRE(registry.size() == 1);
  REQUIRE(registry.has_callable("Host.Echo"));
  REQUIRE(registry.find_callable("Host.Echo") != nullptr);
  auto duplicate = registry.register_callable(fleaux::embed::NativeBinding{
      .symbol = "Host.Echo",
      .signature = fleaux::embed::BindingSignature{.arity = fleaux::embed::ArityRange{.min = 1, .max = 1},
                                                   .description = "Echo value"},
      .callable = callable,
  });
  REQUIRE_FALSE(duplicate.has_value());
  REQUIRE(duplicate.error().code == fleaux::embed::BindingErrorCode::kDuplicateSymbol);
  REQUIRE(registry.unregister_callable("Host.Echo"));
  REQUIRE_FALSE(registry.has_callable("Host.Echo"));
  REQUIRE(registry.size() == 0);
}
TEST_CASE("NativeBindingRegistry validates symbol and signature", "[embed]") {
  fleaux::embed::NativeBindingRegistry registry;
  const auto callable = [](const fleaux::embed::BindingContext&,
                           const fleaux::embed::VmValue& args) -> fleaux::embed::NativeInvokeResult {
    return args;
  };
  auto bad_symbol = registry.register_callable(fleaux::embed::NativeBinding{
      .symbol = "",
      .signature = fleaux::embed::BindingSignature{},
      .callable = callable,
  });
  REQUIRE_FALSE(bad_symbol.has_value());
  REQUIRE(bad_symbol.error().code == fleaux::embed::BindingErrorCode::kInvalidSymbol);
  auto bad_signature = registry.register_callable(fleaux::embed::NativeBinding{
      .symbol = "Host.BadArity",
      .signature = fleaux::embed::BindingSignature{.arity = fleaux::embed::ArityRange{.min = 2, .max = 1}},
      .callable = callable,
  });
  REQUIRE_FALSE(bad_signature.has_value());
  REQUIRE(bad_signature.error().code == fleaux::embed::BindingErrorCode::kInvalidSignature);
}
