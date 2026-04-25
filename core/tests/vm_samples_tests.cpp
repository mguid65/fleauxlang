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
#include "fleaux/vm/interpreter.hpp"
#include "fleaux/vm/runtime.hpp"

#ifndef FLEAUX_REPO_ROOT
#error "FLEAUX_REPO_ROOT must be defined by CMake for sample tests."
#endif

namespace {

using IRProgram = fleaux::frontend::ir::IRProgram;

enum class SampleOutcomeClass {
  kSuccess,
  kImportUnresolved,
  kImportCycle,
  kTypeMismatch,
  kUnresolvedSymbol,
  kOther,
};

struct SampleExecutionObservation {
  bool success = false;
  std::string output;
  std::optional<std::string> error_text;
};

class StdoutCapture {
public:
  StdoutCapture() : old_buffer_(std::cout.rdbuf(buffer_.rdbuf())) {}

  StdoutCapture(const StdoutCapture&) = delete;
  auto operator=(const StdoutCapture&) -> StdoutCapture& = delete;

  ~StdoutCapture() { std::cout.rdbuf(old_buffer_); }

  [[nodiscard]] auto str() const -> std::string { return buffer_.str(); }

private:
  std::ostringstream buffer_;
  std::streambuf* old_buffer_;
};

std::filesystem::path repo_root_path() { return std::filesystem::path(FLEAUX_REPO_ROOT); }

std::filesystem::path samples_dir_path() { return repo_root_path() / "samples"; }

auto make_load_error(const std::string& message, const std::optional<std::string>& hint,
                     const std::optional<fleaux::frontend::diag::SourceSpan>&) -> std::string {
  return hint.has_value() ? message + " (" + *hint + ")" : message;
}

auto classify_sample_message(const std::string& text) -> SampleOutcomeClass {
  if (text.find("import-unresolved:") != std::string::npos) { return SampleOutcomeClass::kImportUnresolved; }
  if (text.find("import-cycle:") != std::string::npos) { return SampleOutcomeClass::kImportCycle; }
  if (text.find("Type mismatch in call target arguments") != std::string::npos) {
    return SampleOutcomeClass::kTypeMismatch;
  }
  if (text.find("Unresolved symbol") != std::string::npos) { return SampleOutcomeClass::kUnresolvedSymbol; }
  return SampleOutcomeClass::kOther;
}

auto interpret_error_text(const fleaux::vm::InterpretError& error) -> std::string {
  if (error.hint.has_value()) { return error.message + " (" + *error.hint + ")"; }
  return error.message;
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
  for (auto& arg : args_storage) { argv_ptrs.push_back(arg.data()); }
  fleaux::runtime::set_process_args(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());
}

void run_sample_in_vm_and_assert(const std::string_view sample_file) {
  const auto sample_path = samples_dir_path() / std::filesystem::path(sample_file);
  REQUIRE(std::filesystem::exists(sample_path));
  const auto runtime_args = sample_runtime_args(sample_file, sample_path);

  constexpr fleaux::vm::Interpreter interpreter;
  const auto result = interpreter.run_file(sample_path, runtime_args);
  INFO("sample file: " << sample_path);
  if (!result.has_value()) { INFO("vm error: " << result.error().message); }
  REQUIRE(result.has_value());
}

void run_sample_in_bytecode_and_assert(const std::string_view sample_file) {
  const auto sample_path = samples_dir_path() / std::filesystem::path(sample_file);
  REQUIRE(std::filesystem::exists(sample_path));
  const auto runtime_args = sample_runtime_args(sample_file, sample_path);

  const auto analyzed = load_ir_program(sample_path);
  INFO("sample file: " << sample_path);
  if (!analyzed) { INFO("analysis error: " << analyzed.error()); }
  REQUIRE(analyzed.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(analyzed.value());
  if (!compiled_module) { INFO("bytecode compile error: " << compiled_module.error().message); }
  REQUIRE(compiled_module.has_value());

  const fleaux::vm::Runtime runtime;
  set_runtime_process_args(sample_path, runtime_args);
  const auto runtime_result = runtime.execute(compiled_module.value());
  if (!runtime_result) { INFO("vm runtime error: " << runtime_result.error().message); }
  REQUIRE(runtime_result.has_value());
}

void run_sample_parity_and_assert(const std::string_view sample_file) {
  const auto sample_path = samples_dir_path() / std::filesystem::path(sample_file);
  REQUIRE(std::filesystem::exists(sample_path));
  const auto runtime_args = sample_runtime_args(sample_file, sample_path);

  constexpr fleaux::vm::Interpreter interpreter;
  SampleExecutionObservation interpreter_observation;
  {
    StdoutCapture capture;
    const auto interp_result = interpreter.run_file(sample_path, runtime_args);
    interpreter_observation.success = interp_result.has_value();
    interpreter_observation.output = capture.str();
    if (!interp_result.has_value()) { interpreter_observation.error_text = interpret_error_text(interp_result.error()); }
  }

  SampleExecutionObservation bytecode_observation;
  if (const auto analyzed = load_ir_program(sample_path); analyzed) {
    constexpr fleaux::bytecode::BytecodeCompiler compiler;
    if (const auto compiled_module = compiler.compile(analyzed.value()); compiled_module) {
      std::ostringstream output;
      const fleaux::vm::Runtime runtime;
      set_runtime_process_args(sample_path, runtime_args);
      const auto runtime_result = runtime.execute(compiled_module.value(), output);
      bytecode_observation.success = runtime_result.has_value();
      bytecode_observation.output = output.str();
      if (!runtime_result) { bytecode_observation.error_text = runtime_result.error().message; }
    } else {
      bytecode_observation.error_text = compiled_module.error().message;
    }
  } else {
    bytecode_observation.error_text = analyzed.error();
  }

  INFO("sample file: " << sample_path);
  INFO("interpreter success: " << interpreter_observation.success);
  INFO("bytecode success: " << bytecode_observation.success);
  INFO("interpreter output: " << interpreter_observation.output);
  INFO("bytecode output: " << bytecode_observation.output);
  if (interpreter_observation.error_text.has_value()) { INFO("interpreter error: " << *interpreter_observation.error_text); }
  if (bytecode_observation.error_text.has_value()) { INFO("bytecode error: " << *bytecode_observation.error_text); }

  REQUIRE(interpreter_observation.success == bytecode_observation.success);
  if (interpreter_observation.success) {
    REQUIRE(interpreter_observation.output == bytecode_observation.output);
    return;
  }

  REQUIRE(interpreter_observation.error_text.has_value());
  REQUIRE(bytecode_observation.error_text.has_value());
  REQUIRE(classify_sample_message(*interpreter_observation.error_text) ==
          classify_sample_message(*bytecode_observation.error_text));
}

constexpr std::array<std::string_view, 38> kExpectedSamples = {
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
};

}  // namespace

TEST_CASE("VM sample list stays in sync with samples directory", "[vm][samples]") {
  std::set<std::string> expected;
  for (const auto name : kExpectedSamples) { expected.insert(std::string(name)); }

  std::set<std::string> discovered;
  for (const auto& entry : std::filesystem::directory_iterator(samples_dir_path())) {
    if (!entry.is_regular_file()) { continue; }
    if (const auto& path = entry.path(); path.extension() == ".fleaux") { discovered.insert(path.filename().string()); }
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

  const auto analyzed = load_ir_program(source_path);
  REQUIRE_FALSE(analyzed.has_value());
}

TEST_CASE("Interpreter file mode rejects bytecode entry files with explicit guidance",
          "[vm][imports][contract][entry]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_interpreter_entry_policy_contract";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto source_path = temp_dir / "entry.fleaux";
  const auto bytecode_path = temp_dir / "entry.fleaux.bc";
  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "(1, 2) -> Std.Add -> Std.Println;\n";
  }

  const auto vm_load_from_source = fleaux::bytecode::load_linked_module(source_path);
  REQUIRE(vm_load_from_source.has_value());
  REQUIRE(std::filesystem::exists(bytecode_path));

  constexpr fleaux::vm::Interpreter interpreter;
  const auto interpreter_result = interpreter.run_file(bytecode_path);
  REQUIRE_FALSE(interpreter_result.has_value());
  REQUIRE(interpreter_result.error().message == "Interpreter file mode only accepts .fleaux source entry files.");
  REQUIRE(interpreter_result.error().hint.has_value());
  REQUIRE_THAT(*interpreter_result.error().hint,
               Catch::Matchers::ContainsSubstring("--mode vm for .fleaux.bc modules"));

  const auto vm_load_from_bytecode = fleaux::bytecode::load_linked_module(bytecode_path);
  if (!vm_load_from_bytecode) { INFO("vm load error: " << vm_load_from_bytecode.error().message); }
  REQUIRE(vm_load_from_bytecode.has_value());

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(*vm_load_from_bytecode, output);
  if (!runtime_result) { INFO("vm runtime error: " << runtime_result.error().message); }
  REQUIRE(runtime_result.has_value());
  REQUIRE(output.str() == "3\n");
}

TEST_CASE("Import unresolved diagnostics are aligned between interpreter and VM loader", "[vm][imports][contract]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_import_unresolved_contract";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto source_path = temp_dir / "entry.fleaux";
  {
    std::ofstream out(source_path);
    out << "import missing_dep;\n"
           "let Main(x: Float64): Float64 = x;\n";
  }

  constexpr fleaux::vm::Interpreter interpreter;
  const auto interpreter_result = interpreter.run_file(source_path);
  REQUIRE_FALSE(interpreter_result.has_value());
  REQUIRE(interpreter_result.error().message.find("import-unresolved:") != std::string::npos);
  REQUIRE(interpreter_result.error().message.find("Import not found: 'missing_dep'") != std::string::npos);

  const auto vm_load_result = fleaux::bytecode::load_linked_module(source_path);
  REQUIRE_FALSE(vm_load_result.has_value());
  REQUIRE(vm_load_result.error().message.find("import-unresolved:") != std::string::npos);
  REQUIRE(vm_load_result.error().message.find("Import not found: 'missing_dep'") != std::string::npos);
}

TEST_CASE("Import cycle diagnostics are aligned between interpreter and VM loader", "[vm][imports][contract]") {
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

  constexpr fleaux::vm::Interpreter interpreter;
  const auto interpreter_result = interpreter.run_file(module_a);
  REQUIRE_FALSE(interpreter_result.has_value());
  REQUIRE(interpreter_result.error().message.find("import-cycle:") != std::string::npos);

  const auto vm_load_result = fleaux::bytecode::load_linked_module(module_a);
  REQUIRE_FALSE(vm_load_result.has_value());
  REQUIRE(vm_load_result.error().message.find("import-cycle:") != std::string::npos);
}

TEST_CASE("Imported type mismatch diagnostics are aligned between interpreter and VM loader",
          "[vm][imports][contract][types]") {
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

  constexpr fleaux::vm::Interpreter interpreter;
  const auto interpreter_result = interpreter.run_file(entry_path);
  REQUIRE_FALSE(interpreter_result.has_value());
  REQUIRE(interpreter_result.error().message == "Type mismatch in call target arguments.");
  REQUIRE(interpreter_result.error().hint.has_value());
  REQUIRE(interpreter_result.error().hint->find("Add4 expects argument 0") != std::string::npos);

  const auto vm_load_result = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE_FALSE(vm_load_result.has_value());
  REQUIRE(vm_load_result.error().message.find("Type mismatch in call target arguments") != std::string::npos);
  REQUIRE(vm_load_result.error().message.find("Add4 expects argument 0") != std::string::npos);
}

TEST_CASE("Direct import visibility is aligned between interpreter and VM loader",
          "[vm][imports][contract][types]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_import_direct_visibility_contract";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto module_c = temp_dir / "c.fleaux";
  const auto module_b = temp_dir / "b.fleaux";
  const auto module_a = temp_dir / "a.fleaux";

  {
    std::ofstream out(module_c);
    out << "import Std;\n"
           "let CFn(x: Float64): Float64 = (x, 1) -> Std.Add;\n";
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
           "(1) -> CFn -> Std.Println;\n";
  }

  constexpr fleaux::vm::Interpreter interpreter;
  const auto interpreter_result = interpreter.run_file(module_a);
  REQUIRE_FALSE(interpreter_result.has_value());
  REQUIRE(interpreter_result.error().message == "Unresolved symbol.");
  REQUIRE(interpreter_result.error().hint.has_value());
  REQUIRE(interpreter_result.error().hint->find("CFn") != std::string::npos);

  const auto vm_load_result = fleaux::bytecode::load_linked_module(module_a);
  REQUIRE_FALSE(vm_load_result.has_value());
  REQUIRE(vm_load_result.error().message.find("Unresolved symbol") != std::string::npos);
  REQUIRE(vm_load_result.error().message.find("CFn") != std::string::npos);
}

TEST_CASE("Qualified export unresolved diagnostics are aligned between interpreter and VM loader",
          "[vm][imports][contract][types]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_import_qualified_unresolved_contract";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto dependency_path = temp_dir / "typed_dep_qualified.fleaux";
  const auto entry_path = temp_dir / "typed_entry_qualified.fleaux";
  {
    std::ofstream out(dependency_path);
    out << "import Std;\n"
           "let MyMath.Add4(x: Float64): Float64 = (4, x) -> Std.Add;\n";
  }
  {
    std::ofstream out(entry_path);
    out << "import Std;\n"
           "import typed_dep_qualified;\n"
           "(1) -> WrongMath.Add4 -> Std.Println;\n";
  }

  constexpr fleaux::vm::Interpreter interpreter;
  const auto interpreter_result = interpreter.run_file(entry_path);
  REQUIRE_FALSE(interpreter_result.has_value());
  REQUIRE(interpreter_result.error().message == "Unresolved symbol.");
  REQUIRE(interpreter_result.error().hint.has_value());
  REQUIRE(interpreter_result.error().hint->find("WrongMath.Add4") != std::string::npos);

  const auto vm_load_result = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE_FALSE(vm_load_result.has_value());
  REQUIRE(vm_load_result.error().message.find("Unresolved symbol") != std::string::npos);
  REQUIRE(vm_load_result.error().message.find("WrongMath.Add4") != std::string::npos);
}

TEST_CASE("Qualified exported overload symbol-key behavior is aligned between interpreter and VM loader",
          "[vm][imports][contract][types]") {
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

  constexpr fleaux::vm::Interpreter interpreter;
  const auto interpreter_result = interpreter.run_file(entry_path);
  if (!interpreter_result) { INFO("interpreter error: " << interpreter_result.error().message); }
  REQUIRE(interpreter_result.has_value());

  const auto vm_load_result = fleaux::bytecode::load_linked_module(entry_path);
  if (!vm_load_result) { INFO("vm load error: " << vm_load_result.error().message); }
  REQUIRE(vm_load_result.has_value());

  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(*vm_load_result);
  if (!runtime_result) { INFO("runtime error: " << runtime_result.error().message); }
  REQUIRE(runtime_result.has_value());
}

TEST_CASE("Interpreter run_file reclaims transient callable refs across runs", "[vm][samples][lifetime]") {
  fleaux::runtime::reset_callable_registry();
  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);

