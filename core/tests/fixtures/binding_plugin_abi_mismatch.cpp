#include "fleaux/embed/fleaux_binding.hpp"

namespace {

auto register_bindings(const fleaux::embed::BindingPluginHostApi*,
                       const char**) -> fleaux::embed::BindingPluginStatus {
  return fleaux::embed::BindingPluginStatus::kAbiMismatch;
}

}  // namespace

FLEAUX_BINDING_ENTRYPOINT(register_bindings)

