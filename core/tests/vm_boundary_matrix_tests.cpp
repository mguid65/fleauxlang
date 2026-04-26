#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/bytecode/module.hpp"
#include "fleaux/bytecode/module_loader.hpp"
#include "fleaux/bytecode/opcode.hpp"
#include "fleaux/vm/builtin_catalog.hpp"
#include "fleaux/vm/runtime.hpp"

namespace {

using fleaux::bytecode::Instruction;
using fleaux::bytecode::Module;
using fleaux::bytecode::Opcode;

struct OpcodeBoundaryCase {
  Opcode opcode;
  std::int64_t operand;
  bool expect_success;
  std::string_view expected_error_substr;
};

constexpr auto kOpcodeBoundaryCases = std::to_array<OpcodeBoundaryCase>({
    {Opcode::kNoOp, 0, true, ""},
    {Opcode::kPushConst, 0, false, "constant pool index out of range"},
    {Opcode::kPop, 0, false, "pop"},
    {Opcode::kDup, 0, false, "dup"},
    {Opcode::kBuildTuple, 1, false, "build_tuple"},
    {Opcode::kMakeValueRef, 0, false, "make_value_ref"},
    {Opcode::kDerefValueRef, 0, false, "deref_value_ref"},
    {Opcode::kCallBuiltin, 0, false, "builtin index out of range"},
    {Opcode::kCallUserFunc, 0, false, "function index out of range"},
    {Opcode::kReturn, 0, false, "return"},
    {Opcode::kLoadLocal, 0, false, "local slot index out of range"},
    {Opcode::kMakeUserFuncRef, 0, false, "function index out of range"},
    {Opcode::kMakeBuiltinFuncRef, 0, false, "builtin index out of range"},
    {Opcode::kMakeClosureRef, 0, false, "closure index out of range"},
    {Opcode::kJump, 99, false, "jump target out of range"},
    {Opcode::kJumpIf, 99, false, "jump_if"},
    {Opcode::kJumpIfNot, 99, false, "jump_if_not"},
    {Opcode::kAdd, 0, false, "add"},
    {Opcode::kSub, 0, false, "sub"},
    {Opcode::kMul, 0, false, "mul"},
    {Opcode::kDiv, 0, false, "div"},
    {Opcode::kMod, 0, false, "mod"},
    {Opcode::kPow, 0, false, "pow"},
    {Opcode::kNeg, 0, false, "neg"},
    {Opcode::kCmpEq, 0, false, "cmp_eq"},
    {Opcode::kCmpNe, 0, false, "cmp_ne"},
    {Opcode::kCmpLt, 0, false, "cmp_lt"},
    {Opcode::kCmpGt, 0, false, "cmp_gt"},
    {Opcode::kCmpLe, 0, false, "cmp_le"},
    {Opcode::kCmpGe, 0, false, "cmp_ge"},
    {Opcode::kAnd, 0, false, "and"},
    {Opcode::kOr, 0, false, "or"},
    {Opcode::kNot, 0, false, "not"},
    {Opcode::kSelect, 0, false, "select"},
    {Opcode::kBranchCall, 0, false, "branch_call"},
    {Opcode::kLoopCall, 0, false, "loop_call"},
    {Opcode::kLoopNCall, 0, false, "loop_n_call"},
    {Opcode::kHalt, 0, true, ""},
});

auto make_opcode_boundary_module(const OpcodeBoundaryCase& boundary_case) -> Module {
  Module bytecode_module;
  bytecode_module.instructions = {
      Instruction{.opcode = boundary_case.opcode, .operand = boundary_case.operand},
      Instruction{.opcode = Opcode::kHalt, .operand = 0},
  };
  return bytecode_module;
}

enum class BuiltinBoundaryMode {
  kInvokeEmptyTuple,
  kMakeRefOnly,
};

auto make_builtin_names() -> std::vector<std::string_view> {
  std::vector<std::string_view> names;
  names.reserve(256);
#define FLEAUX_BUILTIN_NAME_COLLECTOR(name_literal, node_type) names.push_back(std::string_view{name_literal});
  FLEAUX_VM_BUILTINS(FLEAUX_BUILTIN_NAME_COLLECTOR)
#undef FLEAUX_BUILTIN_NAME_COLLECTOR
#define FLEAUX_BUILTIN_FUNCTION_NAME_COLLECTOR(name_literal, builtin_function) names.push_back(std::string_view{name_literal});
  FLEAUX_VM_FUNCTION_BUILTINS(FLEAUX_BUILTIN_FUNCTION_NAME_COLLECTOR)
#undef FLEAUX_BUILTIN_FUNCTION_NAME_COLLECTOR
  return names;
}

auto make_ref_only_boundary_builtins() -> std::unordered_set<std::string_view> {
  return {
      "Std.Input",
      "Std.Exit",
      "Std.OS.Exec",
  };
}

auto make_builtin_boundary_module(const std::vector<std::string_view>& names, const std::size_t index,
                                  const BuiltinBoundaryMode mode) -> Module {
  Module bytecode_module;
  bytecode_module.builtin_names.reserve(names.size());
  for (const auto name : names) { bytecode_module.builtin_names.emplace_back(name); }

  if (mode == BuiltinBoundaryMode::kMakeRefOnly) {
    bytecode_module.instructions = {
        Instruction{.opcode = Opcode::kMakeBuiltinFuncRef, .operand = static_cast<std::int64_t>(index)},
        Instruction{.opcode = Opcode::kPop, .operand = 0},
        Instruction{.opcode = Opcode::kHalt, .operand = 0},
    };
    return bytecode_module;
  }

  bytecode_module.instructions = {
      Instruction{.opcode = Opcode::kBuildTuple, .operand = 0},
      Instruction{.opcode = Opcode::kCallBuiltin, .operand = static_cast<std::int64_t>(index)},
      Instruction{.opcode = Opcode::kPop, .operand = 0},
      Instruction{.opcode = Opcode::kHalt, .operand = 0},
  };
  return bytecode_module;
}

struct ImportBoundaryCase {
  std::string case_name;
  std::string entry_file;
  std::vector<std::pair<std::string, std::string>> files;
  std::string expected_token;
  std::string expected_detail;
};

void write_module_file(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path);
  REQUIRE(out.good());
  out << text;
  REQUIRE(out.good());
}

}  // namespace

