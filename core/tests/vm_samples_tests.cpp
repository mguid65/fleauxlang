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
#include "fleaux/runtime/value.hpp"
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
  if (module_name == "Std" || module_name == "StdBuiltins") {
    return {};
  }

  if (const auto local = current.parent_path() / (module_name + ".fleaux");
      std::filesystem::exists(local)) {
    return std::filesystem::weakly_canonical(local);
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

std::vector<std::string> sample_runtime_args(const std::string_view sample_file,
                                             const std::filesystem::path& sample_path) {
  if (sample_file == "25_fleaux_parser.fleaux") {
    // Parser sample expects a source-file argument; parse itself.
    return {sample_path.string()};
  }
  return {};
}

void set_runtime_process_args(const std::filesystem::path& sample_path,
                              const std::vector<std::string>& runtime_args) {
  std::vector<std::string> args_storage;
  args_storage.reserve(runtime_args.size() + 1U);
  args_storage.push_back(sample_path.string());
  args_storage.insert(args_storage.end(), runtime_args.begin(), runtime_args.end());

  std::vector<char*> argv_ptrs;
  argv_ptrs.reserve(args_storage.size());
  for (auto& arg : args_storage) {
    argv_ptrs.push_back(arg.data());
  }
  fleaux::runtime::set_process_args(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());
}

void run_sample_in_vm_and_assert(const std::string_view sample_file) {
  const auto sample_path = samples_dir_path() / std::filesystem::path(sample_file);
  REQUIRE(std::filesystem::exists(sample_path));
  const auto runtime_args = sample_runtime_args(sample_file, sample_path);

  constexpr fleaux::vm::Interpreter interpreter;
  const auto result = interpreter.run_file(sample_path, runtime_args);
  INFO("sample file: " << sample_path);
  if (!result.has_value()) {
    INFO("vm error: " << result.error().message);
  }
  REQUIRE(result.has_value());
}

void run_sample_in_bytecode_and_assert(const std::string_view sample_file) {
  const auto sample_path = samples_dir_path() / std::filesystem::path(sample_file);
  REQUIRE(std::filesystem::exists(sample_path));
  const auto runtime_args = sample_runtime_args(sample_file, sample_path);

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
  set_runtime_process_args(sample_path, runtime_args);
  const auto runtime_result = runtime.execute(compiled_module.value());
  if (!runtime_result) {
    INFO("vm runtime error: " << runtime_result.error().message);
  }
  REQUIRE(runtime_result.has_value());
}

void run_sample_parity_and_assert(const std::string_view sample_file) {
  const auto sample_path = samples_dir_path() / std::filesystem::path(sample_file);
  REQUIRE(std::filesystem::exists(sample_path));
  const auto runtime_args = sample_runtime_args(sample_file, sample_path);

  constexpr fleaux::vm::Interpreter interpreter;
  const auto interp_result = interpreter.run_file(sample_path, runtime_args);

  std::optional<std::string> bytecode_error;
  bool bytecode_ok = false;
  if (const auto lowered = collect_and_lower(sample_path); lowered) {
    constexpr fleaux::bytecode::BytecodeCompiler compiler;
    if (const auto compiled_module = compiler.compile(lowered.value()); compiled_module) {
      const fleaux::vm::Runtime runtime;
      set_runtime_process_args(sample_path, runtime_args);
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

constexpr std::array<std::string_view, 30> kExpectedSamples = {
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

TEST_CASE("Qualified Std symbols are not callable unqualified", "[vm][samples]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_qualified_std";
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "qualified_std_required.fleaux";

  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "(1, 2) -> Add -> Std.Println;\n";
  }

  constexpr fleaux::vm::Interpreter interpreter;
  const auto interpreter_result = interpreter.run_file(source_path);
  REQUIRE_FALSE(interpreter_result.has_value());

  const auto lowered = collect_and_lower(source_path);
  REQUIRE(lowered.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(lowered.value());
  REQUIRE_FALSE(compiled_module.has_value());
}

TEST_CASE("Std import is symbolic and ignores local Std.fleaux", "[vm][samples]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_symbolic_std";
  std::filesystem::create_directories(temp_dir);
  const auto std_path = temp_dir / "Std.fleaux";
  const auto source_path = temp_dir / "symbolic_std_import.fleaux";

  {
    std::ofstream out(std_path);
    out << "this is invalid fleaux and should never be parsed\n";
  }

  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "(1, 2) -> Std.Add -> Std.Println;\n";
  }

  constexpr fleaux::vm::Interpreter interpreter;
  const auto interpreter_result = interpreter.run_file(source_path);
  REQUIRE(interpreter_result.has_value());

  const auto lowered = collect_and_lower(source_path);
  REQUIRE(lowered.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(lowered.value());
  REQUIRE(compiled_module.has_value());

  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(compiled_module.value());
  if (!runtime_result.has_value()) {
    INFO("bytecode runtime error: " << runtime_result.error().message);
  }
  REQUIRE(runtime_result.has_value());
}

TEST_CASE("User variadic tail captures remaining args", "[vm][samples][variadic]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_variadic_tail";
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "variadic_tail_ok.fleaux";

  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "let Collect(rest: Any...): Any = rest;\n"
           "let HeadTail(head: Number, rest: Any...): Any = rest;\n"
           "(1) -> Collect -> Std.Length -> Std.Println;\n"
           "((10, 20, 30)) -> HeadTail -> Std.Length -> Std.Println;\n";
  }

  constexpr fleaux::vm::Interpreter interpreter;
  const auto interpreter_result = interpreter.run_file(source_path);
  REQUIRE(interpreter_result.has_value());

  const auto lowered = collect_and_lower(source_path);
  REQUIRE(lowered.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(lowered.value());
  REQUIRE(compiled_module.has_value());

  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(compiled_module.value());
  if (!runtime_result.has_value()) {
    INFO("bytecode runtime error: " << runtime_result.error().message);
  }
  REQUIRE(runtime_result.has_value());
}

TEST_CASE("User variadic tail enforces minimum fixed args", "[vm][samples][variadic]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_variadic_tail";
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "variadic_tail_too_few.fleaux";

  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "let HeadTail(head: Number, rest: Any...): Any = rest;\n"
           "() -> HeadTail -> Std.Println;\n";
  }

  constexpr fleaux::vm::Interpreter interpreter;
  const auto interpreter_result = interpreter.run_file(source_path);
  REQUIRE_FALSE(interpreter_result.has_value());

  const auto lowered = collect_and_lower(source_path);
  REQUIRE(lowered.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(lowered.value());
  REQUIRE(compiled_module.has_value());

  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(compiled_module.value());
  REQUIRE_FALSE(runtime_result.has_value());
}

TEST_CASE("Inline closures execute in both interpreter and bytecode modes", "[vm][samples][closure]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_inline_closure";
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "inline_closure_ok.fleaux";

  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "(10, (x: Number): Number = (x, 1) -> Std.Add) -> Std.Apply -> Std.Println;\n"
           "(10) -> (x: Number): Number = (x, 1) -> Std.Add -> Std.Println;\n"
           "let MakeAdder(n: Number): Any = (x: Number): Number = (x, n) -> Std.Add;\n"
           "(10, (4) -> MakeAdder) -> Std.Apply -> Std.Println;\n";
  }

  constexpr fleaux::vm::Interpreter interpreter;
  const auto interpreter_result = interpreter.run_file(source_path);
  REQUIRE(interpreter_result.has_value());

  const auto lowered = collect_and_lower(source_path);
  REQUIRE(lowered.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(lowered.value());
  REQUIRE(compiled_module.has_value());

  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(compiled_module.value());
  REQUIRE(runtime_result.has_value());
}

TEST_CASE("Std.Match executes ordered pattern closures in interpreter and bytecode modes", "[vm][samples][match]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_match";
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "match_ok.fleaux";

  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "let IsEven(n: Number): Bool = ((n, 2) -> Std.Mod, 0) -> Std.Equal;\n"
           "(0, (0, (): Any = \"zero\"), (1, (): Any = \"one\"), (_, (): Any = \"many\")) -> Std.Match -> Std.Println;\n"
           "(3, (0, (): Any = \"zero\"), (1, (): Any = \"one\"), (_, (): Any = \"many\")) -> Std.Match -> Std.Println;\n"
           "(8, (IsEven, (): Any = \"even\"), (_, (): Any = \"odd\")) -> Std.Match -> Std.Println;\n";
  }

  constexpr fleaux::vm::Interpreter interpreter;
  const auto interpreter_result = interpreter.run_file(source_path);
  REQUIRE(interpreter_result.has_value());

  const auto lowered = collect_and_lower(source_path);
  REQUIRE(lowered.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(lowered.value());
  REQUIRE(compiled_module.has_value());

  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(compiled_module.value());
  REQUIRE(runtime_result.has_value());
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
FLEAUX_VM_SAMPLE_TEST("25_fleaux_parser.fleaux")
FLEAUX_VM_SAMPLE_TEST("26_format_specifiers.fleaux")
FLEAUX_VM_SAMPLE_TEST("27_error_handling_branching.fleaux")
FLEAUX_VM_SAMPLE_TEST("28_variadics.fleaux")
FLEAUX_VM_SAMPLE_TEST("29_inline_closures.fleaux")
FLEAUX_VM_SAMPLE_TEST("30_pattern_matching.fleaux")

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
FLEAUX_VM_BYTECODE_SAMPLE_TEST("25_fleaux_parser.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("26_format_specifiers.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("27_error_handling_branching.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("28_variadics.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("29_inline_closures.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("30_pattern_matching.fleaux")

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
FLEAUX_VM_PARITY_SAMPLE_TEST("25_fleaux_parser.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("26_format_specifiers.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("27_error_handling_branching.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("28_variadics.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("29_inline_closures.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("30_pattern_matching.fleaux")

#undef FLEAUX_VM_SAMPLE_TEST
#undef FLEAUX_VM_BYTECODE_SAMPLE_TEST
#undef FLEAUX_VM_PARITY_SAMPLE_TEST
