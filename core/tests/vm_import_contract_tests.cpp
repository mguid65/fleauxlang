#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/bytecode/module_loader.hpp"
#include "fleaux/vm/runtime.hpp"
#include "vm_test_support.hpp"

namespace {

}  // namespace

TEST_CASE("VM import contract fixtures match expected loader outcomes", "[vm][imports][contract]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_import_contract";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const std::vector<fleaux::tests::ImportContractCase> cases = {
      fleaux::tests::ImportContractCase{
          .name = "import_unresolved",
          .entry_file = "entry_unresolved.fleaux",
          .files = {
              fleaux::tests::ImportContractFixtureFile{
                  .name = "entry_unresolved.fleaux",
                  .source = "import missing_dep;\n"
                            "let Main(x: Float64): Float64 = x;\n",
              },
          },
          .expected_outcome = fleaux::tests::ImportContractOutcomeClass::kImportUnresolved,
          .expected_fragments = {"missing_dep", "Import not found"},
      },
      fleaux::tests::ImportContractCase{
          .name = "import_cycle",
          .entry_file = "a.fleaux",
          .files = {
              fleaux::tests::ImportContractFixtureFile{
                  .name = "a.fleaux",
                  .source = "import b;\n"
                            "let A(x: Float64): Float64 = x;\n",
              },
              fleaux::tests::ImportContractFixtureFile{
                  .name = "b.fleaux",
                  .source = "import a;\n"
                            "let B(x: Float64): Float64 = x;\n",
              },
          },
          .expected_outcome = fleaux::tests::ImportContractOutcomeClass::kImportCycle,
                  .expected_fragments = {"Import cycle detected"},
      },
      fleaux::tests::ImportContractCase{
          .name = "import_type_mismatch",
          .entry_file = "typed_entry.fleaux",
          .files = {
              fleaux::tests::ImportContractFixtureFile{
                  .name = "typed_dep.fleaux",
                  .source = "let Add4(x: String): Int64 = 4;\n",
              },
              fleaux::tests::ImportContractFixtureFile{
                  .name = "typed_entry.fleaux",
                  .source = "import Std;\n"
                            "import typed_dep;\n"
                            "(1) -> Add4 -> Std.Println;\n",
              },
          },
          .expected_outcome = fleaux::tests::ImportContractOutcomeClass::kTypeMismatch,
          .expected_fragments = {"Add4 expects argument 0"},
      },
      fleaux::tests::ImportContractCase{
          .name = "direct_import_visibility",
          .entry_file = "a.fleaux",
          .files = {
              fleaux::tests::ImportContractFixtureFile{
                  .name = "c.fleaux",
                  .source = "import Std;\n"
                            "let CFn(x: Float64): Float64 = (x, 1.0) -> Std.Add;\n",
              },
              fleaux::tests::ImportContractFixtureFile{
                  .name = "b.fleaux",
                  .source = "import c;\n"
                            "let BFn(x: Float64): Float64 = (x) -> CFn;\n",
              },
              fleaux::tests::ImportContractFixtureFile{
                  .name = "a.fleaux",
                  .source = "import Std;\n"
                            "import b;\n"
                            "(1.0) -> CFn -> Std.Println;\n",
              },
          },
          .expected_outcome = fleaux::tests::ImportContractOutcomeClass::kUnresolvedSymbol,
          .expected_fragments = {"CFn"},
      },
      fleaux::tests::ImportContractCase{
          .name = "qualified_export_unresolved",
          .entry_file = "typed_entry_qualified.fleaux",
          .files = {
              fleaux::tests::ImportContractFixtureFile{
                  .name = "typed_dep_qualified.fleaux",
                  .source = "import Std;\n"
                            "let MyMath.Add4(x: Float64): Float64 = (4.0, x) -> Std.Add;\n",
              },
              fleaux::tests::ImportContractFixtureFile{
                  .name = "typed_entry_qualified.fleaux",
                  .source = "import Std;\n"
                            "import typed_dep_qualified;\n"
                            "(1.0) -> WrongMath.Add4 -> Std.Println;\n",
              },
          },
          .expected_outcome = fleaux::tests::ImportContractOutcomeClass::kUnresolvedSymbol,
          .expected_fragments = {"WrongMath.Add4"},
      },
      fleaux::tests::ImportContractCase{
          .name = "qualified_overload_success",
          .entry_file = "typed_entry_overloads.fleaux",
          .files = {
              fleaux::tests::ImportContractFixtureFile{
                  .name = "typed_dep_overloads.fleaux",
                  .source = "import Std;\n"
                            "let MyMath.Echo(x: Int64): Int64 = (x, 1) -> Std.Add;\n"
                            "let MyMath.Echo(x: String): String = x;\n",
              },
              fleaux::tests::ImportContractFixtureFile{
                  .name = "typed_entry_overloads.fleaux",
                  .source = "import Std;\n"
                            "import typed_dep_overloads;\n"
                            "(1) -> MyMath.Echo -> Std.Println;\n"
                            "(\"ok\") -> MyMath.Echo -> Std.Println;\n",
              },
          },
          .expected_outcome = fleaux::tests::ImportContractOutcomeClass::kSuccess,
          .expected_fragments = {},
          .execute_vm = true,
      },
  };

  for (const auto& contract_case : cases) {
    const auto case_dir = temp_dir / contract_case.name;
    std::filesystem::remove_all(case_dir);
    std::filesystem::create_directories(case_dir);

    for (const auto& file : contract_case.files) {
      fleaux::tests::write_text_file(case_dir / file.name, file.source);
    }

    const auto entry_path = case_dir / contract_case.entry_file;
    const auto vm_load_result = fleaux::bytecode::load_linked_module(entry_path);

    INFO("contract case: " << contract_case.name);

    if (contract_case.expected_outcome == fleaux::tests::ImportContractOutcomeClass::kSuccess) {
      if (!vm_load_result) { INFO("vm load error: " << vm_load_result.error().message); }
      REQUIRE(vm_load_result.has_value());

      if (contract_case.execute_vm) {
        const fleaux::vm::Runtime runtime;
        const auto runtime_result = runtime.execute(*vm_load_result);
        if (!runtime_result) { INFO("vm runtime error: " << runtime_result.error().message); }
        REQUIRE(runtime_result.has_value());
      }
      continue;
    }

    REQUIRE_FALSE(vm_load_result.has_value());

    const auto vm_text = vm_load_result.error().message;
    INFO("vm text: " << vm_text);
    REQUIRE(fleaux::tests::classify_import_contract_message(vm_text) == contract_case.expected_outcome);

    for (const auto& fragment : contract_case.expected_fragments) {
      REQUIRE(vm_text.find(fragment) != std::string::npos);
    }
  }
}


