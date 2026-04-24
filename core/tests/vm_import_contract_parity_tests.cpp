#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/bytecode/module_loader.hpp"
#include "fleaux/vm/interpreter.hpp"
#include "fleaux/vm/runtime.hpp"

namespace {

enum class OutcomeClass {
  kSuccess,
  kImportUnresolved,
  kImportCycle,
  kTypeMismatch,
  kUnresolvedSymbol,
  kOther,
};

struct FixtureFile {
  std::string name;
  std::string source;
};

struct ParityCase {
  std::string name;
  std::string entry_file;
  std::vector<FixtureFile> files;
  OutcomeClass expected_outcome;
  std::vector<std::string> expected_fragments;
  bool execute_vm{false};
};

void write_fixture_file(const std::filesystem::path& path, const std::string& source) {
  std::ofstream out(path);
  REQUIRE(out.good());
  out << source;
  REQUIRE(out.good());
}

auto classify_message(const std::string& text) -> OutcomeClass {
  if (text.find("import-unresolved:") != std::string::npos) { return OutcomeClass::kImportUnresolved; }
  if (text.find("import-cycle:") != std::string::npos) { return OutcomeClass::kImportCycle; }
  if (text.find("Type mismatch in call target arguments") != std::string::npos) {
    return OutcomeClass::kTypeMismatch;
  }
  if (text.find("Unresolved symbol") != std::string::npos) { return OutcomeClass::kUnresolvedSymbol; }
  return OutcomeClass::kOther;
}

auto interpret_error_text(const fleaux::vm::InterpretError& error) -> std::string {
  if (error.hint.has_value()) { return error.message + " (" + *error.hint + ")"; }
  return error.message;
}

}  // namespace

TEST_CASE("Import contract parity fixtures stay aligned between interpreter and VM loader",
          "[vm][imports][contract][parity]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_import_contract_parity";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const std::vector<ParityCase> cases = {
      ParityCase{
          .name = "import_unresolved",
          .entry_file = "entry_unresolved.fleaux",
          .files = {
              FixtureFile{
                  .name = "entry_unresolved.fleaux",
                  .source = "import missing_dep;\n"
                            "let Main(x: Float64): Float64 = x;\n",
              },
          },
          .expected_outcome = OutcomeClass::kImportUnresolved,
          .expected_fragments = {"missing_dep", "Import not found"},
      },
      ParityCase{
          .name = "import_cycle",
          .entry_file = "a.fleaux",
          .files = {
              FixtureFile{
                  .name = "a.fleaux",
                  .source = "import b;\n"
                            "let A(x: Float64): Float64 = x;\n",
              },
              FixtureFile{
                  .name = "b.fleaux",
                  .source = "import a;\n"
                            "let B(x: Float64): Float64 = x;\n",
              },
          },
          .expected_outcome = OutcomeClass::kImportCycle,
                  .expected_fragments = {"Import cycle detected"},
      },
      ParityCase{
          .name = "import_type_mismatch",
          .entry_file = "typed_entry.fleaux",
          .files = {
              FixtureFile{
                  .name = "typed_dep.fleaux",
                  .source = "let Add4(x: String): Int64 = 4;\n",
              },
              FixtureFile{
                  .name = "typed_entry.fleaux",
                  .source = "import Std;\n"
                            "import typed_dep;\n"
                            "(1) -> Add4 -> Std.Println;\n",
              },
          },
          .expected_outcome = OutcomeClass::kTypeMismatch,
          .expected_fragments = {"Add4 expects argument 0"},
      },
      ParityCase{
          .name = "direct_import_visibility",
          .entry_file = "a.fleaux",
          .files = {
              FixtureFile{
                  .name = "c.fleaux",
                  .source = "import Std;\n"
                            "let CFn(x: Float64): Float64 = (x, 1) -> Std.Add;\n",
              },
              FixtureFile{
                  .name = "b.fleaux",
                  .source = "import c;\n"
                            "let BFn(x: Float64): Float64 = (x) -> CFn;\n",
              },
              FixtureFile{
                  .name = "a.fleaux",
                  .source = "import Std;\n"
                            "import b;\n"
                            "(1) -> CFn -> Std.Println;\n",
              },
          },
          .expected_outcome = OutcomeClass::kUnresolvedSymbol,
          .expected_fragments = {"CFn"},
      },
      ParityCase{
          .name = "qualified_export_unresolved",
          .entry_file = "typed_entry_qualified.fleaux",
          .files = {
              FixtureFile{
                  .name = "typed_dep_qualified.fleaux",
                  .source = "import Std;\n"
                            "let MyMath.Add4(x: Float64): Float64 = (4, x) -> Std.Add;\n",
              },
              FixtureFile{
                  .name = "typed_entry_qualified.fleaux",
                  .source = "import Std;\n"
                            "import typed_dep_qualified;\n"
                            "(1) -> WrongMath.Add4 -> Std.Println;\n",
              },
          },
          .expected_outcome = OutcomeClass::kUnresolvedSymbol,
          .expected_fragments = {"WrongMath.Add4"},
      },
      ParityCase{
          .name = "qualified_overload_success",
          .entry_file = "typed_entry_overloads.fleaux",
          .files = {
              FixtureFile{
                  .name = "typed_dep_overloads.fleaux",
                  .source = "import Std;\n"
                            "let MyMath.Echo(x: Int64): Int64 = (x, 1) -> Std.Add;\n"
                            "let MyMath.Echo(x: String): String = x;\n",
              },
              FixtureFile{
                  .name = "typed_entry_overloads.fleaux",
                  .source = "import Std;\n"
                            "import typed_dep_overloads;\n"
                            "(1) -> MyMath.Echo -> Std.Println;\n"
                            "(\"ok\") -> MyMath.Echo -> Std.Println;\n",
              },
          },
          .expected_outcome = OutcomeClass::kSuccess,
          .expected_fragments = {},
          .execute_vm = true,
      },
  };

  constexpr fleaux::vm::Interpreter interpreter;

  for (const auto& parity_case : cases) {
    const auto case_dir = temp_dir / parity_case.name;
    std::filesystem::remove_all(case_dir);
    std::filesystem::create_directories(case_dir);

    for (const auto& file : parity_case.files) {
      write_fixture_file(case_dir / file.name, file.source);
    }

    const auto entry_path = case_dir / parity_case.entry_file;
    const auto interpreter_result = interpreter.run_file(entry_path);
    const auto vm_load_result = fleaux::bytecode::load_linked_module(entry_path);

    INFO("parity case: " << parity_case.name);

    if (parity_case.expected_outcome == OutcomeClass::kSuccess) {
      if (!interpreter_result) { INFO("interpreter error: " << interpret_error_text(interpreter_result.error())); }
      if (!vm_load_result) { INFO("vm load error: " << vm_load_result.error().message); }
      REQUIRE(interpreter_result.has_value());
      REQUIRE(vm_load_result.has_value());

      if (parity_case.execute_vm) {
        const fleaux::vm::Runtime runtime;
        const auto runtime_result = runtime.execute(*vm_load_result);
        if (!runtime_result) { INFO("vm runtime error: " << runtime_result.error().message); }
        REQUIRE(runtime_result.has_value());
      }
      continue;
    }

    REQUIRE_FALSE(interpreter_result.has_value());
    REQUIRE_FALSE(vm_load_result.has_value());

    const auto interpreter_text = interpret_error_text(interpreter_result.error());
    const auto vm_text = vm_load_result.error().message;
    INFO("interpreter text: " << interpreter_text);
    INFO("vm text: " << vm_text);
    REQUIRE(classify_message(interpreter_text) == parity_case.expected_outcome);
    REQUIRE(classify_message(vm_text) == parity_case.expected_outcome);

    for (const auto& fragment : parity_case.expected_fragments) {
      REQUIRE(interpreter_text.find(fragment) != std::string::npos);
      REQUIRE(vm_text.find(fragment) != std::string::npos);
    }
  }
}

