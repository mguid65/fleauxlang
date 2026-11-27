#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#ifndef _WIN32
#include <sys/wait.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "vm_test_support.hpp"

#ifndef FLEAUX_CORE_BIN_DIR
#error "FLEAUX_CORE_BIN_DIR must be defined by CMake for CLI tests."
#endif

namespace {

auto fleaux_binary_path() -> std::filesystem::path {
#ifdef _WIN32
  return std::filesystem::path(FLEAUX_CORE_BIN_DIR) / "fleaux.exe";
#else
  return std::filesystem::path(FLEAUX_CORE_BIN_DIR) / "fleaux";
#endif
}

auto shell_quote(const std::string_view text) -> std::string {
#ifdef _WIN32
  std::string quoted{"\""};
  for (const char ch : text) {
    if (ch == '"') {
      quoted += "\\\"";
    } else {
      quoted.push_back(ch);
    }
  }
  quoted.push_back('"');
  return quoted;
#else
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
#endif
}

#ifdef _WIN32
auto powershell_quote(const std::string_view text) -> std::string {
  std::string quoted{"'"};
  for (const char ch : text) {
    if (ch == '\'') {
      quoted += "''";
    } else {
      quoted.push_back(ch);
    }
  }
  quoted.push_back('\'');
  return quoted;
}
#endif

struct CommandResult {
  int exit_code;
  std::string stdout_text;
  std::string stderr_text;
};

auto run_cli(const std::string& arguments, const std::filesystem::path& working_dir,
                      const std::string_view stdin_text = {}) -> CommandResult {
  const auto input_path = working_dir / "stdin.txt";
  const auto output_path = working_dir / "stdout.txt";
  const auto error_path = working_dir / "stderr.txt";
  if (!stdin_text.empty()) {
    std::ofstream in(input_path);
    in.write(stdin_text.data(), static_cast<std::streamsize>(stdin_text.size()));
  }

  std::string command;
#ifdef _WIN32
  std::string powershell_script =
      "& { $process = Start-Process -FilePath " + powershell_quote(fleaux_binary_path().string());
  if (!arguments.empty()) {
    powershell_script += " -ArgumentList " + powershell_quote(arguments);
  }
  powershell_script += " -WorkingDirectory " + powershell_quote(working_dir.string()) +
      " -RedirectStandardOutput " + powershell_quote(output_path.string()) +
      " -RedirectStandardError " + powershell_quote(error_path.string()) +
      " -WindowStyle Hidden -PassThru -Wait";
  if (!stdin_text.empty()) {
    powershell_script += " -RedirectStandardInput " + powershell_quote(input_path.string());
  }
  powershell_script += "; exit $process.ExitCode }";
  command = "powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -Command \"" +
            powershell_script + "\"";
#else
  command = "cd " + shell_quote(working_dir.string()) + " && " + shell_quote(fleaux_binary_path().string());
  if (!arguments.empty()) { command += " " + arguments; }
  if (!stdin_text.empty()) { command += " <" + shell_quote(input_path.string()); }
  command += " >" + shell_quote(output_path.string()) + " 2>" + shell_quote(error_path.string());
#endif

  const int status = std::system(command.c_str());
#ifdef _WIN32
  const int exit_code = status;
#else
  // std::system returns the shell's wait status here, which holds the exit code in its second byte.
  const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif

  auto read_file = [](const std::filesystem::path& path) -> std::string {
    std::ifstream in(path);
    if (!in.good()) {
      return {};
    }
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
  };

  return CommandResult{
      .exit_code = exit_code,
      .stdout_text = read_file(output_path),
      .stderr_text = read_file(error_path),
  };
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
  REQUIRE_THAT(stdout_text, Catch::Matchers::ContainsSubstring("Start the interactive REPL"));
  REQUIRE_THAT(stdout_text, Catch::Matchers::ContainsSubstring("--no-emit-bytecode"));
  REQUIRE_THAT(stdout_text, Catch::Matchers::ContainsSubstring("--dump-ast"));
  REQUIRE_THAT(stdout_text, Catch::Matchers::ContainsSubstring("--dump-ir"));
  REQUIRE_THAT(stdout_text, Catch::Matchers::ContainsSubstring("--format"));
  REQUIRE(stdout_text.find("--mode") == std::string::npos);
}

TEST_CASE("CLI bench mode runs the program without its output and reports run times", "[vm][cli][bench]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_bench";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "entry.fleaux";
  fleaux::tests::write_text_file(source_path,
                                 "import Std;\n"
                                 "\"hello\" -> Std.Println;\n");

  REQUIRE(std::filesystem::exists(fleaux_binary_path()));
  const auto result = run_cli("--no-emit-bytecode --bench 3 " + shell_quote(source_path.string()), temp_dir);
  INFO("stdout: " << result.stdout_text);
  INFO("stderr: " << result.stderr_text);
  REQUIRE(result.exit_code == 0);
  REQUIRE(result.stdout_text.find("hello") == std::string::npos);
  REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("bench: 3 runs, fastest "));
  REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring(" us, median "));
}

