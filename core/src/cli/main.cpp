#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fleaux/frontend/cpp_transpiler.hpp"

namespace {
struct CliOptions {
  std::optional<std::filesystem::path> source;
  bool all_samples = false;
  bool show_help = false;
};

std::string usage_text() { return "usage: fleaux_transpile_cli [--all-samples] [file.fleaux]"; }

void print_help() {
  std::cout << usage_text() << '\n'
            << "\n"
            << "Options:\n"
            << "  -h, --help         Show this help message\n"
            << "  --all-samples      Transpile all .fleaux files under samples/\n"
            << "\n"
            << "Notes:\n"
            << "  - If no source is provided, defaults to test.fleaux\n";
}

std::optional<std::filesystem::path> resolve_samples_dir(const std::filesystem::path& executable_path) {
  if (auto cwd_samples = std::filesystem::current_path() / "samples"; std::filesystem::is_directory(cwd_samples)) {
    return cwd_samples;
  }

  const auto exe = std::filesystem::weakly_canonical(executable_path);
  if (auto repo_samples = exe.parent_path().parent_path().parent_path() / "samples";
      std::filesystem::is_directory(repo_samples)) {
    return repo_samples;
  }

  return std::nullopt;
}

std::vector<std::filesystem::path> collect_sample_sources(const std::filesystem::path& samples_dir) {
  std::vector<std::filesystem::path> out;
  for (const auto& entry : std::filesystem::directory_iterator(samples_dir)) {
    if (!entry.is_regular_file()) continue;
    if (entry.path().extension() != ".fleaux") continue;
    out.push_back(entry.path());
  }
  std::ranges::sort(out);
  return out;
}

std::optional<CliOptions> parse_cli_args(int argc, char** argv) {
  CliOptions options;

  for (int i = 1; i < argc; ++i) {
    const std::string_view token = argv[i];
    if (token == "-h" || token == "--help") {
      options.show_help = true;
      continue;
    }
    if (token == "--all-samples") {
      options.all_samples = true;
      continue;
    }
    if (!token.empty() && token[0] == '-') {
      std::cerr << "unknown option: " << token << '\n';
      return std::nullopt;
    }
    if (!options.source.has_value()) {
      options.source = std::filesystem::path(token);
      continue;
    }
    std::cerr << "unexpected extra positional argument: " << token << '\n';
    return std::nullopt;
  }

  if (!options.all_samples && !options.source.has_value()) { options.source = std::filesystem::path("test.fleaux"); }

  return options;
}
}  // namespace

int main(int argc, char** argv) {
  const auto options = parse_cli_args(argc, argv);
  if (!options.has_value()) {
    std::cerr << usage_text() << '\n';
    return 1;
  }

  if (options->show_help) {
    print_help();
    return 0;
  }

  std::vector<std::filesystem::path> sources;
  if (options->all_samples) {
    const auto samples_dir = resolve_samples_dir(argv[0]);
    if (!samples_dir.has_value()) {
      std::cerr << "samples directory not found\n";
      return 2;
    }
    sources = collect_sample_sources(*samples_dir);
  } else {
    sources.push_back(*options->source);
  }

  for (const auto& source : sources) {
    constexpr fleaux::frontend::cpp_transpile::FleauxCppTranspiler transpiler;
    const auto result = transpiler.process(source);

    if (!result) {
      std::cerr << fleaux::frontend::diag::format_diagnostic("transpile-cpp", result.error().message,
                                                             result.error().span, result.error().hint)
                << '\n';
      return 1;
    }

    std::cout << result.value() << '\n';
  }

  return 0;
}
