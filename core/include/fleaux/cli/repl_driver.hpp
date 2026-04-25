#pragma once

#include <string>
#include <vector>

namespace fleaux::cli {

enum class ReplBackend {
  kVm,
  kInterpreter,
};

class ReplDriver {
public:
  [[nodiscard]] auto run(const std::vector<std::string>& process_args, ReplBackend backend,
                         bool color_enabled = true) const -> int;
};

}  // namespace fleaux::cli