TEST_CASE("CLI shows where a run failed and the calls it failed in", "[vm][cli][calls]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_calls";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "entry.fleaux";
  fleaux::tests::write_text_file(source_path,
                                 "import Std;\n"
                                 "let Ratio(x: Int64): Int64 = (10, x) -> Std.Divide;\n"
                                 "let Twice(x: Int64): Int64 = (x) -> Ratio -> (_, 2) -> Std.Multiply;\n"
                                 "(0) -> Twice -> Std.Println;\n");

  const auto result = run_cli(shell_quote(source_path.string()), temp_dir);
  INFO("stderr: " << result.stderr_text);
  REQUIRE(result.exit_code == 2);
  REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("error[vm-run]: Std.Divide: Int64 division by zero"));
  REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("entry.fleaux:2:41"));
  REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("2 | let Ratio(x: Int64): Int64 = (10, x) -> Std.Divide;"));
  REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("^~~~~~~~~~"));
  REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("= note: in Ratio"));
  REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("= note: called from Twice (entry.fleaux:3:37)"));
  REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("= note: called from the top level (entry.fleaux:4:8)"));
}

TEST_CASE("CLI bench mode needs a run count greater than zero", "[vm][cli][bench]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_bench_count";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "entry.fleaux";
  fleaux::tests::write_text_file(source_path, "import Std;\n1 -> Std.Println;\n");

  REQUIRE(std::filesystem::exists(fleaux_binary_path()));
  for (const auto* count : {"0", "many"}) {
    INFO("count: " << count);
    const auto result =
        run_cli(std::string("--bench ") + count + " " + shell_quote(source_path.string()), temp_dir);
    REQUIRE(result.exit_code == 1);
    REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("--bench needs a run count"));
  }
}

TEST_CASE("CLI AST and IR dump modes stop before execution", "[vm][cli][dump]") {
  SECTION("AST dump") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_dump_ast";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    const auto source_path = temp_dir / "entry.fleaux";
    fleaux::tests::write_text_file(source_path,
                                   "import missing_dep;\n"
                                   "(1) -> MissingCall;\n");

    REQUIRE(std::filesystem::exists(fleaux_binary_path()));
    const auto result = run_cli("--dump-ast " + shell_quote(source_path.string()), temp_dir);
    INFO("stdout: " << result.stdout_text);
    INFO("stderr: " << result.stderr_text);
    REQUIRE(result.exit_code == 0);
    REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("Program {"));
    REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("ImportStatement {"));
    REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("module_name: \"missing_dep\""));
    REQUIRE(result.stderr_text.empty());
  }

  SECTION("IR dump") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_dump_ir";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    const auto source_path = temp_dir / "entry.fleaux";
    fleaux::tests::write_text_file(source_path,
                                   "import missing_dep;\n"
                                   "(1) -> MissingCall;\n");

    REQUIRE(std::filesystem::exists(fleaux_binary_path()));
    const auto result = run_cli("--dump-ir " + shell_quote(source_path.string()), temp_dir);
    INFO("stdout: " << result.stdout_text);
    INFO("stderr: " << result.stderr_text);
    REQUIRE(result.exit_code == 0);
    REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("IRProgram {"));
    REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("IRImport {"));
    REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("module_name: \"missing_dep\""));
    REQUIRE(result.stderr_text.empty());
  }
}

