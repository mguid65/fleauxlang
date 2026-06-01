#pragma once

#include <string>

#include <tl/expected.hpp>

#include "fleaux/bytecode/module.hpp"

namespace fleaux::bytecode {

struct VerificationError {
  std::string message{};
};

void rebuild_module_builtin_callsites(Module& module);

[[nodiscard]] auto verify_module_builtin_callsites(const Module& module) -> tl::expected<void, VerificationError>;

}  // namespace fleaux::bytecode


