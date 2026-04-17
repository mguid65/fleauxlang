#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fleaux/common/sample_sources.hpp"
#include "fleaux/frontend/cpp_transpiler.hpp"

namespace {
struct CliOptions {
  std::optional<std::filesystem::path> source;
  bool all_samples = false;
  bool show_help = false;
};

auto usage_text() -> std::string { return "usage: fleaux2cpp [--all-samples] [file.fleaux]"; }

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

auto parse_cli_args(int argc, char** argv) -> std::optional<CliOptions> {
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

auto main(int argc, char** argv) -> int {
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
    const auto samples_dir = fleaux::common::resolve_samples_dir(argv[0]);
    if (!samples_dir.has_value()) {
      std::cerr << "samples directory not found\n";
      return 2;
    }
    sources = fleaux::common::collect_sample_sources(*samples_dir);
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