TEST_CASE("CLI format mode prints, checks, and rewrites source", "[vm][cli][format]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_format";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto source_path = temp_dir / "entry.fleaux";
  fleaux::tests::write_text_file(source_path, "import Std;\n(1,2)->Std.Add->Std.Println;\n");
  const std::string formatted = "import Std;\n(1, 2) -> Std.Add -> Std.Println;\n";
  const std::string quoted_path = shell_quote(source_path.string());
  REQUIRE(std::filesystem::exists(fleaux_binary_path()));

  SECTION("print") {
    const auto result = run_cli("--format " + quoted_path, temp_dir);
    INFO("stderr: " << result.stderr_text);
    REQUIRE(result.exit_code == 0);
    REQUIRE(result.stdout_text == formatted);
  }

  SECTION("check, rewrite, check again") {
    const auto unformatted = run_cli("--format --check " + quoted_path, temp_dir);
    REQUIRE(unformatted.exit_code != 0);
    REQUIRE_THAT(unformatted.stderr_text, Catch::Matchers::ContainsSubstring("is not formatted"));

    const auto rewritten = run_cli("--format --write " + quoted_path, temp_dir);
    INFO("stderr: " << rewritten.stderr_text);
    REQUIRE(rewritten.exit_code == 0);
    REQUIRE(rewritten.stdout_text.empty());

    const auto checked = run_cli("--format --check " + quoted_path, temp_dir);
    REQUIRE(checked.exit_code == 0);
  }

  SECTION("--write and --check need --format and exclude each other") {
    REQUIRE(run_cli("--write " + quoted_path, temp_dir).exit_code != 0);
    REQUIRE(run_cli("--format --write --check " + quoted_path, temp_dir).exit_code != 0);
  }
}

TEST_CASE("CLI loads an extension and runs native functions it declares", "[vm][cli][native]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_extension";
  std::filesystem::remove_all(temp_dir);
  const auto extension_dir = temp_dir / "echo";
  const auto app_dir = temp_dir / "app";
  std::filesystem::create_directories(extension_dir);
  std::filesystem::create_directories(app_dir);

  // The echo plugin returns its argument tuple. Its declaration module sits next to it, apart from the program.
  const std::filesystem::path plugin_path{FLEAUX_TEST_BINDING_PLUGIN_PATH};
  std::filesystem::copy_file(plugin_path, extension_dir / plugin_path.filename());
  fleaux::tests::write_text_file(extension_dir / "Echo.fleaux",
                                 "let Host.ModuleEcho(x: Int64, label: String): Tuple<Int64, String> :: __native__;\n");
  const auto source_path = app_dir / "main.fleaux";
  fleaux::tests::write_text_file(source_path,
                                 "import Std;\n"
                                 "import Echo;\n"
                                 "(7, \"seven\") -> Host.ModuleEcho -> Std.Println;\n");
  REQUIRE(std::filesystem::exists(fleaux_binary_path()));

  SECTION("with the extension") {
    const auto result =
        run_cli("--extension " + shell_quote(extension_dir.string()) + " " + shell_quote(source_path.string()), app_dir);
    INFO("stderr: " << result.stderr_text);
    REQUIRE(result.exit_code == 0);
    REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("(7, seven)"));
  }

  SECTION("a module the program imports finds the extension's module too") {
    fleaux::tests::write_text_file(app_dir / "Shout.fleaux",
                                   "import Std;\n"
                                   "import Echo;\n"
                                   "let Shout(x: Int64): Tuple<Int64, String> = (x, \"loud\") -> Host.ModuleEcho;\n");
    const auto nested_source = app_dir / "nested.fleaux";
    fleaux::tests::write_text_file(nested_source,
                                   "import Std;\n"
                                   "import Shout;\n"
                                   "7 -> Shout -> Std.Println;\n");
    const auto result =
        run_cli("--extension " + shell_quote(extension_dir.string()) + " " + shell_quote(nested_source.string()), app_dir);
    INFO("stderr: " << result.stderr_text);
    REQUIRE(result.exit_code == 0);
    REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("(7, loud)"));
  }

  SECTION("without it, the declaration module is not found") {
    const auto result = run_cli(shell_quote(source_path.string()), app_dir);
    REQUIRE(result.exit_code != 0);
    REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("Import not found: 'Echo'"));
  }

  SECTION("a directory without a library is rejected") {
    const auto result =
        run_cli("--extension " + shell_quote(app_dir.string()) + " " + shell_quote(source_path.string()), app_dir);
    REQUIRE(result.exit_code != 0);
    REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("expected exactly one"));
  }
}

