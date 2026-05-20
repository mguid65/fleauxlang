#include <string>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/embed/native_bindings.hpp"
#include "fleaux/embed/vm_host.hpp"
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
  REQUIRE(registry.find_callable("Host.Echo").has_value());
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

TEST_CASE("NativeBindingRegistry preserves descriptive signature metadata", "[embed]") {
  fleaux::embed::NativeBindingRegistry registry;
  const auto callable = [](const fleaux::embed::BindingContext&,
                           const fleaux::embed::VmValue& args) -> fleaux::embed::NativeInvokeResult {
    return args;
  };

  auto registration = registry.register_callable(fleaux::embed::NativeBinding{
      .symbol = "Host.Metadata",
      .signature = fleaux::embed::BindingSignature{
          .arity = fleaux::embed::ArityRange{.min = 2, .max = 3},
          .description = "Descriptive metadata only",
      },
      .callable = callable,
  });
  REQUIRE(registration.has_value());

  const auto binding = registry.find_callable("Host.Metadata");
  REQUIRE(binding.has_value());
  REQUIRE(binding->get().signature.arity.min == 2);
  REQUIRE(binding->get().signature.arity.max.has_value());
  REQUIRE(*binding->get().signature.arity.max == 3);
  REQUIRE(binding->get().signature.description == "Descriptive metadata only");
}

TEST_CASE("VmHost call_native forwards raw args without enforcing binding arity metadata", "[embed]") {
  fleaux::embed::NativeBindingRegistry registry;
  auto registration = registry.register_callable(fleaux::embed::NativeBinding{
      .symbol = "Host.RawArgs",
      .signature = fleaux::embed::BindingSignature{
          .arity = fleaux::embed::ArityRange{.min = 2, .max = 2},
          .description = "Requires two args by metadata only",
      },
      .callable = [](const fleaux::embed::BindingContext&, const fleaux::embed::VmValue& args)
          -> fleaux::embed::NativeInvokeResult { return args; },
  });
  REQUIRE(registration.has_value());

  fleaux::embed::VmHostConfig config;
  config.binding_registry = &registry;
  fleaux::embed::VmHost host(config);

  const auto call_result = host.call_native("Host.RawArgs", fleaux::runtime::make_int(5));

  REQUIRE(call_result.has_value());
  REQUIRE(fleaux::runtime::as_int_value(*call_result) == 5);
}

