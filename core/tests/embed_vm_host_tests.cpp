#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/embed/binding_plugin.hpp"
#include "fleaux/embed/dynamic_loader.hpp"
#include "fleaux/embed/native_bindings.hpp"
#include "fleaux/embed/vm_host.hpp"
#include "fleaux/runtime/value.hpp"

namespace {

auto fake_plugin_register(const fleaux::embed::BindingPluginHostApi* host_api,
                          const char** error_message) -> fleaux::embed::BindingPluginStatus {
  if (host_api == nullptr) {
    if (error_message != nullptr) {
      *error_message = "host api missing";
    }
    return fleaux::embed::BindingPluginStatus::kRegistrationFailed;
  }

  if (host_api->abi_version != fleaux::embed::kBindingPluginAbiVersion) {
    if (error_message != nullptr) {
      *error_message = "abi mismatch";
    }
    return fleaux::embed::BindingPluginStatus::kAbiMismatch;
  }

  if (host_api->registry == nullptr || host_api->register_callable == nullptr) {
    if (error_message != nullptr) {
      *error_message = "host api incomplete";
    }
    return fleaux::embed::BindingPluginStatus::kRegistrationFailed;
  }

  const auto registration = host_api->register_callable(host_api->registry, fleaux::embed::NativeBinding{
      .symbol = "Host.PluginEcho",
      .signature = fleaux::embed::BindingSignature{},
      .callable = [](const fleaux::embed::BindingContext&, const fleaux::embed::VmValue& args)
          -> fleaux::embed::NativeInvokeResult { return args; },
  });
  if (!registration.has_value()) {
    if (error_message != nullptr) {
      *error_message = "registration failed";
    }
    return fleaux::embed::BindingPluginStatus::kRegistrationFailed;
  }
  return fleaux::embed::BindingPluginStatus::kOk;
}

class FakeDynamicLibrary final : public fleaux::embed::DynamicLibrary {
public:
  [[nodiscard]] auto symbol(const std::string_view symbol_name) const
      -> tl::expected<void*, fleaux::embed::DynamicLoadError> override {
    if (symbol_name == fleaux::embed::kBindingPluginRegisterEntrypoint) {
      return reinterpret_cast<void*>(&fake_plugin_register);
    }
    return tl::unexpected(fleaux::embed::DynamicLoadError{
        .message = "missing symbol",
        .hint = std::nullopt,
    });
  }
};

class FakeDynamicLoader final : public fleaux::embed::DynamicLoader {
public:
  [[nodiscard]] auto open(const std::filesystem::path& library_path) const
      -> tl::expected<std::unique_ptr<fleaux::embed::DynamicLibrary>, fleaux::embed::DynamicLoadError> override {
    if (library_path.empty()) {
      return tl::unexpected(fleaux::embed::DynamicLoadError{.message = "empty path", .hint = std::nullopt});
    }
    return std::make_unique<FakeDynamicLibrary>();
  }
};

}  // namespace

TEST_CASE("VmHost runs source and reports exit code value", "[embed]") {
  std::string captured;
  fleaux::embed::VmHostConfig config;
  config.stdout_sink = [&captured](const std::string_view text) { captured.append(text); };
  fleaux::embed::VmHost host(config);
  const auto run_result = host.run_source("memory/embed_test.fleaux", R"(
import Std;
("embed") -> Std.Println;
)");
  REQUIRE(run_result.has_value());
  REQUIRE(fleaux::runtime::as_int_value(*run_result) == 0);
  REQUIRE(captured.find("embed") != std::string::npos);
}

TEST_CASE("VmHost call dispatches registered native callable", "[embed]") {
  fleaux::embed::NativeBindingRegistry registry;
  auto registration = registry.register_callable(fleaux::embed::NativeBinding{
      .symbol = "Host.Echo",
      .signature = fleaux::embed::BindingSignature{},
      .callable = [](const fleaux::embed::BindingContext&, const fleaux::embed::VmValue& args)
          -> fleaux::embed::NativeInvokeResult { return args; },
  });
  REQUIRE(registration.has_value());

  fleaux::embed::VmHostConfig config;
  config.binding_registry = &registry;
  const fleaux::embed::VmHost host(config);

  const auto arg = fleaux::runtime::make_int(42);
  const auto call_result = host.call_native("Host.Echo", arg);

  REQUIRE(call_result.has_value());
  REQUIRE(fleaux::runtime::as_int_value(*call_result) == 42);
}

TEST_CASE("VmHost call returns binding error for unknown symbol", "[embed]") {
  fleaux::embed::NativeBindingRegistry registry;
  fleaux::embed::VmHostConfig config;
  config.binding_registry = &registry;
  const fleaux::embed::VmHost host(config);

  const auto call_result = host.call_native("Host.Missing", fleaux::runtime::make_null());

  REQUIRE_FALSE(call_result.has_value());
  REQUIRE(call_result.error().category == fleaux::embed::HostErrorCategory::kBinding);
}

TEST_CASE("VmHost loads binding module and dispatches plugin callable", "[embed]") {
  fleaux::embed::NativeBindingRegistry registry;

  fleaux::embed::VmHostConfig config;
  config.binding_registry = &registry;
  fleaux::embed::VmHost host(config);
  host.set_dynamic_loader(std::make_unique<FakeDynamicLoader>());

  const auto loaded = host.load_binding_module("fake/plugin/module");
  REQUIRE(loaded.has_value());

  const auto call_result = host.call_native("Host.PluginEcho", fleaux::runtime::make_int(7));
  REQUIRE(call_result.has_value());
  REQUIRE(fleaux::runtime::as_int_value(*call_result) == 7);
}

