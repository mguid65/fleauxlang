#pragma once

#include <cstdint>
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
  RuntimeResult execute(const bytecode::Module& module) const;
};

}  // namespace fleaux::vm

