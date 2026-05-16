#pragma once

#include <cstdint>

#include "fleaux/embed/native_bindings.hpp"

namespace fleaux::embed {

enum class BindingPluginStatus : std::uint32_t {
  kOk = 0,
  kAbiMismatch = 1,
  kRegistrationFailed = 2,
};

inline constexpr std::uint32_t kBindingPluginAbiVersion = 1;
inline constexpr auto kBindingPluginRegisterEntrypoint = "fleaux_register_bindings_v1";

using RegisterCallableFn = BindingResult (*)(NativeBindingRegistry* registry, NativeBinding binding);

// The host API table is valid only while the plugin is being registered or
// while VmHost retains the loaded module handle. Plugin code must not assume
// any of these pointers outlive VmHost-managed lifetime boundaries.
struct BindingPluginHostApi {
  std::uint32_t abi_version{kBindingPluginAbiVersion};
  NativeBindingRegistry* registry{nullptr};
  VmHost* host{nullptr};
  RegisterCallableFn register_callable{nullptr};
};

using RegisterBindingModuleFn = BindingPluginStatus (*)(const BindingPluginHostApi* host_api, const char** error_message);

}  // namespace fleaux::embed


