#pragma once

#include <string>
#include <vector>

namespace fleaux::cli {

class ReplDriver {
public:
  [[nodiscard]] auto run(const std::vector<std::string>& process_args, bool color_enabled = true) const -> int;
};

}  // namespace fleaux::cli