TEST_CASE("RuntimeSession resolves normal imports relative to the working directory", "[vm][imports][contract][repl]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_repl_import_contract";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  fleaux::tests::write_text_file(temp_dir / "custom_module.fleaux",
                                 "import Std;\n"
                                 "let Add4(x: Int64): Int64 = (x, 4) -> Std.Add;\n");

  fleaux::tests::CurrentPathScope current_path_scope(temp_dir);
  const fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session({});
  std::ostringstream output;

  const auto std_result = session.run_snippet("import Std;\nlet Id(x: Float64): Float64 = x;\n", output);
  if (!std_result) { INFO("Std import error: " << std_result.error().message); }
  REQUIRE(std_result.has_value());

  const auto std_builtins_result = session.run_snippet("import StdBuiltins;\n", output);
  REQUIRE_FALSE(std_builtins_result.has_value());
  REQUIRE(std_builtins_result.error().message.find("import-unresolved:") != std::string::npos);
  REQUIRE(std_builtins_result.error().message.find("StdBuiltins") != std::string::npos);

  const auto import_result = session.run_snippet("import custom_module;\n(3) -> Add4 -> Std.Println;\n", output);
  if (!import_result) { INFO("normal import error: " << import_result.error().message); }
  REQUIRE(import_result.has_value());

  const auto reuse_result = session.run_snippet("(5) -> Add4 -> Std.Println;\n", output);
  if (!reuse_result) { INFO("reused imported let error: " << reuse_result.error().message); }
  REQUIRE(reuse_result.has_value());
  REQUIRE(output.str() == "7\n9\n");

  const auto unresolved_result = session.run_snippet("import missing_dep;\n", output);
  REQUIRE_FALSE(unresolved_result.has_value());
  REQUIRE(unresolved_result.error().message.find("import-unresolved:") != std::string::npos);
  REQUIRE(unresolved_result.error().message.find("missing_dep") != std::string::npos);
  REQUIRE(unresolved_result.error().hint.has_value());
  REQUIRE(unresolved_result.error().hint->find(temp_dir.string()) != std::string::npos);
}



