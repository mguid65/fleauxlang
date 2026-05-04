#pragma once

#include <string>
#include <vector>

#include "fleaux/vm/runtime.hpp"

namespace fleaux::cli {

class ReplDriver {
public:
  [[nodiscard]] auto run(const std::vector<std::string>& process_args, bool color_enabled = true,
                         const fleaux::vm::RuntimeCompileOptions& compile_options = {}) const -> int;
};

}  // namespace fleaux::cli
