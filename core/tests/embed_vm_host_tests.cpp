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
  std::string captured_stderr;
  fleaux::embed::VmHostConfig config;
  config.stdout_sink = [&captured](const std::string_view text) { captured.append(text); };
  config.stderr_sink = [&captured_stderr](const std::string_view text) { captured_stderr.append(text); };
  fleaux::embed::VmHost host(config);
  const auto run_result = host.run_source("memory/embed_test.fleaux", R"(
import Std;
("embed") -> Std.Println;
)");
  REQUIRE(run_result.has_value());
  REQUIRE(fleaux::runtime::as_int_value(*run_result) == 0);
  REQUIRE(captured.find("embed") != std::string::npos);
  REQUIRE(captured_stderr.empty());
}

TEST_CASE("VmHost scopes configured process args to the active execution", "[embed]") {
  fleaux::runtime::set_process_args(std::vector<std::string>{"outer", "persisted"});

  std::string captured;
  fleaux::embed::VmHostConfig config;
  config.process_args = {"alpha", "beta"};
  config.stdout_sink = [&captured](const std::string_view text) { captured.append(text); };
  fleaux::embed::VmHost host(config);

  const auto run_result = host.run_source("memory/embed_process_args.fleaux", R"(
import Std;
() -> Std.GetArgs -> Std.Length -> Std.Println;
)" );

  REQUIRE(run_result.has_value());
  REQUIRE(captured == "3\n");

  const auto ambient_args = fleaux::runtime::get_process_args();
  REQUIRE(ambient_args.size() == 2U);
  REQUIRE(ambient_args[0] == "outer");
  REQUIRE(ambient_args[1] == "persisted");
}

TEST_CASE("VmHost routes Std.Input through configured stdin stream", "[embed]") {
  std::istringstream ambient_input{"Outer\n"};
  fleaux::runtime::RuntimeInputStreamScope ambient_scope(ambient_input);

  std::istringstream configured_input{"Ada\n"};
  std::string captured;
  fleaux::embed::VmHostConfig config;
  config.stdin_stream = std::ref(configured_input);
  config.stdout_sink = [&captured](const std::string_view text) { captured.append(text); };
  fleaux::embed::VmHost host(config);

  const auto run_result = host.run_source("memory/embed_input_stream.fleaux", R"(
import Std;
() -> Std.Input -> Std.Println;
)" );

  REQUIRE(run_result.has_value());
  REQUIRE(captured == "Ada\n");

  std::string ambient_line;
  REQUIRE(std::getline(ambient_input, ambient_line));
  REQUIRE(ambient_line == "Outer");
}

TEST_CASE("VmHost emits parse diagnostics to stderr sink", "[embed]") {
  std::string captured_stderr;
  fleaux::embed::VmHostConfig config;
  config.stderr_sink = [&captured_stderr](const std::string_view text) { captured_stderr.append(text); };
  fleaux::embed::VmHost host(config);

  const auto run_result = host.run_source("memory/embed_parse_error.fleaux", "let Broken =");

  REQUIRE_FALSE(run_result.has_value());
  REQUIRE(run_result.error().category == fleaux::embed::HostErrorCategory::kParse);
  REQUIRE(captured_stderr.find("error[parse]") != std::string::npos);
  REQUIRE(captured_stderr.find(run_result.error().message) != std::string::npos);
  REQUIRE(captured_stderr.find("memory/embed_parse_error.fleaux") != std::string::npos);
}

TEST_CASE("VmHost emits binding diagnostics to stderr sink", "[embed]") {
  std::string captured_stderr;
  fleaux::embed::VmHostConfig config;
  config.stderr_sink = [&captured_stderr](const std::string_view text) { captured_stderr.append(text); };
  fleaux::embed::VmHost host(config);

  const auto call_result = host.call_native("Host.Missing", fleaux::runtime::make_null());

  REQUIRE_FALSE(call_result.has_value());
  REQUIRE(call_result.error().category == fleaux::embed::HostErrorCategory::kBinding);
  REQUIRE(captured_stderr.find("error[binding]") != std::string::npos);
  REQUIRE(captured_stderr.find(call_result.error().message) != std::string::npos);
  REQUIRE(captured_stderr.find("set_binding_registry") != std::string::npos);
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
  fleaux::embed::VmHost host(config);

  const auto arg = fleaux::runtime::make_int(42);
  const auto call_result = host.call("Host.Echo", arg);

  REQUIRE(call_result.has_value());
  REQUIRE(fleaux::runtime::as_int_value(*call_result) == 42);
}

