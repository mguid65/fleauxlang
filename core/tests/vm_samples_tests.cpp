#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <tl/expected.hpp>

#include "fleaux/bytecode/compiler.hpp"
#include "fleaux/bytecode/module_loader.hpp"
#include "fleaux/frontend/source_loader.hpp"
#include "fleaux/runtime/runtime_support.hpp"
#include "fleaux/vm/runtime.hpp"
#include "vm_test_support.hpp"

#ifndef FLEAUX_REPO_ROOT
#error "FLEAUX_REPO_ROOT must be defined by CMake for sample tests."
#endif

namespace {

using IRProgram = fleaux::frontend::ir::IRProgram;

std::filesystem::path repo_root_path() { return std::filesystem::path(FLEAUX_REPO_ROOT); }

std::filesystem::path samples_dir_path() { return repo_root_path() / "samples"; }

auto make_load_error(const std::string& message, const std::optional<std::string>& hint,
                     const std::optional<fleaux::frontend::diag::SourceSpan>&) -> std::string {
  return hint.has_value() ? message + " (" + *hint + ")" : message;
}

tl::expected<IRProgram, std::string> load_ir_program(const std::filesystem::path& source_file) {
  return fleaux::frontend::source_loader::load_ir_program<std::string>(
      source_file, make_load_error, "Cyclic import detected.",
      std::optional<std::string>{"Break the cycle by moving shared definitions into a third module."});
}

std::vector<std::string> sample_runtime_args(const std::string_view sample_file,
                                             const std::filesystem::path& sample_path) {
  if (sample_file == "25_fleaux_parser.fleaux") {
    // Parser sample expects a source-file argument; parse itself.
    return {sample_path.string()};
  }
  return {};
}

void set_runtime_process_args(const std::filesystem::path& sample_path, const std::vector<std::string>& runtime_args) {
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

  const auto loaded_module = fleaux::bytecode::load_linked_module(sample_path);
  INFO("sample file: " << sample_path);
  if (!loaded_module) {
    INFO("vm load error: " << loaded_module.error().message);
  }
  REQUIRE(loaded_module.has_value());

  const fleaux::vm::Runtime runtime;
  set_runtime_process_args(sample_path, runtime_args);
  const auto runtime_result = runtime.execute(*loaded_module);
  if (!runtime_result) {
    INFO("vm runtime error: " << runtime_result.error().message);
  }
  REQUIRE(runtime_result.has_value());
}

void run_sample_in_bytecode_and_assert(const std::string_view sample_file) {
  const auto sample_path = samples_dir_path() / std::filesystem::path(sample_file);
  REQUIRE(std::filesystem::exists(sample_path));
  const auto runtime_args = sample_runtime_args(sample_file, sample_path);

  const auto analyzed = load_ir_program(sample_path);
  INFO("sample file: " << sample_path);
  if (!analyzed) {
    INFO("analysis error: " << analyzed.error());
  }
  REQUIRE(analyzed.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(analyzed.value());
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

constexpr std::array<std::string_view, 53> kExpectedSamples = {
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
    "34_help.fleaux",
    "35_concurrency_tasks.fleaux",
    "36_parallel_options_and_empty_inputs.fleaux",
    "37_parallel_error_paths.fleaux",
    "38_parallel_inline_closures.fleaux",
    "39_if_else_match.fleaux",
    "40_recursion.fleaux",
    "41_function_factory.fleaux",
    "42_dict_dispatch.fleaux",
    "43_accumulator_loops.fleaux",
    "44_tuple_map_filter_reduce.fleaux",
    "45_result_recovery.fleaux",
    "46_select_vs_branch_match.fleaux",
    "47_tag_dispatch_vs_callable_dispatch.fleaux",
    "48_repeat_value_tuple.fleaux",
    "49_same_input_fanout.fleaux",
    "50_multi_accumulator_loop_state.fleaux",
    "51_strong_type_casts.fleaux",
    "52_variables_and_blocks.fleaux",
    "53_random_seeded.fleaux",
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

  const auto analyzed = load_ir_program(source_path);
  REQUIRE_FALSE(analyzed.has_value());
}

TEST_CASE("Qualified Std symbols require an explicit Std import", "[vm][samples][imports]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_std_import_required";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "missing_std_import.fleaux";

  {
    std::ofstream out(source_path);
    out << "(1, 2) -> Std.Add -> Std.Println;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  REQUIRE_FALSE(analyzed.has_value());
  REQUIRE(analyzed.error().find("Unresolved symbol") != std::string::npos);
  REQUIRE((analyzed.error().find("Std.Add") != std::string::npos ||
           analyzed.error().find("Std.Println") != std::string::npos));

  const auto vm_load_result = fleaux::bytecode::load_linked_module(source_path);
  REQUIRE_FALSE(vm_load_result.has_value());
  REQUIRE(vm_load_result.error().message.find("Unresolved symbol") != std::string::npos);
  REQUIRE((vm_load_result.error().message.find("Std.Add") != std::string::npos ||
           vm_load_result.error().message.find("Std.Println") != std::string::npos));
}

TEST_CASE("Std.Help loads canonical Std metadata in VM mode without prior help registry state",
          "[vm][help][contract]") {
  const auto sample_path = samples_dir_path() / "34_help.fleaux";
  REQUIRE(std::filesystem::exists(sample_path));

  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_help_embedded_std_only";
  std::filesystem::remove_all(temp_dir);
  const auto poisoned_fallbacks = fleaux::tests::write_poisoned_symbolic_std_fallbacks(temp_dir);

  const fleaux::tests::CurrentPathScope current_path_scope(temp_dir);
  const fleaux::tests::ScopedEnvVar env_scope("FLEAUX_STD_PATH", poisoned_fallbacks.env_std_path.string());

  const auto analyzed = load_ir_program(sample_path);
  REQUIRE(analyzed.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(analyzed.value());
  REQUIRE(compiled_module.has_value());

  fleaux::runtime::clear_help_metadata_registry();

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  set_runtime_process_args(sample_path, {});
  const auto runtime_result = runtime.execute(compiled_module.value(), output);
  if (!runtime_result) {
    INFO("vm runtime error: " << runtime_result.error().message);
  }
  REQUIRE(runtime_result.has_value());

  REQUIRE_THAT(output.str(), Catch::Matchers::ContainsSubstring("Help on function Std.Add"));
  REQUIRE_THAT(output.str(), Catch::Matchers::ContainsSubstring(
                                 "let Std.Add(lhs: Float64 | Int64 | UInt64, rhs: Float64 | Int64 | UInt64): "
                                 "Float64 | Int64 | UInt64 :: __builtin__"));
}

TEST_CASE("VM loader reports unresolved imports", "[vm][imports][contract]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_import_unresolved_contract";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto source_path = temp_dir / "entry.fleaux";
  {
    std::ofstream out(source_path);
    out << "import missing_dep;\n"
           "let Main(x: Float64): Float64 = x;\n";
  }

  const auto vm_load_result = fleaux::bytecode::load_linked_module(source_path);
  REQUIRE_FALSE(vm_load_result.has_value());
  REQUIRE(vm_load_result.error().message.find("import-unresolved:") != std::string::npos);
  REQUIRE(vm_load_result.error().message.find("Import not found: 'missing_dep'") != std::string::npos);
}

TEST_CASE("VM loader reports import cycles", "[vm][imports][contract]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_import_cycle_contract";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto module_a = temp_dir / "a.fleaux";
  const auto module_b = temp_dir / "b.fleaux";
  {
    std::ofstream out(module_a);
    out << "import b;\n"
           "let A(x: Float64): Float64 = x;\n";
  }
  {
    std::ofstream out(module_b);
    out << "import a;\n"
           "let B(x: Float64): Float64 = x;\n";
  }

  const auto vm_load_result = fleaux::bytecode::load_linked_module(module_a);
  REQUIRE_FALSE(vm_load_result.has_value());
  REQUIRE(vm_load_result.error().message.find("import-cycle:") != std::string::npos);
}

TEST_CASE("VM loader reports imported type mismatches", "[vm][imports][contract][types]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_import_type_mismatch_contract";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto dependency_path = temp_dir / "typed_dep.fleaux";
  const auto entry_path = temp_dir / "typed_entry.fleaux";
  {
    std::ofstream out(dependency_path);
    out << "let Add4(x: String): Int64 = 4;\n";
  }
  {
    std::ofstream out(entry_path);
    out << "import Std;\n"
           "import typed_dep;\n"
           "(1) -> Add4 -> Std.Println;\n";
  }

  const auto vm_load_result = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE_FALSE(vm_load_result.has_value());
  REQUIRE(vm_load_result.error().message.find("Type mismatch in call target arguments") != std::string::npos);
  REQUIRE(vm_load_result.error().hint.value().find("Add4 expects argument 0") != std::string::npos);
}

TEST_CASE("VM loader enforces direct import visibility", "[vm][imports][contract][types]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_import_direct_visibility_contract";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto module_c = temp_dir / "c.fleaux";
  const auto module_b = temp_dir / "b.fleaux";
  const auto module_a = temp_dir / "a.fleaux";

  {
    std::ofstream out(module_c);
    out << "import Std;\n"
           "let CFn(x: Float64): Float64 = (x, 1.0) -> Std.Add;\n";
  }
  {
    std::ofstream out(module_b);
    out << "import c;\n"
           "let BFn(x: Float64): Float64 = (x) -> CFn;\n";
  }
  {
    std::ofstream out(module_a);
    out << "import Std;\n"
           "import b;\n"
           "(1.0) -> CFn -> Std.Println;\n";
  }

  const auto vm_load_result = fleaux::bytecode::load_linked_module(module_a);
  REQUIRE_FALSE(vm_load_result.has_value());
  REQUIRE(vm_load_result.error().message.find("Unresolved symbol") != std::string::npos);
  REQUIRE(vm_load_result.error().message.find("CFn") != std::string::npos);
}

TEST_CASE("VM loader reports unresolved qualified exports", "[vm][imports][contract][types]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_import_qualified_unresolved_contract";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto dependency_path = temp_dir / "typed_dep_qualified.fleaux";
  const auto entry_path = temp_dir / "typed_entry_qualified.fleaux";
  {
    std::ofstream out(dependency_path);
    out << "import Std;\n"
           "let MyMath.Add4(x: Float64): Float64 = (4.0, x) -> Std.Add;\n";
  }
  {
    std::ofstream out(entry_path);
    out << "import Std;\n"
           "import typed_dep_qualified;\n"
           "(1.0) -> WrongMath.Add4 -> Std.Println;\n";
  }

  const auto vm_load_result = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE_FALSE(vm_load_result.has_value());
  REQUIRE(vm_load_result.error().message.find("Unresolved symbol") != std::string::npos);
  REQUIRE(vm_load_result.error().message.find("WrongMath.Add4") != std::string::npos);
}

TEST_CASE("VM loader resolves qualified exported overload symbol keys", "[vm][imports][contract][types]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_import_qualified_symbol_key_contract";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto dependency_path = temp_dir / "typed_dep_overloads.fleaux";
  const auto entry_path = temp_dir / "typed_entry_overloads.fleaux";
  {
    std::ofstream out(dependency_path);
    out << "import Std;\n"
           "let MyMath.Echo(x: Int64): Int64 = (x, 1) -> Std.Add;\n"
           "let MyMath.Echo(x: String): String = x;\n";
  }
  {
    std::ofstream out(entry_path);
    out << "import Std;\n"
           "import typed_dep_overloads;\n"
           "(1) -> MyMath.Echo -> Std.Println;\n"
           "(\"ok\") -> MyMath.Echo -> Std.Println;\n";
  }

  const auto vm_load_result = fleaux::bytecode::load_linked_module(entry_path);
  if (!vm_load_result) {
    INFO("vm load error: " << vm_load_result.error().message);
  }
  REQUIRE(vm_load_result.has_value());

  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(*vm_load_result);
  if (!runtime_result) {
    INFO("runtime error: " << runtime_result.error().message);
  }
  REQUIRE(runtime_result.has_value());
}

TEST_CASE("VM Std.Printf returns the expected tuple shape", "[vm][samples][printf]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_printf_return_shape";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto source_path = temp_dir / "printf_return_shape.fleaux";
  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "(\"value:{}|\", 7) -> Std.Printf -> (_, 1u64) -> Std.ElementAt -> Std.Type -> Std.Println;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  REQUIRE(analyzed.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(*analyzed);
  REQUIRE(compiled_module.has_value());

  {
    std::ostringstream output;
    const fleaux::vm::Runtime runtime;
    const auto runtime_result = runtime.execute(*compiled_module, output);
    if (!runtime_result.has_value()) {
      INFO("bytecode error: " << runtime_result.error().message);
    }
    REQUIRE(runtime_result.has_value());
    REQUIRE(output.str() == "value:7|Int64\n");
  }
}

TEST_CASE("VM Std.Printf accepts a format string with no substitution arguments", "[vm][samples][printf]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_printf_single_argument";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto source_path = temp_dir / "printf_single_argument.fleaux";
  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "\"hello\" -> Std.Printf -> Std.Length -> Std.Println;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  REQUIRE(analyzed.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(*analyzed);
  REQUIRE(compiled_module.has_value());

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(*compiled_module, output);
  if (!runtime_result.has_value()) {
    INFO("bytecode error: " << runtime_result.error().message);
  }
  REQUIRE(runtime_result.has_value());
  REQUIRE(output.str() == "hello1\n");
}

TEST_CASE("VM Std.String.Format accepts a format string with no substitution arguments",
          "[vm][samples][string][format]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_string_format_single_argument";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto source_path = temp_dir / "string_format_single_argument.fleaux";
  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "\"hello\" -> Std.String.Format -> Std.Println;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  REQUIRE(analyzed.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(*analyzed);
  REQUIRE(compiled_module.has_value());

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(*compiled_module, output);
  if (!runtime_result.has_value()) {
    INFO("bytecode error: " << runtime_result.error().message);
  }
  REQUIRE(runtime_result.has_value());
  REQUIRE(output.str() == "hello\n");
}

TEST_CASE("VM Std.Path.Join accepts more than two path segments", "[vm][samples][path]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_path_join_variadic";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto source_path = temp_dir / "path_join_variadic.fleaux";
  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "(\"root\", \"nested\", \"file.txt\") -> Std.Path.Join -> Std.Println;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  REQUIRE(analyzed.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(*analyzed);
  REQUIRE(compiled_module.has_value());

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(*compiled_module, output);
  if (!runtime_result.has_value()) {
    INFO("bytecode error: " << runtime_result.error().message);
  }
  REQUIRE(runtime_result.has_value());
  REQUIRE(output.str() == (std::filesystem::path("root") / "nested" / "file.txt").string() + "\n");
}

TEST_CASE("VM deterministic Std.Random builtins support immutable generator threading", "[vm][samples][random]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_random_generator_sample";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto source_path = temp_dir / "random_generator.fleaux";
  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "let G0: Any = 123456789 -> Std.ToUInt64 -> Std.Random.Create;\n"
           "let Step1: Tuple(UInt64, Any) = G0 -> Std.Random.NextUInt64;\n"
           "let Value1: UInt64 = Step1 -> Std.First;\n"
           "let G1: Any = Step1 -> Std.Second;\n"
           "let Step2: Tuple(Float64, Any) = G1 -> Std.Random.NextFloat64;\n"
           "let SplitPair: Tuple(Any, Any) = G0 -> Std.Random.Split;\n"
           "let LeftStep: Tuple(UInt64, Any) = SplitPair -> Std.First -> Std.Random.NextUInt64;\n"
           "let RightStep: Tuple(UInt64, Any) = SplitPair -> Std.Second -> Std.Random.NextUInt64;\n"
           "Value1 -> Std.Println;\n"
           "Step2 -> Std.First -> Std.Println;\n"
           "G0 -> Std.Random.NextBool -> Std.First -> Std.Println;\n"
           "LeftStep -> Std.First -> Std.Println;\n"
           "RightStep -> Std.First -> Std.Println;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  REQUIRE(analyzed.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(*analyzed);
  REQUIRE(compiled_module.has_value());

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(*compiled_module, output);
  if (!runtime_result.has_value()) {
    INFO("bytecode error: " << runtime_result.error().message);
  }
  REQUIRE(runtime_result.has_value());
  REQUIRE(output.str() ==
          "2466975172287755897\n0.500049775767424\nTrue\n1278849761381179246\n15883818742529415426\n");
}

TEST_CASE("VM Std.Println returns the expected tuple shape", "[vm][samples][println]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_println_return_shape";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto source_path = temp_dir / "println_return_shape.fleaux";
  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "(\"x\") -> Std.Println -> Std.Type -> Std.Println;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  REQUIRE(analyzed.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(*analyzed);
  REQUIRE(compiled_module.has_value());

  {
    std::ostringstream output;
    const fleaux::vm::Runtime runtime;
    const auto runtime_result = runtime.execute(*compiled_module, output);
    if (!runtime_result.has_value()) {
      INFO("bytecode error: " << runtime_result.error().message);
    }
    REQUIRE(runtime_result.has_value());
    REQUIRE(output.str() == "x\nString\n");
  }
}

TEST_CASE("Nested closure dict capture churn stays stable in the VM", "[vm][samples][lifetime]") {
  fleaux::runtime::reset_callable_registry();
  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);

  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_closure_dict_churn";
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "closure_dict_churn.fleaux";

  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "let MakeLookup(d: Dict(String, Int64)): (String) => Int64 =\n"
           "    (k: String): Int64 = (d, k, 0) -> Std.Dict.GetDefault;\n"
           "let MakeDict(): Dict(String, Int64) = (() -> Std.Dict.Create, \"a\", 1) -> Std.Dict.Set;\n"
           "() -> MakeDict -> MakeLookup -> (\"a\", _) -> Std.Apply -> "
           "Std.Println;\n"
           "((1.0, 2.0, 3.0, 4.0), (x: Float64): Float64 = (x, 1.0) -> Std.Add) -> Std.Parallel.Map -> Std.Println;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  REQUIRE(analyzed.has_value());
  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(analyzed.value());
  REQUIRE(compiled_module.has_value());

  for (int iter = 0; iter < 40; ++iter) {
    const fleaux::vm::Runtime runtime;
    const auto runtime_result = runtime.execute(compiled_module.value());
    if (!runtime_result.has_value()) {
      INFO("vm churn error: " << runtime_result.error().message);
    }
    REQUIRE(runtime_result.has_value());
    REQUIRE(fleaux::runtime::callable_registry_size() == 0U);
  }
}

TEST_CASE("Task and Parallel samples keep VM registry stability", "[vm][samples][lifetime][concurrency]") {
  fleaux::runtime::reset_callable_registry();
  fleaux::runtime::reset_value_registry_for_tests();
  fleaux::runtime::reset_task_registry_for_tests();
  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);
  REQUIRE(fleaux::runtime::value_registry_telemetry().active_count == 0U);
  REQUIRE(fleaux::runtime::value_registry_telemetry().rejected_allocations == 0U);
  REQUIRE(fleaux::runtime::value_registry_telemetry().stale_deref_rejections == 0U);
  REQUIRE(fleaux::runtime::task_registry_size() == 0U);

  constexpr std::array<std::string_view, 4> kConcurrencySamples = {
      "35_concurrency_tasks.fleaux",
      "36_parallel_options_and_empty_inputs.fleaux",
      "37_parallel_error_paths.fleaux",
      "38_parallel_inline_closures.fleaux",
  };

  constexpr fleaux::bytecode::BytecodeCompiler compiler;

  for (const auto sample_file : kConcurrencySamples) {
    const auto sample_path = samples_dir_path() / std::filesystem::path(sample_file);
    REQUIRE(std::filesystem::exists(sample_path));
    const auto runtime_args = sample_runtime_args(sample_file, sample_path);

    const auto analyzed = load_ir_program(sample_path);
    REQUIRE(analyzed.has_value());

    const auto compiled_module = compiler.compile(analyzed.value());
    REQUIRE(compiled_module.has_value());

    for (int iter = 0; iter < 30; ++iter) {
      const fleaux::vm::Runtime runtime;
      set_runtime_process_args(sample_path, runtime_args);
      const auto runtime_result = runtime.execute(compiled_module.value());
      if (!runtime_result.has_value()) {
        INFO("vm concurrency sample error: " << runtime_result.error().message);
      }
      REQUIRE(runtime_result.has_value());
      REQUIRE(fleaux::runtime::callable_registry_size() == 0U);
      REQUIRE(fleaux::runtime::task_registry_size() == 0U);
      const auto vm_telemetry = fleaux::runtime::value_registry_telemetry();
      REQUIRE(vm_telemetry.active_count == 0U);
      REQUIRE(vm_telemetry.rejected_allocations == 0U);
      REQUIRE(vm_telemetry.stale_deref_rejections == 0U);
    }
  }
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

  const auto analyzed = load_ir_program(source_path);
  REQUIRE(analyzed.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(analyzed.value());
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
           "let Collect(rest: Any...): Tuple(Any...) = rest;\n"
           "let HeadTail(head: Float64, rest: Any...): Tuple(Any...) = rest;\n"
           "(1) -> Collect -> Std.Length -> Std.Println;\n"
           "((10.0, 20.0, 30.0)) -> HeadTail -> Std.Length -> Std.Println;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  REQUIRE(analyzed.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(analyzed.value());
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
           "let HeadTail(head: Float64, rest: Any...): Tuple(Any...) = rest;\n"
           "() -> HeadTail -> Std.Println;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  REQUIRE_FALSE(analyzed.has_value());
}

TEST_CASE("Imported user overloads dispatch correctly in the VM", "[vm][samples][overload]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_imported_user_overloads";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto dependency_path = temp_dir / "overloads.fleaux";
  const auto entry_path = temp_dir / "entry.fleaux";

  {
    std::ofstream out(dependency_path);
    out << "import Std;\n"
           "let Echo(x: Int64): Int64 = (x, 1) -> Std.Add;\n"
           "let Echo(x: String): String = x;\n";
  }

  {
    std::ofstream out(entry_path);
    out << "import Std;\n"
           "import overloads;\n"
           "(1) -> Echo -> Std.Println;\n"
           "(\"ok\") -> Echo -> Std.Println;\n";
  }

  const auto analyzed = load_ir_program(entry_path);
  if (!analyzed) {
    INFO("analysis error: " << analyzed.error());
  }
  REQUIRE(analyzed.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled = compiler.compile(*analyzed);
  if (!compiled) {
    INFO("bytecode compile error: " << compiled.error().message);
  }
  REQUIRE(compiled.has_value());

  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(*compiled);
  if (!runtime_result) {
    INFO("runtime error: " << runtime_result.error().message);
  }
  REQUIRE(runtime_result.has_value());
}

TEST_CASE("Std.Dict.Create clone overload executes in the VM", "[vm][samples][dict]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_dict_create_clone";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto source_path = temp_dir / "dict_create_clone.fleaux";

  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "let MakeDict(): Dict(String, Int64) = (() -> Std.Dict.Create, \"a\", 1) -> Std.Dict.Set;\n"
           "() -> MakeDict -> Std.Dict.Create -> (_, \"a\") -> Std.Dict.Get -> Std.Println;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  if (!analyzed) {
    INFO("analysis error: " << analyzed.error());
  }
  REQUIRE(analyzed.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled = compiler.compile(*analyzed);
  if (!compiled) {
    INFO("bytecode compile error: " << compiled.error().message);
  }
  REQUIRE(compiled.has_value());

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(*compiled, output);
  if (!runtime_result) {
    INFO("runtime error: " << runtime_result.error().message);
  }
  REQUIRE(runtime_result.has_value());
  REQUIRE(output.str() == "1\n");
}

TEST_CASE("Integer-only Std params reject Float64 during analysis", "[vm][samples]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_integer_only_params";
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "integer_only_params_reject_float.fleaux";

  {
    std::ofstream out(source_path);
    out << "let Std.Array.GetAt(array: Tuple(Any...), index: UInt64): Any :: __builtin__;\n"
           "let Std.Array.GetAtND(value: Any, indices: Tuple(UInt64...)): Any :: __builtin__;\n"
           "let Std.Array.ReshapeND(flat_array: Tuple(Any...), shape: Tuple(UInt64...)): Any :: __builtin__;\n"
           "let Std.Exit(): Never :: __builtin__;\n"
           "let Std.Exit(code: Int64): Never :: __builtin__;\n"
           "let UseFloatExit(code: Float64): Any = (code) -> Std.Exit;\n"
           "let Std.Println(args: Any...): Tuple(Any...) :: __builtin__;\n"
           "((1, 2, 3), 1.5) -> Std.Array.GetAt -> Std.Println;\n"
           "(((1, 2), (3, 4)), (1u64, 0.5)) -> Std.Array.GetAtND -> Std.Println;\n"
           "((1, 2, 3, 4), (2u64, 2.5)) -> Std.Array.ReshapeND -> Std.Println;\n"
           "(1) -> UseFloatExit -> Std.Println;\n"
           "(1.25) -> Std.Exit -> Std.Println;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  REQUIRE_FALSE(analyzed.has_value());
}

TEST_CASE("VM widened Std.Array.Slice integer params reject Float64 during analysis", "[vm][samples][array]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_array_slice_integer_contract";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "array_slice_integer_contract_reject_float.fleaux";

  {
    std::ofstream out(source_path);
    out << "let Std.Array.Slice<T>(array: Tuple(T...), start: Any, stop: Any): Tuple(T...) :: __builtin__;\n"
           "((1, 2, 3, 4), 0.5, 2) -> Std.Array.Slice;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  REQUIRE_FALSE(analyzed.has_value());
  REQUIRE(analyzed.error().find("expects Int64 or UInt64 for integer arguments") != std::string::npos);
}

TEST_CASE("VM widened core sequence integer params reject Float64 during analysis", "[vm][samples][sequence]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_sequence_integer_contract";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "sequence_integer_contract_reject_float.fleaux";

  {
    std::ofstream out(source_path);
    out << "let Std.Take<T>(tuple: Tuple(T...), count: Any): Tuple(T...) :: __builtin__;\n"
           "let Std.Drop<T>(tuple: Tuple(T...), count: Any): Tuple(T...) :: __builtin__;\n"
           "let Std.ElementAt<T>(tuple: Tuple(T...), count: Any): T :: __builtin__;\n"
           "let Std.Slice<T>(tuple: Tuple(T...), stop: Any): Tuple(T...) :: __builtin__;\n"
           "((1, 2, 3), 1.5) -> Std.Take;\n"
           "((1, 2, 3), 1.5) -> Std.Drop;\n"
           "((1, 2, 3), 1.5) -> Std.ElementAt;\n"
           "((1, 2, 3), 1.5) -> Std.Slice;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  REQUIRE_FALSE(analyzed.has_value());
  REQUIRE(analyzed.error().find("expects Int64 or UInt64 for integer arguments") != std::string::npos);
}

TEST_CASE("VM widened string and file integer params reject Float64 during analysis", "[vm][samples][string][io]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_string_file_integer_contract";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "string_file_integer_contract_reject_float.fleaux";

  {
    std::ofstream out(source_path);
    out << "type FileHandle = Any;\n"
           "let Std.String.CharAt(s: String, index: Any): String :: __builtin__;\n"
           "let Std.String.Slice(s: String, start: Any, stop: Any): String :: __builtin__;\n"
           "let Std.String.Find(s: String, sub: String, start: Any): Int64 :: __builtin__;\n"
           "let Std.File.ReadChunk(handle: FileHandle, nbytes: Any): Tuple(FileHandle, String, Bool) :: __builtin__;\n"
           "let Open(): FileHandle :: __builtin__;\n"
           "(\"abc\", 1.5) -> Std.String.CharAt;\n"
           "(\"abcdef\", 1.5, 4) -> Std.String.Slice;\n"
           "(\"abcabc\", \"bc\", 1.5) -> Std.String.Find;\n"
           "() -> Open -> (_, 1.5) -> Std.File.ReadChunk;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  REQUIRE_FALSE(analyzed.has_value());
  REQUIRE(analyzed.error().find("expects Int64 or UInt64 for integer arguments") != std::string::npos);
}

TEST_CASE("VM widened Random.Create seed rejects Float64 during analysis", "[vm][samples][random]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_random_seed_integer_contract";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "random_seed_integer_contract_reject_float.fleaux";

  {
    std::ofstream out(source_path);
    out << "type RandomGenerator = Any;\n"
           "let Std.Random.Create(seed: Any): RandomGenerator :: __builtin__;\n"
           "1.5 -> Std.Random.Create;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  REQUIRE_FALSE(analyzed.has_value());
  REQUIRE(analyzed.error().find("expects Int64 or UInt64 for integer arguments") != std::string::npos);
}

TEST_CASE("VM widened bit shift counts reject Float64 during analysis", "[vm][samples][bit]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_bit_shift_integer_contract";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "bit_shift_integer_contract_reject_float.fleaux";

  {
    std::ofstream out(source_path);
    out << "let Std.Bit.ShiftLeft(value: Int64, bits: Any): Int64 :: __builtin__;\n"
           "let Std.Bit.ShiftRight(value: Int64, bits: Any): Int64 :: __builtin__;\n"
           "(1, 1.5) -> Std.Bit.ShiftLeft;\n"
           "(8, 1.5) -> Std.Bit.ShiftRight;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  REQUIRE_FALSE(analyzed.has_value());
  REQUIRE(analyzed.error().find("expects Int64 or UInt64 for integer arguments") != std::string::npos);
}

TEST_CASE("VM widened Exit and Tuple.Range integer params reject Float64 during analysis",
          "[vm][samples][control][tuple]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_exit_range_integer_contract";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "exit_range_integer_contract_reject_float.fleaux";

  {
    std::ofstream out(source_path);
    out << "let Std.Exit(): Never :: __builtin__;\n"
           "let Std.Exit(code: Any): Never :: __builtin__;\n"
           "let Std.Tuple.Range(stop: Any): Tuple(Int64...) :: __builtin__;\n"
           "(1.25) -> Std.Exit;\n"
           "(1.5) -> Std.Tuple.Range;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  REQUIRE_FALSE(analyzed.has_value());
  REQUIRE(analyzed.error().find("expects Int64 or UInt64 for integer arguments") != std::string::npos);
}

TEST_CASE("VM Std.Array.Shape returns UInt64 dimensions", "[vm][samples][array]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_array_shape_uint64";
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "array_shape_uint64.fleaux";

  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "(((1, 2), (3, 4))) -> Std.Array.Shape -> (_, 0u64) -> Std.ElementAt -> Std.Type -> Std.Println;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  REQUIRE(analyzed.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(analyzed.value());
  REQUIRE(compiled_module.has_value());

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  set_runtime_process_args(source_path, {});
  const auto runtime_result = runtime.execute(compiled_module.value(), output);
  REQUIRE(runtime_result.has_value());
  REQUIRE(output.str() == "UInt64\n");
}

TEST_CASE("Inline closures execute in the VM", "[vm][samples][closure]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_inline_closure";
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "inline_closure_ok.fleaux";

  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "(10.0, (x: Float64): Float64 = (x, 1.0) -> Std.Add) -> Std.Apply -> Std.Println;\n"
           "(10.0) -> (x: Float64): Float64 = (x, 1.0) -> Std.Add -> Std.Println;\n"
           "let MakeAdder(n: Float64): (Float64) => Float64 = (x: Float64): Float64 = (x, n) -> Std.Add;\n"
           "(10.0, (4.0) -> MakeAdder) -> Std.Apply -> Std.Println;\n"
           "() -> (): Any = (\"Empty Closure\") -> Std.Println;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  REQUIRE(analyzed.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(analyzed.value());
  REQUIRE(compiled_module.has_value());

  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(compiled_module.value());
  REQUIRE(runtime_result.has_value());
}

TEST_CASE("Std.Match executes ordered pattern closures in the VM", "[vm][samples][match]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_match";
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "match_ok.fleaux";

  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "let IsEven(n: Float64): Bool = ((n, 2.0) -> Std.Mod, 0.0) -> Std.Equal;\n"
           "(0, (0, (): Any = \"zero\"), (1, (): Any = \"one\"), (_, (): Any = \"many\")) -> Std.Match -> "
           "Std.Println;\n"
           "(3, (0, (): Any = \"zero\"), (1, (): Any = \"one\"), (_, (): Any = \"many\")) -> Std.Match -> "
           "Std.Println;\n"
           "(8.0, (IsEven, (): Any = \"even\"), (_, (): Any = \"odd\")) -> Std.Match -> Std.Println;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  REQUIRE(analyzed.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(analyzed.value());
  REQUIRE(compiled_module.has_value());

  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(compiled_module.value());
  REQUIRE(runtime_result.has_value());
}

TEST_CASE("Std.Type executes in the VM", "[vm][samples]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_typeof";
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "typeof_ok.fleaux";

  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "(1) -> Std.Type -> Std.Println;\n"
           "(1.5) -> Std.Type -> Std.Println;\n"
           "(\"hi\") -> Std.Type -> Std.Println;\n"
           "((1, 2, 3)) -> Std.Type -> Std.Println;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  REQUIRE(analyzed.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(analyzed.value());
  REQUIRE(compiled_module.has_value());

  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(compiled_module.value());
  REQUIRE(runtime_result.has_value());
}

TEST_CASE("Std.TypeOf is unresolved after alias removal", "[vm][samples][types]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_typeof_removed";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "typeof_removed.fleaux";

  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "(\"hi\") -> Std.TypeOf -> Std.Println;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  REQUIRE_FALSE(analyzed.has_value());
  REQUIRE(analyzed.error().find("Unresolved symbol") != std::string::npos);
  REQUIRE(analyzed.error().find("Std.TypeOf") != std::string::npos);
}

TEST_CASE("Float64 constants reject Int64 parameters during analysis", "[vm][samples]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_float64_constants";
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "float64_constant_int64_reject.fleaux";

  {
    std::ofstream out(source_path);
    out << "let Std.Pi(): Float64 = 3.14159;\n"
           "let Std.Println(args: Any...): Tuple(Any...) :: __builtin__;\n"
           "let NeedsInt(x: Int64): Int64 = x;\n"
           "(Std.Pi) -> NeedsInt -> Std.Println;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  REQUIRE_FALSE(analyzed.has_value());
}

TEST_CASE("VM Std.Parallel.WithOptions preflight errors stay typed", "[vm][samples][parallel]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_parallel_with_options_error";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "parallel_with_options_error.fleaux";

  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "let Identity(x: Float64): Float64 = x;\n"
           "let BuildBadOptions(): Dict(String, Any) =\n"
           "    () -> Std.Dict.Create\n"
           "    -> (_, \"max_workers\", 0) -> Std.Dict.Set\n"
           ";\n"
           "((1.0, 2.0), Identity, () -> BuildBadOptions) -> Std.Parallel.WithOptions -> Std.Result.UnwrapErr -> "
           "Std.Println;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  REQUIRE(analyzed.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(analyzed.value());
  REQUIRE(compiled_module.has_value());

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  set_runtime_process_args(source_path, {});
  const auto runtime_result = runtime.execute(compiled_module.value(), output);
  REQUIRE(runtime_result.has_value());
  REQUIRE(output.str() == "0 Parallel.WithOptions: max_workers must be > 0\n");
}

TEST_CASE("VM Std.Parallel.WithOptions rejects non-Dict(String, Any) options during analysis",
          "[vm][samples][parallel]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_parallel_with_options_typed_reject";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "parallel_with_options_typed_reject.fleaux";

  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "let Identity(x: Float64): Float64 = x;\n"
           "let MakeBadOptions(): Dict(String, Float64) :: __builtin__;\n"
           "((1.0, 2.0), Identity, () -> MakeBadOptions) -> Std.Parallel.WithOptions;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  REQUIRE_FALSE(analyzed.has_value());
  REQUIRE(analyzed.error().find("Dict(String, Any)") != std::string::npos);
}

TEST_CASE("VM Std.Parallel.Reduce step failures stay typed", "[vm][samples][parallel]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_parallel_reduce_error";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "parallel_reduce_error.fleaux";

  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "let AddOrFail(acc: Float64, x: Float64): Float64 =\n"
           "  (x,\n"
           "   (4.0, (): Float64 = (\"reduce-boom\") -> Std.Result.Err -> Std.Result.Unwrap),\n"
           "   (_, (): Float64 = (acc, x) -> Std.Add))\n"
           "  -> Std.Match\n"
           ";\n"
           "((1.0, 2.0, 3.0, 4.0), 0.0, AddOrFail) -> Std.Parallel.Reduce -> Std.Result.UnwrapErr -> Std.Println;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  REQUIRE(analyzed.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(analyzed.value());
  REQUIRE(compiled_module.has_value());

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  set_runtime_process_args(source_path, {});
  const auto runtime_result = runtime.execute(compiled_module.value(), output);
  REQUIRE(runtime_result.has_value());
  REQUIRE(output.str() == "3 Result.Unwrap expected Ok (true), got Err (false)\n");
}

TEST_CASE("VM Std.Task.AwaitAll failures stay typed", "[vm][samples][task]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_task_awaitall_error";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "task_awaitall_error.fleaux";

  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "let ReturnSelf(x: Float64): Float64 = x;\n"
           "let MaybeFail(x: Float64): Float64 =\n"
           "  (x,\n"
           "   (2.0, (): Float64 = (\"boom-two\") -> Std.Result.Err -> Std.Result.Unwrap),\n"
           "   (_, (): Float64 = x))\n"
           "  -> Std.Match\n"
           ";\n"
           "(((ReturnSelf, 1.0) -> Std.Task.Spawn, (MaybeFail, 2.0) -> Std.Task.Spawn))\n"
           "  -> Std.Task.AwaitAll\n"
           "  -> Std.Result.UnwrapErr\n"
           "  -> Std.Println;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  REQUIRE(analyzed.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(analyzed.value());
  REQUIRE(compiled_module.has_value());

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  set_runtime_process_args(source_path, {});
  const auto runtime_result = runtime.execute(compiled_module.value(), output);
  REQUIRE(runtime_result.has_value());
  REQUIRE(output.str() == "1 Result.Unwrap expected Ok (true), got Err (false)\n");
}

TEST_CASE("VM Std.Task.WithTimeout negative timeout is rejected during analysis", "[vm][samples][task]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_task_timeout_preflight";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "task_timeout_preflight.fleaux";

  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "let Identity(x: Float64): Float64 = x;\n"
           "(Identity, 1.0) -> Std.Task.Spawn -> (_, -1) -> Std.Task.WithTimeout;\n";
  }

  const auto analyzed = load_ir_program(source_path);
  REQUIRE_FALSE(analyzed.has_value());
  REQUIRE(analyzed.error().find("Type mismatch in call target arguments") != std::string::npos);
  REQUIRE(analyzed.error().find("Std.Task.WithTimeout expects argument 1") != std::string::npos);
}

#define FLEAUX_VM_SAMPLE_TEST(sample_file_literal) \
  TEST_CASE("VM sample: " sample_file_literal, "[vm][samples]") { run_sample_in_vm_and_assert(sample_file_literal); }

#define FLEAUX_VM_BYTECODE_SAMPLE_TEST(sample_file_literal)                    \
  TEST_CASE("Compiled VM sample: " sample_file_literal, "[vm][samples][vm]") { \
    run_sample_in_bytecode_and_assert(sample_file_literal);                    \
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
FLEAUX_VM_SAMPLE_TEST("31_result_ok_err.fleaux")
FLEAUX_VM_SAMPLE_TEST("32_try_empty_tuple.fleaux")
FLEAUX_VM_SAMPLE_TEST("33_exp_parallel.fleaux")
FLEAUX_VM_SAMPLE_TEST("35_concurrency_tasks.fleaux")
FLEAUX_VM_SAMPLE_TEST("36_parallel_options_and_empty_inputs.fleaux")
FLEAUX_VM_SAMPLE_TEST("37_parallel_error_paths.fleaux")
FLEAUX_VM_SAMPLE_TEST("38_parallel_inline_closures.fleaux")
FLEAUX_VM_SAMPLE_TEST("39_if_else_match.fleaux")
FLEAUX_VM_SAMPLE_TEST("40_recursion.fleaux")
FLEAUX_VM_SAMPLE_TEST("41_function_factory.fleaux")
FLEAUX_VM_SAMPLE_TEST("42_dict_dispatch.fleaux")
FLEAUX_VM_SAMPLE_TEST("43_accumulator_loops.fleaux")
FLEAUX_VM_SAMPLE_TEST("44_tuple_map_filter_reduce.fleaux")
FLEAUX_VM_SAMPLE_TEST("45_result_recovery.fleaux")
FLEAUX_VM_SAMPLE_TEST("46_select_vs_branch_match.fleaux")
FLEAUX_VM_SAMPLE_TEST("47_tag_dispatch_vs_callable_dispatch.fleaux")
FLEAUX_VM_SAMPLE_TEST("48_repeat_value_tuple.fleaux")
FLEAUX_VM_SAMPLE_TEST("49_same_input_fanout.fleaux")
FLEAUX_VM_SAMPLE_TEST("50_multi_accumulator_loop_state.fleaux")
FLEAUX_VM_SAMPLE_TEST("51_strong_type_casts.fleaux")
FLEAUX_VM_SAMPLE_TEST("52_variables_and_blocks.fleaux")
FLEAUX_VM_SAMPLE_TEST("53_random_seeded.fleaux")

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
FLEAUX_VM_BYTECODE_SAMPLE_TEST("31_result_ok_err.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("32_try_empty_tuple.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("33_exp_parallel.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("35_concurrency_tasks.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("36_parallel_options_and_empty_inputs.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("37_parallel_error_paths.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("38_parallel_inline_closures.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("39_if_else_match.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("40_recursion.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("41_function_factory.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("42_dict_dispatch.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("43_accumulator_loops.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("44_tuple_map_filter_reduce.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("45_result_recovery.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("46_select_vs_branch_match.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("47_tag_dispatch_vs_callable_dispatch.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("48_repeat_value_tuple.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("49_same_input_fanout.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("50_multi_accumulator_loop_state.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("51_strong_type_casts.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("52_variables_and_blocks.fleaux")
FLEAUX_VM_BYTECODE_SAMPLE_TEST("53_random_seeded.fleaux")

#undef FLEAUX_VM_SAMPLE_TEST
#undef FLEAUX_VM_BYTECODE_SAMPLE_TEST
