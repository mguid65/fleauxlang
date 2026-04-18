#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <tl/expected.hpp>

#include "fleaux/bytecode/module.hpp"

namespace fleaux::bytecode {

struct SerializationError {
  std::string message;
};

// Serialize a compiled Module to a binary format.
// The format is version-tagged to allow future evolution.
// Returns the serialized bytes on success, or an error message.
[[nodiscard]] auto serialize_module(const Module& module) -> tl::expected<std::vector<std::uint8_t>, SerializationError>;

// Deserialize a binary bytecode buffer back into a Module.
// Returns the reconstructed Module on success, or an error message.
[[nodiscard]] auto deserialize_module(const std::vector<std::uint8_t>& buffer) -> tl::expected<Module, SerializationError>;

}  // namespace fleaux::bytecode