  const auto sample_file = std::string_view{"29_inline_closures.fleaux"};
  const auto sample_path = samples_dir_path() / std::filesystem::path(sample_file);
  REQUIRE(std::filesystem::exists(sample_path));
  const auto runtime_args = sample_runtime_args(sample_file, sample_path);

  constexpr fleaux::vm::Interpreter interpreter;

  const auto first = interpreter.run_file(sample_path, runtime_args);
  REQUIRE(first.has_value());
  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);

  const auto second = interpreter.run_file(sample_path, runtime_args);
  REQUIRE(second.has_value());
  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);
}

TEST_CASE("InterpreterSession run_snippet reclaims transient callable refs across runs", "[vm][samples][lifetime]") {
  fleaux::runtime::reset_callable_registry();
  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);

  constexpr fleaux::vm::Interpreter interpreter;
  const auto session = interpreter.create_session({});

  const std::string snippet =
      "import Std;\n"
      "let MakeAdder(n: Float64): Any = (x: Float64): Float64 = (x, n) -> Std.Add;\n"
      "(10, (4) -> MakeAdder) -> Std.Apply -> Std.Println;\n";

  for (int iter = 0; iter < 50; ++iter) {
    const auto result = session.run_snippet(snippet);
    REQUIRE(result.has_value());
    REQUIRE(fleaux::runtime::callable_registry_size() == 0U);
  }
}

