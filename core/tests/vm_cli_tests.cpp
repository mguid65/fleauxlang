#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#ifndef FLEAUX_CORE_BIN_DIR
#error "FLEAUX_CORE_BIN_DIR must be defined by CMake for CLI tests."
#endif

namespace {

std::filesystem::path fleaux_binary_path() { return std::filesystem::path(FLEAUX_CORE_BIN_DIR) / "fleaux"; }

std::string shell_quote(const std::string_view text) {
  std::string quoted{"'"};
  for (const char ch : text) {
    if (ch == '\'') {
      quoted += "'\\''";
    } else {
      quoted.push_back(ch);
    }
  }
  quoted.push_back('\'');
  return quoted;
}

struct CommandResult {
  int exit_code;
  std::string stdout_text;
  std::string stderr_text;
};

CommandResult run_cli(const std::string& arguments, const std::filesystem::path& working_dir,
                      const std::string_view stdin_text = {}) {
  const auto input_path = working_dir / "stdin.txt";
  const auto output_path = working_dir / "stdout.txt";
  const auto error_path = working_dir / "stderr.txt";
  if (!stdin_text.empty()) {
    std::ofstream in(input_path);
    in.write(stdin_text.data(), static_cast<std::streamsize>(stdin_text.size()));
  }

  std::string command =
      "cd " + shell_quote(working_dir.string()) + " && " + shell_quote(fleaux_binary_path().string()) + " " +
      arguments;
  if (!stdin_text.empty()) { command += " <" + shell_quote(input_path.string()); }
  command += " >" + shell_quote(output_path.string()) + " 2>" + shell_quote(error_path.string());

  const int exit_code = std::system(command.c_str());

  auto read_file = [](const std::filesystem::path& path) -> std::string {
    std::ifstream in(path);
    std::string text;
    if (in.good()) {
      text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    return text;
  };

  return CommandResult{
      .exit_code = exit_code,
      .stdout_text = read_file(output_path),
      .stderr_text = read_file(error_path),
  };
}

void write_source_file(const std::filesystem::path& path, const std::string_view source) {
  std::ofstream out(path);
  REQUIRE(out.good());
  out.write(source.data(), static_cast<std::streamsize>(source.size()));
  REQUIRE(out.good());
}

}  // namespace

TEST_CASE("CLI help documents bytecode cache writes as opt-out", "[vm][cli]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_help";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  REQUIRE(std::filesystem::exists(fleaux_binary_path()));
  const auto [exit_code, stdout_text, stderr_text] = run_cli("--help", temp_dir);
  INFO("stdout: " << stdout_text);
  INFO("stderr: " << stderr_text);
  REQUIRE(exit_code == 0);
  REQUIRE_THAT(stdout_text, Catch::Matchers::ContainsSubstring("[--repl]"));
  REQUIRE_THAT(stdout_text, Catch::Matchers::ContainsSubstring("VM mode writes/refreshes .fleaux.bc cache files by default"));
  REQUIRE_THAT(stdout_text, Catch::Matchers::ContainsSubstring("Start the interactive REPL"));
  REQUIRE_THAT(stdout_text, Catch::Matchers::ContainsSubstring("--no-emit-bytecode"));
  REQUIRE(stdout_text.find("--mode") == std::string::npos);
}

TEST_CASE("CLI vm REPL executes snippets in vm mode", "[vm][cli][repl]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_repl_vm";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  REQUIRE(std::filesystem::exists(fleaux_binary_path()));
  const auto result = run_cli("--repl", temp_dir,
                              "import Std;\n"
                              "let AddOne(x: Float64): Float64 = (x, 1.0) -> Std.Add;\n"
                              "2.0 -> AddOne -> Std.Println;\n"
                              ":quit\n");
  INFO("stdout: " << result.stdout_text);
  INFO("stderr: " << result.stderr_text);
  REQUIRE(result.exit_code == 0);
  REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("Fleaux REPL"));
  REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("3"));
  REQUIRE(result.stderr_text.empty());
}

TEST_CASE("CLI REPL rejects non-symbolic imports", "[vm][cli][repl][imports]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_repl_import_contract";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  REQUIRE(std::filesystem::exists(fleaux_binary_path()));
  const auto result = run_cli("--repl", temp_dir,
                              "import custom_module;\n"
                              ":quit\n");
  INFO("stdout: " << result.stdout_text);
  INFO("stderr: " << result.stderr_text);
  REQUIRE(result.exit_code == 0);
  REQUIRE_THAT(result.stderr_text,
               Catch::Matchers::ContainsSubstring("REPL only supports symbolic imports: Std, StdBuiltins."));
  REQUIRE_THAT(result.stderr_text,
               Catch::Matchers::ContainsSubstring("Define helper lets inline, or run a file for module imports."));
}

TEST_CASE("CLI vm mode writes bytecode cache by default", "[vm][cli]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_default_cache";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto source_path = temp_dir / "entry.fleaux";
  const auto bytecode_path = temp_dir / "entry.fleaux.bc";
  write_source_file(source_path, "import Std;\n(1, 2) -> Std.Add -> Std.Println;\n");

  REQUIRE(std::filesystem::exists(fleaux_binary_path()));
  const auto result = run_cli(shell_quote(source_path.string()), temp_dir);
  INFO("stdout: " << result.stdout_text);
  INFO("stderr: " << result.stderr_text);
  REQUIRE(result.exit_code == 0);
  REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("3"));
  REQUIRE(std::filesystem::exists(bytecode_path));
}

TEST_CASE("CLI no-emit-bytecode disables bytecode cache writes", "[vm][cli]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_no_cache";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto source_path = temp_dir / "entry.fleaux";
  const auto bytecode_path = temp_dir / "entry.fleaux.bc";
  write_source_file(source_path, "import Std;\n(1, 2) -> Std.Add -> Std.Println;\n");

  REQUIRE(std::filesystem::exists(fleaux_binary_path()));
  const auto result = run_cli("--no-emit-bytecode " + shell_quote(source_path.string()), temp_dir);
  INFO("stdout: " << result.stdout_text);
  INFO("stderr: " << result.stderr_text);
  REQUIRE(result.exit_code == 0);
  REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("3"));
  REQUIRE_FALSE(std::filesystem::exists(bytecode_path));
}