TEST_CASE("CLI rejects removed AST and IR aliases", "[vm][cli][dump]") {
  SECTION("removed --ast alias") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_removed_ast_alias";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    const auto result = run_cli("--ast", temp_dir);
    INFO("stdout: " << result.stdout_text);
    INFO("stderr: " << result.stderr_text);
    REQUIRE(result.exit_code != 0);
    REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("unknown option: --ast"));
  }

  SECTION("removed --ir alias") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_removed_ir_alias";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    const auto result = run_cli("--ir", temp_dir);
    INFO("stdout: " << result.stdout_text);
    INFO("stderr: " << result.stderr_text);
    REQUIRE(result.exit_code != 0);
    REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("unknown option: --ir"));
  }
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

TEST_CASE("CLI REPL requires import Std before Std symbols can be used", "[vm][cli][repl][imports][stdlib]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_repl_std_import_required";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  REQUIRE(std::filesystem::exists(fleaux_binary_path()));
  const auto result = run_cli("--repl", temp_dir,
                              "(1, 2) -> Std.Add -> Std.Println;\n"
                              ":quit\n");
  INFO("stdout: " << result.stdout_text);
  INFO("stderr: " << result.stderr_text);
  REQUIRE(result.exit_code == 0);
  REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("Fleaux REPL"));
  REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("Unresolved symbol."));
  REQUIRE((result.stderr_text.find("Std.Add") != std::string::npos ||
           result.stderr_text.find("Std.Println") != std::string::npos));
}

TEST_CASE("CLI REPL resolves normal imports relative to the working directory", "[vm][cli][repl][imports]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_repl_import_contract";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  fleaux::tests::write_text_file(temp_dir / "custom_module.fleaux",
                                 "import Std;\n"
                                 "let Add4(x: Int64): Int64 = (x, 4) -> Std.Add;\n");

  REQUIRE(std::filesystem::exists(fleaux_binary_path()));
  const auto result = run_cli("--repl", temp_dir,
                              "import Std;\n"
                              "import custom_module;\n"
                              "(3) -> Add4 -> Std.Println;\n"
                              ":quit\n");
  INFO("stdout: " << result.stdout_text);
  INFO("stderr: " << result.stderr_text);
  REQUIRE(result.exit_code == 0);
  REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("7"));
  REQUIRE(result.stderr_text.empty());
}

TEST_CASE("CLI REPL reports unresolved normal imports", "[vm][cli][repl][imports]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_repl_missing_import";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  REQUIRE(std::filesystem::exists(fleaux_binary_path()));
  const auto result = run_cli("--repl", temp_dir,
                              "import custom_module;\n"
                              ":quit\n");
  INFO("stdout: " << result.stdout_text);
  INFO("stderr: " << result.stderr_text);
  REQUIRE(result.exit_code == 0);
  REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("import-unresolved:"));
  REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("custom_module"));
}

TEST_CASE("CLI REPL Std.Help prints canonical std docs", "[vm][cli][repl][help]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_repl_help_std";
  std::filesystem::remove_all(temp_dir);
  const auto poisoned_fallbacks = fleaux::tests::write_poisoned_symbolic_std_fallbacks(temp_dir);

  const fleaux::tests::ScopedEnvVar env_scope("FLEAUX_STD_PATH", poisoned_fallbacks.env_std_path.string());

  REQUIRE(std::filesystem::exists(fleaux_binary_path()));
  const auto result = run_cli("--repl", temp_dir,
                              "import Std;\n"
                              "(\"Std.Add\") -> Std.Help -> Std.Println;\n"
                              ":quit\n");
  INFO("stdout: " << result.stdout_text);
  INFO("stderr: " << result.stderr_text);
  REQUIRE(result.exit_code == 0);
  REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("Help on function Std.Add"));
  REQUIRE_THAT(result.stdout_text,
               Catch::Matchers::ContainsSubstring("Add two numbers of the same numeric type."));
  REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("Parameters:"));
  REQUIRE(result.stderr_text.empty());
}