TEST_CASE("VM opcode boundary matrix", "[vm][boundary][opcode]") {
  const fleaux::vm::Runtime runtime;

  for (const auto& boundary_case : kOpcodeBoundaryCases) {
    const auto bytecode_module = make_opcode_boundary_module(boundary_case);
    std::ostringstream output;
    const auto result = runtime.execute(bytecode_module, output);

    INFO("opcode boundary case index: " << static_cast<int>(boundary_case.opcode));
    if (boundary_case.expect_success) {
      REQUIRE(result.has_value());
      continue;
    }

    REQUIRE_FALSE(result.has_value());
    INFO("opcode boundary error: " << result.error().message);
    REQUIRE_FALSE(result.error().message.empty());
    if (!boundary_case.expected_error_substr.empty()) {
      REQUIRE(result.error().message.find(boundary_case.expected_error_substr) != std::string::npos);
    }
  }
}

TEST_CASE("VM builtin boundary matrix", "[vm][boundary][builtin]") {
  const auto names = make_builtin_names();
  const auto ref_only = make_ref_only_boundary_builtins();
  const fleaux::vm::Runtime runtime;

  for (std::size_t index = 0; index < names.size(); ++index) {
    const auto name = names[index];
    const auto mode =
        ref_only.contains(name) ? BuiltinBoundaryMode::kMakeRefOnly : BuiltinBoundaryMode::kInvokeEmptyTuple;
    const auto bytecode_module = make_builtin_boundary_module(names, index, mode);

    std::ostringstream output;
    const auto result = runtime.execute(bytecode_module, output);

    INFO("builtin boundary target: " << name);
    if (mode == BuiltinBoundaryMode::kMakeRefOnly) {
      REQUIRE(result.has_value());
      continue;
    }

    if (result.has_value()) {
      SUCCEED();
      continue;
    }

    // Boundary failures must remain attributed to a concrete builtin dispatch.
    const std::string error = result.error().message;
    const std::string needle = std::string{name};
    const bool mentions_builtin_name = error.find(needle) != std::string::npos;
    const bool mentions_builtin_dispatch = error.find("builtin '") != std::string::npos;
    REQUIRE((mentions_builtin_name || mentions_builtin_dispatch));
  }
}

TEST_CASE("VM import boundary matrix", "[vm][boundary][imports][contract]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_vm_boundary_import_matrix";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto cases = std::to_array<ImportBoundaryCase>({
      ImportBoundaryCase{
          .case_name = "import_unresolved",
          .entry_file = "entry_unresolved.fleaux",
          .files = {{
              "entry_unresolved.fleaux",
              "import missing_dep;\n"
              "(1) -> MissingCall;\n",
          }},
          .expected_token = "import-unresolved:",
          .expected_detail = "Import not found: 'missing_dep'",
      },
      ImportBoundaryCase{
          .case_name = "import_cycle",
          .entry_file = "cycle_a.fleaux",
          .files = {
              {
                  "cycle_a.fleaux",
                  "import cycle_b;\n"
                  "let A(x: Float64): Float64 = x;\n",
              },
              {
                  "cycle_b.fleaux",
                  "import cycle_a;\n"
                  "let B(x: Float64): Float64 = x;\n",
              },
          },
          .expected_token = "import-cycle:",
          .expected_detail = "cycle_a.fleaux",
      },
      ImportBoundaryCase{
          .case_name = "import_type_mismatch",
          .entry_file = "typed_entry.fleaux",
          .files = {
              {
                  "typed_dep.fleaux",
                  "let Add4(x: String): Int64 = 4;\n",
              },
              {
                  "typed_entry.fleaux",
                  "import Std;\n"
                  "import typed_dep;\n"
                  "(1) -> Add4 -> Std.Println;\n",
              },
          },
          .expected_token = "Type mismatch in call target arguments",
          .expected_detail = "Add4 expects argument 0",
      },
  });

  for (const auto& boundary_case : cases) {
    const auto case_dir = temp_dir / boundary_case.case_name;
    std::filesystem::remove_all(case_dir);
    std::filesystem::create_directories(case_dir);

    for (const auto& [file_name, source] : boundary_case.files) {
      write_module_file(case_dir / file_name, source);
    }

    const auto loaded = fleaux::bytecode::load_linked_module(case_dir / boundary_case.entry_file);
    INFO("import boundary case: " << boundary_case.case_name);
    REQUIRE_FALSE(loaded.has_value());
    INFO("import boundary error: " << loaded.error().message);
    REQUIRE(loaded.error().message.find(boundary_case.expected_token) != std::string::npos);
    REQUIRE(loaded.error().message.find(boundary_case.expected_detail) != std::string::npos);
  }
}
