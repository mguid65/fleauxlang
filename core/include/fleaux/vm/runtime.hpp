#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>

#include <tl/expected.hpp>

#include "fleaux/bytecode/module.hpp"

namespace fleaux::vm {

struct ExecutionResult {
  std::int64_t exit_code = 0;
};

struct RuntimeError {
  std::string message;
};

using RuntimeResult = tl::expected<ExecutionResult, RuntimeError>;

class Runtime {
public:
  Runtime() = default;

  [[nodiscard]] auto execute(const bytecode::Module& bytecode_module) const -> RuntimeResult;
  auto execute(const bytecode::Module& bytecode_module, std::ostream& output) const -> RuntimeResult;
};

}  // namespace fleaux::vm
