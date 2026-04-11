#include <filesystem>
#include <fstream>
#include <cstdio>
#include <array>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <sys/wait.h>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/frontend/cpp_transpiler.hpp"

#ifndef FLEAUX_REPO_ROOT
#error "FLEAUX_REPO_ROOT must be defined by CMake for sample tests."
#endif

namespace {

std::string read_text(const std::filesystem::path& path) {
  std::ifstream in(path);
  std::string out((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return out;
}

std::filesystem::path repo_root_path() {
  return std::filesystem::path(FLEAUX_REPO_ROOT);
}

std::filesystem::path samples_dir_path() {
  return repo_root_path() / "samples";
}

std::filesystem::path runtime_include_path() {
  return repo_root_path() / "core" / "include";
}

std::filesystem::path datatree_include_path() {
  return repo_root_path() / "third_party" / "datatree" / "include";
}

std::filesystem::path tl_include_path() {
  return repo_root_path() / "third_party" / "tl" / "include";
}

struct CommandResult {
  int exit_code = -1;
  std::string output;
};

std::string shell_quote(const std::string& text) {
  std::string out = "'";
  for (const char c : text) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out.push_back(c);
    }
  }
  out += "'";
  return out;
}

CommandResult run_command_capture(const std::string& command) {
  CommandResult result;
  std::array<char, 4096> buffer{};

  const std::string wrapped = "bash -lc " + shell_quote(command + " 2>&1");
  FILE* pipe = popen(wrapped.c_str(), "r");
  if (pipe == nullptr) {
    result.output = "Failed to spawn command";
    return result;
  }

  while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
    result.output += buffer.data();
  }

  const int status = pclose(pipe);
  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
  } else {
    result.exit_code = status;
  }

  return result;
}

std::filesystem::path stage_samples_to_temp(const std::string_view sample_file) {
  const auto stem = std::filesystem::path(sample_file).stem().string();
  const auto staged_root =
      std::filesystem::temp_directory_path() / "fleaux_core_tests_samples" / stem;

  std::filesystem::remove_all(staged_root);
  std::filesystem::create_directories(staged_root);

  for (const auto& entry : std::filesystem::directory_iterator(samples_dir_path())) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto p = entry.path();
    if (p.extension() != ".fleaux") {
      continue;
    }
    std::filesystem::copy_file(p, staged_root / p.filename(),
                               std::filesystem::copy_options::overwrite_existing);
  }

  return staged_root;
}

std::vector<std::string> sample_runtime_args(const std::string_view sample_file,
                                             const std::filesystem::path& staged_root,
                                             const std::filesystem::path& source_path) {
  (void)staged_root;
  if (sample_file == "25_fleaux_parser.fleaux") {
    // Parser sample needs an input file path; parse itself in staged workspace.
    return {source_path.string()};
  }
  return {};
}

void transpile_sample_and_assert(const std::string_view sample_file) {
  const auto staged_root = stage_samples_to_temp(sample_file);
  const auto source_path = staged_root / std::filesystem::path(sample_file).filename();

  REQUIRE(std::filesystem::exists(source_path));

  const fleaux::frontend::cpp_transpile::FleauxCppTranspiler transpiler;
  const auto result = transpiler.process(source_path);

  REQUIRE(result.has_value());
  REQUIRE(std::filesystem::exists(result.value()));

  const std::string generated = read_text(result.value());
  REQUIRE(generated.find("#include \"fleaux/runtime/fleaux_runtime.hpp\"") != std::string::npos);
  REQUIRE(generated.find("using fleaux::runtime::operator|;") != std::string::npos);
  REQUIRE(generated.find("int main(int argc, char** argv)") != std::string::npos);

  const auto generated_cpp = result.value();
  const auto generated_bin = generated_cpp.parent_path() / generated_cpp.stem();
  const std::string compile_cmd =
      "g++ -std=c++20 "
      "-I" + shell_quote(runtime_include_path().string()) + " "
      "-I" + shell_quote(datatree_include_path().string()) + " "
      "-I" + shell_quote(tl_include_path().string()) + " " +
      shell_quote(generated_cpp.string()) + " -o " + shell_quote(generated_bin.string());

  const auto compile_result = run_command_capture(compile_cmd);
  INFO("Compile command: " << compile_cmd);
  INFO("Compile output:\n" << compile_result.output);
  REQUIRE(compile_result.exit_code == 0);
  REQUIRE(std::filesystem::exists(generated_bin));

  std::string run_cmd = shell_quote(generated_bin.string());
  for (const auto& arg : sample_runtime_args(sample_file, staged_root, source_path)) {
    run_cmd += " ";
    run_cmd += shell_quote(arg);
  }

  const auto run_result = run_command_capture(run_cmd);
  INFO("Run command: " << run_cmd);
  INFO("Run output:\n" << run_result.output);
  REQUIRE(run_result.exit_code == 0);
}