TEST_CASE("CLI REPL handles non-interactive commands, EOF, and no-color mode", "[vm][cli][repl]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_repl_commands";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  REQUIRE(std::filesystem::exists(fleaux_binary_path()));
  const auto result = run_cli("--repl --no-color", temp_dir,
                              ":help\n"
                              ":clear\n"
                              ":mystery\n"
                              "\n");
  INFO("stdout: " << result.stdout_text);
  INFO("stderr: " << result.stderr_text);
  REQUIRE(result.exit_code == 0);
  REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("Fleaux REPL"));
  REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("Commands:"));
  REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring(":clear, :c        Clear current multiline buffer"));
  REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("Unknown REPL command: :mystery"));
  REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("Type :help to list supported commands."));
  REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring(">>> "));
  REQUIRE(result.stderr_text.empty());
}

TEST_CASE("CLI REPL supports multiline non-interactive statements", "[vm][cli][repl]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_repl_multiline";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  REQUIRE(std::filesystem::exists(fleaux_binary_path()));
  const auto result = run_cli("--repl", temp_dir,
                              "import Std;\n"
                              "(1,\n"
                              "2) -> Std.Add -> Std.Println;\n"
                              ":q\n");
  INFO("stdout: " << result.stdout_text);
  INFO("stderr: " << result.stderr_text);
  REQUIRE(result.exit_code == 0);
  REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("Fleaux REPL"));
  REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring(">>> "));
  REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("... "));
  REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("3"));
  REQUIRE(result.stderr_text.empty());
}

TEST_CASE("CLI vm mode writes bytecode cache by default", "[vm][cli]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_default_cache";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto source_path = temp_dir / "entry.fleaux";
  const auto bytecode_path = temp_dir / "entry.fleaux.bc";
  fleaux::tests::write_text_file(source_path, "import Std;\n(1, 2) -> Std.Add -> Std.Println;\n");

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
  fleaux::tests::write_text_file(source_path, "import Std;\n(1, 2) -> Std.Add -> Std.Println;\n");

  REQUIRE(std::filesystem::exists(fleaux_binary_path()));
  const auto result = run_cli("--no-emit-bytecode " + shell_quote(source_path.string()), temp_dir);
  INFO("stdout: " << result.stdout_text);
  INFO("stderr: " << result.stderr_text);
  REQUIRE(result.exit_code == 0);
  REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("3"));
  REQUIRE_FALSE(std::filesystem::exists(bytecode_path));
}

TEST_CASE("CLI with no arguments starts the REPL and accepts immediate quit", "[vm][cli][repl]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_no_args_repl";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  REQUIRE(std::filesystem::exists(fleaux_binary_path()));
  const auto result = run_cli("", temp_dir, ":quit\n");
  INFO("stdout: " << result.stdout_text);
  INFO("stderr: " << result.stderr_text);
  REQUIRE(result.exit_code == 0);
  REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("Fleaux REPL"));
  REQUIRE(result.stderr_text.empty());
}

TEST_CASE("CLI rejects unknown options and extra positional arguments", "[vm][cli]") {
  SECTION("unknown option") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_unknown_option";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    const auto result = run_cli("--definitely-unknown", temp_dir);
    INFO("stdout: " << result.stdout_text);
    INFO("stderr: " << result.stderr_text);
    REQUIRE(result.exit_code != 0);
    REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("usage: fleaux"));
    REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("unknown option: --definitely-unknown"));
    REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("use --help to list supported options"));
  }

  SECTION("extra positional") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_extra_positional";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    const auto result = run_cli(shell_quote("first.fleaux") + " " + shell_quote("second.fleaux"), temp_dir);
    INFO("stdout: " << result.stdout_text);
    INFO("stderr: " << result.stderr_text);
    REQUIRE(result.exit_code != 0);
    REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("usage: fleaux"));
    REQUIRE_THAT(result.stderr_text,
                 Catch::Matchers::ContainsSubstring("unexpected extra positional argument: second.fleaux"));
    REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("use --help for usage"));
  }
}

TEST_CASE("CLI no-run modes skip execution without loading modules", "[vm][cli]") {
  SECTION("source path") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_no_run_source";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    const auto missing_source = temp_dir / "missing_entry.fleaux";
    const auto result = run_cli("--no-run " + shell_quote(missing_source.string()), temp_dir);
    INFO("stdout: " << result.stdout_text);
    INFO("stderr: " << result.stderr_text);
    REQUIRE(result.exit_code == 0);
    REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("[vm] skipped run (--no-run): "));
    REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring(missing_source.string()));
    REQUIRE(result.stderr_text.empty());
  }

  SECTION("repl path") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_no_run_repl";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    const auto result = run_cli("--repl --no-run --no-color", temp_dir);
    INFO("stdout: " << result.stdout_text);
    INFO("stderr: " << result.stderr_text);
    REQUIRE(result.exit_code == 0);
    REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("[vm] skipped run (--no-run): <repl>"));
    REQUIRE(result.stderr_text.empty());
  }
}