TEST_CASE("VmHost call dispatches Fleaux export when available", "[embed]") {
  fleaux::embed::VmHost host;
  const auto run_result = host.run_source("memory/fleaux_call_dispatch.fleaux", R"(
import Std;
let App.Double(x: Int64): Int64 = (x, x) -> Std.Add;
)");
  REQUIRE(run_result.has_value());

  const auto call_result = host.call("App.Double", fleaux::runtime::make_int(9));

  REQUIRE(call_result.has_value());
  REQUIRE(fleaux::runtime::as_int_value(*call_result) == 18);
}

TEST_CASE("VmHost native binding receives active host reference for re-entrant calls", "[embed]") {
  fleaux::embed::NativeBindingRegistry registry;
  fleaux::embed::VmHostConfig config;
  config.binding_registry = &registry;
  fleaux::embed::VmHost host(config);

  auto registration = registry.register_callable(fleaux::embed::NativeBinding{
      .symbol = "Host.CallDouble",
      .signature = fleaux::embed::BindingSignature{},
      .callable = [&host](const fleaux::embed::BindingContext& context, const fleaux::embed::VmValue& args)
          -> fleaux::embed::NativeInvokeResult {
        if (&context.host != &host) {
          return tl::unexpected(fleaux::embed::HostError{
              .category = fleaux::embed::HostErrorCategory::kInternal,
              .message = "BindingContext host reference did not match active VmHost.",
          });
        }
        return context.host.call_fleaux("App.Double", args);
      },
  });
  REQUIRE(registration.has_value());

  const auto run_result = host.run_source("memory/fleaux_binding_host_reference.fleaux", R"(
import Std;
let App.Double(x: Int64): Int64 = (x, x) -> Std.Add;
)" );
  REQUIRE(run_result.has_value());

  const auto call_result = host.call_native("Host.CallDouble", fleaux::runtime::make_int(9));

  REQUIRE(call_result.has_value());
  REQUIRE(fleaux::runtime::as_int_value(*call_result) == 18);
}

TEST_CASE("VmHost call returns binding error for unknown symbol", "[embed]") {
  fleaux::embed::NativeBindingRegistry registry;
  fleaux::embed::VmHostConfig config;
  config.binding_registry = &registry;
  fleaux::embed::VmHost host(config);

  const auto call_result = host.call("Host.Missing", fleaux::runtime::make_null());

  REQUIRE_FALSE(call_result.has_value());
  REQUIRE(call_result.error().category == fleaux::embed::HostErrorCategory::kBinding);
}

TEST_CASE("VmHost call reports ambiguity when symbol exists in both dispatch surfaces", "[embed]") {
  fleaux::embed::NativeBindingRegistry registry;
  auto registration = registry.register_callable(fleaux::embed::NativeBinding{
      .symbol = "App.Double",
      .signature = fleaux::embed::BindingSignature{},
      .callable = [](const fleaux::embed::BindingContext&, const fleaux::embed::VmValue&) -> fleaux::embed::NativeInvokeResult {
        return fleaux::runtime::make_int(99);
      },
  });
  REQUIRE(registration.has_value());

  fleaux::embed::VmHostConfig config;
  config.binding_registry = &registry;
  fleaux::embed::VmHost host(config);
  const auto run_result = host.run_source("memory/fleaux_call_ambiguous.fleaux", R"(
import Std;
let App.Double(x: Int64): Int64 = (x, x) -> Std.Add;
)");
  REQUIRE(run_result.has_value());

  const auto call_result = host.call("App.Double", fleaux::runtime::make_int(9));

  REQUIRE_FALSE(call_result.has_value());
  REQUIRE(call_result.error().category == fleaux::embed::HostErrorCategory::kBinding);
  REQUIRE(call_result.error().message.find("Ambiguous callable symbol") != std::string::npos);
  REQUIRE(call_result.error().hint.has_value());
  REQUIRE(call_result.error().hint->find("call_fleaux") != std::string::npos);
}