TEST_CASE("InterpreterSession preserves typed lets across snippets", "[vm][samples][repl][type]") {
  constexpr fleaux::vm::Interpreter interpreter;
  const auto session = interpreter.create_session({});

  const auto define_result =
      session.run_snippet("import Std;\nlet AddOne(x: Float64): Float64 = (x, 1) -> Std.Add;\n");
  REQUIRE(define_result.has_value());

  const auto use_result = session.run_snippet("import Std;\n2 -> AddOne -> Std.Println;\n");
  if (!use_result.has_value()) { INFO("repl typed let lookup error: " << use_result.error().message); }
  REQUIRE(use_result.has_value());
}

TEST_CASE("InterpreterSession type-checks later snippets against prior lets", "[vm][samples][repl][type]") {
  constexpr fleaux::vm::Interpreter interpreter;
  const auto session = interpreter.create_session({});

  const auto define_result =
      session.run_snippet("import Std;\nlet AddOne(x: Float64): Float64 = (x, 1) -> Std.Add;\n");
  REQUIRE(define_result.has_value());

  const auto mismatch_result = session.run_snippet("\"oops\" -> AddOne;\n");
  REQUIRE_FALSE(mismatch_result.has_value());
  REQUIRE(mismatch_result.error().message == "Type mismatch in call target arguments.");
  REQUIRE(mismatch_result.error().hint.has_value());
  REQUIRE_THAT(*mismatch_result.error().hint, Catch::Matchers::ContainsSubstring("AddOne expects argument 0"));
}

