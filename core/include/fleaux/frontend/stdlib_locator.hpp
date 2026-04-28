#pragma once

#include <cstdlib>
#include <filesystem>
#include <optional>

namespace fleaux::frontend::stdlib_locator {
namespace detail {

[[nodiscard]] inline auto path_exists(const std::filesystem::path& path) -> bool {
  std::error_code ec;
  return !path.empty() && std::filesystem::exists(path, ec);
}

[[nodiscard]] inline auto canonical_if_exists(const std::filesystem::path& path)
    -> std::optional<std::filesystem::path> {
  if (!path_exists(path)) { return std::nullopt; }
  std::error_code ec;
  const auto canonical = std::filesystem::weakly_canonical(path, ec);
  if (ec) { return std::nullopt; }
  return canonical;
}

[[nodiscard]] inline auto normalize_search_root(std::filesystem::path start_path) -> std::filesystem::path {
  if (start_path.empty()) { start_path = std::filesystem::current_path(); }
  if (!start_path.has_extension()) { return std::filesystem::absolute(start_path).lexically_normal(); }
  return std::filesystem::absolute(start_path.parent_path()).lexically_normal();
}

}  // namespace detail

[[nodiscard]] inline auto find_std_file(std::filesystem::path start_path = {}) -> std::optional<std::filesystem::path> {
  if (const char* env_path = std::getenv("FLEAUX_STD_PATH"); env_path != nullptr && *env_path != '\0') {
    if (const auto candidate = detail::canonical_if_exists(std::filesystem::path(env_path)); candidate.has_value()) {
      return candidate;
    }
  }

  std::filesystem::path cursor = detail::normalize_search_root(std::move(start_path));
  while (!cursor.empty()) {
    if (const auto stdlib_candidate = detail::canonical_if_exists(cursor / "stdlib" / "Std.fleaux");
        stdlib_candidate.has_value()) {
      return stdlib_candidate;
    }

    if (detail::path_exists(cursor / "core" / "CMakeLists.txt")) {
      if (const auto root_candidate = detail::canonical_if_exists(cursor / "Std.fleaux"); root_candidate.has_value()) {
        return root_candidate;
      }
    }

    if (cursor == cursor.root_path()) { break; }
    cursor = cursor.parent_path();
  }

  return std::nullopt;
}

}  // namespace fleaux::frontend::stdlib_locator

