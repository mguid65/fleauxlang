#include "fleaux/embed/fleaux_binding.hpp"

namespace {

constexpr const char* k_plugin_error = "registration failed from plugin fixture";

}  // namespace

namespace {

auto register_bindings(const fleaux::embed::BindingPluginHostApi*,
                       const char** error_message) -> fleaux::embed::BindingPluginStatus {
  if (error_message != nullptr) {
    *error_message = k_plugin_error;
  }
  return fleaux::embed::BindingPluginStatus::kRegistrationFailed;
}

}  // namespace

FLEAUX_BINDING_ENTRYPOINT(register_bindings)