TEST_CASE("InterpreterSession keeps REPL imports symbolic-only", "[vm][imports][contract][repl]") {
  constexpr fleaux::vm::Interpreter interpreter;
  const auto session = interpreter.create_session({});

  const auto std_result = session.run_snippet("import Std;\nlet Id(x: Float64): Float64 = x;\n");
  if (!std_result) { INFO("Std import error: " << interpret_error_text(std_result.error())); }
  REQUIRE(std_result.has_value());

  const auto std_builtins_result =
      session.run_snippet("import StdBuiltins;\nlet Keep(x: Float64): Float64 = x;\n");
  if (!std_builtins_result) {
    INFO("StdBuiltins import error: " << interpret_error_text(std_builtins_result.error()));
  }
  REQUIRE(std_builtins_result.has_value());

  const auto nonsymbolic_result = session.run_snippet("import custom_module;\nlet Local(x: Float64): Float64 = x;\n");
  REQUIRE_FALSE(nonsymbolic_result.has_value());
  REQUIRE(nonsymbolic_result.error().message == "REPL only supports symbolic imports: Std, StdBuiltins.");
  REQUIRE(nonsymbolic_result.error().hint.has_value());
  REQUIRE(nonsymbolic_result.error().hint->find("run a file for module imports") != std::string::npos);
}

TEST_CASE("RuntimeSession keeps REPL imports symbolic-only", "[vm][imports][contract][repl]") {
  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream output;

  const auto std_result = session.run_snippet("import Std;\nlet Id(x: Float64): Float64 = x;\n", output);
  if (!std_result) { INFO("Std import error: " << std_result.error().message); }
  REQUIRE(std_result.has_value());

  const auto std_builtins_result =
      session.run_snippet("import StdBuiltins;\nlet Keep(x: Float64): Float64 = x;\n", output);
  if (!std_builtins_result) { INFO("StdBuiltins import error: " << std_builtins_result.error().message); }
  REQUIRE(std_builtins_result.has_value());

  const auto nonsymbolic_result = session.run_snippet("import custom_module;\nlet Local(x: Float64): Float64 = x;\n", output);
  REQUIRE_FALSE(nonsymbolic_result.has_value());
  REQUIRE(nonsymbolic_result.error().message == "REPL only supports symbolic imports: Std, StdBuiltins.");
  REQUIRE(nonsymbolic_result.error().hint.has_value());
  REQUIRE(nonsymbolic_result.error().hint->find("run a file for module imports") != std::string::npos);
}