TEST_CASE("VmHost emits call ambiguity diagnostics to stderr sink", "[embed]") {
  std::string captured_stderr;
  fleaux::embed::NativeBindingRegistry registry;
  auto registration = registry.register_callable(fleaux::embed::NativeBinding{
      .symbol = "App.Double",
      .signature = fleaux::embed::BindingSignature{},
      .callable = [](const fleaux::embed::BindingContext&, const fleaux::embed::VmValue&) -> fleaux::embed::NativeInvokeResult {
        return fleaux::runtime::make_int(99);
      },
  });
  REQUIRE(registration.has_value());

  fleaux::embed::VmHostConfig config;
  config.binding_registry = &registry;
  config.stderr_sink = [&captured_stderr](const std::string_view text) { captured_stderr.append(text); };
  fleaux::embed::VmHost host(config);
  const auto run_result = host.run_source("memory/fleaux_call_ambiguous_stderr.fleaux", R"(
import Std;
let App.Double(x: Int64): Int64 = (x, x) -> Std.Add;
)" );
  REQUIRE(run_result.has_value());

  const auto call_result = host.call("App.Double", fleaux::runtime::make_int(9));

  REQUIRE_FALSE(call_result.has_value());
  REQUIRE(call_result.error().category == fleaux::embed::HostErrorCategory::kBinding);
  REQUIRE(captured_stderr.find("error[binding]") != std::string::npos);
  REQUIRE(captured_stderr.find(call_result.error().message) != std::string::npos);
  REQUIRE(captured_stderr.find("call_fleaux") != std::string::npos);
}

TEST_CASE("VmHost call reports when no dispatch surface is available", "[embed]") {
  fleaux::embed::VmHost host;

  const auto call_result = host.call("Host.Missing", fleaux::runtime::make_null());

  REQUIRE_FALSE(call_result.has_value());
  REQUIRE(call_result.error().category == fleaux::embed::HostErrorCategory::kBinding);
  REQUIRE(call_result.error().message.find("No Fleaux module or native binding registry") != std::string::npos);
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

TEST_CASE("VmHost emits load_binding_module diagnostics to stderr sink", "[embed]") {
  const std::filesystem::path plugin_path{FLEAUX_TEST_BINDING_PLUGIN_REGISTRATION_FAILED_PATH};
  REQUIRE(std::filesystem::exists(plugin_path));

  std::string captured_stderr;
  fleaux::embed::NativeBindingRegistry registry;
  fleaux::embed::VmHostConfig config;
  config.binding_registry = &registry;
  config.stderr_sink = [&captured_stderr](const std::string_view text) { captured_stderr.append(text); };
  fleaux::embed::VmHost host(config);

  const auto loaded = host.load_binding_module(plugin_path);

  REQUIRE_FALSE(loaded.has_value());
  REQUIRE(loaded.error().category == fleaux::embed::HostErrorCategory::kBinding);
  REQUIRE(captured_stderr.find("error[binding]") != std::string::npos);
  REQUIRE(captured_stderr.find(loaded.error().message) != std::string::npos);
  REQUIRE(captured_stderr.find("Inspect module registration logic") != std::string::npos);
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
  fleaux::embed::VmHost host(config);

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

TEST_CASE("VmHost emits call_fleaux runtime diagnostics to stderr sink", "[embed]") {
  std::string captured_stderr;
  fleaux::embed::VmHostConfig config;
  config.stderr_sink = [&captured_stderr](const std::string_view text) { captured_stderr.append(text); };
  fleaux::embed::VmHost host(config);
  const auto run_result = host.run_source("memory/fleaux_call_missing_stderr.fleaux", R"(
import Std;
let App.Id(x: Int64): Int64 = x;
)" );
  REQUIRE(run_result.has_value());

  const auto call_result = host.call_fleaux("App.Missing", fleaux::runtime::make_int(1));

  REQUIRE_FALSE(call_result.has_value());
  REQUIRE(call_result.error().category == fleaux::embed::HostErrorCategory::kRuntime);
  REQUIRE(captured_stderr.find("error[runtime]") != std::string::npos);
  REQUIRE(captured_stderr.find(call_result.error().message) != std::string::npos);
  REQUIRE(captured_stderr.find("App.Missing") != std::string::npos);
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

TEST_CASE("VmHost load_binding_module rolls back partial plugin registration on failure", "[embed]") {
  const std::filesystem::path plugin_path{FLEAUX_TEST_BINDING_PLUGIN_PARTIAL_REGISTRATION_FAILED_PATH};
  REQUIRE(std::filesystem::exists(plugin_path));

  fleaux::embed::NativeBindingRegistry registry;
  fleaux::embed::VmHostConfig config;
  config.binding_registry = &registry;
  fleaux::embed::VmHost host(config);

  const auto loaded = host.load_binding_module(plugin_path);
  REQUIRE_FALSE(loaded.has_value());
  REQUIRE(loaded.error().category == fleaux::embed::HostErrorCategory::kBinding);
  REQUIRE(loaded.error().message.find("partial registration failed from plugin fixture") != std::string::npos);
  REQUIRE_FALSE(registry.has_callable("Host.PartialBeforeFailure"));
  REQUIRE(registry.size() == 0);
}

