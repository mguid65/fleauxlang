#pragma once

#include <cstdint>
#include <string>

#include <tl/expected.hpp>

#include "fleaux/bytecode/module.hpp"

namespace fleaux::bytecode {

struct VerificationError {
  std::string message{};
};

void rebuild_module_builtin_callsites(Module& module);

[[nodiscard]] auto verify_module_builtin_callsites(const Module& module) -> tl::expected<void, VerificationError>;

// Checks that every instruction's location is in the module's table, that each location's file and inlined function
// exist, and that the call an inlined location names comes before it in the table, so that every chain of inlined
// calls ends. A run that fails reads these without checking them again.
[[nodiscard]] auto verify_module_locations(const Module& module) -> tl::expected<void, VerificationError>;

// The most local slots one frame's code may name. The VM grows a frame's locals to the slot a store names, so a slot
// read from a file is bounded before it can ask for that much memory.
inline constexpr std::int64_t kMaxLocalSlots = std::int64_t{1} << 16;

// Checks every instruction operand that names something: a constant, a function, a closure, a type check, a native
// function, a jump target in the same stream, or a local slot under kMaxLocalSlots. Also each closure's function and
// each exported function. The loader links a module only after this, since linking reads those indices unchecked.
[[nodiscard]] auto verify_module_operands(const Module& module) -> tl::expected<void, VerificationError>;

}  // namespace fleaux::bytecode


