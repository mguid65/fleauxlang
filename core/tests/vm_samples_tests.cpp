#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include <tl/expected.hpp>

#include "fleaux/bytecode/compiler.hpp"
#include "fleaux/frontend/lowering.hpp"
#include "fleaux/frontend/parser.hpp"
#include "fleaux/vm/interpreter.hpp"
#include "fleaux/vm/runtime.hpp"

#ifndef FLEAUX_REPO_ROOT
#error "FLEAUX_REPO_ROOT must be defined by CMake for sample tests."
#endif

namespace {

using IRProgram = fleaux::frontend::ir::IRProgram;

std::filesystem::path repo_root_path() {
  return std::filesystem::path(FLEAUX_REPO_ROOT);
}

std::filesystem::path samples_dir_path() {
  return repo_root_path() / "samples";
}

tl::expected<IRProgram, std::string> parse_and_lower_single(const std::filesystem::path& source_file) {
  std::ifstream in(source_file);
  if (!in) {
    return tl::unexpected("Failed to read source file.");
  }
  const std::string source((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(source, source_file.string());
  if (!parsed) {
    return tl::unexpected(parsed.error().message);
  }

  constexpr fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  if (!lowered) {
    return tl::unexpected(lowered.error().message);
  }

  return lowered.value();
}

std::filesystem::path resolve_import_path(const std::filesystem::path& current,
                                          const std::string& module_name) {
  if (module_name == "StdBuiltins") {
    return {};
  }

  if (const auto local = current.parent_path() / (module_name + ".fleaux");
      std::filesystem::exists(local)) {
    return std::filesystem::weakly_canonical(local);
  }

  if (module_name == "Std") {
    if (const auto workspace_std = current.parent_path().parent_path() / "Std.fleaux";
        std::filesystem::exists(workspace_std)) {
      return std::filesystem::weakly_canonical(workspace_std);
    }
  }

  return {};
}

std::string let_key(const std::optional<std::string>& qualifier, const std::string& name) {
  return qualifier.has_value() ? (*qualifier + "." + name) : name;
}

tl::expected<IRProgram, std::string> collect_ir_program(
    const std::filesystem::path& source_file,
    std::unordered_map<std::string, IRProgram>& cache,
    std::unordered_set<std::string>& in_progress) {
  const auto canonical = std::filesystem::weakly_canonical(source_file).string();
  if (cache.contains(canonical)) {
    return cache.at(canonical);
  }

  if (in_progress.contains(canonical)) {
    return tl::unexpected("Cyclic import detected.");
  }

  in_progress.insert(canonical);

  auto current = parse_and_lower_single(source_file);
  if (!current) {
    in_progress.erase(canonical);
    return tl::unexpected(current.error());
  }

  IRProgram merged = current.value();
  std::unordered_set<std::string> seen;
  for (const auto& let : merged.lets) {
    seen.insert(let_key(let.qualifier, let.name));
  }

  std::vector<fleaux::frontend::ir::IRLet> imported_lets;
  std::vector<fleaux::frontend::ir::IRExprStatement> imported_exprs;

  for (const auto& [module_name, _span] : current->imports) {
    const auto import_path = resolve_import_path(source_file, module_name);
    if (import_path.empty()) {
      continue;
    }

    auto imported = collect_ir_program(import_path, cache, in_progress);
    if (!imported) {
      in_progress.erase(canonical);
      return tl::unexpected(imported.error());
    }

    for (const auto& imported_let : imported->lets) {
      if (const auto symbol = let_key(imported_let.qualifier, imported_let.name);
          seen.insert(symbol).second) {
        imported_lets.push_back(imported_let);
      }
    }
    imported_exprs.insert(imported_exprs.end(), imported->expressions.begin(), imported->expressions.end());
  }

  merged.lets.insert(merged.lets.begin(), imported_lets.begin(), imported_lets.end());
  merged.expressions.insert(merged.expressions.begin(), imported_exprs.begin(), imported_exprs.end());

  cache[canonical] = merged;
  in_progress.erase(canonical);
  return merged;
}

tl::expected<IRProgram, std::string> collect_and_lower(const std::filesystem::path& source_file) {
  std::unordered_map<std::string, IRProgram> cache;
  std::unordered_set<std::string> in_progress;
  return collect_ir_program(source_file, cache, in_progress);
}

void run_sample_in_vm_and_assert(const std::string_view sample_file) {
  const auto sample_path = samples_dir_path() / std::filesystem::path(sample_file);
  REQUIRE(std::filesystem::exists(sample_path));

  constexpr fleaux::vm::Interpreter interpreter;
  const auto result = interpreter.run_file(sample_path);
  INFO("sample file: " << sample_path);
  if (!result.has_value()) {
    INFO("vm error: " << result.error().message);
  }
  REQUIRE(result.has_value());
}

void run_sample_in_bytecode_and_assert(const std::string_view sample_file) {
  const auto sample_path = samples_dir_path() / std::filesystem::path(sample_file);
  REQUIRE(std::filesystem::exists(sample_path));

  const auto lowered = collect_and_lower(sample_path);
  INFO("sample file: " << sample_path);
  if (!lowered) {
    INFO("lowering error: " << lowered.error());
  }
  REQUIRE(lowered.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(lowered.value());
  if (!compiled_module) {
    INFO("bytecode compile error: " << compiled_module.error().message);
  }
  REQUIRE(compiled_module.has_value());

  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(compiled_module.value());
  if (!runtime_result) {
    INFO("vm runtime error: " << runtime_result.error().message);
  }
  REQUIRE(runtime_result.has_value());
}

void run_sample_parity_and_assert(const std::string_view sample_file) {
  const auto sample_path = samples_dir_path() / std::filesystem::path(sample_file);
  REQUIRE(std::filesystem::exists(sample_path));

  constexpr fleaux::vm::Interpreter interpreter;
  const auto interp_result = interpreter.run_file(sample_path);

  std::optional<std::string> bytecode_error;
  bool bytecode_ok = false;
  if (const auto lowered = collect_and_lower(sample_path); lowered) {
    constexpr fleaux::bytecode::BytecodeCompiler compiler;
    if (const auto compiled_module = compiler.compile(lowered.value()); compiled_module) {
      const fleaux::vm::Runtime runtime;
      const auto runtime_result = runtime.execute(compiled_module.value());
      bytecode_ok = runtime_result.has_value();
      if (!runtime_result) {
        bytecode_error = runtime_result.error().message;
      }
    } else {
      bytecode_error = compiled_module.error().message;
    }
  } else {
    bytecode_error = lowered.error();
  }

  INFO("sample file: " << sample_path);
  INFO("interpreter success: " << interp_result.has_value());
  INFO("bytecode success: " << bytecode_ok);
  if (!interp_result.has_value()) {
    INFO("interpreter error: " << interp_result.error().message);
  }
  if (bytecode_error.has_value()) {
    INFO("bytecode error: " << *bytecode_error);
  }

  REQUIRE(interp_result.has_value() == bytecode_ok);
}

constexpr std::array<std::string_view, 24> kExpectedSamples = {
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
};

}  // namespace

TEST_CASE("VM sample list stays in sync with samples directory", "[vm][samples]") {
  std::set<std::string> expected;
  for (const auto name : kExpectedSamples) {
    expected.insert(std::string(name));
  }

  std::set<std::string> discovered;
  for (const auto& entry : std::filesystem::directory_iterator(samples_dir_path())) {
    if (!entry.is_regular_file()) {
      continue;
    }
    if (const auto& path = entry.path(); path.extension() == ".fleaux") {
      discovered.insert(path.filename().string());
    }
  }

  REQUIRE(discovered == expected);
}

#define FLEAUX_VM_SAMPLE_TEST(sample_file_literal)                                      \
  TEST_CASE("VM sample: " sample_file_literal, "[vm][samples]") {                      \
    run_sample_in_vm_and_assert(sample_file_literal);                                   \
  }

#define FLEAUX_VM_BYTECODE_SAMPLE_TEST(sample_file_literal)                              \
  TEST_CASE("Bytecode VM sample: " sample_file_literal, "[vm][samples][bytecode]") {   \
    run_sample_in_bytecode_and_assert(sample_file_literal);                              \
  }

#define FLEAUX_VM_PARITY_SAMPLE_TEST(sample_file_literal)                                  \
  TEST_CASE("VM parity sample: " sample_file_literal, "[vm][samples][parity][fast]") {  \
    run_sample_parity_and_assert(sample_file_literal);                                     \
  }

FLEAUX_VM_SAMPLE_TEST("01_hello_world.fleaux")
FLEAUX_VM_SAMPLE_TEST("02_arithmetic.fleaux")
FLEAUX_VM_SAMPLE_TEST("03_pipeline_chaining.fleaux")
FLEAUX_VM_SAMPLE_TEST("04_function_definitions.fleaux")
FLEAUX_VM_SAMPLE_TEST("05_select.fleaux")
FLEAUX_VM_SAMPLE_TEST("06_branch.fleaux")
FLEAUX_VM_SAMPLE_TEST("07_apply.fleaux")
FLEAUX_VM_SAMPLE_TEST("08_loop.fleaux")
FLEAUX_VM_SAMPLE_TEST("09_loop_n.fleaux")
FLEAUX_VM_SAMPLE_TEST("10_strings.fleaux")
FLEAUX_VM_SAMPLE_TEST("11_tuples.fleaux")
FLEAUX_VM_SAMPLE_TEST("12_math.fleaux")
FLEAUX_VM_SAMPLE_TEST("13_comparison_and_logic.fleaux")
FLEAUX_VM_SAMPLE_TEST("14_os.fleaux")
FLEAUX_VM_SAMPLE_TEST("15_path.fleaux")
FLEAUX_VM_SAMPLE_TEST("16_file_and_dir.fleaux")
FLEAUX_VM_SAMPLE_TEST("17_printf_and_tostring.fleaux")
FLEAUX_VM_SAMPLE_TEST("18_constants.fleaux")
FLEAUX_VM_SAMPLE_TEST("19_composition.fleaux")
FLEAUX_VM_SAMPLE_TEST("20_export.fleaux")
FLEAUX_VM_SAMPLE_TEST("21_import.fleaux")
FLEAUX_VM_SAMPLE_TEST("22_file_streaming.fleaux")
FLEAUX_VM_SAMPLE_TEST("23_binary_search.fleaux")
FLEAUX_VM_SAMPLE_TEST("24_dicts.fleaux")

FLEAUX_VM_BYTECODE_SAMPLE_TEST("01_hello_world.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("02_arithmetic.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("03_pipeline_chaining.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("04_function_definitions.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("05_select.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("06_branch.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("07_apply.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("08_loop.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("09_loop_n.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("10_strings.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("11_tuples.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("12_math.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("13_comparison_and_logic.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("14_os.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("15_path.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("16_file_and_dir.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("17_printf_and_tostring.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("18_constants.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("19_composition.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("20_export.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("21_import.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("22_file_streaming.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("23_binary_search.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("24_dicts.fleaux")

FLEAUX_VM_PARITY_SAMPLE_TEST("01_hello_world.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("02_arithmetic.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("03_pipeline_chaining.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("04_function_definitions.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("05_select.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("06_branch.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("07_apply.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("08_loop.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("09_loop_n.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("10_strings.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("11_tuples.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("12_math.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("13_comparison_and_logic.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("14_os.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("15_path.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("16_file_and_dir.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("17_printf_and_tostring.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("18_constants.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("19_composition.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("20_export.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("21_import.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("22_file_streaming.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("23_binary_search.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("24_dicts.fleaux")

#undef FLEAUX_VM_SAMPLE_TEST
#undef FLEAUX_VM_BYTECODE_SAMPLE_TEST
#undef FLEAUX_VM_PARITY_SAMPLE_TEST
