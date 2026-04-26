#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include "fleaux/bytecode/compiler.hpp"
#include "fleaux/bytecode/module.hpp"
#include "fleaux/bytecode/module_loader.hpp"
#include "fleaux/bytecode/opcode.hpp"
#include "fleaux/bytecode/optimizer.hpp"
#include "fleaux/bytecode/serialization.hpp"
#include "fleaux/frontend/lowering.hpp"
#include "fleaux/frontend/parser.hpp"
#include "fleaux/vm/builtin_catalog.hpp"
#include "fleaux/vm/runtime.hpp"

#ifndef FLEAUX_REPO_ROOT
#error "FLEAUX_REPO_ROOT must be defined by CMake for bytecode tests."
#endif

namespace {

auto repo_root_path() -> std::filesystem::path { return std::filesystem::path(FLEAUX_REPO_ROOT); }

auto lower_source_to_ir(const std::string& source_text, const std::string& source_name)
    -> fleaux::frontend::ir::IRProgram {
  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(source_text, source_name);
  REQUIRE(parsed.has_value());

  constexpr fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE(lowered.has_value());
  return lowered.value();
}

void require_copy_vs_auto_byref_equivalent(const std::string& source_text, const std::string& source_name,
                                           const std::size_t cutoff = 32) {
  const auto ir_program = lower_source_to_ir(source_text, source_name);

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result_copy = compiler.compile(ir_program, fleaux::bytecode::CompileOptions{
                                                            .enable_value_ref_gate = false,
                                                            .enable_auto_value_ref = false,
                                                        });
  const auto result_byref = compiler.compile(ir_program, fleaux::bytecode::CompileOptions{
                                                             .enable_value_ref_gate = true,
                                                             .enable_auto_value_ref = true,
                                                             .value_ref_byte_cutoff = cutoff,
                                                         });

  REQUIRE(result_copy.has_value());
  REQUIRE(result_byref.has_value());

  std::ostringstream output_copy;
  std::ostringstream output_byref;
  constexpr fleaux::vm::Runtime runtime;

  const auto exec_copy = runtime.execute(*result_copy, output_copy);
  const auto exec_byref = runtime.execute(*result_byref, output_byref);

  REQUIRE(exec_copy.has_value());
  REQUIRE(exec_byref.has_value());
  REQUIRE(output_copy.str() == output_byref.str());
}

auto make_int_tuple_literal(const std::vector<int>& values) -> std::string {
  std::ostringstream out;
  out << "(";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) out << ", ";
    out << values[i];
  }
  out << ")";
  return out.str();
}

auto make_task_spawn_tuple_literal(const std::vector<int>& values) -> std::string {
  std::ostringstream out;
  out << "(";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0) out << ", ";
    out << "(Inc, " << values[i] << ") -> Std.Task.Spawn";
  }
  out << ")";
  return out.str();
}

auto parse_std_declared_names(const bool builtins_only) -> std::set<std::string> {
  std::ifstream in(repo_root_path() / "Std.fleaux");
  REQUIRE(in.good());

  std::set<std::string> names;
  std::string line;
  // Accumulate a logical declaration that may span multiple physical lines.
  // A declaration starts with "let Std." and ends at the first ";".
  std::string decl;
  while (std::getline(in, line)) {
    if (decl.empty()) {
      if (line.find("let Std.") == std::string::npos) { continue; }
      decl = line;
    } else {
      decl += ' ';
      decl += line;
    }

    // Wait until the statement terminator is present.
    if (decl.find(';') == std::string::npos) { continue; }

    const auto let_pos = decl.find("let Std.");

    if (const bool is_builtin = decl.find(":: __builtin__;") != std::string::npos; builtins_only == is_builtin) {
      const auto open_paren = decl.find('(', let_pos);
      REQUIRE(open_paren != std::string::npos);
      auto name = decl.substr(let_pos + 4, open_paren - (let_pos + 4));
      if (const auto generic_start = name.find('<'); generic_start != std::string::npos) {
        name = name.substr(0, generic_start);
      }
      names.insert(std::move(name));
    }

    decl.clear();
  }

  return names;
}

auto vm_catalog_builtin_names() -> std::set<std::string> {
  std::set<std::string> names;
  for (const auto& spec : fleaux::vm::all_callable_builtin_specs()) { names.insert(std::string{spec.name}); }
  return names;
}

auto vm_catalog_constant_names() -> std::set<std::string> {
  std::set<std::string> names;
  for (const auto& spec : fleaux::vm::all_constant_builtin_specs()) { names.insert(std::string{spec.name}); }
  return names;
}

}  // namespace

TEST_CASE("VM builtin catalog stays in sync with Std.fleaux", "[bytecode]") {
  REQUIRE(vm_catalog_builtin_names() == parse_std_declared_names(true));
  REQUIRE(vm_catalog_constant_names() == parse_std_declared_names(false));
}

TEST_CASE("VM builtin catalog resolves overloaded stdlib symbol keys", "[bytecode][builtins][overload]") {
  REQUIRE(fleaux::vm::builtin_id_from_symbol_key("Std.Dict.Create#0") == fleaux::vm::BuiltinId::DictCreateVoid);
  REQUIRE(fleaux::vm::builtin_id_from_symbol_key("Std.Dict.Create#1") == fleaux::vm::BuiltinId::DictCreateDict);
  REQUIRE(fleaux::vm::builtin_id_from_symbol_key("Std.Exit#0") == fleaux::vm::BuiltinId::ExitVoid);
  REQUIRE(fleaux::vm::builtin_id_from_symbol_key("Std.Exit#1") == fleaux::vm::BuiltinId::ExitInt64);
}

// ---------------------------------------------------------------------------
// Core pipeline: (4, 5) -> Std.Add -> Std.Println
// Expected codegen:
//   [0] kPushConst     idx(4)
//   [1] kPushConst     idx(5)
//   [2] kBuildTuple    2
//   [3] kCallBuiltin   idx(Std.Add)      = 0
//   [4] kCallBuiltin   idx(Std.Println)  = 1
//   [5] kPop
//   [6] kHalt
// ---------------------------------------------------------------------------
TEST_CASE("Bytecode compiler emits pipeline with BuildTuple and CallBuiltin", "[bytecode]") {
  const auto ir_program = lower_source_to_ir("(4, 5) -> Std.Add -> Std.Println;\n", "bytecode_pipeline.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  const auto& instr = result->instructions;
  REQUIRE(instr.size() == 7);

  REQUIRE(instr[0].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(std::get<std::int64_t>(result->constants.at(static_cast<std::size_t>(instr[0].operand)).data) == 4);

  REQUIRE(instr[1].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(std::get<std::int64_t>(result->constants.at(static_cast<std::size_t>(instr[1].operand)).data) == 5);

  REQUIRE(instr[2].opcode == fleaux::bytecode::Opcode::kBuildTuple);
  REQUIRE(instr[2].operand == 2);

  REQUIRE(instr[3].opcode == fleaux::bytecode::Opcode::kCallBuiltin);
  REQUIRE(fleaux::vm::builtin_name(*fleaux::vm::builtin_id_from_operand(instr[3].operand)) == "Std.Add");

  REQUIRE(instr[4].opcode == fleaux::bytecode::Opcode::kCallBuiltin);
  REQUIRE(fleaux::vm::builtin_name(*fleaux::vm::builtin_id_from_operand(instr[4].operand)) == "Std.Println");

  REQUIRE(instr[5].opcode == fleaux::bytecode::Opcode::kPop);
  REQUIRE(instr[6].opcode == fleaux::bytecode::Opcode::kHalt);
}

TEST_CASE("Bytecode compiler dispatches direct calls to the resolved user overload", "[bytecode][overload]") {
  const auto ir_program = lower_source_to_ir(
      "let Echo(x: Int64): Int64 = x;\n"
      "let Echo(x: String): String = x;\n"
      "(1) -> Echo;\n"
      "(\"ok\") -> Echo;\n",
      "bytecode_user_overloads.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);
  REQUIRE(result.has_value());
  REQUIRE(result->functions.size() == 2U);
  REQUIRE(result->functions[0].name == "Echo#0");
  REQUIRE(result->functions[1].name == "Echo#1");

  std::vector<std::int64_t> called_user_indices;
  for (const auto& instruction : result->instructions) {
    if (instruction.opcode == fleaux::bytecode::Opcode::kCallUserFunc) {
      called_user_indices.push_back(instruction.operand);
    }
  }

  REQUIRE(called_user_indices.size() == 2U);
  REQUIRE(called_user_indices[0] == 0);
  REQUIRE(called_user_indices[1] == 1);
}

TEST_CASE("Bytecode compiler dispatches direct calls to the resolved stdlib builtin overload", "[bytecode][builtins][overload]") {
  const auto ir_program = lower_source_to_ir(
      "let Std.Dict.Create(): Dict(Any, Any) :: __builtin__;\n"
      "let Std.Dict.Create<K, V>(dict: Dict(K, V)): Dict(K, V) :: __builtin__;\n"
      "let Std.Dict.Set<K, V>(dict: Dict(K, V), key: K, value: V): Dict(K, V) :: __builtin__;\n"
      "let MakeDict(): Dict(String, Int64) = (() -> Std.Dict.Create, \"a\", 1) -> Std.Dict.Set;\n"
      "() -> Std.Dict.Create;\n"
      "() -> MakeDict -> Std.Dict.Create;\n",
      "bytecode_builtin_overloads.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);
  REQUIRE(result.has_value());

  std::vector<fleaux::vm::BuiltinId> called_builtin_ids;
  const auto collect_builtins = [&](const auto& instructions) {
    for (const auto& instruction : instructions) {
      if (instruction.opcode != fleaux::bytecode::Opcode::kCallBuiltin) { continue; }
      const auto builtin_id = fleaux::vm::builtin_id_from_operand(instruction.operand);
      REQUIRE(builtin_id.has_value());
      called_builtin_ids.push_back(*builtin_id);
    }
  };

  collect_builtins(result->instructions);
  for (const auto& function : result->functions) { collect_builtins(function.instructions); }

  REQUIRE(std::ranges::find(called_builtin_ids, fleaux::vm::BuiltinId::DictCreateVoid) != called_builtin_ids.end());
  REQUIRE(std::ranges::find(called_builtin_ids, fleaux::vm::BuiltinId::DictCreateDict) != called_builtin_ids.end());
}

TEST_CASE("Bytecode compiler emits native opcode for binary operator shorthand", "[bytecode]") {
  const auto ir_program = lower_source_to_ir("(4, 5) -> + -> Std.Println;\n", "bytecode_native_add.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  const auto& instr = result->instructions;
  REQUIRE(instr.size() == 6);
  REQUIRE(instr[0].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(std::get<std::int64_t>(result->constants.at(static_cast<std::size_t>(instr[0].operand)).data) == 4);
  REQUIRE(instr[1].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(std::get<std::int64_t>(result->constants.at(static_cast<std::size_t>(instr[1].operand)).data) == 5);
  REQUIRE(instr[2].opcode == fleaux::bytecode::Opcode::kAdd);
  REQUIRE(instr[3].opcode == fleaux::bytecode::Opcode::kCallBuiltin);
  REQUIRE(fleaux::vm::builtin_name(*fleaux::vm::builtin_id_from_operand(instr[3].operand)) == "Std.Println");
  REQUIRE(instr[4].opcode == fleaux::bytecode::Opcode::kPop);
  REQUIRE(instr[5].opcode == fleaux::bytecode::Opcode::kHalt);
}

TEST_CASE("Bytecode compiler rejects explicit Std.Ref and Std.Deref without catalog entries", "[bytecode][value_ref]") {
  // Std.Ref and Std.Deref are internal by-ref helpers. The VM now resolves builtins
  // through the explicit BuiltinId catalog, so user-authored builtin declarations do
  // not manufacture new runtime builtin IDs.
  const auto ir_program = lower_source_to_ir(
      "let Std.Ref(value: Any): Any :: __builtin__;\n"
      "let Std.Deref(value_ref: Any): Any :: __builtin__;\n"
      "let Std.Println(args: Any...): Tuple(Any...) :: __builtin__;\n"
      "((1, 2, 3)) -> Std.Ref -> Std.Deref -> Std.Println;\n",
      "bytecode_value_ref_explicit_always_builtin.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;

  for (const bool gate_on : {false, true}) {
    const auto result = compiler.compile(ir_program, fleaux::bytecode::CompileOptions{
                                                         .enable_value_ref_gate = gate_on,
                                                         .enable_auto_value_ref = false,
                                                     });
    INFO("enable_value_ref_gate = " << gate_on);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message == "Unknown builtin in bytecode compiler: 'Std.Ref'.");
  }
}

TEST_CASE("Bytecode compiler rejects auto value-ref when gate is disabled", "[bytecode][value_ref][auto]") {
  const auto ir_program = lower_source_to_ir(
      "let Echo(x: String): String = x;\n"
      "(\"hello\") -> Echo -> Std.Println;\n",
      "bytecode_value_ref_invalid_option_gate.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program, fleaux::bytecode::CompileOptions{
                                                       .enable_value_ref_gate = false,
                                                       .enable_auto_value_ref = true,
                                                       .value_ref_byte_cutoff = 32,
                                                   });
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message.find("enable_auto_value_ref requires enable_value_ref_gate=true") !=
          std::string::npos);
}

TEST_CASE("Bytecode compiler rejects zero value_ref_byte_cutoff when auto by-ref is enabled",
          "[bytecode][value_ref][auto]") {
  const auto ir_program = lower_source_to_ir(
      "let Echo(x: String): String = x;\n"
      "(\"hello\") -> Echo -> Std.Println;\n",
      "bytecode_value_ref_invalid_option_cutoff.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program, fleaux::bytecode::CompileOptions{
                                                       .enable_value_ref_gate = true,
                                                       .enable_auto_value_ref = true,
                                                       .value_ref_byte_cutoff = 0,
                                                   });
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message.find("value_ref_byte_cutoff must be > 0") != std::string::npos);
}

TEST_CASE("Bytecode compiler auto-emits value-ref ops for large safe user-function arguments",
          "[bytecode][value_ref][auto]") {
  const auto ir_program = lower_source_to_ir(
      "let Echo(x: String): String = x;\n"
      "(\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\")"
      " -> Echo -> Std.Println;\n",
      "bytecode_value_ref_auto_on.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program, fleaux::bytecode::CompileOptions{
                                                       .enable_value_ref_gate = true,
                                                       .enable_auto_value_ref = true,
                                                       .value_ref_byte_cutoff = 32,
                                                   });
  REQUIRE(result.has_value());

  bool saw_make_value_ref = false;
  bool saw_call_user_func = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kMakeValueRef) { saw_make_value_ref = true; }
    if (ins.opcode == fleaux::bytecode::Opcode::kCallUserFunc) { saw_call_user_func = true; }
  }
  REQUIRE(saw_make_value_ref);
  REQUIRE(saw_call_user_func);

  const auto echo_it = std::find_if(result->functions.begin(), result->functions.end(),
                                    [](const auto& fn) { return fn.name == "Echo#0"; });
  REQUIRE(echo_it != result->functions.end());

  bool saw_deref_value_ref = false;
  for (const auto& ins : echo_it->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kDerefValueRef) {
      saw_deref_value_ref = true;
      break;
    }
  }
  REQUIRE(saw_deref_value_ref);
}

