#include "fleaux/embed/fleaux_binding.hpp"

namespace {

constexpr const char* k_plugin_error = "partial registration failed from plugin fixture";

auto register_bindings(const fleaux::embed::BindingPluginHostApi* host_api,
                       const char** error_message) -> fleaux::embed::BindingPluginStatus {
  if (!fleaux::embed::require_abi(host_api, error_message)) {
    return fleaux::embed::BindingPluginStatus::kAbiMismatch;
  }
  if (!fleaux::embed::require_host_api(host_api, error_message)) {
    if (error_message != nullptr) {
      *error_message = k_plugin_error;
    }
    return fleaux::embed::BindingPluginStatus::kRegistrationFailed;
  }

  const auto registration = fleaux::embed::register_native(host_api, fleaux::embed::NativeBinding{
      .symbol = "Host.PartialBeforeFailure",
      .signature = fleaux::embed::BindingSignature{},
      .callable = [](const fleaux::embed::BindingContext&, const fleaux::embed::VmValue& args)
          -> fleaux::embed::NativeInvokeResult { return args; },
  });

  if (!registration.has_value()) {
    if (error_message != nullptr) {
      *error_message = k_plugin_error;
    }
    return fleaux::embed::BindingPluginStatus::kRegistrationFailed;
  }

  if (error_message != nullptr) {
    *error_message = k_plugin_error;
  }
  return fleaux::embed::BindingPluginStatus::kRegistrationFailed;
}

}  // namespace

FLEAUX_BINDING_ENTRYPOINT(register_bindings);

