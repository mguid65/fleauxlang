#pragma once

#include <algorithm>
#include <filesystem>
#include <optional>
#include <vector>

namespace fleaux::common {

[[nodiscard]] inline auto resolve_samples_dir(const std::filesystem::path& executable_path)
    -> std::optional<std::filesystem::path> {
  if (const auto cwd_samples = std::filesystem::current_path() / "samples"; std::filesystem::is_directory(cwd_samples)) {
    return cwd_samples;
  }

  const auto exe = std::filesystem::weakly_canonical(executable_path);
  if (const auto repo_samples = exe.parent_path().parent_path().parent_path() / "samples";
      std::filesystem::is_directory(repo_samples)) {
    return repo_samples;
  }

  return std::nullopt;
}

[[nodiscard]] inline auto collect_sample_sources(const std::filesystem::path& samples_dir)
    -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> out;
  for (const auto& entry : std::filesystem::directory_iterator(samples_dir)) {
    if (!entry.is_regular_file()) { continue; }
    if (entry.path().extension() != ".fleaux") { continue; }
    out.push_back(entry.path());
  }
  std::ranges::sort(out);
  return out;
}

}  // namespace fleaux::common

