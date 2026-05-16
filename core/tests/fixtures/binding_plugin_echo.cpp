#include "fleaux/embed/fleaux_binding.hpp"

namespace {

constexpr const char* k_registration_failed_message = "plugin registration failed";

auto register_bindings(const fleaux::embed::BindingPluginHostApi* host_api,
                       const char** error_message) -> fleaux::embed::BindingPluginStatus {
  if (!fleaux::embed::require_abi(host_api, error_message)) {
    return fleaux::embed::BindingPluginStatus::kAbiMismatch;
  }
  if (!fleaux::embed::require_host_api(host_api, error_message)) {
    if (error_message != nullptr) {
      *error_message = k_registration_failed_message;
    }
    return fleaux::embed::BindingPluginStatus::kRegistrationFailed;
  }

  const auto registration = fleaux::embed::register_native(host_api, fleaux::embed::NativeBinding{
      .symbol = "Host.ModuleEcho",
      .signature = fleaux::embed::BindingSignature{},
      .callable = [](const fleaux::embed::BindingContext&, const fleaux::embed::VmValue& args)
          -> fleaux::embed::NativeInvokeResult { return args; },
  });

  if (!registration.has_value()) {
    if (error_message != nullptr) {
      *error_message = k_registration_failed_message;
    }
    return fleaux::embed::BindingPluginStatus::kRegistrationFailed;
  }

  return fleaux::embed::BindingPluginStatus::kOk;
}

}  // namespace

FLEAUX_BINDING_ENTRYPOINT(register_bindings)