TEST_CASE("InterpreterSession typed let redefinition replaces prior signature", "[vm][samples][repl][type]") {
  constexpr fleaux::vm::Interpreter interpreter;
  const auto session = interpreter.create_session({});

  REQUIRE(session.run_snippet("import Std;\nlet Convert(x: Float64): Float64 = (x, 1) -> Std.Add;\n").has_value());
  REQUIRE(session.run_snippet("let Convert(x: String): String = x;\n").has_value());

  const auto string_ok = session.run_snippet("\"text\" -> Convert;\n");
  REQUIRE(string_ok.has_value());

  const auto stale_signature = session.run_snippet("1 -> Convert;\n");
  REQUIRE_FALSE(stale_signature.has_value());
  REQUIRE(stale_signature.error().message == "Type mismatch in call target arguments.");
}

TEST_CASE("Nested closure dict capture churn stays stable in interpreter and VM", "[vm][samples][lifetime]") {
  fleaux::runtime::reset_callable_registry();
  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);

  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_closure_dict_churn";
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "closure_dict_churn.fleaux";

  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "let MakeLookup(d: Any): Any = (k: Any): Any = (d, k, 0) -> Std.Dict.GetDefault;\n"
           "let MakeDict(): Any = (() -> Std.Dict.Create, \"a\", 1) -> Std.Dict.Set;\n"
           "() -> MakeDict -> MakeLookup -> (\"a\", _) -> Std.Apply -> "
           "Std.Println;\n"
           "((1, 2, 3, 4), (x: Float64): Float64 = (x, 1) -> Std.Add) -> Std.Parallel.Map -> Std.Println;\n";
  }

  constexpr fleaux::vm::Interpreter interpreter;

  const auto analyzed = load_ir_program(source_path);
  REQUIRE(analyzed.has_value());
  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(analyzed.value());
  REQUIRE(compiled_module.has_value());

  for (int iter = 0; iter < 40; ++iter) {
    const auto interp_result = interpreter.run_file(source_path);
    if (!interp_result.has_value()) { INFO("interpreter churn error: " << interp_result.error().message); }
    REQUIRE(interp_result.has_value());
    REQUIRE(fleaux::runtime::callable_registry_size() == 0U);

    const fleaux::vm::Runtime runtime;
    const auto runtime_result = runtime.execute(compiled_module.value());
    if (!runtime_result.has_value()) { INFO("vm churn error: " << runtime_result.error().message); }
    REQUIRE(runtime_result.has_value());
    REQUIRE(fleaux::runtime::callable_registry_size() == 0U);
  }
}

