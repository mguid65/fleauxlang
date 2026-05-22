#include <memory>
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

TEST_CASE("NativeBindingRegistry stores optional binary callable specializations", "[embed]") {
  fleaux::embed::NativeBindingRegistry registry;
  auto registration = registry.register_callable(
      fleaux::embed::NativeBinding{
          .symbol = "Host.Sum",
          .signature = fleaux::embed::BindingSignature{},
          .callable = [](const fleaux::embed::BindingContext&, const fleaux::embed::VmValue& args)
              -> fleaux::embed::NativeInvokeResult { return args; },
      },
      [](const fleaux::embed::BindingContext&, const fleaux::embed::VmValue& lhs, const fleaux::embed::VmValue& rhs)
          -> fleaux::embed::NativeInvokeResult {
        return fleaux::runtime::make_int(fleaux::runtime::as_int_value(lhs) + fleaux::runtime::as_int_value(rhs));
      });

  REQUIRE(registration.has_value());
  REQUIRE(registry.has_callable("Host.Sum"));
  REQUIRE(registry.has_binary_callable("Host.Sum"));
  REQUIRE(registry.find_binary_callable("Host.Sum").has_value());
  REQUIRE(registry.unregister_callable("Host.Sum"));
  REQUIRE_FALSE(registry.has_binary_callable("Host.Sum"));
}

TEST_CASE("VmHost call_native_binary uses registered binary specialization", "[embed]") {
  fleaux::embed::NativeBindingRegistry registry;
  const auto unary_calls = std::make_shared<int>(0);
  const auto binary_calls = std::make_shared<int>(0);
  auto registration = registry.register_callable(
      fleaux::embed::NativeBinding{
          .symbol = "Host.Sum",
          .signature = fleaux::embed::BindingSignature{},
          .callable = [unary_calls](const fleaux::embed::BindingContext&, const fleaux::embed::VmValue& args)
              -> fleaux::embed::NativeInvokeResult {
            ++*unary_calls;
            return args;
          },
      },
      [binary_calls](const fleaux::embed::BindingContext&, const fleaux::embed::VmValue& lhs,
                     const fleaux::embed::VmValue& rhs) -> fleaux::embed::NativeInvokeResult {
        ++*binary_calls;
        return fleaux::runtime::make_int(fleaux::runtime::as_int_value(lhs) + fleaux::runtime::as_int_value(rhs));
      });
  REQUIRE(registration.has_value());

  fleaux::embed::VmHostConfig config;
  config.binding_registry = &registry;
  fleaux::embed::VmHost host(config);

  const auto call_result = host.call_native_binary("Host.Sum", fleaux::runtime::make_int(2), fleaux::runtime::make_int(3));

  REQUIRE(call_result.has_value());
  REQUIRE(fleaux::runtime::as_int_value(*call_result) == 5);
  REQUIRE(*binary_calls == 1);
  REQUIRE(*unary_calls == 0);
}

TEST_CASE("VmHost call_native_binary falls back to unary tuple invocation", "[embed]") {
  fleaux::embed::NativeBindingRegistry registry;
  const auto unary_calls = std::make_shared<int>(0);
  auto registration = registry.register_callable(fleaux::embed::NativeBinding{
      .symbol = "Host.SumUnary",
      .signature = fleaux::embed::BindingSignature{},
      .callable = [unary_calls](const fleaux::embed::BindingContext&, const fleaux::embed::VmValue& args)
          -> fleaux::embed::NativeInvokeResult {
        ++*unary_calls;
        const auto& pair = fleaux::runtime::as_array(args);
        return fleaux::runtime::make_int(fleaux::runtime::as_int_value(*pair.TryGet(0)) +
                                         fleaux::runtime::as_int_value(*pair.TryGet(1)));
      },
  });
  REQUIRE(registration.has_value());

  fleaux::embed::VmHostConfig config;
  config.binding_registry = &registry;
  fleaux::embed::VmHost host(config);

  const auto call_result =
      host.call_native_binary("Host.SumUnary", fleaux::runtime::make_int(7), fleaux::runtime::make_int(8));

  REQUIRE(call_result.has_value());
  REQUIRE(fleaux::runtime::as_int_value(*call_result) == 15);
  REQUIRE(*unary_calls == 1);
}