TEST_CASE("Bytecode compiler keeps copy semantics when auto by-ref safety/size checks fail",
          "[bytecode][value_ref][auto]") {
  const auto ir_program = lower_source_to_ir(
      "let Echo(x: String): String = x;\n"
      "(\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\")"
      " -> Echo -> Std.Println;\n"
      "(\"tiny\") -> Echo -> Std.Println;\n",
      "bytecode_value_ref_auto_fallback.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program, fleaux::bytecode::CompileOptions{
                                                       .enable_value_ref_gate = true,
                                                       .enable_auto_value_ref = true,
                                                       .value_ref_byte_cutoff = 32,
                                                   });
  REQUIRE(result.has_value());

  bool saw_make_value_ref = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kMakeValueRef) {
      saw_make_value_ref = true;
      break;
    }
  }
  REQUIRE_FALSE(saw_make_value_ref);

  const auto echo_it = std::find_if(result->functions.begin(), result->functions.end(),
                                    [](const auto& fn) { return fn.name == "Echo#0"; });
  REQUIRE(echo_it != result->functions.end());

  bool saw_deref_value_ref = false;
  for (const auto& ins : echo_it->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kDerefValueRef) {
      saw_deref_value_ref = true;
      break;
    }
  }
  REQUIRE_FALSE(saw_deref_value_ref);
}

TEST_CASE("Bytecode compiler keeps copy semantics when parameter flows through Std.Apply",
          "[bytecode][value_ref][auto]") {
  const auto ir_program = lower_source_to_ir(
      "let Echo(x: String): String = x;\n"
      "let IndirectApply(f: Any, v: String): String = (v, f) -> Std.Apply;\n"
      "(Echo, "
      "\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\")"
      " -> IndirectApply -> Std.Println;\n",
      "bytecode_value_ref_escape_apply.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program, fleaux::bytecode::CompileOptions{
                                                       .enable_value_ref_gate = true,
                                                       .enable_auto_value_ref = true,
                                                       .value_ref_byte_cutoff = 32,
                                                   });
  REQUIRE(result.has_value());

  bool saw_make_value_ref = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kMakeValueRef) {
      saw_make_value_ref = true;
      break;
    }
  }
  REQUIRE_FALSE(saw_make_value_ref);

  const auto indirect_it = std::find_if(result->functions.begin(), result->functions.end(),
                                        [](const auto& fn) { return fn.name == "IndirectApply#0"; });
  REQUIRE(indirect_it != result->functions.end());

  bool saw_deref_value_ref = false;
  for (const auto& ins : indirect_it->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kDerefValueRef) {
      saw_deref_value_ref = true;
      break;
    }
  }
  REQUIRE_FALSE(saw_deref_value_ref);
}

TEST_CASE("Bytecode compiler keeps copy semantics through Parallel and Task escape builtins",
          "[bytecode][value_ref][auto][concurrency]") {
  const fleaux::bytecode::BytecodeCompiler compiler;

  const auto assert_no_value_ref_opcodes = [&](const std::string& source_text, const std::string& source_name,
                                               const std::string& wrapper_name) {
    INFO("source=" << source_name << ", wrapper=" << wrapper_name);
    const auto ir_program = lower_source_to_ir(source_text, source_name);
    const auto result = compiler.compile(ir_program, fleaux::bytecode::CompileOptions{
                                                         .enable_value_ref_gate = true,
                                                         .enable_auto_value_ref = true,
                                                         .value_ref_byte_cutoff = 32,
                                                     });
    REQUIRE(result.has_value());

    bool saw_make_value_ref = false;
    for (const auto& ins : result->instructions) {
      if (ins.opcode == fleaux::bytecode::Opcode::kMakeValueRef) {
        saw_make_value_ref = true;
        break;
      }
    }
    REQUIRE_FALSE(saw_make_value_ref);

    const auto wrapper_it = std::find_if(result->functions.begin(), result->functions.end(),
                                         [&](const auto& fn) { return fn.name == wrapper_name + "#0"; });
    REQUIRE(wrapper_it != result->functions.end());

    bool saw_deref_value_ref = false;
    for (const auto& ins : wrapper_it->instructions) {
      if (ins.opcode == fleaux::bytecode::Opcode::kDerefValueRef) {
        saw_deref_value_ref = true;
        break;
      }
    }
    REQUIRE_FALSE(saw_deref_value_ref);
  };

  SECTION("Std.Parallel.Map") {
    assert_no_value_ref_opcodes(
        "let Echo(x: String): String = x;\n"
        "let EscapeViaParallelMap(v: String): Result(Tuple(String...), Tuple(Int64, String)) = ((v), Echo) -> "
        "Std.Parallel.Map;\n"
        "(\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\")"
        " -> EscapeViaParallelMap -> Std.Result.Unwrap -> Std.Println;\n",
        "bytecode_value_ref_escape_parallel_map.fleaux", "EscapeViaParallelMap");
  }

  SECTION("Std.Parallel.WithOptions") {
    assert_no_value_ref_opcodes(
        "let Echo(x: String): String = x;\n"
        "let BuildOptions(): Dict(String, Any) = () -> Std.Dict.Create -> (_, \"max_workers\", 2) -> Std.Dict.Set;\n"
        "let EscapeViaParallelWithOptions(v: String): Result(Tuple(String...), Tuple(Int64, String)) = ((v), Echo, "
        "() -> BuildOptions) -> Std.Parallel.WithOptions;\n"
        "(\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\")"
        " -> EscapeViaParallelWithOptions -> Std.Result.Unwrap -> Std.Println;\n",
        "bytecode_value_ref_escape_parallel_with_options.fleaux", "EscapeViaParallelWithOptions");
  }

  SECTION("Std.Parallel.ForEach") {
    assert_no_value_ref_opcodes(
        "let Echo(x: String): String = x;\n"
        "let EscapeViaParallelForEach(v: String): Result(Tuple(), Tuple(Int64, String)) = ((v), Echo) -> "
        "Std.Parallel.ForEach;\n"
        "(\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\")"
        " -> EscapeViaParallelForEach -> Std.Result.IsOk -> Std.Println;\n",
        "bytecode_value_ref_escape_parallel_foreach.fleaux", "EscapeViaParallelForEach");
  }

  SECTION("Std.Parallel.Reduce") {
    assert_no_value_ref_opcodes(
        "let Merge(acc: String, x: String): String = (acc, x) -> Std.Add;\n"
        "let EscapeViaParallelReduce(v: String): Result(String, Tuple(Int64, String)) = ((v), \"\", Merge) -> "
        "Std.Parallel.Reduce;\n"
        "(\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\")"
        " -> EscapeViaParallelReduce -> Std.Result.Unwrap -> Std.Println;\n",
        "bytecode_value_ref_escape_parallel_reduce.fleaux", "EscapeViaParallelReduce");
  }

  SECTION("Std.Task.Spawn") {
    assert_no_value_ref_opcodes(
        "let Echo(x: String): String = x;\n"
        "let EscapeViaTaskSpawn(v: String): TaskHandle = (Echo, v) -> Std.Task.Spawn;\n"
        "(\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\")"
        " -> EscapeViaTaskSpawn -> Std.Task.Await -> Std.Result.Unwrap -> Std.Println;\n",
        "bytecode_value_ref_escape_task_spawn.fleaux", "EscapeViaTaskSpawn");
  }
}