TEST_CASE("Task and Parallel samples keep interpreter/VM parity and registry stability",
          "[vm][samples][lifetime][concurrency]") {
  fleaux::runtime::reset_callable_registry();
  fleaux::runtime::reset_value_registry_for_tests();
  fleaux::runtime::reset_task_registry_for_tests();
  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);
  REQUIRE(fleaux::runtime::value_registry_telemetry().active_count == 0U);
  REQUIRE(fleaux::runtime::value_registry_telemetry().rejected_allocations == 0U);
  REQUIRE(fleaux::runtime::value_registry_telemetry().stale_deref_rejections == 0U);
  REQUIRE(fleaux::runtime::task_registry_size() == 0U);

  constexpr fleaux::vm::Interpreter interpreter;

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
      const auto interp_result = interpreter.run_file(sample_path, runtime_args);
      INFO("sample file: " << sample_path);
      if (!interp_result.has_value()) { INFO("interpreter concurrency sample error: " << interp_result.error().message); }
      REQUIRE(interp_result.has_value());
      REQUIRE(fleaux::runtime::callable_registry_size() == 0U);
      REQUIRE(fleaux::runtime::task_registry_size() == 0U);
      const auto interp_telemetry = fleaux::runtime::value_registry_telemetry();
      REQUIRE(interp_telemetry.active_count == 0U);
      REQUIRE(interp_telemetry.rejected_allocations == 0U);
      REQUIRE(interp_telemetry.stale_deref_rejections == 0U);

      const fleaux::vm::Runtime runtime;
      set_runtime_process_args(sample_path, runtime_args);
      const auto runtime_result = runtime.execute(compiled_module.value());
      if (!runtime_result.has_value()) { INFO("vm concurrency sample error: " << runtime_result.error().message); }
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

