#pragma once

#include <utility>

#include "fleaux/embed/binding_plugin.hpp"

#if defined(_WIN32)
#define FLEAUX_BINDING_EXPORT extern "C" __declspec(dllexport)
#else
#define FLEAUX_BINDING_EXPORT extern "C" __attribute__((visibility("default")))
#endif

// Helper macros and wrappers here are intentionally header-only. They are safe
// to use only within VmHost-managed plugin registration and must not be used to
// preserve callable references beyond plugin unload boundaries.
#define FLEAUX_BINDING_ENTRYPOINT(register_fn)                                                                  \
  FLEAUX_BINDING_EXPORT auto fleaux_register_bindings_v1(                                                       \
      const ::fleaux::embed::BindingPluginHostApi* host_api, const char** error_message)                       \
      -> ::fleaux::embed::BindingPluginStatus {                                                                  \
    return register_fn(host_api, error_message);                                                                 \
  }

namespace fleaux::embed {

[[nodiscard]] inline auto require_host_api(const BindingPluginHostApi* host_api, const char** error_message)
    -> bool {
  if (host_api == nullptr) {
    if (error_message != nullptr) {
      *error_message = "binding host API pointer is null";
    }
    return false;
  }

  if (host_api->registry == nullptr || host_api->register_callable == nullptr) {
    if (error_message != nullptr) {
      *error_message = "binding host API is missing registry callbacks";
    }
    return false;
  }

  return true;
}

[[nodiscard]] inline auto require_abi(const BindingPluginHostApi* host_api, const char** error_message) -> bool {
  if (host_api == nullptr) {
    if (error_message != nullptr) {
      *error_message = "binding host API pointer is null";
    }
    return false;
  }

  if (host_api->abi_version != kBindingPluginAbiVersion) {
    if (error_message != nullptr) {
      *error_message = "binding host ABI mismatch";
    }
    return false;
  }

  return true;
}

[[nodiscard]] inline auto register_native(const BindingPluginHostApi* host_api, NativeBinding binding)
    -> BindingResult {
  if (host_api == nullptr || host_api->registry == nullptr || host_api->register_callable == nullptr) {
    return tl::unexpected(BindingError{
        .code = BindingErrorCode::kInternal,
        .message = "binding host API is not initialized",
        .symbol = std::move(binding.symbol),
    });
  }

  return host_api->register_callable(host_api->registry, std::move(binding));
}

}  // namespace fleaux::embed