TEST_CASE("Auto by-ref semantic equivalence: tuple passthrough same result with/without optimization",
          "[bytecode][value_ref][semantic_equiv]") {
  const auto source =
      "let Echo(x: Tuple(Float64...)): Tuple(Float64...) = x;\n"
      "((1.0, 2.0, 3.0, 4.0, 5.0)) -> Echo -> Std.Println;\n";

  const auto ir_program = lower_source_to_ir(source, "bytecode_semantic_equiv_tuple.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result_copy = compiler.compile(ir_program, fleaux::bytecode::CompileOptions{
                                                            .enable_value_ref_gate = false,
                                                            .enable_auto_value_ref = false,
                                                        });
  const auto result_byref = compiler.compile(ir_program, fleaux::bytecode::CompileOptions{
                                                             .enable_value_ref_gate = true,
                                                             .enable_auto_value_ref = true,
                                                             .value_ref_byte_cutoff = 32,
                                                         });

  REQUIRE(result_copy.has_value());
  REQUIRE(result_byref.has_value());

  std::ostringstream output_copy, output_byref;
  const fleaux::vm::Runtime runtime;

  const auto exec_copy = runtime.execute(*result_copy, output_copy);
  const auto exec_byref = runtime.execute(*result_byref, output_byref);

  REQUIRE(exec_copy.has_value());
  REQUIRE(exec_byref.has_value());

  REQUIRE(output_copy.str() == output_byref.str());
}

TEST_CASE("Auto by-ref semantic equivalence: Parallel and Task call shapes",
          "[bytecode][value_ref][semantic_equiv][concurrency]") {
  SECTION("Parallel.Map") {
    require_copy_vs_auto_byref_equivalent(
        "let Inc(x: Float64): Float64 = (x, 1) -> Std.Add;\n"
        "((1, 2, 3, 4), Inc) -> Std.Parallel.Map -> Std.Result.Unwrap -> Std.Println;\n",
        "bytecode_semantic_equiv_parallel_map.fleaux");
  }

  SECTION("Parallel.WithOptions") {
    require_copy_vs_auto_byref_equivalent(
        "let Inc(x: Float64): Float64 = (x, 1) -> Std.Add;\n"
        "let BuildOptions(): Dict(String, Any) = () -> Std.Dict.Create -> (_, \"max_workers\", 2) -> Std.Dict.Set;\n"
        "((1, 2, 3, 4), Inc, () -> BuildOptions) -> Std.Parallel.WithOptions -> Std.Result.Unwrap -> Std.Println;\n",
        "bytecode_semantic_equiv_parallel_with_options.fleaux");
  }

  SECTION("Parallel.ForEach") {
    require_copy_vs_auto_byref_equivalent(
        "let Inc(x: Float64): Float64 = (x, 1) -> Std.Add;\n"
        "((10, 20, 30), Inc) -> Std.Parallel.ForEach -> Std.Result.IsOk -> Std.Println;\n",
        "bytecode_semantic_equiv_parallel_foreach.fleaux");
  }

  SECTION("Parallel.Reduce") {
    require_copy_vs_auto_byref_equivalent(
        "let Add(acc: Float64, x: Float64): Float64 = (acc, x) -> Std.Add;\n"
        "((1, 2, 3, 4, 5), 0, Add) -> Std.Parallel.Reduce -> Std.Result.Unwrap -> Std.Println;\n",
        "bytecode_semantic_equiv_parallel_reduce.fleaux");
  }

  SECTION("Task.Spawn + Task.Await") {
    require_copy_vs_auto_byref_equivalent(
        "let Inc(x: Float64): Float64 = (x, 1) -> Std.Add;\n"
        "(Inc, 41) -> Std.Task.Spawn -> Std.Task.Await -> Std.Result.Unwrap -> Std.Println;\n",
        "bytecode_semantic_equiv_task_spawn_await.fleaux");
  }

  SECTION("Task.AwaitAll") {
    require_copy_vs_auto_byref_equivalent(
        "let Inc(x: Float64): Float64 = (x, 1) -> Std.Add;\n"
        "(((Inc, 1) -> Std.Task.Spawn, (Inc, 2) -> Std.Task.Spawn, (Inc, 3) -> Std.Task.Spawn))\n"
        "  -> Std.Task.AwaitAll\n"
        "  -> Std.Result.Unwrap\n"
        "  -> Std.Println;\n",
        "bytecode_semantic_equiv_task_await_all.fleaux");
  }

  SECTION("Task.WithTimeout non-timeout path") {
    require_copy_vs_auto_byref_equivalent(
        "let Inc(x: Float64): Float64 = (x, 1) -> Std.Add;\n"
        "(Inc, 7) -> Std.Task.Spawn -> (_, 500) -> Std.Task.WithTimeout -> Std.Result.Unwrap -> Std.Println;\n",
        "bytecode_semantic_equiv_task_with_timeout.fleaux");
  }
}

TEST_CASE("Auto by-ref property equivalence: randomized Parallel and Task call shapes",
          "[bytecode][value_ref][semantic_equiv][property][concurrency]") {
  std::mt19937 rng(1337);
  std::uniform_int_distribution<int> size_dist(2, 7);
  std::uniform_int_distribution<int> value_dist(-100, 100);

  constexpr int runs = 10;
  for (int run = 0; run < runs; ++run) {
    const int n = size_dist(rng);
    std::vector<int> values;
    values.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) { values.push_back(value_dist(rng)); }

    const std::string tuple_literal = make_int_tuple_literal(values);
    const std::string tasks_literal = make_task_spawn_tuple_literal(values);
    const int max_workers = 1 + (run % 3);

    INFO("run=" << run << ", n=" << n);

    require_copy_vs_auto_byref_equivalent(
        "let Inc(x: Float64): Float64 = (x, 1) -> Std.Add;\n"
        "(" +
            tuple_literal + ", Inc) -> Std.Parallel.Map -> Std.Result.Unwrap -> Std.Println;\n",
        "bytecode_semantic_equiv_property_parallel_map_" + std::to_string(run) + ".fleaux");

    require_copy_vs_auto_byref_equivalent(
        "let Inc(x: Float64): Float64 = (x, 1) -> Std.Add;\n"
        "let BuildOptions(): Dict(String, Any) = () -> Std.Dict.Create -> (_, \"max_workers\", " +
            std::to_string(max_workers) +
            ") -> Std.Dict.Set;\n"
            "(" +
            tuple_literal +
            ", Inc, () -> BuildOptions) -> Std.Parallel.WithOptions -> Std.Result.Unwrap -> Std.Println;\n",
        "bytecode_semantic_equiv_property_parallel_with_options_" + std::to_string(run) + ".fleaux");

    require_copy_vs_auto_byref_equivalent(
        "let Add(acc: Float64, x: Float64): Float64 = (acc, x) -> Std.Add;\n"
        "(" +
            tuple_literal + ", 0, Add) -> Std.Parallel.Reduce -> Std.Result.Unwrap -> Std.Println;\n",
        "bytecode_semantic_equiv_property_parallel_reduce_" + std::to_string(run) + ".fleaux");

    require_copy_vs_auto_byref_equivalent(
        "let Inc(x: Float64): Float64 = (x, 1) -> Std.Add;\n"
        "(" +
            tasks_literal + ") -> Std.Task.AwaitAll -> Std.Result.Unwrap -> Std.Println;\n",
        "bytecode_semantic_equiv_property_task_await_all_" + std::to_string(run) + ".fleaux");
  }
}

TEST_CASE("Auto by-ref benchmark: large tuple processing", "[bytecode][value_ref][benchmark]") {
  const auto ir_program = lower_source_to_ir(
      "let ProcessTuple(t: Tuple(Float64...)): Tuple(Float64...) = t;\n"
      "((1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0,\n"
      "  11.0, 12.0, 13.0, 14.0, 15.0, 16.0, 17.0, 18.0, 19.0, 20.0)) -> ProcessTuple -> Std.Println;\n",
      "bytecode_benchmark_tuple.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result_copy = compiler.compile(ir_program, fleaux::bytecode::CompileOptions{
                                                            .enable_value_ref_gate = false,
                                                            .enable_auto_value_ref = false,
                                                        });
  const auto result_byref = compiler.compile(ir_program, fleaux::bytecode::CompileOptions{
                                                             .enable_value_ref_gate = true,
                                                             .enable_auto_value_ref = true,
                                                             .value_ref_byte_cutoff = 64,
                                                         });

  REQUIRE(result_copy.has_value());
  REQUIRE(result_byref.has_value());

  const fleaux::vm::Runtime runtime;

  BENCHMARK("Tuple processing with copy semantics") {
    std::ostringstream output;
    const auto result = runtime.execute(*result_copy, output);
    REQUIRE(result.has_value());
    return output.str();
  };

  BENCHMARK("Tuple processing with auto by-ref") {
    std::ostringstream output;
    const auto result = runtime.execute(*result_byref, output);
    REQUIRE(result.has_value());
    return output.str();
  };
}

TEST_CASE("Auto by-ref benchmark: pipeline chain with strings", "[bytecode][value_ref][benchmark]") {
  const auto source =
      "let Upper(s: String): String = s;\n"
      "let Repeat(s: String): String = s;\n"
      "(\"The quick brown fox jumps over the lazy dog\") -> Upper -> Repeat -> Std.Println;\n";

  const auto ir_program = lower_source_to_ir(source, "bytecode_benchmark_pipeline.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result_copy = compiler.compile(ir_program, fleaux::bytecode::CompileOptions{
                                                            .enable_value_ref_gate = false,
                                                            .enable_auto_value_ref = false,
                                                        });
  const auto result_byref = compiler.compile(ir_program, fleaux::bytecode::CompileOptions{
                                                             .enable_value_ref_gate = true,
                                                             .enable_auto_value_ref = true,
                                                             .value_ref_byte_cutoff = 16,
                                                         });

  REQUIRE(result_copy.has_value());
  REQUIRE(result_byref.has_value());

  const fleaux::vm::Runtime runtime;

  BENCHMARK("Pipeline chain with copy semantics") {
    std::ostringstream output;
    const auto result = runtime.execute(*result_copy, output);
    REQUIRE(result.has_value());
    return output.str();
  };

  BENCHMARK("Pipeline chain with auto by-ref") {
    std::ostringstream output;
    const auto result = runtime.execute(*result_byref, output);
    REQUIRE(result.has_value());
    return output.str();
  };
}