TEST_CASE("CLI vm mode forwards process args after -- and supports optimize", "[vm][cli]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_process_args";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto source_path = temp_dir / "args_entry.fleaux";
  fleaux::tests::write_text_file(source_path,
                                 "import Std;\n"
                                 "() -> Std.GetArgs -> Std.Println;\n");

  REQUIRE(std::filesystem::exists(fleaux_binary_path()));
  const auto result = run_cli("--optimize " + shell_quote(source_path.string()) + " -- alpha beta", temp_dir);
  INFO("stdout: " << result.stdout_text);
  INFO("stderr: " << result.stderr_text);
  REQUIRE(result.exit_code == 0);
  REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring(source_path.string()));
  REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("alpha"));
  REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("beta"));
  REQUIRE(result.stderr_text.empty());
}

TEST_CASE("CLI vm mode requires an explicit module path outside the REPL", "[vm][cli]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_missing_source";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto result = run_cli("--optimize", temp_dir);
  INFO("stdout: " << result.stdout_text);
  INFO("stderr: " << result.stderr_text);
  REQUIRE(result.exit_code != 0);
  REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("vm-run"));
  REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("vm mode requires a module path"));
  REQUIRE_THAT(result.stderr_text,
               Catch::Matchers::ContainsSubstring("Pass a .fleaux file, a .fleaux.bc file, or use --repl."));
}

TEST_CASE("CLI vm mode reports loader failures with actionable hints", "[vm][cli]") {
  SECTION("parse diagnostic includes location context and caret") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_loader_parse_context";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    const auto source_path = temp_dir / "entry_parse_error.fleaux";
    fleaux::tests::write_text_file(source_path, "import Std;\n(1) -> Std.Println\n");

    const auto result = run_cli(shell_quote(source_path.string()), temp_dir);
    INFO("stdout: " << result.stdout_text);
    INFO("stderr: " << result.stderr_text);
    REQUIRE(result.exit_code != 0);
    REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("error[vm-run]"));
    REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring(source_path.string() + ":3:1"));
    REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("2 | (1) -> Std.Println"));
    REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("^"));
    REQUIRE_THAT(result.stderr_text,
                 Catch::Matchers::ContainsSubstring("Add ';' to terminate the final statement"));
  }

  SECTION("import unresolved") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_loader_hint_unresolved";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    const auto source_path = temp_dir / "entry.fleaux";
    fleaux::tests::write_text_file(source_path, "import missing_dep;\n(1) -> MissingCall;\n");

    const auto result = run_cli(shell_quote(source_path.string()), temp_dir);
    INFO("stdout: " << result.stdout_text);
    INFO("stderr: " << result.stderr_text);
    REQUIRE(result.exit_code != 0);
    REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("vm-run"));
    REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("import-unresolved:"));
    REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring(source_path.string() + ":1:"));
    REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("1 | import missing_dep;"));
    REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("^"));
    REQUIRE_THAT(result.stderr_text,
                 Catch::Matchers::ContainsSubstring("Verify module name and file location."));
  }

  SECTION("import cycle") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_loader_hint_cycle";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    fleaux::tests::write_text_file(temp_dir / "cycle_a.fleaux", "import cycle_b;\nlet A(x: Float64): Float64 = x;\n");
    fleaux::tests::write_text_file(temp_dir / "cycle_b.fleaux", "import cycle_a;\nlet B(x: Float64): Float64 = x;\n");

    const auto result = run_cli(shell_quote((temp_dir / "cycle_a.fleaux").string()), temp_dir);
    INFO("stdout: " << result.stdout_text);
    INFO("stderr: " << result.stderr_text);
    REQUIRE(result.exit_code != 0);
    REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("import-cycle:"));
    REQUIRE_THAT(result.stderr_text,
                 Catch::Matchers::ContainsSubstring("Break the cycle by extracting shared declarations into a separate module."));
  }

  SECTION("typed import mismatch") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_loader_hint_type_mismatch";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    fleaux::tests::write_text_file(temp_dir / "typed_dep.fleaux", "let Add4(x: String): Int64 = 4;\n");
    fleaux::tests::write_text_file(temp_dir / "typed_entry.fleaux",
                                   "import Std;\n"
                                   "import typed_dep;\n"
                                   "(1) -> Add4 -> Std.Println;\n");

    const auto result = run_cli(shell_quote((temp_dir / "typed_entry.fleaux").string()), temp_dir);
    INFO("stdout: " << result.stdout_text);
    INFO("stderr: " << result.stderr_text);
    REQUIRE(result.exit_code != 0);
    REQUIRE_THAT(result.stderr_text,
                 Catch::Matchers::ContainsSubstring("Type mismatch in call target arguments"));
    REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring((temp_dir / "typed_entry.fleaux").string() + ":3:"));
    REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("3 | (1) -> Add4 -> Std.Println;"));
    REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("^"));
    REQUIRE_THAT(result.stderr_text,
                 Catch::Matchers::ContainsSubstring("Add4 expects argument 1 to be"));
  }
}

