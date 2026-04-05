#include <filesystem>
#include <iostream>

#include "fleaux/frontend/cpp_transpiler.hpp"

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: fleaux_transpile_cli <file.fleaux>\n";
    return 1;
  }

  const std::filesystem::path source = argv[1];
  const fleaux::frontend::cpp_transpile::FleauxCppTranspiler transpiler;
  const auto result = transpiler.process(source);

  if (!result) {
    std::cerr << fleaux::frontend::diag::format_diagnostic(
                     "transpile-cpp",
                     result.error().message,
                     result.error().span,
                     result.error().hint)
              << '\n';
    return 1;
  }

  std::cout << result.value() << '\n';
  return 0;
}

