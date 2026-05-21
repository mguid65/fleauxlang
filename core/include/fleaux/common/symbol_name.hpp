#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace fleaux::common {

[[nodiscard]] inline auto full_symbol_name(const std::string_view qualifier, const std::string_view name) -> std::string {
  if (qualifier.empty()) {
    return std::string(name);
  }

  std::string full_name;
  full_name.reserve(qualifier.size() + 1U + name.size());
  full_name.append(qualifier);
  full_name.push_back('.');
  full_name.append(name);
  return full_name;
}

[[nodiscard]] inline auto full_symbol_name(const std::string& qualifier, const std::string_view name) -> std::string {
  return full_symbol_name(std::string_view{qualifier}, name);
}

[[nodiscard]] inline auto full_symbol_name(const std::optional<std::string>& qualifier, const std::string_view name)
    -> std::string {
  return qualifier.has_value() ? full_symbol_name(std::string_view{*qualifier}, name) : std::string(name);
}

}  // namespace fleaux::common