TEST_CASE("Interpreter session stale callable ref is rejected after scope cleanup", "[vm][samples][lifetime]") {
  fleaux::runtime::reset_callable_registry();
  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);

  constexpr fleaux::vm::Interpreter interpreter;
  const auto session = interpreter.create_session({});

  // Snippet produces a closure ref via MakeAdder and applies it in the same run.
  const std::string snippet =
      "import Std;\n"
      "let MakeAdder(n: Float64): Any = (x: Float64): Float64 = (x, n) -> Std.Add;\n"
      "(10, (4) -> MakeAdder) -> Std.Apply -> Std.Println;\n";

  // First run: registry should return to zero after the scope.
  {
    const auto result = session.run_snippet(snippet);
    REQUIRE(result.has_value());
    REQUIRE(fleaux::runtime::callable_registry_size() == 0U);
  }

  // Manually forge a ref to slot 0 gen 0 -- these would have been valid during the first run.
  fleaux::runtime::Array forged_ref;
  forged_ref.Reserve(3);
  forged_ref.PushBack(fleaux::runtime::Value{fleaux::runtime::String{fleaux::runtime::k_callable_tag}});
  forged_ref.PushBack(fleaux::runtime::Value{fleaux::runtime::UInt{0}});
  forged_ref.PushBack(fleaux::runtime::Value{fleaux::runtime::UInt{0}});

  // Must be rejected: the slot was retired at scope exit.
  REQUIRE_THROWS_WITH(
      fleaux::runtime::invoke_callable_ref(fleaux::runtime::Value{std::move(forged_ref)}, fleaux::runtime::make_int(1)),
      Catch::Matchers::ContainsSubstring("Unknown callable reference"));

  // Second run must still work and return registry to zero.
  {
    const auto result = session.run_snippet(snippet);
    REQUIRE(result.has_value());
    REQUIRE(fleaux::runtime::callable_registry_size() == 0U);
  }
}