constexpr std::array<std::string_view, 33> kExpectedSamples = {
    "01_hello_world.fleaux",
    "02_arithmetic.fleaux",
    "03_pipeline_chaining.fleaux",
    "04_function_definitions.fleaux",
    "05_select.fleaux",
    "06_branch.fleaux",
    "07_apply.fleaux",
    "08_loop.fleaux",
    "09_loop_n.fleaux",
    "10_strings.fleaux",
    "11_tuples.fleaux",
    "12_math.fleaux",
    "13_comparison_and_logic.fleaux",
    "14_os.fleaux",
    "15_path.fleaux",
    "16_file_and_dir.fleaux",
    "17_printf_and_tostring.fleaux",
    "18_constants.fleaux",
    "19_composition.fleaux",
    "20_export.fleaux",
    "21_import.fleaux",
    "22_file_streaming.fleaux",
    "23_binary_search.fleaux",
    "24_dicts.fleaux",
    "25_fleaux_parser.fleaux",
    "26_format_specifiers.fleaux",
    "27_error_handling_branching.fleaux",
    "28_variadics.fleaux",
    "29_inline_closures.fleaux",
    "30_pattern_matching.fleaux",
    "31_result_ok_err.fleaux",
    "32_try_empty_tuple.fleaux",
    "33_exp_parallel.fleaux",
};

}  // namespace

TEST_CASE("Transpiler emits runtime scaffold and let symbols", "[transpiler]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_transpiler_basic";
  std::filesystem::create_directories(temp_dir);

  const auto source_path = temp_dir / "basic.fleaux";
  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "let Add4(x: Number): Number = (4, x) -> Std.Add;\n"
           "(4) -> Add4 -> Std.Println;\n";
  }

  const fleaux::frontend::cpp_transpile::FleauxCppTranspiler transpiler;
  const auto result = transpiler.process(source_path);

  REQUIRE(result.has_value());
  REQUIRE(std::filesystem::exists(result.value()));

  const std::string generated = read_text(result.value());
  REQUIRE(generated.find("#include \"fleaux/runtime/fleaux_runtime.hpp\"") != std::string::npos);
  REQUIRE(generated.find("using fleaux::runtime::operator|;") != std::string::npos);
  REQUIRE(generated.find("fleaux::runtime::Value Add4") != std::string::npos);
  REQUIRE(generated.find("fleaux::runtime::Add{}") != std::string::npos);
}

TEST_CASE("Transpiler resolves local module imports", "[transpiler]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_transpiler_imports";
  std::filesystem::create_directories(temp_dir);

  const auto export_path = temp_dir / "20_export.fleaux";
  {
    std::ofstream out(export_path);
    out << "import Std;\n"
           "let Add4(x: Number): Number = (4, x) -> Std.Add;\n";
  }

  const auto main_path = temp_dir / "21_import.fleaux";
  {
    std::ofstream out(main_path);
    out << "import 20_export;\n"
           "(4) -> Add4 -> Std.Println;\n";
  }

  const fleaux::frontend::cpp_transpile::FleauxCppTranspiler transpiler;
  const auto result = transpiler.process(main_path);

  REQUIRE(result.has_value());
  REQUIRE(std::filesystem::exists(result.value()));

  const std::string generated = read_text(result.value());
  REQUIRE(generated.find("fleaux::runtime::Value Add4") != std::string::npos);
  REQUIRE(generated.find("| Add4") != std::string::npos);
}

TEST_CASE("Transpiler reports missing source path", "[transpiler]") {
  const auto missing_path = std::filesystem::temp_directory_path() / "fleaux_core_tests_missing" /
                            "does_not_exist.fleaux";

  const fleaux::frontend::cpp_transpile::FleauxCppTranspiler transpiler;
  const auto result = transpiler.process(missing_path);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message.find("Failed to read source file") != std::string::npos);
}