TEST_CASE("Bytecode compiler emits native opcode for unary operator shorthand", "[bytecode]") {
  const auto ir_program = lower_source_to_ir("(True) -> ! -> Std.Println;\n", "bytecode_native_not.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  const auto& instr = result->instructions;
  REQUIRE(instr.size() == 5);
  REQUIRE(instr[0].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(instr[1].opcode == fleaux::bytecode::Opcode::kNot);
  REQUIRE(instr[2].opcode == fleaux::bytecode::Opcode::kCallBuiltin);
  REQUIRE(fleaux::vm::builtin_name(*fleaux::vm::builtin_id_from_operand(instr[2].operand)) == "Std.Println");
  REQUIRE(instr[3].opcode == fleaux::bytecode::Opcode::kPop);
  REQUIRE(instr[4].opcode == fleaux::bytecode::Opcode::kHalt);
}

TEST_CASE("Bytecode compiler emits kSelect for Std.Select", "[bytecode]") {
  const auto ir_program =
      lower_source_to_ir("(True, 10, 20) -> Std.Select -> Std.Println;\n", "bytecode_native_select.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  const auto& instr = result->instructions;
  bool found_select = false;
  for (const auto& ins : instr) {
    if (ins.opcode == fleaux::bytecode::Opcode::kSelect) { found_select = true; }
  }
  REQUIRE(found_select);
}

TEST_CASE("Bytecode compiler emits kLoopCall for Std.Loop", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(R"(
let Continue(n: Float64): Bool = (n, 0) -> Std.GreaterThan;
let Step(n: Float64): Float64 = (n, 1) -> Std.Subtract;
(10, Continue, Step) -> Std.Loop -> Std.Println;
)",
                                             "bytecode_native_loop.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  bool found_loop_call = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kLoopCall) { found_loop_call = true; }
  }
  REQUIRE(found_loop_call);
}

TEST_CASE("Bytecode compiler emits kLoopNCall for Std.LoopN", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(R"(
let Continue(n: Float64): Bool = (n, 0) -> Std.GreaterThan;
let Step(n: Float64): Float64 = (n, 1) -> Std.Subtract;
(10, Continue, Step, 100) -> Std.LoopN -> Std.Println;
)",
                                             "bytecode_native_loopn.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  bool found_loop_n_call = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kLoopNCall) { found_loop_n_call = true; }
  }
  REQUIRE(found_loop_n_call);
}

TEST_CASE("Bytecode compiler emits kBranchCall for Std.Branch with user function refs", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(R"(
let Inc(x: Float64): Float64 = (x, 1) -> Std.Add;
let Dec(x: Float64): Float64 = (x, 1) -> Std.Subtract;
(True, 10, Inc, Dec) -> Std.Branch -> Std.Println;
)",
                                             "bytecode_native_branch_safe.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  bool found_branch_call = false;
  bool found_builtin_branch = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kBranchCall) { found_branch_call = true; }
    if (ins.opcode == fleaux::bytecode::Opcode::kCallBuiltin) {
      const auto builtin_id = fleaux::vm::builtin_id_from_operand(ins.operand);
      REQUIRE(builtin_id.has_value());
      const auto name = fleaux::vm::builtin_name(*builtin_id);
      if (name == "Std.Branch") { found_builtin_branch = true; }
    }
  }

  REQUIRE(found_branch_call);
  REQUIRE_FALSE(found_builtin_branch);
}

TEST_CASE("Bytecode compiler emits builtin callable refs for Std.Apply value call", "[bytecode]") {
  const auto ir_program = lower_source_to_ir("(5, Std.UnaryMinus) -> Std.Apply -> Std.Println;\n",
                                             "bytecode_builtin_callable_apply.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  bool found_make_builtin_ref = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kMakeBuiltinFuncRef) {
      found_make_builtin_ref = true;
      const auto builtin_id = fleaux::vm::builtin_id_from_operand(ins.operand);
      REQUIRE(builtin_id.has_value());
      const auto name = fleaux::vm::builtin_name(*builtin_id);
      REQUIRE(name == "Std.UnaryMinus");
    }
  }
  REQUIRE(found_make_builtin_ref);
}

TEST_CASE("Bytecode compiler emits kBranchCall for Std.Branch with builtin refs", "[bytecode]") {
  const auto ir_program =
      lower_source_to_ir("(True, 10, Std.UnaryMinus, Std.UnaryPlus) -> Std.Branch -> Std.Println;\n",
                         "bytecode_builtin_callable_branch.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  bool found_branch_call = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kBranchCall) { found_branch_call = true; }
  }
  REQUIRE(found_branch_call);
}

TEST_CASE("Bytecode compiler emits closure materialization for inline closure literals", "[bytecode]") {
  const auto ir_program =
      lower_source_to_ir("(10, (x: Float64): Float64 = (x, 1) -> Std.Add) -> Std.Apply -> Std.Println;\n",
                         "bytecode_inline_closure.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());
  REQUIRE(!result->closures.empty());

  bool found_make_closure = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kMakeClosureRef) { found_make_closure = true; }
  }
  REQUIRE(found_make_closure);
}

TEST_CASE("Bytecode compiler wires captured closure factory function", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(
      "let MakeAdder(n: Float64): Any = (x: Float64): Float64 = (x, n) -> Std.Add;\n"
      "(10, (4) -> MakeAdder) -> Std.Apply -> Std.Println;\n",
      "bytecode_captured_closure.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);
  REQUIRE(result.has_value());

  REQUIRE(result->functions.size() >= 2);
  REQUIRE(!result->closures.empty());

  const auto make_it = std::find_if(result->functions.begin(), result->functions.end(),
                                    [](const fleaux::bytecode::FunctionDef& fn) { return fn.name == "MakeAdder#0"; });
  REQUIRE(make_it != result->functions.end());
  const auto& make_adder_fn = *make_it;
  bool found_capture_load = false;
  bool found_capture_tuple = false;
  bool found_make_closure = false;
  for (const auto& ins : make_adder_fn.instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kLoadLocal) { found_capture_load = true; }
    if (ins.opcode == fleaux::bytecode::Opcode::kBuildTuple) { found_capture_tuple = true; }
    if (ins.opcode == fleaux::bytecode::Opcode::kMakeClosureRef) { found_make_closure = true; }
  }

  REQUIRE(found_capture_load);
  REQUIRE(found_capture_tuple);
  REQUIRE(found_make_closure);
}

TEST_CASE("Bytecode compiler emits variadic metadata for inline closures", "[bytecode]") {
  const auto ir_program =
      lower_source_to_ir("(10, ((head: Float64, tail: Any...): Float64 = head)) -> Std.Apply -> Std.Println;\n",
                         "bytecode_variadic_inline_closure.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);
  REQUIRE(result.has_value());

  REQUIRE(result->closures.size() == 1);
  const auto& closure = result->closures[0];
  REQUIRE(closure.declared_arity == 2);
  REQUIRE(closure.declared_has_variadic_tail);
}

TEST_CASE("Bytecode compiler emits builtin call for Std.Match", "[bytecode]") {
  const auto ir_program =
      lower_source_to_ir("(1, (0, (): Any = \"zero\"), (_, (): Any = \"many\")) -> Std.Match -> Std.Println;\n",
                         "bytecode_std_match.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);
  REQUIRE(result.has_value());

  bool found_match_call = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kCallBuiltin) {
      const auto builtin_id = fleaux::vm::builtin_id_from_operand(ins.operand);
      REQUIRE(builtin_id.has_value());
      const auto name = fleaux::vm::builtin_name(*builtin_id);
      if (name == "Std.Match") { found_match_call = true; }
    }
  }
  REQUIRE(found_match_call);
}

TEST_CASE("Bytecode compiler keeps Std.Branch as builtin for callable locals", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(R"(
let Inc(x: Float64): Float64 = (x, 1) -> Std.Add;
let Dec(x: Float64): Float64 = (x, 1) -> Std.Subtract;
let ChooseApply(x: Float64, tf: Any, ff: Any): Float64 =
    ((x, 0) -> Std.GreaterThan, x, tf, ff) -> Std.Branch;
(10, Inc, Dec) -> ChooseApply -> Std.Println;
)",
                                             "bytecode_branch_callable_locals.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  const auto choose_it = std::find_if(result->functions.begin(), result->functions.end(),
                                      [](const fleaux::bytecode::FunctionDef& fn) { return fn.name == "ChooseApply#0"; });
  REQUIRE(choose_it != result->functions.end());

  bool found_branch_call = false;
  bool found_builtin_branch = false;
  for (const auto& ins : choose_it->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kBranchCall) { found_branch_call = true; }
    if (ins.opcode == fleaux::bytecode::Opcode::kCallBuiltin) {
      const auto builtin_id = fleaux::vm::builtin_id_from_operand(ins.operand);
      REQUIRE(builtin_id.has_value());
      const auto name = fleaux::vm::builtin_name(*builtin_id);
      if (name == "Std.Branch") { found_builtin_branch = true; }
    }
  }

  REQUIRE_FALSE(found_branch_call);
  REQUIRE(found_builtin_branch);
}

// ---------------------------------------------------------------------------
// Stdlib ToString now compiles successfully via kCallBuiltin.
// ---------------------------------------------------------------------------
TEST_CASE("Bytecode compiler supports Std.ToString via kCallBuiltin", "[bytecode]") {
  const auto ir_program = lower_source_to_ir("(123) -> Std.ToString -> Std.Println;\n", "bytecode_tostring.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  // Both Std.ToString and Std.Println are compiled as kCallBuiltin.
  bool found_to_string = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kCallBuiltin) {
      const auto builtin_id = fleaux::vm::builtin_id_from_operand(ins.operand);
      REQUIRE(builtin_id.has_value());
      const auto name = fleaux::vm::builtin_name(*builtin_id);
      if (name == "Std.ToString") { found_to_string = true; }
    }
  }
  REQUIRE(found_to_string);
}

// ---------------------------------------------------------------------------
// Truly unsupported call target: an unknown unqualified name that is neither a
// stdlib builtin nor a user-defined function.
// ---------------------------------------------------------------------------
TEST_CASE("Analysis rejects unknown unqualified call target before bytecode compilation", "[bytecode]") {
  const std::string src = "(42) -> UnknownFunction;\n";

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(src, "bytecode_unknown_target.fleaux");
  REQUIRE(parsed.has_value());

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  REQUIRE_FALSE(lowered.has_value());
  REQUIRE(lowered.error().message.find("Unresolved symbol") != std::string::npos);
  REQUIRE(lowered.error().hint.has_value());
  REQUIRE(lowered.error().hint->find("UnknownFunction") != std::string::npos);
}

// ---------------------------------------------------------------------------
// User-defined function: let and call.
// ---------------------------------------------------------------------------
TEST_CASE("Bytecode compiler emits user function and kCallUserFunc", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(R"(
let Double(x: Float64): Float64 = (x, x) -> Std.Add;
(7) -> Double -> Std.Println;
)",
                                             "bytecode_user_func.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());
  // One function should be compiled.
  REQUIRE(result->functions.size() == 1);
  REQUIRE(result->functions[0].name == "Double#0");
  REQUIRE(result->functions[0].arity == 1);

  // The top-level code should include a kCallUserFunc.
  bool found_call_user_func = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kCallUserFunc) {
      REQUIRE(ins.operand == 0);  // function index 0
      found_call_user_func = true;
    }
  }
  REQUIRE(found_call_user_func);

  // The function body should end with kReturn.
  const auto& fn_instrs = result->functions[0].instructions;
  REQUIRE(!fn_instrs.empty());
  REQUIRE(fn_instrs.back().opcode == fleaux::bytecode::Opcode::kReturn);
}