TEST_CASE("CLI disassembly mode covers success and failure paths", "[vm][cli][disassemble]") {
  SECTION("missing path") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_disassemble_missing_path";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    const auto result = run_cli("--disassemble", temp_dir);
    INFO("stdout: " << result.stdout_text);
    INFO("stderr: " << result.stderr_text);
    REQUIRE(result.exit_code != 0);
    REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("vm-disassemble"));
    REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("disassembly mode requires a module path"));
    REQUIRE_THAT(result.stderr_text,
                 Catch::Matchers::ContainsSubstring("Pass a .fleaux.bc file (or .fleaux with sibling .bc)."));
  }

  SECTION("missing bytecode file") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_disassemble_missing_file";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    const auto result = run_cli("--disassemble " + shell_quote((temp_dir / "missing_module.fleaux").string()), temp_dir);
    INFO("stdout: " << result.stdout_text);
    INFO("stderr: " << result.stderr_text);
    REQUIRE(result.exit_code != 0);
    REQUIRE_THAT(result.stderr_text, Catch::Matchers::ContainsSubstring("Failed to read bytecode file"));
    REQUIRE_THAT(result.stderr_text,
                 Catch::Matchers::ContainsSubstring("Provide a .fleaux.bc file or a source path with a sibling .fleaux.bc."));
  }

  SECTION("corrupted bytecode file") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_disassemble_corrupt";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    const auto bytecode_path = temp_dir / "broken.fleaux.bc";
    fleaux::tests::write_text_file(bytecode_path, "not-valid-bytecode");

    const auto result = run_cli("--disassemble " + shell_quote(bytecode_path.string()), temp_dir);
    INFO("stdout: " << result.stdout_text);
    INFO("stderr: " << result.stderr_text);
    REQUIRE(result.exit_code != 0);
    REQUIRE_THAT(result.stderr_text,
                 Catch::Matchers::ContainsSubstring("The file may be corrupted or from an incompatible bytecode schema."));
  }

  SECTION("successful disassembly from sibling source cache") {
    const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_cli_disassemble_success";
    std::filesystem::remove_all(temp_dir);
    std::filesystem::create_directories(temp_dir);

    const auto source_path = temp_dir / "entry.fleaux";
    fleaux::tests::write_text_file(source_path, "import Std;\n(1, 2) -> Std.Add -> Std.Println;\n");

    const auto compile_result = run_cli(shell_quote(source_path.string()), temp_dir);
    INFO("compile stdout: " << compile_result.stdout_text);
    INFO("compile stderr: " << compile_result.stderr_text);
    REQUIRE(compile_result.exit_code == 0);
    REQUIRE(std::filesystem::exists(temp_dir / "entry.fleaux.bc"));

    const auto result = run_cli("--disassemble " + shell_quote(source_path.string()), temp_dir);
    INFO("stdout: " << result.stdout_text);
    INFO("stderr: " << result.stderr_text);
    REQUIRE(result.exit_code == 0);
    REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("Instruction 0:"));
    REQUIRE_THAT(result.stdout_text, Catch::Matchers::ContainsSubstring("opcode: PushConst"));
    REQUIRE(result.stderr_text.empty());
  }
}