TEST_CASE("Interpreter session registry grows only with let-registered callables and shrinks on each run",
          "[vm][samples][lifetime]") {
  fleaux::runtime::reset_callable_registry();
  REQUIRE(fleaux::runtime::callable_registry_size() == 0U);

  constexpr fleaux::vm::Interpreter interpreter;
  const auto session = interpreter.create_session({});

  // Multiple distinct closures registered per snippet run.
  const std::string snippet =
      "import Std;\n"
      "let Add1(x: Float64): Float64 = (x, 1) -> Std.Add;\n"
      "let Mul2(x: Float64): Float64 = (x, 2) -> Std.Multiply;\n"
      "let Neg(x: Float64): Float64 = (-1, x) -> Std.Multiply;\n"
      "5 -> Add1 -> Mul2 -> Neg -> Std.Println;\n";

  std::size_t baseline = 0;
  for (int run_index = 0; run_index < 30; ++run_index) {
    const auto result = session.run_snippet(snippet);
    REQUIRE(result.has_value());
    const auto size_after_run = fleaux::runtime::callable_registry_size();
    if (run_index == 0) {
      baseline = size_after_run;
    } else {
      // Registry must not grow beyond the baseline established on the first run.
      REQUIRE(size_after_run <= baseline);
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

  constexpr fleaux::vm::Interpreter interpreter;
  const auto interpreter_result = interpreter.run_file(source_path);
  REQUIRE(interpreter_result.has_value());

  const auto analyzed = load_ir_program(source_path);
  REQUIRE(analyzed.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(analyzed.value());
  REQUIRE(compiled_module.has_value());

  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(compiled_module.value());
  if (!runtime_result.has_value()) { INFO("bytecode runtime error: " << runtime_result.error().message); }
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
           "let HeadTail(head: Float64, rest: Any...): Any = rest;\n"
           "(1) -> Collect -> Std.Length -> Std.Println;\n"
           "((10, 20, 30)) -> HeadTail -> Std.Length -> Std.Println;\n";
  }

  constexpr fleaux::vm::Interpreter interpreter;
  const auto interpreter_result = interpreter.run_file(source_path);
  REQUIRE(interpreter_result.has_value());

  const auto analyzed = load_ir_program(source_path);
  REQUIRE(analyzed.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(analyzed.value());
  REQUIRE(compiled_module.has_value());

  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(compiled_module.value());
  if (!runtime_result.has_value()) { INFO("bytecode runtime error: " << runtime_result.error().message); }
  REQUIRE(runtime_result.has_value());
}

TEST_CASE("User variadic tail enforces minimum fixed args", "[vm][samples][variadic]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_variadic_tail";
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "variadic_tail_too_few.fleaux";

  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "let HeadTail(head: Float64, rest: Any...): Any = rest;\n"
           "() -> HeadTail -> Std.Println;\n";
  }

  constexpr fleaux::vm::Interpreter interpreter;
  const auto interpreter_result = interpreter.run_file(source_path);
  REQUIRE_FALSE(interpreter_result.has_value());

  const auto analyzed = load_ir_program(source_path);
  REQUIRE_FALSE(analyzed.has_value());
}

TEST_CASE("Imported user overloads dispatch consistently in interpreter and VM", "[vm][samples][overload]") {
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

  constexpr fleaux::vm::Interpreter interpreter;
  const auto interpreter_result = interpreter.run_file(entry_path);
  if (!interpreter_result) { INFO("interpreter error: " << interpreter_result.error().message); }
  REQUIRE(interpreter_result.has_value());

  const auto analyzed = load_ir_program(entry_path);
  if (!analyzed) { INFO("analysis error: " << analyzed.error()); }
  REQUIRE(analyzed.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled = compiler.compile(*analyzed);
  if (!compiled) { INFO("bytecode compile error: " << compiled.error().message); }
  REQUIRE(compiled.has_value());

  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(*compiled);
  if (!runtime_result) { INFO("runtime error: " << runtime_result.error().message); }
  REQUIRE(runtime_result.has_value());
}

TEST_CASE("Std.Dict.Create clone overload executes in interpreter and VM", "[vm][samples][dict]") {
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

  constexpr fleaux::vm::Interpreter interpreter;
  const auto interpreter_result = interpreter.run_file(source_path);
  if (!interpreter_result) { INFO("interpreter error: " << interpreter_result.error().message); }
  REQUIRE(interpreter_result.has_value());

  const auto analyzed = load_ir_program(source_path);
  if (!analyzed) { INFO("analysis error: " << analyzed.error()); }
  REQUIRE(analyzed.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled = compiler.compile(*analyzed);
  if (!compiled) { INFO("bytecode compile error: " << compiled.error().message); }
  REQUIRE(compiled.has_value());

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(*compiled, output);
  if (!runtime_result) { INFO("runtime error: " << runtime_result.error().message); }
  REQUIRE(runtime_result.has_value());
  REQUIRE(output.str() == "1\n");
}

TEST_CASE("Integer-only Std params reject Float64 during analysis", "[vm][samples]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_integer_only_params";
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "integer_only_params_reject_float.fleaux";

  {
    std::ofstream out(source_path);
    out << "let Std.Array.GetAt(array: Tuple(Any...), index: Int64): Any :: __builtin__;\n"
           "let Std.Array.GetAtND(value: Any, indices: Tuple(Int64...)): Any :: __builtin__;\n"
           "let Std.Array.ReshapeND(flat_array: Tuple(Any...), shape: Tuple(Int64...)): Any :: __builtin__;\n"
           "let Std.Exit(): Never :: __builtin__;\n"
           "let Std.Exit(code: Int64): Never :: __builtin__;\n"
           "let UseFloatExit(code: Float64): Any = (code) -> Std.Exit;\n"
           "let Std.Println(args: Any...): Tuple(Any...) :: __builtin__;\n"
           "((1, 2, 3), 1.5) -> Std.Array.GetAt -> Std.Println;\n"
           "(((1, 2), (3, 4)), (1, 0.5)) -> Std.Array.GetAtND -> Std.Println;\n"
           "((1, 2, 3, 4), (2, 2.5)) -> Std.Array.ReshapeND -> Std.Println;\n"
           "(1) -> UseFloatExit -> Std.Println;\n"
           "(1.25) -> Std.Exit -> Std.Println;\n";
  }

  constexpr fleaux::vm::Interpreter interpreter;
  const auto interpreter_result = interpreter.run_file(source_path);
  REQUIRE_FALSE(interpreter_result.has_value());

  const auto analyzed = load_ir_program(source_path);
  REQUIRE_FALSE(analyzed.has_value());
}

TEST_CASE("Inline closures execute in both interpreter and VM modes", "[vm][samples][closure]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_inline_closure";
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "inline_closure_ok.fleaux";

  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "(10, (x: Float64): Float64 = (x, 1) -> Std.Add) -> Std.Apply -> Std.Println;\n"
           "(10) -> (x: Float64): Float64 = (x, 1) -> Std.Add -> Std.Println;\n"
           "let MakeAdder(n: Float64): Any = (x: Float64): Float64 = (x, n) -> Std.Add;\n"
           "(10, (4) -> MakeAdder) -> Std.Apply -> Std.Println;\n";
  }

  constexpr fleaux::vm::Interpreter interpreter;
  const auto interpreter_result = interpreter.run_file(source_path);
  REQUIRE(interpreter_result.has_value());

  const auto analyzed = load_ir_program(source_path);
  REQUIRE(analyzed.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(analyzed.value());
  REQUIRE(compiled_module.has_value());

  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(compiled_module.value());
  REQUIRE(runtime_result.has_value());
}

TEST_CASE("Std.Match executes ordered pattern closures in interpreter and VM modes", "[vm][samples][match]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_match";
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "match_ok.fleaux";

  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "let IsEven(n: Float64): Bool = ((n, 2) -> Std.Mod, 0) -> Std.Equal;\n"
           "(0, (0, (): Any = \"zero\"), (1, (): Any = \"one\"), (_, (): Any = \"many\")) -> Std.Match -> "
           "Std.Println;\n"
           "(3, (0, (): Any = \"zero\"), (1, (): Any = \"one\"), (_, (): Any = \"many\")) -> Std.Match -> "
           "Std.Println;\n"
           "(8, (IsEven, (): Any = \"even\"), (_, (): Any = \"odd\")) -> Std.Match -> Std.Println;\n";
  }

  constexpr fleaux::vm::Interpreter interpreter;
  const auto interpreter_result = interpreter.run_file(source_path);
  REQUIRE(interpreter_result.has_value());

  const auto analyzed = load_ir_program(source_path);
  REQUIRE(analyzed.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(analyzed.value());
  REQUIRE(compiled_module.has_value());

  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(compiled_module.value());
  REQUIRE(runtime_result.has_value());
}

TEST_CASE("Std.Type and Std.TypeOf execute in both interpreter and VM modes", "[vm][samples]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_typeof";
  std::filesystem::create_directories(temp_dir);
  const auto source_path = temp_dir / "typeof_ok.fleaux";

  {
    std::ofstream out(source_path);
    out << "import Std;\n"
           "(1) -> Std.Type -> Std.Println;\n"
           "(1.5) -> Std.Type -> Std.Println;\n"
           "(\"hi\") -> Std.TypeOf -> Std.Println;\n"
           "((1, 2, 3)) -> Std.Type -> Std.Println;\n";
  }

  constexpr fleaux::vm::Interpreter interpreter;
  const auto interpreter_result = interpreter.run_file(source_path);
  REQUIRE(interpreter_result.has_value());

  const auto analyzed = load_ir_program(source_path);
  REQUIRE(analyzed.has_value());

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled_module = compiler.compile(analyzed.value());
  REQUIRE(compiled_module.has_value());

  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(compiled_module.value());
  REQUIRE(runtime_result.has_value());
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

  constexpr fleaux::vm::Interpreter interpreter;
  const auto interpreter_result = interpreter.run_file(source_path);
  REQUIRE_FALSE(interpreter_result.has_value());

  const auto analyzed = load_ir_program(source_path);
  REQUIRE_FALSE(analyzed.has_value());
}

#define FLEAUX_VM_SAMPLE_TEST(sample_file_literal) \
  TEST_CASE("VM sample: " sample_file_literal, "[vm][samples]") { run_sample_in_vm_and_assert(sample_file_literal); }

#define FLEAUX_VM_BYTECODE_SAMPLE_TEST(sample_file_literal)                    \
  TEST_CASE("Compiled VM sample: " sample_file_literal, "[vm][samples][vm]") { \
    run_sample_in_bytecode_and_assert(sample_file_literal);                    \
  }

#define FLEAUX_VM_PARITY_SAMPLE_TEST(sample_file_literal)                              \
  TEST_CASE("VM parity sample: " sample_file_literal, "[vm][samples][parity][fast]") { \
    run_sample_parity_and_assert(sample_file_literal);                                 \
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
FLEAUX_VM_PARITY_SAMPLE_TEST("31_result_ok_err.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("32_try_empty_tuple.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("33_exp_parallel.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("35_concurrency_tasks.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("36_parallel_options_and_empty_inputs.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("37_parallel_error_paths.fleaux")
FLEAUX_VM_PARITY_SAMPLE_TEST("38_parallel_inline_closures.fleaux")

#undef FLEAUX_VM_SAMPLE_TEST
#undef FLEAUX_VM_BYTECODE_SAMPLE_TEST
#undef FLEAUX_VM_PARITY_SAMPLE_TEST
