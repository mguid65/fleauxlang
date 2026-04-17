#pragma once

#include <tl/expected.hpp>

#include "fleaux/frontend/lowering.hpp"

namespace fleaux::frontend::type_check {

using TypeCheckError = lowering::LoweringError;

[[nodiscard]] auto validate_program(const ir::IRProgram& program) -> tl::expected<void, TypeCheckError>;

}  // namespace fleaux::frontend::type_check