// ---------------------------------------------------------------------------
// Constant pool: constants go into Module::constants.
// ---------------------------------------------------------------------------
TEST_CASE("Bytecode compiler stores constants in pool", "[bytecode]") {
  const auto ir_program = lower_source_to_ir("(\"hello\") -> Std.Println;\n", "bytecode_string_const.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());
  REQUIRE(!result->constants.empty());

  // The first constant should be the string "hello".
  const auto& c = result->constants[0];
  REQUIRE(std::holds_alternative<std::string>(c.data));
  REQUIRE(std::get<std::string>(c.data) == "hello");
}

// ---------------------------------------------------------------------------
// NoOpEliminationPass
// ---------------------------------------------------------------------------
TEST_CASE("NoOpEliminationPass removes kNoOp from top-level instructions", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.instructions = {
      {fleaux::bytecode::Opcode::kNoOp, 0},
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kNoOp, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::NoOpEliminationPass{}.run(module);

  REQUIRE(module.instructions.size() == 2);
  REQUIRE(module.instructions[0].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(module.instructions[1].opcode == fleaux::bytecode::Opcode::kHalt);
}

TEST_CASE("NoOpEliminationPass removes kNoOp from function body", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  fleaux::bytecode::FunctionDef fn;
  fn.name = "Foo";
  fn.arity = 0;
  fn.instructions = {
      {fleaux::bytecode::Opcode::kNoOp, 0},
      {fleaux::bytecode::Opcode::kLoadLocal, 0},
      {fleaux::bytecode::Opcode::kNoOp, 0},
      {fleaux::bytecode::Opcode::kReturn, 0},
  };
  module.functions.push_back(std::move(fn));

  fleaux::bytecode::NoOpEliminationPass{}.run(module);

  REQUIRE(module.functions[0].instructions.size() == 2);
  REQUIRE(module.functions[0].instructions[0].opcode == fleaux::bytecode::Opcode::kLoadLocal);
  REQUIRE(module.functions[0].instructions[1].opcode == fleaux::bytecode::Opcode::kReturn);
}

TEST_CASE("NoOpEliminationPass is a no-op when there are no kNoOps", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::NoOpEliminationPass{}.run(module);

  REQUIRE(module.instructions.size() == 2);
}

TEST_CASE("BytecodeOptimizer baseline always runs NoOpEliminationPass", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.instructions = {
      {fleaux::bytecode::Opcode::kNoOp, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  constexpr fleaux::bytecode::BytecodeOptimizer optimizer;
  const auto result = optimizer.optimize(module);  // default = kBaseline

  REQUIRE(result.has_value());
  REQUIRE(module.instructions.size() == 1);
  REQUIRE(module.instructions[0].opcode == fleaux::bytecode::Opcode::kHalt);
}

// ---------------------------------------------------------------------------
// ConstantPoolDeduplicationPass
// ---------------------------------------------------------------------------
TEST_CASE("ConstantPoolDeduplicationPass merges duplicate pool entries", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  // Pool: [42, 99, 42] -- entry 0 and 2 are duplicates.
  module.constants = {
      fleaux::bytecode::ConstValue{std::int64_t{42}},
      fleaux::bytecode::ConstValue{std::int64_t{99}},
      fleaux::bytecode::ConstValue{std::int64_t{42}},
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},  // push 42 (first)
      {fleaux::bytecode::Opcode::kPushConst, 1},  // push 99
      {fleaux::bytecode::Opcode::kPushConst, 2},  // push 42 (duplicate)
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::ConstantPoolDeduplicationPass{}.run(module);

  // Pool should now have 2 entries: 42 and 99.
  REQUIRE(module.constants.size() == 2);
  REQUIRE(std::get<std::int64_t>(module.constants[0].data) == 42);
  REQUIRE(std::get<std::int64_t>(module.constants[1].data) == 99);

  // Both pushes of 42 now point to index 0; the 99 push stays at index 1.
  REQUIRE(module.instructions[0].operand == 0);
  REQUIRE(module.instructions[1].operand == 1);
  REQUIRE(module.instructions[2].operand == 0);
}

TEST_CASE("ConstantPoolDeduplicationPass is a no-op when all entries are unique", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.constants = {
      fleaux::bytecode::ConstValue{std::int64_t{1}},
      fleaux::bytecode::ConstValue{std::int64_t{2}},
      fleaux::bytecode::ConstValue{std::int64_t{3}},
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kPushConst, 1},
      {fleaux::bytecode::Opcode::kPushConst, 2},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::ConstantPoolDeduplicationPass{}.run(module);

  REQUIRE(module.constants.size() == 3);
  REQUIRE(module.instructions[0].operand == 0);
  REQUIRE(module.instructions[1].operand == 1);
  REQUIRE(module.instructions[2].operand == 2);
}

TEST_CASE("ConstantPoolDeduplicationPass deduplicates string constants", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.constants = {
      fleaux::bytecode::ConstValue{std::string{"hello"}},
      fleaux::bytecode::ConstValue{std::string{"hello"}},
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kPushConst, 1},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::ConstantPoolDeduplicationPass{}.run(module);

  REQUIRE(module.constants.size() == 1);
  REQUIRE(module.instructions[0].operand == 0);
  REQUIRE(module.instructions[1].operand == 0);
}

// ---------------------------------------------------------------------------
// DeadPushEliminationPass
// ---------------------------------------------------------------------------
TEST_CASE("DeadPushEliminationPass removes kPushConst followed by kPop", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kPop, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::DeadPushEliminationPass{}.run(module);

  REQUIRE(module.instructions.size() == 1);
  REQUIRE(module.instructions[0].opcode == fleaux::bytecode::Opcode::kHalt);
}

TEST_CASE("DeadPushEliminationPass removes kLoadLocal followed by kPop", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.instructions = {
      {fleaux::bytecode::Opcode::kLoadLocal, 0},
      {fleaux::bytecode::Opcode::kPop, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::DeadPushEliminationPass{}.run(module);

  REQUIRE(module.instructions.size() == 1);
  REQUIRE(module.instructions[0].opcode == fleaux::bytecode::Opcode::kHalt);
}

TEST_CASE("DeadPushEliminationPass does not remove kPop after a non-push", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  // kCallBuiltin has side effects; the kPop after it is live.
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kCallBuiltin, 0},
      {fleaux::bytecode::Opcode::kPop, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::DeadPushEliminationPass{}.run(module);

  REQUIRE(module.instructions.size() == 4);
}

TEST_CASE("DeadPushEliminationPass handles multiple consecutive dead pairs", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0}, {fleaux::bytecode::Opcode::kPop, 0},
      {fleaux::bytecode::Opcode::kPushConst, 1}, {fleaux::bytecode::Opcode::kPop, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::DeadPushEliminationPass{}.run(module);

  REQUIRE(module.instructions.size() == 1);
  REQUIRE(module.instructions[0].opcode == fleaux::bytecode::Opcode::kHalt);
}

TEST_CASE("BytecodeOptimizer extended mode also eliminates kNoOps", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.instructions = {
      {fleaux::bytecode::Opcode::kNoOp, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  constexpr fleaux::bytecode::BytecodeOptimizer optimizer;
  const auto result = optimizer.optimize(
      module, fleaux::bytecode::OptimizerConfig{.mode = fleaux::bytecode::OptimizationMode::kExtended});

  REQUIRE(result.has_value());
  REQUIRE(module.instructions.size() == 1);
  REQUIRE(module.instructions[0].opcode == fleaux::bytecode::Opcode::kHalt);
}

// ---------------------------------------------------------------------------
// Bytecode Serialization
// ---------------------------------------------------------------------------
TEST_CASE("Bytecode serialization and deserialization round-trips", "[bytecode][serialization]") {
  fleaux::bytecode::Module original;
  original.header.module_name = "RoundTrip";
  original.header.source_path = "RoundTrip.fleaux";
  original.header.source_hash = 12345;
  original.header.optimization_mode = 1;
  original.dependencies = {
      fleaux::bytecode::ModuleDependency{.module_name = "Std", .is_symbolic = true},
      fleaux::bytecode::ModuleDependency{.module_name = "20_export", .is_symbolic = false},
  };
  original.exports = {
      fleaux::bytecode::ExportedSymbol{
          .name = "RoundTrip.Double",
          .kind = fleaux::bytecode::ExportKind::kFunction,
          .index = 0,
          .builtin_name = {},
      },
      fleaux::bytecode::ExportedSymbol{
          .name = "RoundTrip.Println",
          .kind = fleaux::bytecode::ExportKind::kBuiltinAlias,
          .index = 0,
          .builtin_name = "RoundTrip.Println",
      },
  };
  original.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kPushConst, 1},
      {fleaux::bytecode::Opcode::kAdd, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };
  original.constants = {
      fleaux::bytecode::ConstValue{std::int64_t{10}},
      fleaux::bytecode::ConstValue{std::int64_t{20}},
  };
  const auto serialized = fleaux::bytecode::serialize_module(original);
  REQUIRE(serialized.has_value());

  const auto deserialized = fleaux::bytecode::deserialize_module(serialized.value());
  REQUIRE(deserialized.has_value());

  const auto& restored = deserialized.value();
  REQUIRE(restored.header.module_name == original.header.module_name);
  REQUIRE(restored.header.source_path == original.header.source_path);
  REQUIRE(restored.header.source_hash == original.header.source_hash);
  REQUIRE(restored.header.optimization_mode == original.header.optimization_mode);
  REQUIRE(restored.header.payload_checksum != 0);
  REQUIRE(restored.dependencies.size() == original.dependencies.size());
  REQUIRE(restored.exports.size() == original.exports.size());
  REQUIRE(restored.instructions.size() == original.instructions.size());
  REQUIRE(restored.constants.size() == original.constants.size());
  REQUIRE(restored.dependencies[0].module_name == "Std");
  REQUIRE(restored.dependencies[0].is_symbolic);
  REQUIRE(restored.dependencies[1].module_name == "20_export");
  REQUIRE_FALSE(restored.dependencies[1].is_symbolic);
  REQUIRE(restored.exports[0].name == "RoundTrip.Double");
  REQUIRE(restored.exports[0].kind == fleaux::bytecode::ExportKind::kFunction);
  REQUIRE(restored.exports[0].index == 0);
  REQUIRE(restored.exports[1].kind == fleaux::bytecode::ExportKind::kBuiltinAlias);
  REQUIRE(restored.exports[1].builtin_name == "RoundTrip.Println");

  for (std::size_t i = 0; i < restored.instructions.size(); ++i) {
    REQUIRE(restored.instructions[i].opcode == original.instructions[i].opcode);
    REQUIRE(restored.instructions[i].operand == original.instructions[i].operand);
  }

  REQUIRE(std::get<std::int64_t>(restored.constants[0].data) == 10);
  REQUIRE(std::get<std::int64_t>(restored.constants[1].data) == 20);
}

TEST_CASE("Bytecode serialization handles all constant types", "[bytecode][serialization]") {
  fleaux::bytecode::Module original;
  original.constants = {
      fleaux::bytecode::ConstValue{std::int64_t{-42}},
      fleaux::bytecode::ConstValue{std::uint64_t{9999}},
      fleaux::bytecode::ConstValue{3.14159},
      fleaux::bytecode::ConstValue{true},
      fleaux::bytecode::ConstValue{std::string{"hello world"}},
      fleaux::bytecode::ConstValue{std::monostate{}},
  };

  const auto serialized = fleaux::bytecode::serialize_module(original);
  REQUIRE(serialized.has_value());

  const auto deserialized = fleaux::bytecode::deserialize_module(serialized.value());
  REQUIRE(deserialized.has_value());

  const auto& restored = deserialized.value();
  REQUIRE(restored.constants.size() == 6);
  REQUIRE(std::get<std::int64_t>(restored.constants[0].data) == -42);
  REQUIRE(std::get<std::uint64_t>(restored.constants[1].data) == 9999);
  REQUIRE(std::abs(std::get<double>(restored.constants[2].data) - 3.14159) < 0.00001);
  REQUIRE(std::get<bool>(restored.constants[3].data) == true);
  REQUIRE(std::get<std::string>(restored.constants[4].data) == "hello world");
  REQUIRE(std::holds_alternative<std::monostate>(restored.constants[5].data));
}

TEST_CASE("Bytecode serialization handles functions", "[bytecode][serialization]") {
  fleaux::bytecode::Module original;
  fleaux::bytecode::FunctionDef fn;
  fn.name = "MyFunc";
  fn.arity = 2;
  fn.has_variadic_tail = false;
  fn.instructions = {
      {fleaux::bytecode::Opcode::kLoadLocal, 0},
      {fleaux::bytecode::Opcode::kLoadLocal, 1},
      {fleaux::bytecode::Opcode::kAdd, 0},
      {fleaux::bytecode::Opcode::kReturn, 0},
  };
  original.functions.push_back(std::move(fn));

  const auto serialized = fleaux::bytecode::serialize_module(original);
  REQUIRE(serialized.has_value());

  const auto deserialized = fleaux::bytecode::deserialize_module(serialized.value());
  REQUIRE(deserialized.has_value());

  const auto& restored = deserialized.value();
  REQUIRE(restored.functions.size() == 1);
  REQUIRE(restored.functions[0].name == "MyFunc");
  REQUIRE(restored.functions[0].arity == 2);
  REQUIRE(restored.functions[0].has_variadic_tail == false);
  REQUIRE(restored.functions[0].instructions.size() == 4);
}

TEST_CASE("Bytecode deserialization rejects invalid magic", "[bytecode][serialization]") {
  std::vector<std::uint8_t> bad_buffer;
  bad_buffer.push_back(0xFF);
  bad_buffer.push_back(0xFF);
  bad_buffer.push_back(0xFF);
  bad_buffer.push_back(0xFF);

  const auto result = fleaux::bytecode::deserialize_module(bad_buffer);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message.find("Invalid magic") != std::string::npos);
}

TEST_CASE("Bytecode deserialization rejects version mismatch", "[bytecode][serialization]") {
  std::vector<std::uint8_t> bad_buffer;
  // Write correct magic.
  std::uint32_t magic = 0x464C4558;
  std::uint32_t bad_version = 999;
  const auto* magic_bytes = reinterpret_cast<const std::uint8_t*>(&magic);
  const auto* version_bytes = reinterpret_cast<const std::uint8_t*>(&bad_version);
  bad_buffer.insert(bad_buffer.end(), magic_bytes, magic_bytes + 4);
  bad_buffer.insert(bad_buffer.end(), version_bytes, version_bytes + 4);

  const auto result = fleaux::bytecode::deserialize_module(bad_buffer);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message.find("Version") != std::string::npos);
}

TEST_CASE("Bytecode deserialization rejects payload checksum mismatch", "[bytecode][serialization]") {
  fleaux::bytecode::Module module;
  module.header.module_name = "Checksum";
  module.instructions = {{fleaux::bytecode::Opcode::kHalt, 0}};

  const auto serialized = fleaux::bytecode::serialize_module(module);
  REQUIRE(serialized.has_value());

  auto corrupted = serialized.value();
  REQUIRE(corrupted.size() > 16);
  corrupted.back() ^= 0x01;

  const auto result = fleaux::bytecode::deserialize_module(corrupted);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message.find("checksum") != std::string::npos);
}

TEST_CASE("Bytecode round-trip: compile -> serialize -> deserialize -> verify", "[bytecode][serialization]") {
  const std::string source_text =
      "import Std;\n"
      "import 20_export;\n"
      "let Local(x: Float64): Any = (y: Float64): Float64 = (x, y) -> Std.Add;\n"
      "(4, 5) -> Std.Add -> Std.Println;\n";
  const auto ir_program = lower_source_to_ir(source_text, "bytecode_roundtrip.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled =
      compiler.compile(ir_program, fleaux::bytecode::CompileOptions{
                                       .source_path = std::filesystem::path("bytecode_roundtrip.fleaux"),
                                       .source_text = source_text,
                                       .module_name = "bytecode_roundtrip",
                                   });
  REQUIRE(compiled.has_value());

  const auto& original = compiled.value();
  REQUIRE(original.header.module_name == "bytecode_roundtrip");
  REQUIRE(original.header.source_path == "bytecode_roundtrip.fleaux");
  REQUIRE(original.header.source_hash != 0);
  REQUIRE(original.dependencies.size() == 2);
  REQUIRE(original.dependencies[0].module_name == "Std");
  REQUIRE(original.dependencies[0].is_symbolic);
  REQUIRE(original.dependencies[1].module_name == "20_export");
  REQUIRE_FALSE(original.dependencies[1].is_symbolic);
  REQUIRE(original.exports.size() == 1);
  REQUIRE(original.exports[0].name == "Local");
  REQUIRE(original.exports[0].kind == fleaux::bytecode::ExportKind::kFunction);
  REQUIRE(original.functions.size() >= 2);  // Local + synthetic closure helper.
  const auto serialized = fleaux::bytecode::serialize_module(original);
  REQUIRE(serialized.has_value());

  const auto deserialized = fleaux::bytecode::deserialize_module(serialized.value());
  REQUIRE(deserialized.has_value());

  const auto& restored = deserialized.value();

  REQUIRE(restored.instructions.size() == original.instructions.size());
  REQUIRE(restored.constants.size() == original.constants.size());
  REQUIRE(restored.functions.size() == original.functions.size());
  REQUIRE(restored.closures.size() == original.closures.size());
  REQUIRE(restored.header.module_name == original.header.module_name);
  REQUIRE(restored.header.source_hash == original.header.source_hash);
  REQUIRE(restored.dependencies.size() == original.dependencies.size());
  REQUIRE(restored.exports.size() == original.exports.size());
  REQUIRE(restored.exports[0].name == "Local");

  for (std::size_t i = 0; i < restored.instructions.size(); ++i) {
    REQUIRE(restored.instructions[i].opcode == original.instructions[i].opcode);
    REQUIRE(restored.instructions[i].operand == original.instructions[i].operand);
  }
}

TEST_CASE("Bytecode module loader reports unresolved import category and diagnostic shape",
          "[bytecode][serialization][imports][contract]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_bytecode_import_unresolved_shape";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto entry_path = temp_dir / "entry_unresolved.fleaux";
  {
    std::ofstream out(entry_path);
    out << "import missing_dep;\n"
           "(1) -> MissingCall;\n";
  }

  const auto loaded = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE_FALSE(loaded.has_value());
  REQUIRE(loaded.error().message.starts_with("import-unresolved:"));
  REQUIRE(loaded.error().message.find("Import not found: 'missing_dep'") != std::string::npos);
  REQUIRE(loaded.error().message.find("Verify module name and file location") != std::string::npos);
}

TEST_CASE("Bytecode module loader reports import cycle category and diagnostic shape",
          "[bytecode][serialization][imports][contract]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_bytecode_import_cycle_shape";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto cycle_a_path = temp_dir / "cycle_a.fleaux";
  const auto cycle_b_path = temp_dir / "cycle_b.fleaux";

  {
    std::ofstream out(cycle_a_path);
    out << "import cycle_b;\n"
           "let A(x: Float64): Float64 = x;\n";
  }

  {
    std::ofstream out(cycle_b_path);
    out << "import cycle_a;\n"
           "let B(x: Float64): Float64 = x;\n";
  }

  const auto loaded = fleaux::bytecode::load_linked_module(cycle_a_path);
  REQUIRE_FALSE(loaded.has_value());
  REQUIRE(loaded.error().message.starts_with("import-cycle:"));
  REQUIRE(loaded.error().message.find("Import cycle detected involving") != std::string::npos);
  REQUIRE(loaded.error().message.find("cycle_a.fleaux") != std::string::npos);
}

TEST_CASE("Bytecode module loader falls back to source when dependency bytecode is invalid",
          "[bytecode][serialization][imports][contract]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_bytecode_invalid_dependency_cache_fallback";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto dependency_path = temp_dir / "typed_dep.fleaux";
  const auto dependency_bytecode_path = temp_dir / "typed_dep.fleaux.bc";
  const auto entry_path = temp_dir / "typed_entry.fleaux";
  const auto entry_bytecode_path = temp_dir / "typed_entry.fleaux.bc";

  {
    std::ofstream out(dependency_path);
    out << "import Std;\n"
           "let Add4(x: Float64): Float64 = (4, x) -> Std.Add;\n";
  }

  {
    std::ofstream out(entry_path);
    out << "import Std;\n"
           "import typed_dep;\n"
           "(1) -> Add4 -> Std.Println;\n";
  }

  const auto initial_load = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE(initial_load.has_value());
  REQUIRE(std::filesystem::exists(entry_bytecode_path));
  REQUIRE(std::filesystem::exists(dependency_bytecode_path));

  {
    std::ofstream out(dependency_bytecode_path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out << "not-a-valid-bytecode-payload";
    REQUIRE(out.good());
  }

  const auto fallback_load = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE(fallback_load.has_value());

  const fleaux::vm::Runtime runtime;
  std::ostringstream output;
  const auto runtime_result = runtime.execute(*fallback_load, output);
  if (!runtime_result) { INFO("vm runtime error: " << runtime_result.error().message); }
  REQUIRE(runtime_result.has_value());
  REQUIRE(output.str().find('5') != std::string::npos);
}

TEST_CASE("Bytecode module loader imports serialized dependency modules", "[bytecode][serialization][imports]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_bytecode_serialized_imports";
  std::filesystem::create_directories(temp_dir);

  const auto dependency_path = temp_dir / "20_export.fleaux";
  const auto entry_path = temp_dir / "21_import.fleaux";

  {
    std::ofstream out(dependency_path);
    out << "import Std;\n"
           "let Add4(x: Float64): Float64 = (4, x) -> Std.Add;\n";
  }

  {
    std::ofstream out(entry_path);
    out << "import 20_export;\n"
           "(4) -> Add4 -> Std.Println;\n";
  }

  const auto initial_load = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE(initial_load.has_value());
  REQUIRE(std::filesystem::exists(temp_dir / "20_export.fleaux.bc"));
  REQUIRE(std::filesystem::exists(temp_dir / "21_import.fleaux.bc"));

  std::filesystem::remove(dependency_path);

  const auto cached_load = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE(cached_load.has_value());

  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(cached_load.value());
  if (!runtime_result) { INFO("vm runtime error: " << runtime_result.error().message); }
  REQUIRE(runtime_result.has_value());
}

TEST_CASE("Bytecode module loader preserves exported user overload link names", "[bytecode][serialization][imports][overload]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_bytecode_overloaded_imports";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto dependency_path = temp_dir / "typed_dep.fleaux";
  const auto dependency_bytecode_path = temp_dir / "typed_dep.fleaux.bc";
  const auto entry_path = temp_dir / "typed_entry.fleaux";

  {
    std::ofstream out(dependency_path);
    out << "import Std;\n"
           "let Echo(x: Int64): Int64 = (x, 1) -> Std.Add;\n"
           "let Echo(x: String): String = x;\n";
  }

  {
    std::ofstream out(entry_path);
    out << "import Std;\n"
           "import typed_dep;\n"
           "(1) -> Echo -> Std.Println;\n"
           "(\"ok\") -> Echo -> Std.Println;\n";
  }

  const auto loaded = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE(loaded.has_value());
  REQUIRE(std::filesystem::exists(dependency_bytecode_path));

  std::vector<std::uint8_t> dependency_bytes;
  {
    std::ifstream in(dependency_bytecode_path, std::ios::binary);
    REQUIRE(in.good());
    dependency_bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }

  const auto deserialized = fleaux::bytecode::deserialize_module(dependency_bytes);
  REQUIRE(deserialized.has_value());
  REQUIRE(deserialized->exports.size() == 2U);
  REQUIRE(deserialized->exports[0].name == "Echo");
  REQUIRE(deserialized->exports[0].link_name == "Echo#0");
  REQUIRE(deserialized->exports[1].name == "Echo");
  REQUIRE(deserialized->exports[1].link_name == "Echo#1");

  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(*loaded);
  if (!runtime_result) { INFO("vm runtime error: " << runtime_result.error().message); }
  REQUIRE(runtime_result.has_value());
}

TEST_CASE("Bytecode module loader enforces typed imported signatures during analysis",
          "[bytecode][serialization][imports][stage4g]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_bytecode_typed_imports";
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

  const auto loaded = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE_FALSE(loaded.has_value());
  REQUIRE(loaded.error().message.find("Type mismatch in call target arguments") != std::string::npos);
  REQUIRE(loaded.error().message.find("Add4 expects argument 0") != std::string::npos);
}

TEST_CASE("Bytecode module loader revalidates typed import seeds when entry cache is stale",
          "[bytecode][serialization][imports][stage4g]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_bytecode_typed_imports_seed_refresh";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto dependency_path = temp_dir / "typed_dep.fleaux";
  const auto dependency_bytecode_path = temp_dir / "typed_dep.fleaux.bc";
  const auto entry_path = temp_dir / "typed_entry.fleaux";
  const auto entry_bytecode_path = temp_dir / "typed_entry.fleaux.bc";

  {
    std::ofstream out(dependency_path);
    out << "import Std;\n"
           "let Add4(x: Float64): Float64 = (4, x) -> Std.Add;\n";
  }

  {
    std::ofstream out(entry_path);
    out << "import Std;\n"
           "import typed_dep;\n"
           "(1) -> Add4 -> Std.Println;\n";
  }

  const auto initial_load = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE(initial_load.has_value());
  REQUIRE(std::filesystem::exists(entry_path));
  REQUIRE(std::filesystem::exists(dependency_path));
  REQUIRE(std::filesystem::exists(entry_bytecode_path));
  REQUIRE(std::filesystem::exists(dependency_bytecode_path));

  {
    std::ofstream out(dependency_path);
    out << "let Add4(x: String): Int64 = 4;\n";
  }

  REQUIRE(std::filesystem::exists(dependency_bytecode_path));
  std::filesystem::remove(entry_bytecode_path);
  REQUIRE_FALSE(std::filesystem::exists(entry_bytecode_path));

  const auto loaded = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE_FALSE(loaded.has_value());
  REQUIRE(loaded.error().message.find("Type mismatch in call target arguments") != std::string::npos);
  REQUIRE(loaded.error().message.find("Add4 expects argument 0") != std::string::npos);
  REQUIRE(loaded.error().message.find("Module source and bytecode were both unavailable") == std::string::npos);
  REQUIRE(loaded.error().message.find("Failed to resolve imported module") == std::string::npos);
}

TEST_CASE("Bytecode module loader reports missing typed import seed declaration for exported symbol",
          "[bytecode][serialization][imports][stage4g]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_bytecode_typed_imports_missing_seed_decl";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto dependency_path = temp_dir / "typed_dep.fleaux";
  const auto dependency_bytecode_path = temp_dir / "typed_dep.fleaux.bc";
  const auto entry_path = temp_dir / "typed_entry.fleaux";
  const auto entry_bytecode_path = temp_dir / "typed_entry.fleaux.bc";

  {
    std::ofstream out(dependency_path);
    out << "import Std;\n"
           "let Add4(x: Float64): Float64 = (4, x) -> Std.Add;\n";
  }

  {
    std::ofstream out(entry_path);
    out << "import Std;\n"
           "import typed_dep;\n"
           "(1) -> Add4 -> Std.Println;\n";
  }

  const auto initial_load = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE(initial_load.has_value());
  REQUIRE(std::filesystem::exists(entry_bytecode_path));
  REQUIRE(std::filesystem::exists(dependency_bytecode_path));

  std::vector<std::uint8_t> dependency_bytes;
  {
    std::ifstream in(dependency_bytecode_path, std::ios::binary);
    REQUIRE(in.good());
    dependency_bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }

  const auto deserialized = fleaux::bytecode::deserialize_module(dependency_bytes);
  REQUIRE(deserialized.has_value());
  auto corrupted_dependency = deserialized.value();

  corrupted_dependency.exports.push_back(fleaux::bytecode::ExportedSymbol{
      .name = "Ghost.Export",
      .kind = fleaux::bytecode::ExportKind::kFunction,
      .index = 0,
      .builtin_name = {},
  });

  const auto serialized = fleaux::bytecode::serialize_module(corrupted_dependency);
  REQUIRE(serialized.has_value());

  {
    std::ofstream out(dependency_bytecode_path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out.write(reinterpret_cast<const char*>(serialized->data()), static_cast<std::streamsize>(serialized->size()));
    REQUIRE(out.good());
  }

  std::filesystem::remove(entry_bytecode_path);
  REQUIRE_FALSE(std::filesystem::exists(entry_bytecode_path));

  const auto loaded = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE_FALSE(loaded.has_value());
  REQUIRE(loaded.error().message.find("Missing exported declaration for typed import seed") != std::string::npos);
  REQUIRE(loaded.error().message.find("Ghost.Export") != std::string::npos);
  REQUIRE(loaded.error().message.find("Module source and bytecode were both unavailable") == std::string::npos);
}

TEST_CASE("Bytecode module loader enforces typed signatures for qualified imported exports",
          "[bytecode][serialization][imports][stage4g]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_bytecode_typed_imports_qualified_mismatch";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto dependency_path = temp_dir / "typed_dep_qualified.fleaux";
  const auto entry_path = temp_dir / "typed_entry_qualified.fleaux";

  {
    std::ofstream out(dependency_path);
    out << "let MyMath.Add4(x: String): Int64 = 4;\n";
  }

  {
    std::ofstream out(entry_path);
    out << "import Std;\n"
           "import typed_dep_qualified;\n"
           "(1) -> MyMath.Add4 -> Std.Println;\n";
  }

  const auto loaded = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE_FALSE(loaded.has_value());
  REQUIRE(loaded.error().message.find("Type mismatch in call target arguments") != std::string::npos);
  REQUIRE(loaded.error().message.find("expects argument 0") != std::string::npos);
}

TEST_CASE("Bytecode module loader accepts qualified imported exports when typed signatures match",
          "[bytecode][serialization][imports][stage4g]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_bytecode_typed_imports_qualified_match";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto dependency_path = temp_dir / "typed_dep_qualified_ok.fleaux";
  const auto entry_path = temp_dir / "typed_entry_qualified_ok.fleaux";

  {
    std::ofstream out(dependency_path);
    out << "import Std;\n"
           "let MyMath.Add4(x: Float64): Float64 = (4, x) -> Std.Add;\n";
  }

  {
    std::ofstream out(entry_path);
    out << "import Std;\n"
           "import typed_dep_qualified_ok;\n"
           "(1) -> MyMath.Add4 -> Std.Println;\n";
  }

  const auto loaded = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE(loaded.has_value());

  const fleaux::vm::Runtime runtime;
  std::ostringstream output;
  const auto runtime_result = runtime.execute(loaded.value(), output);
  if (!runtime_result) { INFO("vm runtime error: " << runtime_result.error().message); }
  REQUIRE(runtime_result.has_value());
  REQUIRE(output.str().find('5') != std::string::npos);
}

TEST_CASE("Bytecode module loader can start from a serialized entry module", "[bytecode][serialization][imports]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_bytecode_serialized_entry";
  std::filesystem::create_directories(temp_dir);

  const auto dependency_path = temp_dir / "20_export.fleaux";
  const auto entry_path = temp_dir / "21_import.fleaux";
  const auto dependency_bytecode_path = temp_dir / "20_export.fleaux.bc";
  const auto entry_bytecode_path = temp_dir / "21_import.fleaux.bc";

  {
    std::ofstream out(dependency_path);
    out << "import Std;\n"
           "let Add4(x: Float64): Float64 = (4, x) -> Std.Add;\n";
  }

  {
    std::ofstream out(entry_path);
    out << "import 20_export;\n"
           "(4) -> Add4 -> Std.Println;\n";
  }

  const auto initial_load = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE(initial_load.has_value());
  REQUIRE(std::filesystem::exists(entry_bytecode_path));
  REQUIRE(std::filesystem::exists(dependency_bytecode_path));

  std::filesystem::remove(entry_path);
  std::filesystem::remove(dependency_path);

  const auto bytecode_only_load = fleaux::bytecode::load_linked_module(entry_bytecode_path);
  REQUIRE(bytecode_only_load.has_value());
  REQUIRE(bytecode_only_load->exports.empty());

  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(bytecode_only_load.value());
  if (!runtime_result) { INFO("vm runtime error: " << runtime_result.error().message); }
  REQUIRE(runtime_result.has_value());
}

TEST_CASE("Bytecode module loader handles diamond dependencies with serialized fallback",
          "[bytecode][serialization][imports][diamond]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_bytecode_diamond_imports";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto shared_path = temp_dir / "shared.fleaux";
  const auto left_path = temp_dir / "left.fleaux";
  const auto right_path = temp_dir / "right.fleaux";
  const auto root_path = temp_dir / "root.fleaux";

  {
    std::ofstream out(shared_path);
    out << "import Std;\n"
           "(\"shared-init\") -> Std.Println;\n"
           "let Add1(x: Float64): Float64 = (x, 1) -> Std.Add;\n";
  }

  {
    std::ofstream out(left_path);
    out << "import shared;\n"
           "let Left(x: Float64): Float64 = (x) -> Add1;\n";
  }

  {
    std::ofstream out(right_path);
    out << "import shared;\n"
           "let Right(x: Float64): Float64 = (x) -> Add1;\n";
  }

  {
    std::ofstream out(root_path);
    out << "import left;\n"
           "import right;\n"
           "(4) -> Left -> Right -> Std.Println;\n";
  }

  const auto initial_load = fleaux::bytecode::load_linked_module(root_path);
  REQUIRE(initial_load.has_value());

  REQUIRE(std::filesystem::exists(temp_dir / "shared.fleaux.bc"));
  REQUIRE(std::filesystem::exists(temp_dir / "left.fleaux.bc"));
  REQUIRE(std::filesystem::exists(temp_dir / "right.fleaux.bc"));
  REQUIRE(std::filesystem::exists(temp_dir / "root.fleaux.bc"));

  std::filesystem::remove(shared_path);

  const auto cached_load = fleaux::bytecode::load_linked_module(root_path);
  REQUIRE(cached_load.has_value());

  const fleaux::vm::Runtime runtime;
  std::ostringstream output;
  const auto runtime_result = runtime.execute(cached_load.value(), output);
  if (!runtime_result) { INFO("vm runtime error: " << runtime_result.error().message); }
  REQUIRE(runtime_result.has_value());

  const std::string text = output.str();
  REQUIRE(text.find("shared-init") != std::string::npos);
  const auto first = text.find("shared-init");
  REQUIRE(text.find("shared-init", first + 1) == std::string::npos);
}

TEST_CASE("ConstantFoldingPass folds safe literal unary and binary sequences", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.constants = {
      fleaux::bytecode::ConstValue{true},
      fleaux::bytecode::ConstValue{std::int64_t{2}},
      fleaux::bytecode::ConstValue{std::int64_t{3}},
      fleaux::bytecode::ConstValue{std::int64_t{10}},
      fleaux::bytecode::ConstValue{std::int64_t{4}},
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0}, {fleaux::bytecode::Opcode::kNot, 0},
      {fleaux::bytecode::Opcode::kPushConst, 1}, {fleaux::bytecode::Opcode::kPushConst, 2},
      {fleaux::bytecode::Opcode::kAdd, 0},       {fleaux::bytecode::Opcode::kPushConst, 3},
      {fleaux::bytecode::Opcode::kPushConst, 4}, {fleaux::bytecode::Opcode::kCmpGt, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::ConstantFoldingPass{}.run(module);

  REQUIRE(module.instructions.size() == 4);
  REQUIRE(module.instructions[0].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(module.instructions[1].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(module.instructions[2].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(module.instructions[3].opcode == fleaux::bytecode::Opcode::kHalt);

  REQUIRE(std::get<bool>(module.constants[static_cast<std::size_t>(module.instructions[0].operand)].data) == false);
  REQUIRE(std::get<std::int64_t>(module.constants[static_cast<std::size_t>(module.instructions[1].operand)].data) == 5);
  REQUIRE(std::get<bool>(module.constants[static_cast<std::size_t>(module.instructions[2].operand)].data) == true);
}

TEST_CASE("ConstantFoldingPass folds kDiv and preserves floating semantics", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.constants = {
      fleaux::bytecode::ConstValue{std::int64_t{10}},
      fleaux::bytecode::ConstValue{std::int64_t{4}},
      fleaux::bytecode::ConstValue{std::int64_t{0}},
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0}, {fleaux::bytecode::Opcode::kPushConst, 1},
      {fleaux::bytecode::Opcode::kDiv, 0},       {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kPushConst, 2}, {fleaux::bytecode::Opcode::kDiv, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::ConstantFoldingPass{}.run(module);

  REQUIRE(module.instructions.size() == 3);
  REQUIRE(module.instructions[0].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(module.instructions[1].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(module.instructions[2].opcode == fleaux::bytecode::Opcode::kHalt);

  REQUIRE(
      std::holds_alternative<double>(module.constants[static_cast<std::size_t>(module.instructions[0].operand)].data));
  REQUIRE(std::get<double>(module.constants[static_cast<std::size_t>(module.instructions[0].operand)].data) == 2.5);

  REQUIRE(
      std::holds_alternative<double>(module.constants[static_cast<std::size_t>(module.instructions[1].operand)].data));
  REQUIRE(
      std::isinf(std::get<double>(module.constants[static_cast<std::size_t>(module.instructions[1].operand)].data)));
}

TEST_CASE("ConstantFoldingPass folds kMod and preserves NaN on mod-by-zero", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.constants = {
      fleaux::bytecode::ConstValue{std::int64_t{10}},
      fleaux::bytecode::ConstValue{std::int64_t{4}},
      fleaux::bytecode::ConstValue{std::int64_t{0}},
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0}, {fleaux::bytecode::Opcode::kPushConst, 1},
      {fleaux::bytecode::Opcode::kMod, 0},       {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kPushConst, 2}, {fleaux::bytecode::Opcode::kMod, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::ConstantFoldingPass{}.run(module);

  REQUIRE(module.instructions.size() == 3);
  REQUIRE(module.instructions[0].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(module.instructions[1].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(module.instructions[2].opcode == fleaux::bytecode::Opcode::kHalt);

  REQUIRE(std::holds_alternative<std::int64_t>(
      module.constants[static_cast<std::size_t>(module.instructions[0].operand)].data));
  REQUIRE(std::get<std::int64_t>(module.constants[static_cast<std::size_t>(module.instructions[0].operand)].data) == 2);

  REQUIRE(
      std::holds_alternative<double>(module.constants[static_cast<std::size_t>(module.instructions[1].operand)].data));
  REQUIRE(
      std::isnan(std::get<double>(module.constants[static_cast<std::size_t>(module.instructions[1].operand)].data)));
}

TEST_CASE("ConstantFoldingPass folds kPow with integer and floating outputs", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.constants = {
      fleaux::bytecode::ConstValue{std::int64_t{2}},
      fleaux::bytecode::ConstValue{std::int64_t{8}},
      fleaux::bytecode::ConstValue{std::int64_t{-1}},
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0}, {fleaux::bytecode::Opcode::kPushConst, 1},
      {fleaux::bytecode::Opcode::kPow, 0},       {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kPushConst, 2}, {fleaux::bytecode::Opcode::kPow, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::ConstantFoldingPass{}.run(module);

  REQUIRE(module.instructions.size() == 3);
  REQUIRE(module.instructions[0].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(module.instructions[1].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(module.instructions[2].opcode == fleaux::bytecode::Opcode::kHalt);

  REQUIRE(std::holds_alternative<std::int64_t>(
      module.constants[static_cast<std::size_t>(module.instructions[0].operand)].data));
  REQUIRE(std::get<std::int64_t>(module.constants[static_cast<std::size_t>(module.instructions[0].operand)].data) ==
          256);

  REQUIRE(
      std::holds_alternative<double>(module.constants[static_cast<std::size_t>(module.instructions[1].operand)].data));
  REQUIRE(std::get<double>(module.constants[static_cast<std::size_t>(module.instructions[1].operand)].data) == 0.5);
}

TEST_CASE("BytecodeOptimizer baseline does not run ConstantFoldingPass", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.constants = {
      fleaux::bytecode::ConstValue{std::int64_t{2}},
      fleaux::bytecode::ConstValue{std::int64_t{3}},
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kPushConst, 1},
      {fleaux::bytecode::Opcode::kAdd, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  constexpr fleaux::bytecode::BytecodeOptimizer optimizer;
  const auto result = optimizer.optimize(module, fleaux::bytecode::OptimizerConfig{
                                                     .mode = fleaux::bytecode::OptimizationMode::kBaseline,
                                                 });
  REQUIRE(result.has_value());
  REQUIRE(module.instructions.size() == 4);
  REQUIRE(module.instructions[2].opcode == fleaux::bytecode::Opcode::kAdd);
}

TEST_CASE("BytecodeOptimizer extended mode runs ConstantFoldingPass", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.constants = {
      fleaux::bytecode::ConstValue{std::int64_t{2}},
      fleaux::bytecode::ConstValue{std::int64_t{3}},
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kPushConst, 1},
      {fleaux::bytecode::Opcode::kAdd, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  constexpr fleaux::bytecode::BytecodeOptimizer optimizer;
  const auto result = optimizer.optimize(module, fleaux::bytecode::OptimizerConfig{
                                                     .mode = fleaux::bytecode::OptimizationMode::kExtended,
                                                 });
  REQUIRE(result.has_value());
  REQUIRE(module.instructions.size() == 2);
  REQUIRE(module.instructions[0].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(module.instructions[1].opcode == fleaux::bytecode::Opcode::kHalt);
  REQUIRE(std::get<std::int64_t>(module.constants[static_cast<std::size_t>(module.instructions[0].operand)].data) == 5);
}

TEST_CASE("ConstantPropagationSelectPass folds literal kSelect true branch", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.constants = {
      fleaux::bytecode::ConstValue{true},
      fleaux::bytecode::ConstValue{std::int64_t{10}},
      fleaux::bytecode::ConstValue{std::int64_t{20}},
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0}, {fleaux::bytecode::Opcode::kPushConst, 1},
      {fleaux::bytecode::Opcode::kPushConst, 2}, {fleaux::bytecode::Opcode::kSelect, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::ConstantPropagationSelectPass{}.run(module);

  REQUIRE(module.instructions.size() == 2);
  REQUIRE(module.instructions[0].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(module.instructions[0].operand == 1);
  REQUIRE(module.instructions[1].opcode == fleaux::bytecode::Opcode::kHalt);
}

TEST_CASE("ConstantPropagationSelectPass folds literal kSelect false branch", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.constants = {
      fleaux::bytecode::ConstValue{false},
      fleaux::bytecode::ConstValue{std::int64_t{10}},
      fleaux::bytecode::ConstValue{std::int64_t{20}},
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0}, {fleaux::bytecode::Opcode::kPushConst, 1},
      {fleaux::bytecode::Opcode::kPushConst, 2}, {fleaux::bytecode::Opcode::kSelect, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  fleaux::bytecode::ConstantPropagationSelectPass{}.run(module);

  REQUIRE(module.instructions.size() == 2);
  REQUIRE(module.instructions[0].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(module.instructions[0].operand == 2);
  REQUIRE(module.instructions[1].opcode == fleaux::bytecode::Opcode::kHalt);
}

TEST_CASE("BytecodeOptimizer extended mode propagates folded kSelect condition", "[bytecode][optimizer]") {
  fleaux::bytecode::Module module;
  module.constants = {
      fleaux::bytecode::ConstValue{std::int64_t{1}},
      fleaux::bytecode::ConstValue{std::int64_t{1}},
      fleaux::bytecode::ConstValue{std::int64_t{10}},
      fleaux::bytecode::ConstValue{std::int64_t{20}},
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0}, {fleaux::bytecode::Opcode::kPushConst, 1},
      {fleaux::bytecode::Opcode::kCmpEq, 0},     {fleaux::bytecode::Opcode::kPushConst, 2},
      {fleaux::bytecode::Opcode::kPushConst, 3}, {fleaux::bytecode::Opcode::kSelect, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  constexpr fleaux::bytecode::BytecodeOptimizer optimizer;
  const auto result = optimizer.optimize(module, fleaux::bytecode::OptimizerConfig{
                                                     .mode = fleaux::bytecode::OptimizationMode::kExtended,
                                                 });
  REQUIRE(result.has_value());
  REQUIRE(module.instructions.size() == 2);
  REQUIRE(module.instructions[0].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(module.instructions[1].opcode == fleaux::bytecode::Opcode::kHalt);
  REQUIRE(std::get<std::int64_t>(module.constants[static_cast<std::size_t>(module.instructions[0].operand)].data) ==
          10);
}