TEST_CASE("VmHost load_binding_module requires configured registry", "[embed]") {
  fleaux::embed::VmHost host;
  host.set_dynamic_loader(std::make_unique<FakeDynamicLoader>());

  const auto loaded = host.load_binding_module("fake/plugin/module");
  REQUIRE_FALSE(loaded.has_value());
  REQUIRE(loaded.error().category == fleaux::embed::HostErrorCategory::kBinding);
}

TEST_CASE("VmHost loads real shared plugin module and dispatches module binding", "[embed]") {
  const std::filesystem::path plugin_path{FLEAUX_TEST_BINDING_PLUGIN_PATH};
  REQUIRE(std::filesystem::exists(plugin_path));

  fleaux::embed::NativeBindingRegistry registry;
  fleaux::embed::VmHostConfig config;
  config.binding_registry = &registry;
  fleaux::embed::VmHost host(config);

  const auto loaded = host.load_binding_module(plugin_path);
  REQUIRE(loaded.has_value());

  const auto call_result = host.call_native("Host.ModuleEcho", fleaux::runtime::make_int(11));
  REQUIRE(call_result.has_value());
  REQUIRE(fleaux::runtime::as_int_value(*call_result) == 11);
}

TEST_CASE("VmHost call_fleaux requires an available module", "[embed]") {
  fleaux::embed::NativeBindingRegistry registry;
  fleaux::embed::VmHostConfig config;
  config.binding_registry = &registry;
  const fleaux::embed::VmHost host(config);

  const auto call_result = host.call_fleaux("App.Main", fleaux::runtime::make_null());
  REQUIRE_FALSE(call_result.has_value());
  REQUIRE(call_result.error().category == fleaux::embed::HostErrorCategory::kBinding);
}

TEST_CASE("VmHost call_fleaux invokes exported function symbol", "[embed]") {
  fleaux::embed::VmHost host;
  const auto run_result = host.run_source("memory/fleaux_call.fleaux", R"(
import Std;
let App.Double(x: Int64): Int64 = (x, x) -> Std.Add;
)");
  REQUIRE(run_result.has_value());

  const auto call_result = host.call_fleaux("App.Double", fleaux::runtime::make_int(9));
  REQUIRE(call_result.has_value());
  REQUIRE(fleaux::runtime::as_int_value(*call_result) == 18);
}

TEST_CASE("VmHost call_fleaux reports unresolved symbol", "[embed]") {
  fleaux::embed::VmHost host;
  const auto run_result = host.run_source("memory/fleaux_call_missing.fleaux", R"(
import Std;
let App.Id(x: Int64): Int64 = x;
)");
  REQUIRE(run_result.has_value());

  const auto call_result = host.call_fleaux("App.Missing", fleaux::runtime::make_int(1));
  REQUIRE_FALSE(call_result.has_value());
  REQUIRE(call_result.error().category == fleaux::embed::HostErrorCategory::kRuntime);
  REQUIRE(call_result.error().message.find("Unresolved exported symbol") != std::string::npos);
}

TEST_CASE("VmHost load_binding_module reports ABI mismatch from real shared plugin", "[embed]") {
  const std::filesystem::path plugin_path{FLEAUX_TEST_BINDING_PLUGIN_ABI_MISMATCH_PATH};
  REQUIRE(std::filesystem::exists(plugin_path));

  fleaux::embed::NativeBindingRegistry registry;
  fleaux::embed::VmHostConfig config;
  config.binding_registry = &registry;
  fleaux::embed::VmHost host(config);

  const auto loaded = host.load_binding_module(plugin_path);
  REQUIRE_FALSE(loaded.has_value());
  REQUIRE(loaded.error().category == fleaux::embed::HostErrorCategory::kBinding);
  REQUIRE(loaded.error().message.find("ABI version mismatch") != std::string::npos);
}

TEST_CASE("VmHost load_binding_module reports missing plugin entrypoint", "[embed]") {
  const std::filesystem::path plugin_path{FLEAUX_TEST_BINDING_PLUGIN_MISSING_ENTRYPOINT_PATH};
  REQUIRE(std::filesystem::exists(plugin_path));

  fleaux::embed::NativeBindingRegistry registry;
  fleaux::embed::VmHostConfig config;
  config.binding_registry = &registry;
  fleaux::embed::VmHost host(config);

  const auto loaded = host.load_binding_module(plugin_path);
  REQUIRE_FALSE(loaded.has_value());
  REQUIRE(loaded.error().category == fleaux::embed::HostErrorCategory::kBinding);
  REQUIRE(loaded.error().hint.has_value());
  REQUIRE(loaded.error().hint->find("required registration entrypoint") != std::string::npos);
}

TEST_CASE("VmHost load_binding_module reports plugin registration failure", "[embed]") {
  const std::filesystem::path plugin_path{FLEAUX_TEST_BINDING_PLUGIN_REGISTRATION_FAILED_PATH};
  REQUIRE(std::filesystem::exists(plugin_path));

  fleaux::embed::NativeBindingRegistry registry;
  fleaux::embed::VmHostConfig config;
  config.binding_registry = &registry;
  fleaux::embed::VmHost host(config);

  const auto loaded = host.load_binding_module(plugin_path);
  REQUIRE_FALSE(loaded.has_value());
  REQUIRE(loaded.error().category == fleaux::embed::HostErrorCategory::kBinding);
  REQUIRE(loaded.error().message.find("registration failed from plugin fixture") != std::string::npos);
}