TEST_CASE("Transpiler emits inline closure callable refs", "[transpiler]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_transpiler_closure";
  std::filesystem::create_directories(temp_dir);

  const auto source_path = temp_dir / "closure_apply.fleaux";
  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "(10) -> (x: Number): Number = (x, 1) -> Std.Add -> Std.Println;\n";
  }

  const fleaux::frontend::cpp_transpile::FleauxCppTranspiler transpiler;
  const auto result = transpiler.process(source_path);
  REQUIRE(result.has_value());

  const std::string generated = read_text(result.value());
  REQUIRE(generated.find("make_callable_ref(") != std::string::npos);
  REQUIRE(generated.find("Apply{}") != std::string::npos);
}

TEST_CASE("Transpiler sample list stays in sync with samples directory", "[transpiler][samples]") {
  std::set<std::string> expected;
  for (const auto name : kExpectedSamples) {
    expected.insert(std::string(name));
  }

  std::set<std::string> discovered;
  for (const auto& entry : std::filesystem::directory_iterator(samples_dir_path())) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const auto path = entry.path();
    if (path.extension() == ".fleaux") {
      discovered.insert(path.filename().string());
    }
  }

  REQUIRE(discovered == expected);
}

#define FLEAUX_SAMPLE_TEST(sample_file_literal)                                        \
  TEST_CASE("Transpiler sample: " sample_file_literal, "[transpiler][samples]") {    \
    transpile_sample_and_assert(sample_file_literal);                                   \
  }

FLEAUX_SAMPLE_TEST("01_hello_world.fleaux")
FLEAUX_SAMPLE_TEST("02_arithmetic.fleaux")
FLEAUX_SAMPLE_TEST("03_pipeline_chaining.fleaux")
FLEAUX_SAMPLE_TEST("04_function_definitions.fleaux")
FLEAUX_SAMPLE_TEST("05_select.fleaux")
FLEAUX_SAMPLE_TEST("06_branch.fleaux")
FLEAUX_SAMPLE_TEST("07_apply.fleaux")
FLEAUX_SAMPLE_TEST("08_loop.fleaux")
FLEAUX_SAMPLE_TEST("09_loop_n.fleaux")
FLEAUX_SAMPLE_TEST("10_strings.fleaux")
FLEAUX_SAMPLE_TEST("11_tuples.fleaux")
FLEAUX_SAMPLE_TEST("12_math.fleaux")
FLEAUX_SAMPLE_TEST("13_comparison_and_logic.fleaux")
FLEAUX_SAMPLE_TEST("14_os.fleaux")
FLEAUX_SAMPLE_TEST("15_path.fleaux")
FLEAUX_SAMPLE_TEST("16_file_and_dir.fleaux")
FLEAUX_SAMPLE_TEST("17_printf_and_tostring.fleaux")
FLEAUX_SAMPLE_TEST("18_constants.fleaux")
FLEAUX_SAMPLE_TEST("19_composition.fleaux")
FLEAUX_SAMPLE_TEST("20_export.fleaux")
FLEAUX_SAMPLE_TEST("21_import.fleaux")
FLEAUX_SAMPLE_TEST("22_file_streaming.fleaux")
FLEAUX_SAMPLE_TEST("23_binary_search.fleaux")
FLEAUX_SAMPLE_TEST("24_dicts.fleaux")
FLEAUX_SAMPLE_TEST("25_fleaux_parser.fleaux")
FLEAUX_SAMPLE_TEST("26_format_specifiers.fleaux")
FLEAUX_SAMPLE_TEST("27_error_handling_branching.fleaux")
FLEAUX_SAMPLE_TEST("28_variadics.fleaux")
FLEAUX_SAMPLE_TEST("29_inline_closures.fleaux")
FLEAUX_SAMPLE_TEST("30_pattern_matching.fleaux")
FLEAUX_SAMPLE_TEST("31_result_ok_err.fleaux")
FLEAUX_SAMPLE_TEST("32_try_empty_tuple.fleaux")
FLEAUX_SAMPLE_TEST("33_exp_parallel.fleaux")

#undef FLEAUX_SAMPLE_TEST






