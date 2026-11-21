#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
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
#include "fleaux/bytecode/escape_analyzer.hpp"
#include "fleaux/bytecode/module.hpp"
#include "fleaux/bytecode/module_loader.hpp"
#include "fleaux/bytecode/opcode.hpp"
#include "fleaux/bytecode/optimizer.hpp"
#include "fleaux/bytecode/serialization.hpp"
#include "fleaux/bytecode/verifier.hpp"
#include "fleaux/frontend/lowering.hpp"
#include "fleaux/frontend/parser.hpp"
#include "fleaux/frontend/source_loader.hpp"
#include "fleaux/frontend/type_check.hpp"
#include "fleaux/vm/builtin_catalog.hpp"
#include "fleaux/vm/runtime.hpp"

#ifndef FLEAUX_REPO_ROOT
#error "FLEAUX_REPO_ROOT must be defined by CMake for bytecode tests."
#endif

namespace {

auto repo_root_path() -> std::filesystem::path { return std::filesystem::path(FLEAUX_REPO_ROOT); }

auto lower_source_to_ir(const std::string& source_text, const std::string& source_name)
    -> fleaux::frontend::ir::IRProgram {
  std::string effective_source = source_text;
  if (effective_source.find("Std.") != std::string::npos && effective_source.find("import Std;") == std::string::npos &&
      effective_source.find("let Std.") == std::string::npos) {
    effective_source = "import Std;\n" + effective_source;
  }

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(effective_source, source_name);
  REQUIRE(parsed.has_value());

  constexpr fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower_only(parsed.value());
  REQUIRE(lowered.has_value());

  auto make_error =
      [](const std::string& message, const std::optional<std::string>& hint,
         const std::optional<fleaux::frontend::diag::SourceSpan>& span) -> fleaux::frontend::type_check::AnalysisError {
    return fleaux::frontend::type_check::AnalysisError{
        .message = message,
        .hint = hint,
        .span = span,
    };
  };

  std::unordered_set<std::string> imported_symbols;
  std::vector<fleaux::frontend::ir::IRLet> imported_typed_lets;
  std::vector<fleaux::frontend::ir::IRTypeDecl> imported_type_decls;
  std::vector<fleaux::frontend::ir::IRAliasDecl> imported_alias_decls;
  const auto seeded =
      fleaux::frontend::source_loader::seed_symbolic_imports_for_program<fleaux::frontend::type_check::AnalysisError>(
          *lowered, make_error, imported_symbols, imported_typed_lets, imported_type_decls, imported_alias_decls);
  REQUIRE(seeded.has_value());

  const auto analyzed = fleaux::frontend::type_check::analyze_program(*lowered, imported_symbols, imported_typed_lets,
                                                                      imported_type_decls, imported_alias_decls);
  REQUIRE(analyzed.has_value());
  return analyzed.value();
}

auto make_test_span(const std::string& source_name) -> std::optional<fleaux::frontend::diag::SourceSpan> {
  return fleaux::frontend::diag::SourceSpan{
      .source_name = source_name,
      .source_text = {},
      .line = 1,
      .col = 1,
      .end_line = 1,
      .end_col = 1,
  };
}

auto make_ir_expr(std::variant<fleaux::frontend::ir::IRFlowExpr, fleaux::frontend::ir::IRTupleExpr,
                               fleaux::frontend::ir::IRConstant, fleaux::frontend::ir::IRNameRef,
                               fleaux::frontend::ir::IRClosureExprBox, fleaux::frontend::ir::IRBlockExprBox>
                      node,
                  std::optional<fleaux::frontend::diag::SourceSpan> span = std::nullopt)
    -> fleaux::frontend::ir::IRExpr {
  return fleaux::frontend::ir::IRExpr{.node = std::move(node), .span = std::move(span)};
}

auto make_ir_box(std::variant<fleaux::frontend::ir::IRFlowExpr, fleaux::frontend::ir::IRTupleExpr,
                              fleaux::frontend::ir::IRConstant, fleaux::frontend::ir::IRNameRef,
                              fleaux::frontend::ir::IRClosureExprBox, fleaux::frontend::ir::IRBlockExprBox>
                     node,
                 std::optional<fleaux::frontend::diag::SourceSpan> span = std::nullopt)
    -> fleaux::frontend::ir::IRExprBox {
  return fleaux::frontend::ir::IRExprBox(make_ir_expr(std::move(node), std::move(span)));
}

auto make_int_constant_box(const std::int64_t value) -> fleaux::frontend::ir::IRExprBox {
  return make_ir_box(fleaux::frontend::ir::IRConstant{.val = value, .span = std::nullopt});
}

auto make_bool_constant_box(const bool value) -> fleaux::frontend::ir::IRExprBox {
  return make_ir_box(fleaux::frontend::ir::IRConstant{.val = value, .span = std::nullopt});
}

auto make_string_constant_box(std::string value) -> fleaux::frontend::ir::IRExprBox {
  return make_ir_box(fleaux::frontend::ir::IRConstant{.val = std::move(value), .span = std::nullopt});
}

auto make_null_constant_box() -> fleaux::frontend::ir::IRExprBox {
  return make_ir_box(fleaux::frontend::ir::IRConstant{.val = std::monostate{}, .span = std::nullopt});
}

auto make_name_ref_box(std::optional<std::string> qualifier, std::string name,
                       std::optional<std::string> resolved_symbol_key = std::nullopt)
    -> fleaux::frontend::ir::IRExprBox {
  fleaux::frontend::ir::IRNameRef name_ref{};
  name_ref.qualifier = std::move(qualifier);
  name_ref.name = std::move(name);
  name_ref.resolved_symbol_key = std::move(resolved_symbol_key);
  name_ref.span = std::nullopt;
  return make_ir_box(std::move(name_ref));
}

auto make_simple_type(std::string name) -> fleaux::frontend::ir::IRSimpleType {
  fleaux::frontend::ir::IRSimpleType type{};
  type.name = std::move(name);
  return type;
}

auto make_variadic_simple_type(std::string name) -> fleaux::frontend::ir::IRSimpleType {
  auto type = make_simple_type(std::move(name));
  type.variadic = true;
  return type;
}

auto make_ir_name_ref(std::optional<std::string> qualifier, std::string name,
                      std::optional<std::string> resolved_symbol_key = std::nullopt)
    -> fleaux::frontend::ir::IRNameRef {
  fleaux::frontend::ir::IRNameRef name_ref{};
  name_ref.qualifier = std::move(qualifier);
  name_ref.name = std::move(name);
  name_ref.resolved_symbol_key = std::move(resolved_symbol_key);
  name_ref.span = std::nullopt;
  return name_ref;
}

auto make_compile_options(const bool enable_value_ref_gate = false, const bool enable_auto_value_ref = false,
                          const std::size_t value_ref_byte_cutoff = 256,
                          std::optional<std::filesystem::path> source_path = std::nullopt,
                          std::optional<std::string> source_text = std::nullopt,
                          std::optional<std::string> module_name = std::nullopt,
                          std::vector<fleaux::bytecode::Module> imported_modules = {},
                          const bool enable_experimental_builtin_reductions = false)
    -> fleaux::bytecode::CompileOptions {
  fleaux::bytecode::CompileOptions options{};
  options.source_path = std::move(source_path);
  options.source_text = std::move(source_text);
  options.module_name = std::move(module_name);
  options.imported_modules = std::move(imported_modules);
  options.enable_experimental_builtin_reductions = enable_experimental_builtin_reductions;
  options.enable_value_ref_gate = enable_value_ref_gate;
  options.enable_auto_value_ref = enable_auto_value_ref;
  options.value_ref_byte_cutoff = value_ref_byte_cutoff;
  return options;
}

auto read_binary_file_bytes(const std::filesystem::path& path) -> std::vector<std::uint8_t> {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.good());
  in.seekg(0, std::ios::end);
  const auto size = in.tellg();
  REQUIRE(size >= 0);
  in.seekg(0, std::ios::beg);

  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  if (!bytes.empty()) {
    in.read(reinterpret_cast<char*>(bytes.data()), size);
    REQUIRE((in.good() || in.eof()));
  }
  return bytes;
}

void require_copy_vs_auto_byref_equivalent(const std::string& source_text, const std::string& source_name,
                                           const std::size_t cutoff = 32) {
  const auto ir_program = lower_source_to_ir(source_text, source_name);

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result_copy = compiler.compile(ir_program, make_compile_options(false, false));
  const auto result_byref = compiler.compile(ir_program, make_compile_options(true, true, cutoff));

  REQUIRE(result_copy.has_value());
  REQUIRE(result_byref.has_value());

  std::ostringstream output_copy;
  std::ostringstream output_byref;
  const fleaux::vm::Runtime runtime;

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
    if (i > 0)
      out << ", ";
    out << values[i];
  }
  out << ")";
  return out.str();
}

auto make_task_spawn_tuple_literal(const std::vector<int>& values) -> std::string {
  std::ostringstream out;
  out << "(";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i > 0)
      out << ", ";
    out << "(Inc, " << values[i] << ") -> Std.Task.Spawn";
  }
  out << ")";
  return out.str();
}

auto parse_std_declared_names(const bool builtins_only) -> std::set<std::string> {
  std::ifstream in(repo_root_path() / "stdlib" / "Std.fleaux");
  REQUIRE(in.good());

  std::set<std::string> names;
  std::string line;
  // Accumulate a logical declaration that may span multiple physical lines.
  // A declaration starts with "let Std." and ends at the first ";".
  std::string decl;
  while (std::getline(in, line)) {
    if (decl.empty()) {
      if (line.find("let Std.") == std::string::npos) {
        continue;
      }
      decl = line;
    } else {
      decl += ' ';
      decl += line;
    }

    // Wait until the statement terminator is present.
    if (decl.find(';') == std::string::npos) {
      continue;
    }

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
  for (const auto& spec : fleaux::vm::all_callable_builtin_specs()) {
    names.insert(std::string{spec.name});
  }
  return names;
}

auto vm_catalog_constant_names() -> std::set<std::string> {
  std::set<std::string> names;
  for (const auto& spec : fleaux::vm::all_constant_builtin_specs()) {
    names.insert(std::string{spec.name});
  }
  return names;
}

}  // namespace

TEST_CASE("VM builtin catalog stays in sync with stdlib/Std.fleaux", "[bytecode]") {
  REQUIRE(vm_catalog_builtin_names() == parse_std_declared_names(true));
  REQUIRE(vm_catalog_constant_names() == parse_std_declared_names(false));
}

TEST_CASE("VM constant catalog values match the values declared in stdlib/Std.fleaux", "[bytecode][builtins]") {
  std::ifstream in(repo_root_path() / "stdlib" / "Std.fleaux");
  REQUIRE(in.good());

  // Collect 'let Std.Name(): Float64 :: <literal>;' declarations, which may span lines.
  std::map<std::string, double> declared_values;
  std::string line;
  std::string decl;
  while (std::getline(in, line)) {
    if (decl.empty() && !line.starts_with("let Std.")) {
      continue;
    }
    decl += decl.empty() ? line : " " + line;
    if (decl.find(';') == std::string::npos) {
      continue;
    }
    constexpr std::string_view kMarker = "(): Float64 :: ";
    if (const auto marker = decl.find(kMarker); marker != std::string::npos) {
      const std::string name = decl.substr(4, marker - 4);
      const auto value_start = marker + kMarker.size();
      const std::string literal = decl.substr(value_start, decl.find(';', value_start) - value_start);
      declared_values.insert_or_assign(name, std::stod(literal));
    }
    decl.clear();
  }

  REQUIRE_FALSE(fleaux::vm::kConstantBuiltinSpecs.empty());
  for (const auto& spec : fleaux::vm::kConstantBuiltinSpecs) {
    const std::string name{spec.name};
    INFO("constant: " << name);
    const auto declared = declared_values.find(name);
    REQUIRE(declared != declared_values.end());
    INFO("catalog value: " << spec.value << ", Std value: " << declared->second);
    REQUIRE(std::abs(spec.value - declared->second) <= 1e-15 * std::abs(declared->second));
  }
}

TEST_CASE("VM builtin catalog resolves overloaded stdlib symbol keys", "[bytecode][builtins][overload]") {
  REQUIRE(fleaux::vm::builtin_id_from_symbol_key("Std.Dict.Create#0") == fleaux::vm::BuiltinId::DictCreateVoid);
  REQUIRE(fleaux::vm::builtin_id_from_symbol_key("Std.Dict.Create#1") == fleaux::vm::BuiltinId::DictCreateDict);
  REQUIRE(fleaux::vm::builtin_id_from_symbol_key("Std.Exit#0") == fleaux::vm::BuiltinId::ExitVoid);
  REQUIRE(fleaux::vm::builtin_id_from_symbol_key("Std.Exit#1") == fleaux::vm::BuiltinId::ExitInt64);
  REQUIRE(fleaux::vm::builtin_id_from_symbol_key("Std.Input#0") == fleaux::vm::BuiltinId::InputVoid);
  REQUIRE(fleaux::vm::builtin_id_from_symbol_key("Std.Input#1") == fleaux::vm::BuiltinId::InputString);
  REQUIRE(fleaux::vm::builtin_id_from_symbol_key("Std.Tuple.Range#0") == fleaux::vm::BuiltinId::TupleRangeInt64);
  REQUIRE(fleaux::vm::builtin_id_from_symbol_key("Std.Tuple.Range#1") == fleaux::vm::BuiltinId::TupleRangeInt64Int64);
  REQUIRE(fleaux::vm::builtin_id_from_symbol_key("Std.Tuple.Range#2") ==
          fleaux::vm::BuiltinId::TupleRangeInt64Int64Int64);
  REQUIRE(fleaux::vm::builtin_id_from_symbol_key("Std.Slice#0") == fleaux::vm::BuiltinId::SliceTupleUInt64);
  REQUIRE(fleaux::vm::builtin_id_from_symbol_key("Std.Slice#1") == fleaux::vm::BuiltinId::SliceTupleUInt64UInt64);
  REQUIRE(fleaux::vm::builtin_id_from_symbol_key("Std.Slice#2") == fleaux::vm::BuiltinId::SliceTupleUInt64UInt64UInt64);
  REQUIRE(fleaux::vm::builtin_id_from_symbol_key("Std.String.Slice#0") ==
          fleaux::vm::BuiltinId::StringSliceStringUInt64);
  REQUIRE(fleaux::vm::builtin_id_from_symbol_key("Std.String.Slice#1") ==
          fleaux::vm::BuiltinId::StringSliceStringUInt64UInt64);
  REQUIRE(fleaux::vm::builtin_id_from_symbol_key("Std.String.Find#0") == fleaux::vm::BuiltinId::StringFindStringString);
  REQUIRE(fleaux::vm::builtin_id_from_symbol_key("Std.String.Find#1") ==
          fleaux::vm::BuiltinId::StringFindStringStringUInt64);
}

TEST_CASE("VM builtin catalog exposes complete scalar, variadic, and overloaded builtin arity contracts",
          "[bytecode][builtins][arity]") {
  for (const auto& spec : fleaux::vm::all_callable_builtin_specs()) {
    INFO(spec.name);
    REQUIRE(fleaux::vm::builtin_has_arity_contract(spec.id));
  }

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::UnaryPlus));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::UnaryPlus, 1U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::UnaryPlus, 0U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::TupleRangeInt64));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleRangeInt64, 1U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleRangeInt64, 2U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::TupleRangeInt64Int64));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleRangeInt64Int64, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleRangeInt64Int64, 1U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleRangeInt64Int64, 3U));
  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::BitAnd));
  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::TupleRangeInt64Int64Int64));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleRangeInt64Int64Int64, 3U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleRangeInt64Int64Int64, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleRangeInt64Int64Int64, 4U));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::BitNot, 1U));
  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::SliceTupleUInt64));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::SliceTupleUInt64, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::SliceTupleUInt64, 3U));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::BitShiftLeft, 2U));
  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::SliceTupleUInt64UInt64));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::SliceTupleUInt64UInt64, 3U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::SliceTupleUInt64UInt64, 2U));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::Not, 1U));
  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::SliceTupleUInt64UInt64UInt64));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::SliceTupleUInt64UInt64UInt64, 4U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::SliceTupleUInt64UInt64UInt64, 3U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::SliceTupleUInt64UInt64UInt64, 5U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::ToUInt64, 0U));
  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::StringSliceStringUInt64));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringSliceStringUInt64, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringSliceStringUInt64, 3U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::Cwd, 1U));
  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::StringSliceStringUInt64UInt64));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringSliceStringUInt64UInt64, 3U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringSliceStringUInt64UInt64, 2U));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::Printf, 4U));
  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::StringFindStringString));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringFindStringString, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringFindStringString, 3U));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::OSIsWindows, 0U));
  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::StringFindStringStringUInt64));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringFindStringStringUInt64, 3U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringFindStringStringUInt64, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringFindStringStringUInt64, 4U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::OSMakeTempFile, 1U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::PathJoin));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::PathJoin, 2U));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::PathJoin, 4U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::PathJoin, 1U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::PathWithExtension));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::PathWithExtension, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::PathWithExtension, 1U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::OSSetEnv));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::OSSetEnv, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::OSSetEnv, 1U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::FileReadChunk));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::FileReadChunk, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::FileReadChunk, 3U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::FileWithOpen));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::FileWithOpen, 3U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::FileWithOpen, 2U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::DictSet));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::DictSet, 3U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::DictSet, 2U));

  for (const auto& spec : fleaux::vm::all_constant_builtin_specs()) {
    INFO(spec.name);
    REQUIRE(fleaux::vm::builtin_has_arity_contract(spec.id));
    REQUIRE(fleaux::vm::builtin_accepts_arity(spec.id, 0U));
    REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(spec.id, 1U));
  }

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::DictGet));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::DictGet, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::DictGet, 1U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::DictGetDefault));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::DictGetDefault, 3U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::DictGetDefault, 2U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::DictMerge));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::DictMerge, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::DictMerge, 3U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::TupleAppend));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleAppend, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleAppend, 1U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::TupleZip));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleZip, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleZip, 3U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::TupleMap));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleMap, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleMap, 1U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::TupleFilter));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleFilter, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleFilter, 3U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::TupleFindIndex));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleFindIndex, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleFindIndex, 1U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::TupleAny));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleAny, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleAny, 3U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::TupleAll));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleAll, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleAll, 1U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::TupleReduce));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleReduce, 3U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleReduce, 2U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::StringSplit));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringSplit, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringSplit, 1U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::StringJoin));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringJoin, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringJoin, 3U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::StringContains));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringContains, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringContains, 1U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::StringCharAt));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringCharAt, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringCharAt, 3U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::StringReplace));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringReplace, 3U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringReplace, 2U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::StringRegexIsMatch));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringRegexIsMatch, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringRegexIsMatch, 1U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::StringRegexFind));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringRegexFind, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringRegexFind, 3U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::StringRegexReplace));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringRegexReplace, 3U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringRegexReplace, 2U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::StringRegexSplit));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringRegexSplit, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringRegexSplit, 1U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::TaskSpawn));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TaskSpawn, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TaskSpawn, 1U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::TaskWithTimeout));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TaskWithTimeout, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TaskWithTimeout, 3U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::ParallelMap));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::ParallelMap, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::ParallelMap, 1U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::ParallelWithOptions));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::ParallelWithOptions, 3U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::ParallelWithOptions, 2U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::ParallelForEach));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::ParallelForEach, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::ParallelForEach, 3U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::ParallelReduce));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::ParallelReduce, 3U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::ParallelReduce, 2U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::Cast));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::Cast, 1U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::Cast, 0U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::ToString));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::ToString, 1U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::ToString, 0U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::ExitVoid));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::ExitVoid, 0U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::ExitVoid, 1U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::ExitInt64));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::ExitInt64, 1U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::ExitInt64, 0U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::InputVoid));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::InputVoid, 0U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::InputVoid, 1U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::InputString));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::InputString, 1U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::InputString, 0U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::InputString, 2U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::TupleRangeInt64));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleRangeInt64, 1U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleRangeInt64, 2U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::TupleRangeInt64Int64));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleRangeInt64Int64, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleRangeInt64Int64, 1U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleRangeInt64Int64, 3U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::TupleRangeInt64Int64Int64));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleRangeInt64Int64Int64, 3U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleRangeInt64Int64Int64, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TupleRangeInt64Int64Int64, 4U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::SliceTupleUInt64));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::SliceTupleUInt64, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::SliceTupleUInt64, 3U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::SliceTupleUInt64UInt64));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::SliceTupleUInt64UInt64, 3U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::SliceTupleUInt64UInt64, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::SliceTupleUInt64UInt64, 4U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::SliceTupleUInt64UInt64UInt64));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::SliceTupleUInt64UInt64UInt64, 4U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::SliceTupleUInt64UInt64UInt64, 3U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::SliceTupleUInt64UInt64UInt64, 5U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::StringSliceStringUInt64));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringSliceStringUInt64, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringSliceStringUInt64, 1U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringSliceStringUInt64, 3U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::StringSliceStringUInt64UInt64));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringSliceStringUInt64UInt64, 3U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringSliceStringUInt64UInt64, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringSliceStringUInt64UInt64, 4U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::StringFindStringString));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringFindStringString, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringFindStringString, 1U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringFindStringString, 3U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::StringFindStringStringUInt64));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringFindStringStringUInt64, 3U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringFindStringStringUInt64, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringFindStringStringUInt64, 4U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::StringFormat));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringFormat, 1U));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringFormat, 4U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringFormat, 0U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::Help));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::Help, 1U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::Help, 0U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::OSEnv));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::OSEnv, 1U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::OSEnv, 0U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::FileOpen));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::FileOpen, 2U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::FileOpen, 1U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::DictKeys));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::DictKeys, 1U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::DictKeys, 0U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::RandomCreate));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::RandomCreate, 1U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::RandomCreate, 0U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::MathFloor));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::MathFloor, 1U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::MathFloor, 0U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::Type));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::Type, 1U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::Type, 0U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::ResultTag));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::ResultTag, 1U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::ResultTag, 0U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::TaskAwait));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TaskAwait, 1U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::TaskAwait, 0U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::StringParseInt64));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringParseInt64, 1U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::StringParseInt64, 0U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::Match));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::Match, 2U));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::Match, 5U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::Match, 1U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::Unwrap));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::Unwrap, 1U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::Unwrap, 0U));
  REQUIRE_FALSE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::Unwrap, 2U));

  REQUIRE(fleaux::vm::builtin_has_arity_contract(fleaux::vm::BuiltinId::Println));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::Println, 0U));
  REQUIRE(fleaux::vm::builtin_accepts_arity(fleaux::vm::BuiltinId::Println, 3U));
}

TEST_CASE("Bytecode compiler rejects malformed builtin arity for covered metadata families",
          "[bytecode][builtins][arity]") {
  SECTION("Tuple literal with wrong arity is rejected for Std.Add") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(1)}}),
                .rhs = make_ir_name_ref(std::string{"Std"}, "Add", std::string{"Std.Add"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Add' expects 2 arguments but got 1.");
  }

  SECTION("Tuple literal with wrong arity is rejected for Std.Help") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {}}),
                .rhs = make_ir_name_ref(std::string{"Std"}, "Help", std::string{"Std.Help"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Help' expects 1 argument but got 0.");
  }

  SECTION("Tuple-valued unary call is accepted for Std.Type") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                    .items = {make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                        .items = {make_int_constant_box(1), make_int_constant_box(2), make_int_constant_box(3)}})}}),
                .rhs = make_ir_name_ref(std::string{"Std"}, "Type", std::string{"Std.Type"})})},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE(result.has_value());
  }

  SECTION("Nested singleton tuple wrappers are preserved for exact-1 Std.Type calls") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                    .items = {make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(1)}})}}),
                .rhs = make_ir_name_ref(std::string{"Std"}, "Type", std::string{"Std.Type"})})},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE(result.has_value());
    const auto build_tuple_count = std::count_if(
        result->instructions.begin(), result->instructions.end(), [](const fleaux::bytecode::Instruction& ins) {
          return ins.opcode == fleaux::bytecode::Opcode::kBuildTuple && ins.operand == 1;
        });
    REQUIRE(build_tuple_count == 2);
  }

  SECTION("Tuple literal with wrong arity is rejected for Std.Match") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(1)}}),
                .rhs = make_ir_name_ref(std::string{"Std"}, "Match", std::string{"Std.Match"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Match' expects 2+ arguments but got 1.");
  }

  SECTION("Tuple literal with wrong arity is rejected for Std.Subtract") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(1)}}),
                .rhs = make_ir_name_ref(std::string{"Std"}, "Subtract", std::string{"Std.Subtract"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Subtract' expects 2 arguments but got 1.");
  }

  SECTION("Materialized zero-arg builtin value is rejected for non-nullary Std.Bit.Not") {
    fleaux::frontend::ir::IRProgram ir_program;
    auto name_ref = make_ir_name_ref(std::string{"Std"}, "Bit.Not", std::string{"Std.Bit.Not"});
    name_ref.materialize_as_value = true;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{.expr = make_ir_expr(std::move(name_ref))},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Bit.Not' expects 1 argument but got 0.");
  }

  SECTION("Tuple literal with wrong arity is rejected for unary Std.Not") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                    .items = {make_bool_constant_box(true), make_bool_constant_box(false)}}),
                .rhs = make_ir_name_ref(std::string{"Std"}, "Not", std::string{"Std.Not"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Not' expects 1 argument but got 2.");
  }

  SECTION("Materialized zero-arg builtin value is rejected for unary Std.ToUInt64") {
    fleaux::frontend::ir::IRProgram ir_program;
    auto name_ref = make_ir_name_ref(std::string{"Std"}, "ToUInt64", std::string{"Std.ToUInt64"});
    name_ref.materialize_as_value = true;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{.expr = make_ir_expr(std::move(name_ref))},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.ToUInt64' expects 1 argument but got 0.");
  }

  SECTION("Tuple literal with wrong arity is rejected for nullary Std.OS.Cwd") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(1)}}),
                .rhs = make_ir_name_ref(std::string{"Std.OS"}, "Cwd", std::string{"Std.OS.Cwd"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.OS.Cwd' expects 0 arguments but got 1.");
  }

  SECTION("Tuple literal with wrong arity is rejected for binary Std.OS.SetEnv") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {make_string_constant_box("NAME")}}),
                .rhs = make_ir_name_ref(std::string{"Std.OS"}, "SetEnv", std::string{"Std.OS.SetEnv"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.OS.SetEnv' expects 2 arguments but got 1.");
  }

  SECTION("Tuple literal with wrong arity is rejected for binary Std.File.ReadChunk") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                    .items = {make_int_constant_box(1), make_int_constant_box(2), make_int_constant_box(3)}}),
                .rhs = make_ir_name_ref(std::string{"Std.File"}, "ReadChunk", std::string{"Std.File.ReadChunk"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.File.ReadChunk' expects 2 arguments but got 3.");
  }

  SECTION("Tuple literal with wrong arity is rejected for ternary Std.File.WithOpen") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                    .items = {make_string_constant_box("file.txt"), make_string_constant_box("r")}}),
                .rhs = make_ir_name_ref(std::string{"Std.File"}, "WithOpen", std::string{"Std.File.WithOpen"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.File.WithOpen' expects 3 arguments but got 2.");
  }

  SECTION("Tuple literal with wrong arity is rejected for ternary Std.Dict.Set") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                    .items = {make_string_constant_box("dict"), make_string_constant_box("key")}}),
                .rhs = make_ir_name_ref(std::string{"Std.Dict"}, "Set", std::string{"Std.Dict.Set"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Dict.Set' expects 3 arguments but got 2.");
  }

  SECTION("Tuple literal with wrong arity is rejected for binary Std.Dict.Get") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {make_string_constant_box("dict")}}),
                .rhs = make_ir_name_ref(std::string{"Std.Dict"}, "Get", std::string{"Std.Dict.Get"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Dict.Get' expects 2 arguments but got 1.");
  }

  SECTION("Tuple literal with wrong arity is rejected for binary Std.Tuple.Append") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(1)}}),
                .rhs = make_ir_name_ref(std::string{"Std.Tuple"}, "Append", std::string{"Std.Tuple.Append"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Tuple.Append' expects 2 arguments but got 1.");
  }

  SECTION("Tuple literal with wrong arity is rejected for binary Std.Tuple.Zip") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                    .items = {make_int_constant_box(1), make_int_constant_box(2), make_int_constant_box(3)}}),
                .rhs = make_ir_name_ref(std::string{"Std.Tuple"}, "Zip", std::string{"Std.Tuple.Zip"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Tuple.Zip' expects 2 arguments but got 3.");
  }

  SECTION("Tuple literal with wrong arity is rejected for binary Std.Tuple.Map") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(1)}}),
                .rhs = make_ir_name_ref(std::string{"Std.Tuple"}, "Map", std::string{"Std.Tuple.Map"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Tuple.Map' expects 2 arguments but got 1.");
  }

  SECTION("Tuple literal with wrong arity is rejected for binary Std.Tuple.Any") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                    .items = {make_int_constant_box(1), make_int_constant_box(2), make_int_constant_box(3)}}),
                .rhs = make_ir_name_ref(std::string{"Std.Tuple"}, "Any", std::string{"Std.Tuple.Any"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Tuple.Any' expects 2 arguments but got 3.");
  }

  SECTION("Tuple literal with wrong arity is rejected for ternary Std.Tuple.Reduce") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(
                    fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(1), make_int_constant_box(2)}}),
                .rhs = make_ir_name_ref(std::string{"Std.Tuple"}, "Reduce", std::string{"Std.Tuple.Reduce"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Tuple.Reduce' expects 3 arguments but got 2.");
  }

  SECTION("Tuple literal with wrong arity is rejected for binary Std.String.Split") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {make_string_constant_box("a,b,c")}}),
                .rhs = make_ir_name_ref(std::string{"Std.String"}, "Split", std::string{"Std.String.Split"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.String.Split' expects 2 arguments but got 1.");
  }

  SECTION("Tuple literal with wrong arity is rejected for binary Std.String.Join") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {make_string_constant_box(","),
                                                                               make_string_constant_box("abc"),
                                                                               make_string_constant_box("def")}}),
                .rhs = make_ir_name_ref(std::string{"Std.String"}, "Join", std::string{"Std.String.Join"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.String.Join' expects 2 arguments but got 3.");
  }

  SECTION("Tuple literal with wrong arity is rejected for ternary Std.String.Replace") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                    .items = {make_string_constant_box("a,b,c"), make_string_constant_box(",")}}),
                .rhs = make_ir_name_ref(std::string{"Std.String"}, "Replace", std::string{"Std.String.Replace"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.String.Replace' expects 3 arguments but got 2.");
  }

  SECTION("Tuple literal with wrong arity is rejected for binary Std.String.Regex.IsMatch") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {make_string_constant_box("abc")}}),
                .rhs = make_ir_name_ref(std::string{"Std.String.Regex"}, "IsMatch",
                                        std::string{"Std.String.Regex.IsMatch"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.String.Regex.IsMatch' expects 2 arguments but got 1.");
  }

  SECTION("Tuple literal with wrong arity is rejected for ternary Std.String.Regex.Replace") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                    .items = {make_string_constant_box("abc-123"), make_string_constant_box("[0-9]+")}}),
                .rhs = make_ir_name_ref(std::string{"Std.String.Regex"}, "Replace",
                                        std::string{"Std.String.Regex.Replace"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.String.Regex.Replace' expects 3 arguments but got 2.");
  }

  SECTION("Tuple literal with wrong arity is rejected for binary Std.Task.Spawn") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(1)}}),
                .rhs = make_ir_name_ref(std::string{"Std.Task"}, "Spawn", std::string{"Std.Task.Spawn"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Task.Spawn' expects 2 arguments but got 1.");
  }

  SECTION("Tuple literal with wrong arity is rejected for ternary Std.Parallel.Reduce") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(
                    fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(1), make_int_constant_box(2)}}),
                .rhs = make_ir_name_ref(std::string{"Std.Parallel"}, "Reduce", std::string{"Std.Parallel.Reduce"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Parallel.Reduce' expects 3 arguments but got 2.");
  }

  SECTION("Tuple literal with wrong arity is rejected for ternary Std.Select") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                    .items = {make_bool_constant_box(true), make_int_constant_box(10)}}),
                .rhs = make_ir_name_ref(std::string{"Std"}, "Select", std::string{"Std.Select"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Select' expects 3 arguments but got 2.");
  }

  SECTION("Tuple literal with wrong arity is rejected for nullary Std.GetArgs") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {make_string_constant_box("ignored")}}),
                .rhs = make_ir_name_ref(std::string{"Std"}, "GetArgs", std::string{"Std.GetArgs"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.GetArgs' expects 0 arguments but got 1.");
  }

  SECTION("Tuple literal with wrong arity is rejected for binary Std.ElementAt") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(1)}}),
                .rhs = make_ir_name_ref(std::string{"Std"}, "ElementAt", std::string{"Std.ElementAt"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.ElementAt' expects 2 arguments but got 1.");
  }

  SECTION("Tuple literal with wrong arity is rejected for binary Std.Take") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(1)}}),
                .rhs = make_ir_name_ref(std::string{"Std"}, "Take", std::string{"Std.Take"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Take' expects 2 arguments but got 1.");
  }

  SECTION("Tuple literal with wrong arity is rejected for binary Std.Drop") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                    .items = {make_int_constant_box(1), make_int_constant_box(2), make_int_constant_box(3)}}),
                .rhs = make_ir_name_ref(std::string{"Std"}, "Drop", std::string{"Std.Drop"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Drop' expects 2 arguments but got 3.");
  }

  SECTION("Tuple literal with wrong arity is rejected for binary Std.Try") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(1)}}),
                .rhs = make_ir_name_ref(std::string{"Std"}, "Try", std::string{"Std.Try"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Try' expects 2 arguments but got 1.");
  }

  SECTION("Tuple literal with wrong arity is rejected for binary Std.Apply") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(1)}}),
                .rhs = make_ir_name_ref(std::string{"Std"}, "Apply", std::string{"Std.Apply"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Apply' expects 2 arguments but got 1.");
  }

  SECTION("Tuple literal with wrong arity is rejected for quaternary Std.Branch") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                    .items = {make_bool_constant_box(true), make_int_constant_box(1), make_int_constant_box(2)}}),
                .rhs = make_ir_name_ref(std::string{"Std"}, "Branch", std::string{"Std.Branch"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Branch' expects 4 arguments but got 3.");
  }

  SECTION("Tuple literal with wrong arity is rejected for ternary Std.Loop") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(
                    fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(0), make_int_constant_box(1)}}),
                .rhs = make_ir_name_ref(std::string{"Std"}, "Loop", std::string{"Std.Loop"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Loop' expects 3 arguments but got 2.");
  }

  SECTION("Tuple literal with wrong arity is rejected for quaternary Std.LoopN") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                    .items = {make_int_constant_box(0), make_int_constant_box(1), make_int_constant_box(2)}}),
                .rhs = make_ir_name_ref(std::string{"Std"}, "LoopN", std::string{"Std.LoopN"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.LoopN' expects 4 arguments but got 3.");
  }

  SECTION("Tuple literal with wrong arity is rejected for binary Std.Array.GetAt") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(1)}}),
                .rhs = make_ir_name_ref(std::string{"Std.Array"}, "GetAt", std::string{"Std.Array.GetAt"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Array.GetAt' expects 2 arguments but got 1.");
  }

  SECTION("Tuple literal with wrong arity is rejected for ternary Std.Array.SetAt") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(
                    fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(1), make_int_constant_box(2)}}),
                .rhs = make_ir_name_ref(std::string{"Std.Array"}, "SetAt", std::string{"Std.Array.SetAt"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Array.SetAt' expects 3 arguments but got 2.");
  }

  SECTION("Tuple literal with wrong arity is rejected for ternary Std.Array.InsertAt") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(
                    fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(1), make_int_constant_box(2)}}),
                .rhs = make_ir_name_ref(std::string{"Std.Array"}, "InsertAt", std::string{"Std.Array.InsertAt"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Array.InsertAt' expects 3 arguments but got 2.");
  }

  SECTION("Tuple literal with wrong arity is rejected for binary Std.Array.RemoveAt") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                    .items = {make_int_constant_box(1), make_int_constant_box(2), make_int_constant_box(3)}}),
                .rhs = make_ir_name_ref(std::string{"Std.Array"}, "RemoveAt", std::string{"Std.Array.RemoveAt"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Array.RemoveAt' expects 2 arguments but got 3.");
  }

  SECTION("Tuple literal with wrong arity is rejected for ternary Std.Array.Slice") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(
                    fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(1), make_int_constant_box(2)}}),
                .rhs = make_ir_name_ref(std::string{"Std.Array"}, "Slice", std::string{"Std.Array.Slice"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Array.Slice' expects 3 arguments but got 2.");
  }

  SECTION("Tuple literal with wrong arity is rejected for binary Std.Array.Concat") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(1)}}),
                .rhs = make_ir_name_ref(std::string{"Std.Array"}, "Concat", std::string{"Std.Array.Concat"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Array.Concat' expects 2 arguments but got 1.");
  }

  SECTION("Tuple literal with wrong arity is rejected for quaternary Std.Array.SetAt2D") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                    .items = {make_int_constant_box(1), make_int_constant_box(2), make_int_constant_box(3)}}),
                .rhs = make_ir_name_ref(std::string{"Std.Array"}, "SetAt2D", std::string{"Std.Array.SetAt2D"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Array.SetAt2D' expects 4 arguments but got 3.");
  }

  SECTION("Tuple literal with wrong arity is rejected for quaternary Std.Array.Fill") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                    .items = {make_int_constant_box(1), make_int_constant_box(2), make_int_constant_box(3)}}),
                .rhs = make_ir_name_ref(std::string{"Std.Array"}, "Fill", std::string{"Std.Array.Fill"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Array.Fill' expects 4 arguments but got 3.");
  }

  SECTION("Tuple literal with wrong arity is rejected for quinary Std.Array.Slice2D") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(
                    fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(1), make_int_constant_box(2),
                                                                make_int_constant_box(3), make_int_constant_box(4)}}),
                .rhs = make_ir_name_ref(std::string{"Std.Array"}, "Slice2D", std::string{"Std.Array.Slice2D"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Array.Slice2D' expects 5 arguments but got 4.");
  }

  SECTION("Tuple literal with wrong arity is rejected for ternary Std.Array.Reshape") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(
                    fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(1), make_int_constant_box(2)}}),
                .rhs = make_ir_name_ref(std::string{"Std.Array"}, "Reshape", std::string{"Std.Array.Reshape"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Array.Reshape' expects 3 arguments but got 2.");
  }

  SECTION("Tuple literal with wrong arity is rejected for binary Std.Array.GetAtND") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(1)}}),
                .rhs = make_ir_name_ref(std::string{"Std.Array"}, "GetAtND", std::string{"Std.Array.GetAtND"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Array.GetAtND' expects 2 arguments but got 1.");
  }

  SECTION("Tuple literal with wrong arity is rejected for ternary Std.Array.SetAtND") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(
                    fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(1), make_int_constant_box(2)}}),
                .rhs = make_ir_name_ref(std::string{"Std.Array"}, "SetAtND", std::string{"Std.Array.SetAtND"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Array.SetAtND' expects 3 arguments but got 2.");
  }

  SECTION("Tuple literal with wrong arity is rejected for binary Std.Array.ReshapeND") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                    .items = {make_int_constant_box(1), make_int_constant_box(2), make_int_constant_box(3)}}),
                .rhs = make_ir_name_ref(std::string{"Std.Array"}, "ReshapeND", std::string{"Std.Array.ReshapeND"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Array.ReshapeND' expects 2 arguments but got 3.");
  }

  SECTION("Tuple literal with wrong arity is rejected for ternary Std.Math.Clamp") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(
                    fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(9), make_int_constant_box(0)}}),
                .rhs = make_ir_name_ref(std::string{"Std.Math"}, "Clamp", std::string{"Std.Math.Clamp"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Math.Clamp' expects 3 arguments but got 2.");
  }

  SECTION("Tuple literal with wrong arity is rejected for nullary Std.Exit overload") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(1)}}),
                .rhs = make_ir_name_ref(std::string{"Std"}, "Exit", std::string{"Std.Exit#0"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Exit' expects 0 arguments but got 1.");
  }

  SECTION("Tuple literal with wrong arity is rejected for unary Std.Exit overload") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {}}),
                .rhs = make_ir_name_ref(std::string{"Std"}, "Exit", std::string{"Std.Exit#1"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Exit' expects 1 argument but got 0.");
  }

  SECTION("Tuple literal with wrong arity is rejected for nullary Std.Dict.Create overload") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {make_string_constant_box("seed")}}),
                .rhs = make_ir_name_ref(std::string{"Std.Dict"}, "Create", std::string{"Std.Dict.Create#0"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Dict.Create' expects 0 arguments but got 1.");
  }

  SECTION("Tuple literal with wrong arity is rejected for unary Std.Dict.Create overload") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {}}),
                .rhs = make_ir_name_ref(std::string{"Std.Dict"}, "Create", std::string{"Std.Dict.Create#1"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Dict.Create' expects 1 argument but got 0.");
  }

  SECTION("Tuple literal with wrong arity is rejected for nullary Std.Input overload") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {make_string_constant_box("name> ")}}),
                .rhs = make_ir_name_ref(std::string{"Std"}, "Input", std::string{"Std.Input#0"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Input' expects 0 arguments but got 1.");
  }

  SECTION("Tuple literal with wrong arity is rejected for prompt Std.Input overload") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                    .items = {make_string_constant_box("name> "), make_string_constant_box("extra")}}),
                .rhs = make_ir_name_ref(std::string{"Std"}, "Input", std::string{"Std.Input#1"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Input' expects 1 argument but got 2.");
  }

  SECTION("Tuple literal with wrong arity is rejected for variadic Std.Printf") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {}}),
                .rhs = make_ir_name_ref(std::string{"Std"}, "Printf", std::string{"Std.Printf"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Printf' expects 1+ arguments but got 0.");
  }

  SECTION("Tuple literal with wrong arity is rejected for variadic Std.Path.Join") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {make_string_constant_box("tmp")}}),
                .rhs = make_ir_name_ref(std::string{"Std"}, "Path.Join", std::string{"Std.Path.Join"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Path.Join' expects 2+ arguments but got 1.");
  }

  SECTION("Tuple literal with wrong arity is rejected for variadic Std.String.Format") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {}}),
                .rhs = make_ir_name_ref(std::string{"Std.String"}, "Format", std::string{"Std.String.Format"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.String.Format' expects 1+ arguments but got 0.");
  }

  SECTION("Tuple literal with wrong arity is rejected for unary Std.Unwrap") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(
                    fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(1), make_int_constant_box(2)}}),
                .rhs = make_ir_name_ref(std::string{"Std"}, "Unwrap", std::string{"Std.Unwrap"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Unwrap' expects 1 argument but got 2.");
  }

  SECTION("Tuple literal with wrong arity is rejected for Std.Tuple.Range") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(
                    fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(0), make_int_constant_box(1),
                                                                make_int_constant_box(2), make_int_constant_box(3)}}),
                .rhs = make_ir_name_ref(std::string{"Std"}, "Tuple.Range", std::string{"Std.Tuple.Range#2"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Tuple.Range' expects 3 arguments but got 4.");
  }

  SECTION("Tuple literal with wrong arity is rejected for Std.String.Slice") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {make_string_constant_box("abcdef")}}),
                .rhs = make_ir_name_ref(std::string{"Std.String"}, "Slice", std::string{"Std.String.Slice#0"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.String.Slice' expects 2 arguments but got 1.");
  }

  SECTION("Tuple literal with wrong arity is rejected for Std.String.Find") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                    .items = {make_string_constant_box("abcabc"), make_string_constant_box("bc"),
                              make_int_constant_box(2), make_int_constant_box(4)}}),
                .rhs = make_ir_name_ref(std::string{"Std.String"}, "Find", std::string{"Std.String.Find#1"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.String.Find' expects 3 arguments but got 4.");
  }

  SECTION("Tuple literal with wrong arity is rejected for Std.Slice") {
    fleaux::frontend::ir::IRProgram ir_program;
    ir_program.expressions = {
        fleaux::frontend::ir::IRExprStatement{
            .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
                .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                    .items = {make_ir_box(fleaux::frontend::ir::IRTupleExpr{.items = {make_int_constant_box(10),
                                                                                      make_int_constant_box(20),
                                                                                      make_int_constant_box(30)}}),
                              make_int_constant_box(0), make_int_constant_box(3), make_int_constant_box(1),
                              make_int_constant_box(9)}}),
                .rhs = make_ir_name_ref(std::string{"Std"}, "Slice", std::string{"Std.Slice#2"}),
            })},
    };

    const fleaux::bytecode::BytecodeCompiler compiler;
    const auto result = compiler.compile(ir_program);

    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message ==
            "Builtin arity mismatch in bytecode compiler: 'Std.Slice' expects 4 arguments but got 5.");
  }
}

// ---------------------------------------------------------------------------
// Core pipeline: (4, 5) -> Std.Add -> Std.Println
// Expected codegen:
//   [0] kPushConst     idx(4)
//   [1] kPushConst     idx(5)
//   [2] kAdd
//   [3] kCallBuiltin   idx(Std.Println)
//   [4] kPop
//   [5] kHalt
// ---------------------------------------------------------------------------
TEST_CASE("Bytecode compiler emits pipeline with native Std.Add lowering", "[bytecode]") {
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
  REQUIRE(instr[2].opcode == fleaux::bytecode::Opcode::kAdd);
  // Println is variadic: its one argument travels in a one-item argument tuple.
  REQUIRE(instr[3].opcode == fleaux::bytecode::Opcode::kBuildTuple);
  REQUIRE(instr[3].operand == 1);
  REQUIRE(instr[4].opcode == fleaux::bytecode::Opcode::kCallBuiltin);
  REQUIRE(fleaux::vm::builtin_name(*fleaux::vm::builtin_id_from_operand(instr[4].operand)) == "Std.Println");
  REQUIRE(instr[5].opcode == fleaux::bytecode::Opcode::kPop);
  REQUIRE(instr[6].opcode == fleaux::bytecode::Opcode::kHalt);
}

TEST_CASE("Bytecode compiler emits builtin callsite metadata for module and function streams",
          "[bytecode][verification][builtins]") {
  const auto ir_program = lower_source_to_ir(
      "import Std;\n"
      "let ShowHelp(name: String): Any = (name) -> Std.Help;\n"
      "(\"Std.Add\") -> ShowHelp -> Std.Println;\n",
      "bytecode_builtin_callsite_metadata.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);
  REQUIRE(result.has_value());

  const auto& module = *result;
  REQUIRE(module.builtin_callsites.size() == 2U);

  const auto& function_callsite = module.builtin_callsites[0];
  REQUIRE(function_callsite.owner_kind == fleaux::bytecode::BuiltinCallsiteOwnerKind::kFunction);
  REQUIRE(function_callsite.owner_index < module.functions.size());
  REQUIRE(function_callsite.instruction_index < module.functions[function_callsite.owner_index].instructions.size());
  REQUIRE(function_callsite.builtin_id == fleaux::vm::BuiltinId::Help);
  REQUIRE(function_callsite.call_shape == fleaux::vm::BuiltinCallShape::kDirectValue);
  REQUIRE(function_callsite.static_argument_count == 1U);
  REQUIRE(module.functions[function_callsite.owner_index].instructions[function_callsite.instruction_index].opcode ==
          fleaux::bytecode::Opcode::kCallBuiltin);

  const auto& module_callsite = module.builtin_callsites[1];
  REQUIRE(module_callsite.owner_kind == fleaux::bytecode::BuiltinCallsiteOwnerKind::kModule);
  REQUIRE(module_callsite.owner_index == 0U);
  REQUIRE(module_callsite.instruction_index < module.instructions.size());
  REQUIRE(module_callsite.builtin_id == fleaux::vm::BuiltinId::Println);
  REQUIRE(module_callsite.call_shape == fleaux::vm::BuiltinCallShape::kTupleExpanded);
  REQUIRE(module_callsite.static_argument_count == 1U);
  REQUIRE(module.instructions[module_callsite.instruction_index].opcode == fleaux::bytecode::Opcode::kCallBuiltin);

  const auto verified = fleaux::bytecode::verify_module_builtin_callsites(module);
  REQUIRE(verified.has_value());
}

TEST_CASE("Bytecode compiler emits callable-slot metadata for higher-order builtin callsites",
          "[bytecode][verification][builtins]") {
  const auto ir_program = lower_source_to_ir(
      "import Std;\n"
      "let Inc(x: Float64): Float64 = (x, 1.0) -> Std.Add;\n"
      "(5.0, Inc) -> Std.Apply -> Std.Println;\n"
      "(5.0, (x: Float64): Float64 = (x, 2.0) -> Std.Add) -> Std.Apply -> Std.Println;\n"
      "(5.0, Std.UnaryMinus<Float64>) -> Std.Apply -> Std.Println;\n",
      "bytecode_builtin_callable_slot_metadata.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);
  REQUIRE(result.has_value());

  const auto& module = *result;
  std::vector<fleaux::bytecode::BuiltinCallsiteRecord> apply_callsites;
  for (const auto& callsite : module.builtin_callsites) {
    if (callsite.builtin_id == fleaux::vm::BuiltinId::Apply) {
      apply_callsites.push_back(callsite);
    }
  }

  REQUIRE(apply_callsites.size() == 3U);
  REQUIRE(apply_callsites[0].callable_slots.size() == 1U);
  REQUIRE(apply_callsites[0].callable_slots[0].slot_index == 1U);
  REQUIRE(apply_callsites[0].callable_slots[0].kind == fleaux::bytecode::BuiltinCallableSlotKind::kFunction);
  REQUIRE(apply_callsites[0].callable_slots[0].declared_arity == 1U);
  REQUIRE_FALSE(apply_callsites[0].callable_slots[0].has_variadic_tail);

  REQUIRE(apply_callsites[1].callable_slots.size() == 1U);
  REQUIRE(apply_callsites[1].callable_slots[0].slot_index == 1U);
  REQUIRE(apply_callsites[1].callable_slots[0].kind == fleaux::bytecode::BuiltinCallableSlotKind::kClosure);
  REQUIRE(apply_callsites[1].callable_slots[0].declared_arity == 1U);
  REQUIRE_FALSE(apply_callsites[1].callable_slots[0].has_variadic_tail);

  REQUIRE(apply_callsites[2].callable_slots.size() == 1U);
  REQUIRE(apply_callsites[2].callable_slots[0].slot_index == 1U);
  REQUIRE(apply_callsites[2].callable_slots[0].kind == fleaux::bytecode::BuiltinCallableSlotKind::kBuiltin);
  REQUIRE(apply_callsites[2].callable_slots[0].target_index ==
          static_cast<std::uint32_t>(fleaux::vm::BuiltinId::UnaryMinus));
  REQUIRE(apply_callsites[2].callable_slots[0].declared_arity == 1U);
  REQUIRE_FALSE(apply_callsites[2].callable_slots[0].has_variadic_tail);

  const auto verified = fleaux::bytecode::verify_module_builtin_callsites(module);
  if (!verified.has_value()) {
    FAIL(verified.error().message);
  }
  REQUIRE(verified.has_value());
}

TEST_CASE("Bytecode compiler emits Dict<String, Any> options metadata for Parallel.WithOptions",
          "[bytecode][verification][builtins]") {
  const auto ir_program = lower_source_to_ir(
      "import Std;\n"
      "let Inc(x: Int64): Int64 = (x, 1) -> Std.Add;\n"
      "let BuildOptions(): Dict<String, Any> = () -> Std.Dict.Create -> (_, \"max_workers\", 2) -> Std.Dict.Set;\n"
      "((1, 2, 3), Inc, () -> BuildOptions) -> Std.Parallel.WithOptions -> Std.Result.IsOk -> Std.Println;\n",
      "bytecode_parallel_with_options_metadata.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);
  REQUIRE(result.has_value());

  const auto& module = *result;
  const auto callsite_it = std::find_if(module.builtin_callsites.begin(), module.builtin_callsites.end(),
                                        [](const fleaux::bytecode::BuiltinCallsiteRecord& record) {
                                          return record.builtin_id == fleaux::vm::BuiltinId::ParallelWithOptions;
                                        });
  REQUIRE(callsite_it != module.builtin_callsites.end());
  REQUIRE(callsite_it->value_slots.size() == 1U);
  REQUIRE(callsite_it->value_slots[0].slot_index == 2U);
  REQUIRE(callsite_it->value_slots[0].summary_kind == fleaux::bytecode::BuiltinValueSlotSummaryKind::kDictStringAny);
  REQUIRE(callsite_it->value_slots[0].source_kind == fleaux::bytecode::BuiltinValueSlotSourceKind::kFunctionReturn);
  REQUIRE(callsite_it->value_slots[0].source_index < module.functions.size());
  REQUIRE(module.functions[callsite_it->value_slots[0].source_index].return_type_is_dict_string_any);

  const auto verified = fleaux::bytecode::verify_module_builtin_callsites(module);
  REQUIRE(verified.has_value());
}

TEST_CASE("Bytecode compiler emits exact UInt64 metadata for selected builtin value slots",
          "[bytecode][verification][builtins]") {
  const auto ir_program = lower_source_to_ir(
      "import Std;\n"
      "let BuildIndex(): UInt64 = 1u64;\n"
      "(\"abc\", () -> BuildIndex) -> Std.String.CharAt -> Std.Println;\n",
      "bytecode_exact_uint64_value_slot_metadata.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);
  REQUIRE(result.has_value());

  const auto& module = *result;
  const auto callsite_it = std::find_if(module.builtin_callsites.begin(), module.builtin_callsites.end(),
                                        [](const fleaux::bytecode::BuiltinCallsiteRecord& record) {
                                          return record.builtin_id == fleaux::vm::BuiltinId::StringCharAt;
                                        });
  REQUIRE(callsite_it != module.builtin_callsites.end());
  REQUIRE(callsite_it->call_shape == fleaux::vm::BuiltinCallShape::kTupleExpanded);
  REQUIRE(callsite_it->static_argument_count == 2U);
  REQUIRE(callsite_it->value_slots.size() == 1U);
  REQUIRE(callsite_it->value_slots[0].slot_index == 1U);
  REQUIRE(callsite_it->value_slots[0].summary_kind == fleaux::bytecode::BuiltinValueSlotSummaryKind::kExactUInt64);
  REQUIRE(callsite_it->value_slots[0].source_kind == fleaux::bytecode::BuiltinValueSlotSourceKind::kFunctionReturn);
  REQUIRE(callsite_it->value_slots[0].source_index < module.functions.size());
  REQUIRE(module.functions[callsite_it->value_slots[0].source_index].return_type_is_exact_uint64);

  const auto verified = fleaux::bytecode::verify_module_builtin_callsites(module);
  REQUIRE(verified.has_value());
}

TEST_CASE("Bytecode compiler emits exact Int64 metadata for selected builtin value slots",
          "[bytecode][verification][builtins]") {
  const auto ir_program = lower_source_to_ir(
      "import Std;\n"
      "let BuildStop(): Int64 = 5;\n"
      "(() -> BuildStop) -> Std.Tuple.Range -> Std.Println;\n",
      "bytecode_exact_int64_value_slot_metadata.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);
  REQUIRE(result.has_value());

  const auto& module = *result;
  const auto callsite_it = std::find_if(module.builtin_callsites.begin(), module.builtin_callsites.end(),
                                        [](const fleaux::bytecode::BuiltinCallsiteRecord& record) {
                                          return record.builtin_id == fleaux::vm::BuiltinId::TupleRangeInt64;
                                        });
  REQUIRE(callsite_it != module.builtin_callsites.end());
  REQUIRE(callsite_it->call_shape == fleaux::vm::BuiltinCallShape::kDirectValue);
  REQUIRE(callsite_it->static_argument_count == 1U);
  REQUIRE(callsite_it->value_slots.size() == 1U);
  REQUIRE(callsite_it->value_slots[0].slot_index == 0U);
  REQUIRE(callsite_it->value_slots[0].summary_kind == fleaux::bytecode::BuiltinValueSlotSummaryKind::kExactInt64);
  REQUIRE(callsite_it->value_slots[0].source_kind == fleaux::bytecode::BuiltinValueSlotSourceKind::kFunctionReturn);
  REQUIRE(callsite_it->value_slots[0].source_index < module.functions.size());
  REQUIRE(module.functions[callsite_it->value_slots[0].source_index].return_type_is_exact_int64);

  const auto verified = fleaux::bytecode::verify_module_builtin_callsites(module);
  REQUIRE(verified.has_value());
}

TEST_CASE("Bytecode compiler emits opaque handle metadata for selected builtin value slots",
          "[bytecode][verification][builtins]") {
  const auto ir_program = lower_source_to_ir(
      "import Std;\n"
      "let Inc(x: Int64): Int64 = (x, 1) -> Std.Add;\n"
      "let BuildTask(): TaskHandle<Int64> = (Inc, 1) -> Std.Task.Spawn;\n"
      "(() -> BuildTask) -> Std.Task.Await -> Std.Result.IsOk -> Std.Println;\n",
      "bytecode_opaque_handle_value_slot_metadata.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);
  REQUIRE(result.has_value());

  const auto& module = *result;
  const auto callsite_it = std::find_if(module.builtin_callsites.begin(), module.builtin_callsites.end(),
                                        [](const fleaux::bytecode::BuiltinCallsiteRecord& record) {
                                          return record.builtin_id == fleaux::vm::BuiltinId::TaskAwait;
                                        });
  REQUIRE(callsite_it != module.builtin_callsites.end());
  REQUIRE(callsite_it->call_shape == fleaux::vm::BuiltinCallShape::kDirectValue);
  REQUIRE(callsite_it->static_argument_count == 1U);
  REQUIRE(callsite_it->value_slots.size() == 1U);
  REQUIRE(callsite_it->value_slots[0].slot_index == 0U);
  REQUIRE(callsite_it->value_slots[0].summary_kind == fleaux::bytecode::BuiltinValueSlotSummaryKind::kTaskHandle);
  REQUIRE(callsite_it->value_slots[0].source_kind == fleaux::bytecode::BuiltinValueSlotSourceKind::kFunctionReturn);
  REQUIRE(callsite_it->value_slots[0].source_index < module.functions.size());
  REQUIRE(module.functions[callsite_it->value_slots[0].source_index].return_type_is_task_handle);

  const auto verified = fleaux::bytecode::verify_module_builtin_callsites(module);
  REQUIRE(verified.has_value());
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

TEST_CASE("Bytecode compiler dispatches direct calls to the resolved stdlib builtin overload",
          "[bytecode][builtins][overload]") {
  const auto ir_program = lower_source_to_ir(
      "type Dict<K, V> :: __builtin__;\n"
      "let Std.Dict.Create(): Dict<Never, Never> :: __builtin__;\n"
      "let Std.Dict.Create<K, V>(dict: Dict<K, V>): Dict<K, V> :: __builtin__;\n"
      "let Std.Dict.Set<K, V>(dict: Dict<K, V>, key: K, value: V): Dict<K, V> :: __builtin__;\n"
      "let MakeDict(): Dict<String, Int64> = (() -> Std.Dict.Create, \"a\", 1) -> Std.Dict.Set;\n"
      "() -> Std.Dict.Create;\n"
      "() -> MakeDict -> Std.Dict.Create;\n",
      "bytecode_builtin_overloads.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);
  REQUIRE(result.has_value());

  std::vector<fleaux::vm::BuiltinId> called_builtin_ids;
  const auto collect_builtins = [&](const auto& instructions) {
    for (const auto& instruction : instructions) {
      if (instruction.opcode != fleaux::bytecode::Opcode::kCallBuiltin) {
        continue;
      }
      const auto builtin_id = fleaux::vm::builtin_id_from_operand(instruction.operand);
      REQUIRE(builtin_id.has_value());
      called_builtin_ids.push_back(*builtin_id);
    }
  };

  collect_builtins(result->instructions);
  for (const auto& function : result->functions) {
    collect_builtins(function.instructions);
  }

  REQUIRE(std::ranges::find(called_builtin_ids, fleaux::vm::BuiltinId::DictCreateVoid) != called_builtin_ids.end());
  REQUIRE(std::ranges::find(called_builtin_ids, fleaux::vm::BuiltinId::DictCreateDict) != called_builtin_ids.end());
}

TEST_CASE("Bytecode compiler emits native opcode for binary operator shorthand", "[bytecode]") {
  const auto ir_program = lower_source_to_ir("(4, 5) -> + -> Std.Println;\n", "bytecode_native_add.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  const auto& instr = result->instructions;
  REQUIRE(instr.size() == 7);
  REQUIRE(instr[0].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(std::get<std::int64_t>(result->constants.at(static_cast<std::size_t>(instr[0].operand)).data) == 4);
  REQUIRE(instr[1].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(std::get<std::int64_t>(result->constants.at(static_cast<std::size_t>(instr[1].operand)).data) == 5);
  REQUIRE(instr[2].opcode == fleaux::bytecode::Opcode::kAdd);
  // Println is variadic: its one argument travels in a one-item argument tuple.
  REQUIRE(instr[3].opcode == fleaux::bytecode::Opcode::kBuildTuple);
  REQUIRE(instr[3].operand == 1);
  REQUIRE(instr[4].opcode == fleaux::bytecode::Opcode::kCallBuiltin);
  REQUIRE(fleaux::vm::builtin_name(*fleaux::vm::builtin_id_from_operand(instr[4].operand)) == "Std.Println");
  REQUIRE(instr[5].opcode == fleaux::bytecode::Opcode::kPop);
  REQUIRE(instr[6].opcode == fleaux::bytecode::Opcode::kHalt);
}

TEST_CASE("Bytecode compiler rejects explicit Std.Ref and Std.Deref without catalog entries", "[bytecode][value_ref]") {
  // Std.Ref and Std.Deref are internal by-ref helpers. The VM now resolves builtins
  // through the explicit BuiltinId catalog, so user-authored builtin declarations do
  // not manufacture new runtime builtin IDs.
  const auto ir_program = lower_source_to_ir(
      "let Std.Ref(value: Any): Any :: __builtin__;\n"
      "let Std.Deref(value_ref: Any): Any :: __builtin__;\n"
      "let Std.Println(args: Any...): Tuple<Any...> :: __builtin__;\n"
      "((1, 2, 3)) -> Std.Ref -> Std.Deref -> Std.Println;\n",
      "bytecode_value_ref_explicit_always_builtin.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;

  for (const bool gate_on : {false, true}) {
    const auto result = compiler.compile(ir_program, make_compile_options(gate_on, false));
    INFO("enable_value_ref_gate = " << gate_on);
    REQUIRE_FALSE(result.has_value());
    REQUIRE((result.error().message == "Unknown builtin in bytecode compiler: 'Std.Ref'." ||
             result.error().message == "Unknown builtin in bytecode compiler: 'Std.Deref'."));
  }
}

TEST_CASE("Bytecode compiler rejects auto value-ref when gate is disabled", "[bytecode][value_ref][auto]") {
  const auto ir_program = lower_source_to_ir(
      "let Echo(x: String): String = x;\n"
      "(\"hello\") -> Echo -> Std.Println;\n",
      "bytecode_value_ref_invalid_option_gate.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program, make_compile_options(false, true, 32));
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
  const auto result = compiler.compile(ir_program, make_compile_options(true, true, 0));
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
  const auto result = compiler.compile(ir_program, make_compile_options(true, true, 32));
  REQUIRE(result.has_value());

  bool saw_make_value_ref = false;
  bool saw_call_user_func = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kMakeValueRef) {
      saw_make_value_ref = true;
    }
    if (ins.opcode == fleaux::bytecode::Opcode::kCallUserFunc) {
      saw_call_user_func = true;
    }
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
  const auto result = compiler.compile(ir_program, make_compile_options(true, true, 32));
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
      "let IndirectApply(f: (String) => String, v: String): String = (v, f) -> Std.Apply;\n"
      "(Echo, "
      "\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\")"
      " -> IndirectApply -> Std.Println;\n",
      "bytecode_value_ref_escape_apply.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program, make_compile_options(true, true, 32));
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

TEST_CASE("Bytecode compiler keeps copy semantics when parameter is captured by a nested closure",
          "[bytecode][value_ref][auto]") {
  const auto ir_program = lower_source_to_ir(
      "let Capture(v: String): Any = (): String = v;\n"
      "(\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\")"
      " -> Capture -> Std.Println;\n",
      "bytecode_value_ref_nested_closure_capture.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program, make_compile_options(true, true, 32));
  REQUIRE(result.has_value());

  bool saw_make_value_ref = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kMakeValueRef) {
      saw_make_value_ref = true;
      break;
    }
  }
  REQUIRE_FALSE(saw_make_value_ref);

  const auto capture_it = std::find_if(result->functions.begin(), result->functions.end(),
                                       [](const auto& fn) { return fn.name == "Capture#0"; });
  REQUIRE(capture_it != result->functions.end());

  bool saw_deref_value_ref = false;
  for (const auto& ins : capture_it->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kDerefValueRef) {
      saw_deref_value_ref = true;
      break;
    }
  }
  REQUIRE_FALSE(saw_deref_value_ref);
}

TEST_CASE("Auto by-ref analyzer marks only safe large fixed parameters and resolves unique short names",
          "[bytecode][value_ref][analyzer]") {
  fleaux::frontend::ir::IRProgram ir_program;
  ir_program.lets = {
      fleaux::frontend::ir::IRLet{
          .qualifier = std::nullopt,
          .name = "Process",
          .symbol_key = "Process#0",
          .generic_params = {},
          .params =
              {
                  fleaux::frontend::ir::IRParam{
                      .name = "head", .type = make_simple_type("String"), .span = std::nullopt},
                  fleaux::frontend::ir::IRParam{
                      .name = "tail", .type = make_simple_type("String"), .span = std::nullopt},
                  fleaux::frontend::ir::IRParam{
                      .name = "anything", .type = make_simple_type("Any"), .span = std::nullopt},
                  fleaux::frontend::ir::IRParam{
                      .name = "rest", .type = make_variadic_simple_type("String"), .span = std::nullopt},
              },
          .return_type = make_simple_type("String"),
          .doc_comments = {},
          .body = make_ir_box(make_ir_name_ref(std::nullopt, "head")),
          .is_builtin = false,
          .span = std::nullopt,
      },
  };
  ir_program.expressions = {
      fleaux::frontend::ir::IRExprStatement{
          .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
              .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                  .items = {make_string_constant_box(std::string(80, 'a')), make_string_constant_box("tiny"),
                            make_string_constant_box(std::string(80, 'b')),
                            make_string_constant_box(std::string(80, 'c'))},
                  .span = std::nullopt,
              }),
              .rhs = make_ir_name_ref(std::nullopt, "Process"),
              .span = std::nullopt,
          }),
          .span = std::nullopt,
      },
  };

  const auto analysis = fleaux::bytecode::analyze_auto_value_ref_params(
      ir_program, fleaux::bytecode::AutoValueRefAnalysisOptions{.enabled = true, .byte_cutoff = 32});

  REQUIRE(analysis.size() == 1);
  const auto process_it = analysis.find("Process#0");
  REQUIRE(process_it != analysis.end());
  REQUIRE(process_it->second.size() == 1);
  REQUIRE(process_it->second.contains(0));
  REQUIRE_FALSE(process_it->second.contains(1));
  REQUIRE_FALSE(process_it->second.contains(2));
  REQUIRE_FALSE(process_it->second.contains(3));
}

TEST_CASE("Auto by-ref analyzer rejects ambiguous names and unsafe callsite argument shapes",
          "[bytecode][value_ref][analyzer]") {
  fleaux::frontend::ir::IRProgram ir_program;
  ir_program.lets =
      {
          fleaux::frontend::ir::IRLet{
              .qualifier = std::nullopt,
              .name = "Dup",
              .symbol_key = "Dup#0",
              .generic_params = {},
              .params = {fleaux::frontend::ir::IRParam{
                  .name = "x", .type = make_simple_type("String"), .span = std::nullopt}},
              .return_type = make_simple_type("String"),
              .doc_comments = {},
              .body = make_ir_box(make_ir_name_ref(std::nullopt, "x")),
              .is_builtin = false,
              .span = std::nullopt,
          },
          fleaux::frontend::ir::IRLet{
              .qualifier = std::nullopt,
              .name = "Dup",
              .symbol_key = "Dup#1",
              .generic_params = {},
              .params = {fleaux::frontend::ir::IRParam{
                  .name = "x", .type = make_simple_type("String"), .span = std::nullopt}},
              .return_type = make_simple_type("String"),
              .doc_comments = {},
              .body = make_ir_box(make_ir_name_ref(std::nullopt, "x")),
              .is_builtin = false,
              .span = std::nullopt,
          },
          fleaux::frontend::ir::IRLet{
              .qualifier = std::nullopt,
              .name = "NeedPair",
              .symbol_key = "NeedPair#0",
              .generic_params = {},
              .params =
                  {
                      fleaux::frontend::ir::IRParam{
                          .name = "lhs", .type = make_simple_type("String"), .span = std::nullopt},
                      fleaux::frontend::ir::IRParam{
                          .name = "rhs", .type = make_simple_type("String"), .span = std::nullopt},
                  },
              .return_type = make_simple_type("String"),
              .doc_comments = {},
              .body = make_ir_box(make_ir_name_ref(std::nullopt, "lhs")),
              .is_builtin = false,
              .span = std::nullopt,
          },
          fleaux::frontend::ir::IRLet{
              .qualifier = std::nullopt,
              .name = "NeedSized",
              .symbol_key = "NeedSized#0",
              .generic_params = {},
              .params = {fleaux::frontend::ir::IRParam{
                  .name = "value", .type = make_simple_type("String"), .span = std::nullopt}},
              .return_type = make_simple_type("String"),
              .doc_comments = {},
              .body = make_ir_box(make_ir_name_ref(std::nullopt, "value")),
              .is_builtin = false,
              .span = std::nullopt,
          },
          fleaux::frontend::ir::IRLet{
              .qualifier = std::nullopt,
              .name = "Caller",
              .symbol_key = "Caller#0",
              .generic_params = {},
              .params = {fleaux::frontend::ir::IRParam{
                  .name = "payload", .type = make_simple_type("String"), .span = std::nullopt}},
              .return_type = make_simple_type("String"),
              .doc_comments = {},
              .body = make_ir_box(fleaux::frontend::ir::IRFlowExpr{
                  .lhs = make_name_ref_box(std::nullopt, "payload"),
                  .rhs = make_ir_name_ref(std::nullopt, "NeedSized"),
                  .span = std::nullopt,
              }),
              .is_builtin = false,
              .span = std::nullopt,
          },
      };
  ir_program.expressions = {
      fleaux::frontend::ir::IRExprStatement{
          .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
              .lhs = make_string_constant_box(std::string(80, 'd')),
              .rhs = make_ir_name_ref(std::nullopt, "Dup"),
              .span = std::nullopt,
          }),
          .span = std::nullopt,
      },
      fleaux::frontend::ir::IRExprStatement{
          .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
              .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                  .items = {make_string_constant_box(std::string(80, 'e'))},
                  .span = std::nullopt,
              }),
              .rhs = make_ir_name_ref(std::nullopt, "NeedPair"),
              .span = std::nullopt,
          }),
          .span = std::nullopt,
      },
      fleaux::frontend::ir::IRExprStatement{
          .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
              .lhs = make_string_constant_box(std::string(80, 'f')),
              .rhs = make_ir_name_ref(std::nullopt, "Caller"),
              .span = std::nullopt,
          }),
          .span = std::nullopt,
      },
      fleaux::frontend::ir::IRExprStatement{
          .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
              .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                  .items = {make_int_constant_box(1), make_int_constant_box(2)},
                  .span = std::nullopt,
              }),
              .rhs = fleaux::frontend::ir::IROperatorRef{.op = "+", .span = std::nullopt},
              .span = std::nullopt,
          }),
          .span = std::nullopt,
      },
  };

  const auto analysis = fleaux::bytecode::analyze_auto_value_ref_params(
      ir_program, fleaux::bytecode::AutoValueRefAnalysisOptions{.enabled = true, .byte_cutoff = 32});

  REQUIRE(analysis.size() == 1);
  REQUIRE_FALSE(analysis.contains("Dup#0"));
  REQUIRE_FALSE(analysis.contains("Dup#1"));
  REQUIRE_FALSE(analysis.contains("NeedPair#0"));
  REQUIRE_FALSE(analysis.contains("NeedSized#0"));
  const auto caller_it = analysis.find("Caller#0");
  REQUIRE(caller_it != analysis.end());
  REQUIRE(caller_it->second.size() == 1);
  REQUIRE(caller_it->second.contains(0));
}

TEST_CASE("Auto by-ref analyzer skips nested closure captures and escape builtins reached through recursive bodies",
          "[bytecode][value_ref][analyzer]") {
  auto nested_capture = fleaux::frontend::ir::IRClosureExprBox(fleaux::frontend::ir::IRClosureExpr{
      .generic_params = {},
      .params = {},
      .return_type = make_simple_type("String"),
      .body = make_name_ref_box(std::nullopt, "v"),
      .captures = {"v"},
      .span = std::nullopt,
  });
  auto nested_escape = fleaux::frontend::ir::IRClosureExprBox(fleaux::frontend::ir::IRClosureExpr{
      .generic_params = {},
      .params = {},
      .return_type = make_simple_type("String"),
      .body = make_ir_box(fleaux::frontend::ir::IRFlowExpr{
          .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
              .items = {make_name_ref_box(std::nullopt, "v"), make_name_ref_box(std::nullopt, "Echo")},
              .span = std::nullopt,
          }),
          .rhs = make_ir_name_ref(std::string{"Std"}, "Apply", std::string{"Std.Apply"}),
          .span = std::nullopt,
      }),
      .captures = {},
      .span = std::nullopt,
  });

  fleaux::frontend::ir::IRProgram ir_program;
  ir_program.lets = {
      fleaux::frontend::ir::IRLet{
          .qualifier = std::nullopt,
          .name = "Captured",
          .symbol_key = "Captured#0",
          .generic_params = {},
          .params = {fleaux::frontend::ir::IRParam{
              .name = "v", .type = make_simple_type("String"), .span = std::nullopt}},
          .return_type = make_simple_type("Any"),
          .doc_comments = {},
          .body = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
              .items = {make_ir_box(fleaux::frontend::ir::IRClosureExprBox{std::move(nested_capture)})},
              .span = std::nullopt,
          }),
          .is_builtin = false,
          .span = std::nullopt,
      },
      fleaux::frontend::ir::IRLet{
          .qualifier = std::nullopt,
          .name = "Escaped",
          .symbol_key = "Escaped#0",
          .generic_params = {},
          .params = {fleaux::frontend::ir::IRParam{
              .name = "v", .type = make_simple_type("String"), .span = std::nullopt}},
          .return_type = make_simple_type("Any"),
          .doc_comments = {},
          .body = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
              .items = {make_ir_box(fleaux::frontend::ir::IRClosureExprBox{std::move(nested_escape)})},
              .span = std::nullopt,
          }),
          .is_builtin = false,
          .span = std::nullopt,
      },
  };
  ir_program.expressions = {
      fleaux::frontend::ir::IRExprStatement{
          .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
              .lhs = make_string_constant_box(std::string(80, 'g')),
              .rhs = make_ir_name_ref(std::nullopt, "Captured"),
              .span = std::nullopt,
          }),
          .span = std::nullopt,
      },
      fleaux::frontend::ir::IRExprStatement{
          .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
              .lhs = make_string_constant_box(std::string(80, 'h')),
              .rhs = make_ir_name_ref(std::nullopt, "Escaped"),
              .span = std::nullopt,
          }),
          .span = std::nullopt,
      },
  };

  const auto analysis = fleaux::bytecode::analyze_auto_value_ref_params(
      ir_program, fleaux::bytecode::AutoValueRefAnalysisOptions{.enabled = true, .byte_cutoff = 32});

  REQUIRE(analysis.empty());
}

TEST_CASE("Bytecode compiler keeps copy semantics through Parallel and Task escape builtins",
          "[bytecode][value_ref][auto][concurrency]") {
  const fleaux::bytecode::BytecodeCompiler compiler;

  const auto assert_no_value_ref_opcodes = [&](const std::string& source_text, const std::string& source_name,
                                               const std::string& wrapper_name) {
    INFO("source=" << source_name << ", wrapper=" << wrapper_name);
    const auto ir_program = lower_source_to_ir(source_text, source_name);
    const auto result = compiler.compile(ir_program, make_compile_options(true, true, 32));
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
        "let EscapeViaParallelMap(v: String): Result<Tuple<String...>, Tuple<Int64, String>> = ((v) -> Std.Wrap, Echo) "
        "-> "
        "Std.Parallel.Map;\n"
        "(\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\")"
        " -> EscapeViaParallelMap -> Std.Result.Unwrap -> Std.Println;\n",
        "bytecode_value_ref_escape_parallel_map.fleaux", "EscapeViaParallelMap");
  }

  SECTION("Std.Parallel.WithOptions") {
    assert_no_value_ref_opcodes(
        "let Echo(x: String): String = x;\n"
        "let BuildOptions(): Dict<String, Any> = () -> Std.Dict.Create -> (_, \"max_workers\", 2) -> Std.Dict.Set;\n"
        "let EscapeViaParallelWithOptions(v: String): Result<Tuple<String...>, Tuple<Int64, String>> = ((v) -> "
        "Std.Wrap, Echo, "
        "() -> BuildOptions) -> Std.Parallel.WithOptions;\n"
        "(\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\")"
        " -> EscapeViaParallelWithOptions -> Std.Result.Unwrap -> Std.Println;\n",
        "bytecode_value_ref_escape_parallel_with_options.fleaux", "EscapeViaParallelWithOptions");
  }

  SECTION("Std.Parallel.ForEach") {
    assert_no_value_ref_opcodes(
        "let Echo(x: String): String = x;\n"
        "let EscapeViaParallelForEach(v: String): Result<Tuple<>, Tuple<Int64, String>> = ((v) -> Std.Wrap, Echo) -> "
        "Std.Parallel.ForEach;\n"
        "(\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\")"
        " -> EscapeViaParallelForEach -> Std.Result.IsOk -> Std.Println;\n",
        "bytecode_value_ref_escape_parallel_foreach.fleaux", "EscapeViaParallelForEach");
  }

  SECTION("Std.Parallel.Reduce") {
    assert_no_value_ref_opcodes(
        "let Merge(acc: String, x: String): String = (\"\", (acc, x)) -> Std.String.Join;\n"
        "let EscapeViaParallelReduce(v: String): Result<String, Tuple<Int64, String>> = ((v) -> Std.Wrap, \"\", Merge) "
        "-> "
        "Std.Parallel.Reduce;\n"
        "(\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\")"
        " -> EscapeViaParallelReduce -> Std.Result.Unwrap -> Std.Println;\n",
        "bytecode_value_ref_escape_parallel_reduce.fleaux", "EscapeViaParallelReduce");
  }

  SECTION("Std.Task.Spawn") {
    assert_no_value_ref_opcodes(
        "let Echo(x: String): String = x;\n"
        "let EscapeViaTaskSpawn(v: String): TaskHandle<String> = (Echo, v) -> Std.Task.Spawn;\n"
        "(\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\")"
        " -> EscapeViaTaskSpawn -> Std.Task.Await -> Std.Result.Unwrap -> Std.Println;\n",
        "bytecode_value_ref_escape_task_spawn.fleaux", "EscapeViaTaskSpawn");
  }
}

TEST_CASE("Auto by-ref semantic equivalence: tuple passthrough same result with/without optimization",
          "[bytecode][value_ref][semantic_equiv]") {
  const auto source =
      "let Echo(x: Tuple<Float64...>): Tuple<Float64...> = x;\n"
      "((1.0, 2.0, 3.0, 4.0, 5.0)) -> Echo -> Std.Println;\n";

  const auto ir_program = lower_source_to_ir(source, "bytecode_semantic_equiv_tuple.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result_copy = compiler.compile(ir_program, make_compile_options(false, false));
  const auto result_byref = compiler.compile(ir_program, make_compile_options(true, true, 32));

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
        "let Inc(x: Int64): Int64 = (x, 1) -> Std.Add;\n"
        "((1, 2, 3, 4), Inc) -> Std.Parallel.Map -> Std.Result.Unwrap -> Std.Println;\n",
        "bytecode_semantic_equiv_parallel_map.fleaux");
  }

  SECTION("Parallel.WithOptions") {
    require_copy_vs_auto_byref_equivalent(
        "let Inc(x: Int64): Int64 = (x, 1) -> Std.Add;\n"
        "let BuildOptions(): Dict<String, Any> = () -> Std.Dict.Create -> (_, \"max_workers\", 2) -> Std.Dict.Set;\n"
        "((1, 2, 3, 4), Inc, () -> BuildOptions) -> Std.Parallel.WithOptions -> Std.Result.Unwrap -> Std.Println;\n",
        "bytecode_semantic_equiv_parallel_with_options.fleaux");
  }

  SECTION("Parallel.ForEach") {
    require_copy_vs_auto_byref_equivalent(
        "let Inc(x: Int64): Int64 = (x, 1) -> Std.Add;\n"
        "((10, 20, 30), Inc) -> Std.Parallel.ForEach -> Std.Result.IsOk -> Std.Println;\n",
        "bytecode_semantic_equiv_parallel_foreach.fleaux");
  }

  SECTION("Parallel.Reduce") {
    require_copy_vs_auto_byref_equivalent(
        "let Add(acc: Int64, x: Int64): Int64 = (acc, x) -> Std.Add;\n"
        "((1, 2, 3, 4, 5), 0, Add) -> Std.Parallel.Reduce -> Std.Result.Unwrap -> Std.Println;\n",
        "bytecode_semantic_equiv_parallel_reduce.fleaux");
  }

  SECTION("Task.Spawn + Task.Await") {
    require_copy_vs_auto_byref_equivalent(
        "let Inc(x: Int64): Int64 = (x, 1) -> Std.Add;\n"
        "(Inc, 41) -> Std.Task.Spawn -> Std.Task.Await -> Std.Result.Unwrap -> Std.Println;\n",
        "bytecode_semantic_equiv_task_spawn_await.fleaux");
  }

  SECTION("Task.AwaitAll") {
    require_copy_vs_auto_byref_equivalent(
        "let Inc(x: Int64): Int64 = (x, 1) -> Std.Add;\n"
        "(((Inc, 1) -> Std.Task.Spawn, (Inc, 2) -> Std.Task.Spawn, (Inc, 3) -> Std.Task.Spawn))\n"
        "  -> Std.Task.AwaitAll\n"
        "  -> Std.Result.Unwrap\n"
        "  -> Std.Println;\n",
        "bytecode_semantic_equiv_task_await_all.fleaux");
  }

  SECTION("Task.WithTimeout non-timeout path") {
    require_copy_vs_auto_byref_equivalent(
        "let Inc(x: Int64): Int64 = (x, 1) -> Std.Add;\n"
        "(Inc, 7) -> Std.Task.Spawn -> (_, 500u64) -> Std.Task.WithTimeout -> Std.Result.Unwrap -> Std.Println;\n",
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
    for (int i = 0; i < n; ++i) {
      values.push_back(value_dist(rng));
    }

    const std::string tuple_literal = make_int_tuple_literal(values);
    const std::string tasks_literal = make_task_spawn_tuple_literal(values);
    const int max_workers = 1 + (run % 3);

    INFO("run=" << run << ", n=" << n);

    require_copy_vs_auto_byref_equivalent(
        "let Inc(x: Int64): Int64 = (x, 1) -> Std.Add;\n"
        "(" +
            tuple_literal + ", Inc) -> Std.Parallel.Map -> Std.Result.Unwrap -> Std.Println;\n",
        "bytecode_semantic_equiv_property_parallel_map_" + std::to_string(run) + ".fleaux");

    require_copy_vs_auto_byref_equivalent(
        "let Inc(x: Int64): Int64 = (x, 1) -> Std.Add;\n"
        "let BuildOptions(): Dict<String, Any> = () -> Std.Dict.Create -> (_, \"max_workers\", " +
            std::to_string(max_workers) +
            ") -> Std.Dict.Set;\n"
            "(" +
            tuple_literal +
            ", Inc, () -> BuildOptions) -> Std.Parallel.WithOptions -> Std.Result.Unwrap -> Std.Println;\n",
        "bytecode_semantic_equiv_property_parallel_with_options_" + std::to_string(run) + ".fleaux");

    require_copy_vs_auto_byref_equivalent(
        "let Add(acc: Int64, x: Int64): Int64 = (acc, x) -> Std.Add;\n"
        "(" +
            tuple_literal + ", 0, Add) -> Std.Parallel.Reduce -> Std.Result.Unwrap -> Std.Println;\n",
        "bytecode_semantic_equiv_property_parallel_reduce_" + std::to_string(run) + ".fleaux");

    require_copy_vs_auto_byref_equivalent(
        "let Inc(x: Int64): Int64 = (x, 1) -> Std.Add;\n"
        "(" +
            tasks_literal + ") -> Std.Task.AwaitAll -> Std.Result.Unwrap -> Std.Println;\n",
        "bytecode_semantic_equiv_property_task_await_all_" + std::to_string(run) + ".fleaux");
  }
}

TEST_CASE("Auto by-ref benchmark: large tuple processing", "[bytecode][value_ref][benchmark]") {
  const auto ir_program = lower_source_to_ir(
      "let ProcessTuple(t: Tuple<Float64...>): Tuple<Float64...> = t;\n"
      "((1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0,\n"
      "  11.0, 12.0, 13.0, 14.0, 15.0, 16.0, 17.0, 18.0, 19.0, 20.0)) -> ProcessTuple -> Std.Println;\n",
      "bytecode_benchmark_tuple.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result_copy = compiler.compile(ir_program, make_compile_options(false, false));
  const auto result_byref = compiler.compile(ir_program, make_compile_options(true, true, 64));

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
  const auto result_copy = compiler.compile(ir_program, make_compile_options(false, false));
  const auto result_byref = compiler.compile(ir_program, make_compile_options(true, true, 16));

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
  REQUIRE(instr.size() == 6);
  REQUIRE(instr[0].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(instr[1].opcode == fleaux::bytecode::Opcode::kNot);
  // Println is variadic: its one argument travels in a one-item argument tuple.
  REQUIRE(instr[2].opcode == fleaux::bytecode::Opcode::kBuildTuple);
  REQUIRE(instr[3].opcode == fleaux::bytecode::Opcode::kCallBuiltin);
  REQUIRE(fleaux::vm::builtin_name(*fleaux::vm::builtin_id_from_operand(instr[3].operand)) == "Std.Println");
  REQUIRE(instr[4].opcode == fleaux::bytecode::Opcode::kPop);
  REQUIRE(instr[5].opcode == fleaux::bytecode::Opcode::kHalt);
}

TEST_CASE("Bytecode compiler emits native opcode for non-tuple unary operator shorthand", "[bytecode]") {
  const auto ir_program = lower_source_to_ir("True -> ! -> Std.Println;\n", "bytecode_native_not_non_tuple.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  const auto& instr = result->instructions;
  REQUIRE(instr.size() == 6);
  REQUIRE(instr[0].opcode == fleaux::bytecode::Opcode::kPushConst);
  REQUIRE(instr[1].opcode == fleaux::bytecode::Opcode::kNot);
  // Println is variadic: its one argument travels in a one-item argument tuple.
  REQUIRE(instr[2].opcode == fleaux::bytecode::Opcode::kBuildTuple);
  REQUIRE(instr[3].opcode == fleaux::bytecode::Opcode::kCallBuiltin);
  REQUIRE(fleaux::vm::builtin_name(*fleaux::vm::builtin_id_from_operand(instr[3].operand)) == "Std.Println");
  REQUIRE(instr[4].opcode == fleaux::bytecode::Opcode::kPop);
  REQUIRE(instr[5].opcode == fleaux::bytecode::Opcode::kHalt);
}

TEST_CASE("Bytecode compiler emits type-checked operators as their resolved Std builtins", "[bytecode]") {
  const auto collect = [](const fleaux::bytecode::Module& module) {
    std::vector<fleaux::bytecode::Opcode> opcodes;
    std::vector<std::string> builtin_names;
    for (const auto& ins : module.instructions) {
      opcodes.push_back(ins.opcode);
      if (ins.opcode == fleaux::bytecode::Opcode::kCallBuiltin) {
        const auto builtin_id = fleaux::vm::builtin_id_from_operand(ins.operand);
        REQUIRE(builtin_id.has_value());
        builtin_names.emplace_back(fleaux::vm::builtin_name(*builtin_id));
      }
    }
    return std::pair{opcodes, builtin_names};
  };
  const auto contains = [](const auto& items, const auto& value) {
    return std::ranges::find(items, value) != items.end();
  };
  const fleaux::bytecode::BytecodeCompiler compiler;

  SECTION("A tuple literal operand uses the native binary opcode") {
    const auto ir_program = lower_source_to_ir("import Std;\n(1, 2) -> + -> Std.Println;\n", "operator_native_add.fleaux");
    const auto result = compiler.compile(ir_program);
    REQUIRE(result.has_value());
    const auto [opcodes, builtin_names] = collect(*result);
    REQUIRE(contains(opcodes, fleaux::bytecode::Opcode::kAdd));
    REQUIRE_FALSE(contains(builtin_names, std::string{"Std.Add"}));
  }

  SECTION("A single operand selects the unary minus opcode") {
    const auto ir_program = lower_source_to_ir("import Std;\n5 -> - -> Std.Println;\n", "operator_unary_minus.fleaux");
    const auto result = compiler.compile(ir_program);
    REQUIRE(result.has_value());
    const auto [opcodes, builtin_names] = collect(*result);
    REQUIRE(contains(opcodes, fleaux::bytecode::Opcode::kNeg));
    REQUIRE_FALSE(contains(opcodes, fleaux::bytecode::Opcode::kSub));
  }

  SECTION("A spread tuple value is binary subtraction") {
    const auto ir_program = lower_source_to_ir(
        "import Std;\nlet P(): Tuple<Int64, Int64> = (5, 2);\n(P...) -> - -> Std.Println;\n", "operator_tuple_value.fleaux");
    const auto result = compiler.compile(ir_program);
    REQUIRE(result.has_value());
    const auto [opcodes, builtin_names] = collect(*result);
    REQUIRE_FALSE(contains(opcodes, fleaux::bytecode::Opcode::kNeg));
    REQUIRE(contains(builtin_names, std::string{"Std.Subtract"}));
  }
}

TEST_CASE("Bytecode compiler lowers explicit Std.Add to native opcode by default", "[bytecode]") {
  fleaux::frontend::ir::IRProgram ir_program;
  ir_program.expressions = {
      fleaux::frontend::ir::IRExprStatement{
          .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
              .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                  .items = {make_int_constant_box(1), make_int_constant_box(2)},
                  .span = std::nullopt,
              }),
              .rhs = make_ir_name_ref("Std", "Add"),
              .span = std::nullopt,
          }),
          .span = std::nullopt,
      },
  };

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  bool found_native_add = false;
  bool found_builtin_add = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kAdd) {
      found_native_add = true;
    }
    if (ins.opcode == fleaux::bytecode::Opcode::kCallBuiltin) {
      const auto builtin_id = fleaux::vm::builtin_id_from_operand(ins.operand);
      REQUIRE(builtin_id.has_value());
      if (fleaux::vm::builtin_name(*builtin_id) == "Std.Add") {
        found_builtin_add = true;
      }
    }
  }

  REQUIRE(found_native_add);
  REQUIRE_FALSE(found_builtin_add);
}

TEST_CASE("Bytecode compiler keeps reducible builtin call chains intact by default", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(
      "import Std;\n"
      "\"  hello  \" -> Std.String.TrimStart -> Std.String.TrimEnd;\n",
      "builtin_reductions_default_off.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  bool found_builtin_trim = false;
  bool found_builtin_trim_start = false;
  bool found_builtin_trim_end = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kCallBuiltin) {
      const auto builtin_id = fleaux::vm::builtin_id_from_operand(ins.operand);
      REQUIRE(builtin_id.has_value());
      const auto& builtin_name = fleaux::vm::builtin_name(*builtin_id);
      if (builtin_name == "Std.String.Trim")
        found_builtin_trim = true;
      if (builtin_name == "Std.String.TrimStart")
        found_builtin_trim_start = true;
      if (builtin_name == "Std.String.TrimEnd")
        found_builtin_trim_end = true;
    }
  }

  REQUIRE_FALSE(found_builtin_trim);
  REQUIRE(found_builtin_trim_start);
  REQUIRE(found_builtin_trim_end);
}

TEST_CASE("Bytecode compiler can experimentally reduce builtin call chains to equivalent single calls",
          "[bytecode][optimizer][experimental]") {
  const auto ir_program = lower_source_to_ir(
      "import Std;\n"
      "\"  hello  \" -> Std.String.TrimStart -> Std.String.TrimEnd;\n"
      "1 -> Std.Result.Ok -> Std.Result.Tag -> Std.Not;\n"
      "1 -> Std.Result.Ok -> Std.Result.Tag;\n"
      "((1, 2), (3, 4),) -> Std.Array.Shape -> Std.Length;\n"
      "(\"abc\", \"b\", 0u64) -> Std.String.Find -> (_, -1) -> Std.NotEqual;\n"
      "(\"abc\", \"b\") -> Std.String.Regex.Find -> (_, -1) -> Std.NotEqual;\n",
      "builtin_reductions.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(
      ir_program, make_compile_options(false, false, 256, std::nullopt, std::nullopt, std::nullopt, {}, true));

  REQUIRE(result.has_value());

  bool found_builtin_trim = false;
  bool found_builtin_trim_start = false;
  bool found_builtin_trim_end = false;
  bool found_builtin_result_is_err = false;
  bool found_builtin_result_tag = false;
  bool found_builtin_result_is_ok = false;
  bool found_builtin_array_rank = false;
  bool found_builtin_array_shape = false;
  bool found_builtin_length = false;
  bool found_builtin_string_contains = false;
  bool found_builtin_string_find = false;
  bool found_builtin_regex_is_match = false;
  bool found_builtin_regex_find = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kCallBuiltin) {
      const auto builtin_id = fleaux::vm::builtin_id_from_operand(ins.operand);
      REQUIRE(builtin_id.has_value());
      const auto& builtin_name = fleaux::vm::builtin_name(*builtin_id);
      if (builtin_name == "Std.String.Trim")
        found_builtin_trim = true;
      if (builtin_name == "Std.String.TrimStart")
        found_builtin_trim_start = true;
      if (builtin_name == "Std.String.TrimEnd")
        found_builtin_trim_end = true;
      if (builtin_name == "Std.Result.IsErr")
        found_builtin_result_is_err = true;
      if (builtin_name == "Std.Result.Tag")
        found_builtin_result_tag = true;
      if (builtin_name == "Std.Result.IsOk")
        found_builtin_result_is_ok = true;
      if (builtin_name == "Std.Array.Rank")
        found_builtin_array_rank = true;
      if (builtin_name == "Std.Array.Shape")
        found_builtin_array_shape = true;
      if (builtin_name == "Std.Length")
        found_builtin_length = true;
      if (builtin_name == "Std.String.Contains")
        found_builtin_string_contains = true;
      if (builtin_name == "Std.String.Find")
        found_builtin_string_find = true;
      if (builtin_name == "Std.String.Regex.IsMatch")
        found_builtin_regex_is_match = true;
      if (builtin_name == "Std.String.Regex.Find")
        found_builtin_regex_find = true;
    }
  }

  REQUIRE(found_builtin_trim);
  REQUIRE_FALSE(found_builtin_trim_start);
  REQUIRE_FALSE(found_builtin_trim_end);
  REQUIRE(found_builtin_result_is_err);
  REQUIRE_FALSE(found_builtin_result_tag);
  REQUIRE(found_builtin_result_is_ok);
  REQUIRE(found_builtin_array_rank);
  REQUIRE_FALSE(found_builtin_array_shape);
  REQUIRE_FALSE(found_builtin_length);
  REQUIRE(found_builtin_string_contains);
  REQUIRE_FALSE(found_builtin_string_find);
  REQUIRE(found_builtin_regex_is_match);
  REQUIRE_FALSE(found_builtin_regex_find);
}

TEST_CASE("Bytecode compiler can experimentally reduce Std.OS.Env null comparison to Std.OS.HasEnv",
          "[bytecode][experimental][optimizer]") {
  fleaux::frontend::ir::IRProgram ir_program;
  ir_program.expressions =
      {
          fleaux::frontend::ir::IRExprStatement{
              .expr = make_ir_expr(fleaux::frontend::ir::
                                       IRFlowExpr{
                                           .lhs = make_ir_box(fleaux::frontend::ir::
                                                                  IRTupleExpr{
                                                                      .items =
                                                                          {
                                                                              make_ir_box(
                                                                                  fleaux::frontend::ir::IRFlowExpr{
                                                                                      .lhs = make_string_constant_box(
                                                                                          "PATH"),
                                                                                      .rhs = make_ir_name_ref("Std.OS",
                                                                                                              "Env"),
                                                                                      .span = std::nullopt,
                                                                                  }),
                                                                              make_null_constant_box(),
                                                                          },
                                                                      .span = std::nullopt,
                                                                  }),
                                           .rhs = make_ir_name_ref("Std", "NotEqual"),
                                           .span = std::nullopt,
                                       }),
              .span = std::nullopt,
          },
      };

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(
      ir_program, make_compile_options(false, false, 256, std::nullopt, std::nullopt, std::nullopt, {}, true));

  REQUIRE(result.has_value());

  bool found_builtin_os_has_env = false;
  bool found_builtin_os_env = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kCallBuiltin) {
      const auto builtin_id = fleaux::vm::builtin_id_from_operand(ins.operand);
      REQUIRE(builtin_id.has_value());
      const auto& builtin_name = fleaux::vm::builtin_name(*builtin_id);
      if (builtin_name == "Std.OS.HasEnv")
        found_builtin_os_has_env = true;
      if (builtin_name == "Std.OS.Env")
        found_builtin_os_env = true;
    }
  }

  REQUIRE(found_builtin_os_has_env);
  REQUIRE_FALSE(found_builtin_os_env);
}

TEST_CASE("Bytecode compiler rejects operators the type checker did not resolve", "[bytecode]") {
  fleaux::frontend::ir::IRProgram ir_program;
  ir_program.expressions = {
      fleaux::frontend::ir::IRExprStatement{
          .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
              .lhs = make_int_constant_box(5),
              .rhs = fleaux::frontend::ir::IROperatorRef{.op = "??", .span = std::nullopt},
              .span = std::nullopt,
          }),
          .span = std::nullopt,
      },
  };

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message ==
          "Operator '?\?' reached the bytecode compiler without being resolved by the type checker.");
}

TEST_CASE("Bytecode compiler rejects unresolved names used as values", "[bytecode]") {
  fleaux::frontend::ir::IRProgram ir_program;
  ir_program.expressions = {
      fleaux::frontend::ir::IRExprStatement{
          .expr = make_ir_expr(make_ir_name_ref(std::nullopt, "MissingValue")),
          .span = std::nullopt,
      },
  };

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message == "Name 'MissingValue' used as a value is not supported in the bytecode compiler.");
}

TEST_CASE("Bytecode compiler rejects closure captures that are unavailable in the enclosing locals", "[bytecode]") {
  auto closure = fleaux::frontend::ir::IRClosureExprBox(fleaux::frontend::ir::IRClosureExpr{
      .generic_params = {},
      .params = {},
      .return_type = make_simple_type("Int64"),
      .body = make_int_constant_box(42),
      .captures = {"missing_capture"},
      .span = std::nullopt,
  });

  fleaux::frontend::ir::IRProgram ir_program;
  ir_program.expressions = {
      fleaux::frontend::ir::IRExprStatement{
          .expr = make_ir_expr(fleaux::frontend::ir::IRClosureExprBox{std::move(closure)}),
          .span = std::nullopt,
      },
  };

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message == "Unsupported closure capture in bytecode compiler: 'missing_capture'.");
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
    if (ins.opcode == fleaux::bytecode::Opcode::kSelect) {
      found_select = true;
    }
  }
  REQUIRE(found_select);
}

TEST_CASE("Bytecode compiler keeps Std.Select as builtin when tuple arms are user function refs", "[bytecode]") {
  fleaux::frontend::ir::IRProgram ir_program;
  ir_program.lets = {
      fleaux::frontend::ir::IRLet{
          .qualifier = std::nullopt,
          .name = "Inc",
          .symbol_key = "Inc#0",
          .generic_params = {},
          .params = {fleaux::frontend::ir::IRParam{
              .name = "x", .type = make_simple_type("Int64"), .span = std::nullopt}},
          .return_type = make_simple_type("Int64"),
          .doc_comments = {},
          .body = make_ir_box(fleaux::frontend::ir::IRConstant{.val = std::int64_t{1}, .span = std::nullopt}),
          .is_builtin = false,
          .span = std::nullopt,
      },
      fleaux::frontend::ir::IRLet{
          .qualifier = std::nullopt,
          .name = "Dec",
          .symbol_key = "Dec#0",
          .generic_params = {},
          .params = {fleaux::frontend::ir::IRParam{
              .name = "x", .type = make_simple_type("Int64"), .span = std::nullopt}},
          .return_type = make_simple_type("Int64"),
          .doc_comments = {},
          .body = make_ir_box(fleaux::frontend::ir::IRConstant{.val = std::int64_t{0}, .span = std::nullopt}),
          .is_builtin = false,
          .span = std::nullopt,
      },
  };
  ir_program.expressions = {
      fleaux::frontend::ir::IRExprStatement{
          .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
              .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                  .items = {make_bool_constant_box(true), make_name_ref_box(std::nullopt, "Inc", "Inc#0"),
                            make_name_ref_box(std::nullopt, "Dec", "Dec#0")},
                  .span = std::nullopt,
              }),
              .rhs = make_ir_name_ref(std::string{"Std"}, "Select", std::string{"Std.Select"}),
              .span = std::nullopt,
          }),
          .span = std::nullopt,
      },
  };

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  bool found_native_select = false;
  bool found_builtin_select = false;
  bool found_user_function_ref = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kSelect) {
      found_native_select = true;
    }
    if (ins.opcode == fleaux::bytecode::Opcode::kMakeUserFuncRef) {
      found_user_function_ref = true;
    }
    if (ins.opcode == fleaux::bytecode::Opcode::kCallBuiltin) {
      const auto builtin_id = fleaux::vm::builtin_id_from_operand(ins.operand);
      REQUIRE(builtin_id.has_value());
      if (fleaux::vm::builtin_name(*builtin_id) == "Std.Select") {
        found_builtin_select = true;
      }
    }
  }

  REQUIRE_FALSE(found_native_select);
  REQUIRE(found_builtin_select);
  REQUIRE(found_user_function_ref);
}

TEST_CASE("Bytecode compiler emits kLoopCall for Std.Loop", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(R"(
let Continue(n: Float64): Bool = (n, 0.0) -> Std.GreaterThan;
let Step(n: Float64): Float64 = (n, 1.0) -> Std.Subtract;
(10.0, Continue, Step) -> Std.Loop -> Std.Println;
)",
                                             "bytecode_native_loop.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  bool found_loop_call = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kLoopCall) {
      found_loop_call = true;
    }
  }
  REQUIRE(found_loop_call);
}

TEST_CASE("Bytecode compiler emits kLoopNCall for Std.LoopN", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(R"(
let Continue(n: Float64): Bool = (n, 0.0) -> Std.GreaterThan;
let Step(n: Float64): Float64 = (n, 1.0) -> Std.Subtract;
(10.0, Continue, Step, 100u64) -> Std.LoopN -> Std.Println;
)",
                                             "bytecode_native_loopn.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  bool found_loop_n_call = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kLoopNCall) {
      found_loop_n_call = true;
    }
  }
  REQUIRE(found_loop_n_call);
}

TEST_CASE("Bytecode compiler emits kBranchCall for Std.Branch with user function refs", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(R"(
let Inc(x: Float64): Float64 = (x, 1.0) -> Std.Add;
let Dec(x: Float64): Float64 = (x, 1.0) -> Std.Subtract;
(True, 10.0, Inc, Dec) -> Std.Branch -> Std.Println;
)",
                                             "bytecode_native_branch_safe.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  bool found_branch_call = false;
  bool found_builtin_branch = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kBranchCall) {
      found_branch_call = true;
    }
    if (ins.opcode == fleaux::bytecode::Opcode::kCallBuiltin) {
      const auto builtin_id = fleaux::vm::builtin_id_from_operand(ins.operand);
      REQUIRE(builtin_id.has_value());
      const auto name = fleaux::vm::builtin_name(*builtin_id);
      if (name == "Std.Branch") {
        found_builtin_branch = true;
      }
    }
  }

  REQUIRE(found_branch_call);
  REQUIRE_FALSE(found_builtin_branch);
}

TEST_CASE("Bytecode compiler emits builtin callable refs for Std.Apply value call", "[bytecode]") {
  const auto ir_program = lower_source_to_ir("(5.0, Std.UnaryMinus<Float64>) -> Std.Apply -> Std.Println;\n",
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
      const auto builtin_shape = fleaux::vm::builtin_call_shape_from_operand(ins.operand);
      REQUIRE(builtin_shape.has_value());
      const auto name = fleaux::vm::builtin_name(*builtin_id);
      REQUIRE(name == "Std.UnaryMinus");
      REQUIRE(*builtin_shape == fleaux::vm::BuiltinCallShape::kDirectValue);
    }
  }
  REQUIRE(found_make_builtin_ref);
}

TEST_CASE("Bytecode compiler emits kBranchCall for Std.Branch with builtin refs", "[bytecode]") {
  const auto ir_program =
      lower_source_to_ir("(True, 10.0, Std.UnaryMinus<Float64>, Std.UnaryPlus<Float64>) -> Std.Branch -> Std.Println;\n",
                         "bytecode_builtin_callable_branch.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  bool found_branch_call = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kBranchCall) {
      found_branch_call = true;
    }
  }
  REQUIRE(found_branch_call);
}

TEST_CASE("Bytecode compiler emits closure materialization for inline closure literals", "[bytecode]") {
  const auto ir_program =
      lower_source_to_ir("(10.0, (x: Float64): Float64 = (x, 1.0) -> Std.Add) -> Std.Apply -> Std.Println;\n",
                         "bytecode_inline_closure.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());
  REQUIRE(!result->closures.empty());

  bool found_make_closure = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kMakeClosureRef) {
      found_make_closure = true;
    }
  }
  REQUIRE(found_make_closure);
}

TEST_CASE("Bytecode compiler wires captured closure factory function", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(
      "let MakeAdder(n: Float64): (Float64) => Float64 = (x: Float64): Float64 = (x, n) -> Std.Add;\n"
      "(10.0, (4.0) -> MakeAdder) -> Std.Apply -> Std.Println;\n",
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
    if (ins.opcode == fleaux::bytecode::Opcode::kLoadLocal) {
      found_capture_load = true;
    }
    if (ins.opcode == fleaux::bytecode::Opcode::kBuildTuple) {
      found_capture_tuple = true;
    }
    if (ins.opcode == fleaux::bytecode::Opcode::kMakeClosureRef) {
      found_make_closure = true;
    }
  }

  REQUIRE(found_capture_load);
  REQUIRE(found_capture_tuple);
  REQUIRE(found_make_closure);
}

TEST_CASE("Bytecode compiler emits variadic metadata for inline closures", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(
      "let MakeHeadPicker(): (Float64, Any...) => Float64 =\n"
      "    (head: Float64, tail: Any...): Float64 = head;\n"
      "(1.0) -> Std.Println;\n",
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
      if (name == "Std.Match") {
        found_match_call = true;
      }
    }
  }
  REQUIRE(found_match_call);
}

TEST_CASE("Bytecode compiler keeps Std.Branch as builtin for callable locals", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(R"(
let Inc(x: Float64): Float64 = (x, 1.0) -> Std.Add;
let Dec(x: Float64): Float64 = (x, 1.0) -> Std.Subtract;
let ChooseApply(x: Float64, tf: (Float64) => Float64, ff: (Float64) => Float64): Float64 =
    ((x, 0.0) -> Std.GreaterThan, x, tf, ff) -> Std.Branch;
(10.0, Inc, Dec) -> ChooseApply -> Std.Println;
)",
                                             "bytecode_branch_callable_locals.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());

  const auto choose_it =
      std::find_if(result->functions.begin(), result->functions.end(),
                   [](const fleaux::bytecode::FunctionDef& fn) { return fn.name == "ChooseApply#0"; });
  REQUIRE(choose_it != result->functions.end());

  bool found_branch_call = false;
  bool found_builtin_branch = false;
  for (const auto& ins : choose_it->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kBranchCall) {
      found_branch_call = true;
    }
    if (ins.opcode == fleaux::bytecode::Opcode::kCallBuiltin) {
      const auto builtin_id = fleaux::vm::builtin_id_from_operand(ins.operand);
      REQUIRE(builtin_id.has_value());
      const auto name = fleaux::vm::builtin_name(*builtin_id);
      if (name == "Std.Branch") {
        found_builtin_branch = true;
      }
    }
  }

  REQUIRE_FALSE(found_branch_call);
  REQUIRE(found_builtin_branch);
}

TEST_CASE("Bytecode compiler seeds imported placeholders and builtin aliases from imported modules", "[bytecode]") {
  fleaux::frontend::ir::IRProgram ir_program;
  ir_program.expressions = {
      fleaux::frontend::ir::IRExprStatement{
          .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
              .lhs = make_int_constant_box(5),
              .rhs = make_ir_name_ref(std::nullopt, "Identity"),
              .span = std::nullopt,
          }),
          .span = std::nullopt,
      },
      fleaux::frontend::ir::IRExprStatement{
          .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
              .lhs = make_int_constant_box(5),
              .rhs = make_ir_name_ref(std::nullopt, "Neg"),
              .span = std::nullopt,
          }),
          .span = std::nullopt,
      },
      fleaux::frontend::ir::IRExprStatement{
          .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
              .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                  .items = {make_int_constant_box(5), make_name_ref_box(std::nullopt, "Identity")},
                  .span = std::nullopt,
              }),
              .rhs = make_ir_name_ref(std::string{"Std"}, "Apply", std::string{"Std.Apply"}),
              .span = std::nullopt,
          }),
          .span = std::nullopt,
      },
      fleaux::frontend::ir::IRExprStatement{
          .expr = make_ir_expr(fleaux::frontend::ir::IRFlowExpr{
              .lhs = make_ir_box(fleaux::frontend::ir::IRTupleExpr{
                  .items = {make_int_constant_box(5), make_name_ref_box(std::nullopt, "Neg")},
                  .span = std::nullopt,
              }),
              .rhs = make_ir_name_ref(std::string{"Std"}, "Apply", std::string{"Std.Apply"}),
              .span = std::nullopt,
          }),
          .span = std::nullopt,
      },
  };

  fleaux::bytecode::Module imported_module;
  imported_module.functions.push_back(fleaux::bytecode::FunctionDef{
      .name = "Foreign.Identity#0",
      .arity = 1,
      .has_variadic_tail = false,
      .is_import_placeholder = false,
      .instructions = {},
      .generic_params = {},
      .param_type_names = {},
      .return_type_name = {},
  });
  imported_module.exports = {
      fleaux::bytecode::ExportedSymbol{
          .name = "Identity",
          .link_name = {},
          .kind = fleaux::bytecode::ExportKind::kFunction,
          .index = 0,
          .builtin_name = {},
      },
      fleaux::bytecode::ExportedSymbol{
          .name = "AliasIdentity",
          .link_name = "Identity",
          .kind = fleaux::bytecode::ExportKind::kFunction,
          .index = 0,
          .builtin_name = {},
      },
      fleaux::bytecode::ExportedSymbol{
          .name = "Neg",
          .link_name = {},
          .kind = fleaux::bytecode::ExportKind::kBuiltinAlias,
          .index = 0,
          .builtin_name = "Std.UnaryMinus",
      },
      fleaux::bytecode::ExportedSymbol{
          .name = "Broken",
          .link_name = "Missing",
          .kind = fleaux::bytecode::ExportKind::kFunction,
          .index = 99,
          .builtin_name = {},
      },
  };

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(
      ir_program, make_compile_options(false, false, 256, std::nullopt, std::nullopt, std::nullopt, {imported_module}));

  REQUIRE(result.has_value());
  const auto identity_it =
      std::find_if(result->functions.begin(), result->functions.end(),
                   [](const fleaux::bytecode::FunctionDef& fn) -> bool { return fn.name == "Identity"; });
  REQUIRE(identity_it != result->functions.end());
  REQUIRE(identity_it->arity == 1);
  REQUIRE(identity_it->is_import_placeholder);
  const auto identity_index = std::distance(result->functions.begin(), identity_it);
  REQUIRE(std::none_of(result->functions.begin(), result->functions.end(),
                       [](const fleaux::bytecode::FunctionDef& fn) -> bool {
                         return fn.name == "AliasIdentity" || fn.name == "Missing";
                       }));

  bool found_import_call = false;
  bool found_builtin_alias_call = false;
  bool found_import_ref = false;
  bool found_builtin_alias_ref = false;
  for (const auto& ins : result->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kCallUserFunc) {
      found_import_call = true;
      REQUIRE(ins.operand == identity_index);
    }
    if (ins.opcode == fleaux::bytecode::Opcode::kMakeUserFuncRef) {
      found_import_ref = true;
      REQUIRE(ins.operand == identity_index);
    }
    // A direct call through the alias lowers to the native negation opcode for Std.UnaryMinus.
    if (ins.opcode == fleaux::bytecode::Opcode::kNeg) {
      found_builtin_alias_call = true;
    }
    if (ins.opcode == fleaux::bytecode::Opcode::kMakeBuiltinFuncRef) {
      const auto builtin_id = fleaux::vm::builtin_id_from_operand(ins.operand);
      REQUIRE(builtin_id.has_value());
      if (fleaux::vm::builtin_name(*builtin_id) == "Std.UnaryMinus") {
        found_builtin_alias_ref = true;
      }
    }
  }

  REQUIRE(found_import_call);
  REQUIRE(found_builtin_alias_call);
  REQUIRE(found_import_ref);
  REQUIRE(found_builtin_alias_ref);
}

TEST_CASE("Bytecode compiler exports only lets from the current source module", "[bytecode]") {
  const auto local_span = make_test_span("main.fleaux");
  const auto foreign_span = make_test_span("dependency.fleaux");

  fleaux::frontend::ir::IRProgram ir_program;
  ir_program.lets = {
      fleaux::frontend::ir::IRLet{
          .qualifier = std::string{"Std"},
          .name = "LocalBuiltin",
          .symbol_key = "Std.LocalBuiltin",
          .generic_params = {},
          .params = {},
          .return_type = make_simple_type("Any"),
          .doc_comments = {},
          .body = fleaux::frontend::ir::IRExprBox(std::nullopt),
          .is_builtin = true,
          .span = local_span,
      },
      fleaux::frontend::ir::IRLet{
          .qualifier = std::string{"Std"},
          .name = "ForeignBuiltin",
          .symbol_key = "Std.ForeignBuiltin",
          .generic_params = {},
          .params = {},
          .return_type = make_simple_type("Any"),
          .doc_comments = {},
          .body = fleaux::frontend::ir::IRExprBox(std::nullopt),
          .is_builtin = true,
          .span = foreign_span,
      },
      fleaux::frontend::ir::IRLet{
          .qualifier = std::nullopt,
          .name = "LocalFn",
          .symbol_key = "LocalFn#0",
          .generic_params = {},
          .params = {},
          .return_type = make_simple_type("Int64"),
          .doc_comments = {},
          .body = make_ir_box(fleaux::frontend::ir::IRConstant{.val = std::int64_t{7}, .span = local_span}, local_span),
          .is_builtin = false,
          .span = local_span,
      },
      fleaux::frontend::ir::IRLet{
          .qualifier = std::nullopt,
          .name = "ForeignFn",
          .symbol_key = "ForeignFn#0",
          .generic_params = {},
          .params = {},
          .return_type = make_simple_type("Int64"),
          .doc_comments = {},
          .body =
              make_ir_box(fleaux::frontend::ir::IRConstant{.val = std::int64_t{9}, .span = foreign_span}, foreign_span),
          .is_builtin = false,
          .span = foreign_span,
      },
      fleaux::frontend::ir::IRLet{
          .qualifier = std::string{"Std"},
          .name = "NoSpanBuiltin",
          .symbol_key = "Std.NoSpanBuiltin",
          .generic_params = {},
          .params = {},
          .return_type = make_simple_type("Any"),
          .doc_comments = {},
          .body = fleaux::frontend::ir::IRExprBox(std::nullopt),
          .is_builtin = true,
          .span = std::nullopt,
      },
      fleaux::frontend::ir::IRLet{
          .qualifier = std::nullopt,
          .name = "NoSpanFn",
          .symbol_key = "NoSpanFn#0",
          .generic_params = {},
          .params = {},
          .return_type = make_simple_type("Int64"),
          .doc_comments = {},
          .body = make_ir_box(fleaux::frontend::ir::IRConstant{.val = std::int64_t{11}, .span = std::nullopt}),
          .is_builtin = false,
          .span = std::nullopt,
      },
  };

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto filtered =
      compiler.compile(ir_program, make_compile_options(false, false, 256, std::filesystem::path("main.fleaux")));
  const auto unfiltered = compiler.compile(ir_program);

  REQUIRE(filtered.has_value());
  REQUIRE(unfiltered.has_value());
  REQUIRE(filtered->functions.size() == 3);

  REQUIRE(filtered->exports.size() == 4);
  REQUIRE(filtered->exports[0].name == "Std.LocalBuiltin");
  REQUIRE(filtered->exports[0].kind == fleaux::bytecode::ExportKind::kBuiltinAlias);
  REQUIRE(filtered->exports[1].name == "LocalFn");
  REQUIRE(filtered->exports[1].kind == fleaux::bytecode::ExportKind::kFunction);
  REQUIRE(filtered->exports[2].name == "Std.NoSpanBuiltin");
  REQUIRE(filtered->exports[2].kind == fleaux::bytecode::ExportKind::kBuiltinAlias);
  REQUIRE(filtered->exports[3].name == "NoSpanFn");
  REQUIRE(filtered->exports[3].kind == fleaux::bytecode::ExportKind::kFunction);

  REQUIRE(unfiltered->exports.size() == 6);
  REQUIRE(std::any_of(unfiltered->exports.begin(), unfiltered->exports.end(),
                      [](const fleaux::bytecode::ExportedSymbol& export_symbol) {
                        return export_symbol.name == "Std.ForeignBuiltin" &&
                               export_symbol.kind == fleaux::bytecode::ExportKind::kBuiltinAlias;
                      }));
  REQUIRE(std::any_of(unfiltered->exports.begin(), unfiltered->exports.end(),
                      [](const fleaux::bytecode::ExportedSymbol& export_symbol) {
                        return export_symbol.name == "ForeignFn" &&
                               export_symbol.kind == fleaux::bytecode::ExportKind::kFunction;
                      }));
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
      if (name == "Std.ToString") {
        found_to_string = true;
      }
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
(7.0) -> Double -> Std.Println;
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

TEST_CASE("Bytecode compiler emits kCallUserFuncBinary for direct binary user calls", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(R"(
import Std;
let AddPair(a: Int64, b: Int64): Int64 = (a, b) -> Std.Add;
(7, 8) -> AddPair -> Std.Println;
)",
                                             "bytecode_user_func_binary.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);

  REQUIRE(result.has_value());
  REQUIRE(result->functions.size() == 1U);
  REQUIRE(result->functions[0].name == "AddPair#0");
  REQUIRE(result->functions[0].arity == 2U);

  bool found_binary_call = false;
  bool found_generic_call = false;
  bool found_pair_build = false;
  for (const auto& ins : result->instructions) {
    found_binary_call = found_binary_call || ins.opcode == fleaux::bytecode::Opcode::kCallUserFuncBinary;
    found_generic_call = found_generic_call || ins.opcode == fleaux::bytecode::Opcode::kCallUserFunc;
    found_pair_build = found_pair_build || (ins.opcode == fleaux::bytecode::Opcode::kBuildTuple && ins.operand == 2);
  }

  REQUIRE(found_binary_call);
  REQUIRE_FALSE(found_generic_call);
  REQUIRE_FALSE(found_pair_build);
}

TEST_CASE("Bytecode compiler emits store/load locals for block-scoped lets", "[bytecode][blocks]") {
  const auto ir_program = lower_source_to_ir(R"(
import Std;
let Compute(): Int64 = {
  let x: Int64 = 1;
  let y: Int64 = (x, 2) -> Std.Add;
  y;
};
() -> Compute -> Std.Println;
)",
                                             "bytecode_block_locals.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);
  REQUIRE(result.has_value());

  const auto compute_it = std::find_if(result->functions.begin(), result->functions.end(),
                                       [](const auto& fn) { return fn.name == "Compute#0"; });
  REQUIRE(compute_it != result->functions.end());

  bool found_store_local = false;
  bool found_load_local = false;
  for (const auto& ins : compute_it->instructions) {
    found_store_local = found_store_local || ins.opcode == fleaux::bytecode::Opcode::kStoreLocal;
    found_load_local = found_load_local || ins.opcode == fleaux::bytecode::Opcode::kLoadLocal;
  }

  REQUIRE(found_store_local);
  REQUIRE(found_load_local);
}

TEST_CASE("Bytecode compiler materializes bare top-level values as zero-arg calls", "[bytecode][blocks][let]") {
  const auto ir_program = lower_source_to_ir(R"(
import Std;
let Meaning: Int64 = 42;
Meaning -> Std.Println;
)",
                                             "bytecode_top_level_value_materialization.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);
  REQUIRE(result.has_value());
  REQUIRE(result->functions.size() == 1U);
  REQUIRE(result->functions[0].name == "Meaning#0");

  bool found_call_user_func = false;
  bool found_make_user_func_ref = false;
  bool found_empty_tuple = false;
  for (const auto& ins : result->instructions) {
    found_call_user_func = found_call_user_func || ins.opcode == fleaux::bytecode::Opcode::kCallUserFunc;
    found_make_user_func_ref = found_make_user_func_ref || ins.opcode == fleaux::bytecode::Opcode::kMakeUserFuncRef;
    found_empty_tuple = found_empty_tuple || (ins.opcode == fleaux::bytecode::Opcode::kBuildTuple && ins.operand == 0);
  }

  REQUIRE(found_call_user_func);
  REQUIRE(found_empty_tuple);
  REQUIRE_FALSE(found_make_user_func_ref);
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
          .link_name = {},
          .kind = fleaux::bytecode::ExportKind::kFunction,
          .index = 0,
          .builtin_name = {},
      },
      fleaux::bytecode::ExportedSymbol{
          .name = "RoundTrip.Println",
          .link_name = {},
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
  REQUIRE(std::get<bool>(restored.constants[3].data));
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
  REQUIRE_FALSE(restored.functions[0].has_variadic_tail);
  REQUIRE_FALSE(restored.functions[0].is_value_binding);
  REQUIRE(restored.functions[0].instructions.size() == 4);
}

TEST_CASE("Bytecode serialization keeps value lets", "[bytecode][serialization][function-values]") {
  fleaux::bytecode::Module original;
  fleaux::bytecode::FunctionDef fn;
  fn.name = "Twice";
  fn.is_value_binding = true;
  fn.instructions = {{fleaux::bytecode::Opcode::kReturn, 0}};
  original.functions.push_back(std::move(fn));

  const auto serialized = fleaux::bytecode::serialize_module(original);
  REQUIRE(serialized.has_value());
  const auto deserialized = fleaux::bytecode::deserialize_module(serialized.value());
  REQUIRE(deserialized.has_value());
  REQUIRE(deserialized->functions.size() == 1U);
  REQUIRE(deserialized->functions[0].is_value_binding);
}

TEST_CASE("Bytecode serialization round-trips builtin callsite metadata", "[bytecode][serialization][verification]") {
  fleaux::bytecode::Module original;
  original.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kMakeUserFuncRef, 0},
      {fleaux::bytecode::Opcode::kBuildTuple, 0},
      {fleaux::bytecode::Opcode::kCallUserFunc, 1},
      {fleaux::bytecode::Opcode::kBuildTuple, 3},
      {fleaux::bytecode::Opcode::kCallBuiltin,
       fleaux::vm::builtin_operand(fleaux::vm::BuiltinId::ParallelWithOptions,
                                   fleaux::vm::BuiltinCallShape::kTupleExpanded)},
      {fleaux::bytecode::Opcode::kBuildTuple, 0},
      {fleaux::bytecode::Opcode::kCallUserFunc, 2},
      {fleaux::bytecode::Opcode::kCallBuiltin,
       fleaux::vm::builtin_operand(fleaux::vm::BuiltinId::RandomCreate, fleaux::vm::BuiltinCallShape::kDirectValue)},
      {fleaux::bytecode::Opcode::kBuildTuple, 0},
      {fleaux::bytecode::Opcode::kCallUserFunc, 3},
      {fleaux::bytecode::Opcode::kCallBuiltin,
       fleaux::vm::builtin_operand(fleaux::vm::BuiltinId::ExitInt64, fleaux::vm::BuiltinCallShape::kDirectValue)},
      {fleaux::bytecode::Opcode::kBuildTuple, 0},
      {fleaux::bytecode::Opcode::kCallUserFunc, 4},
      {fleaux::bytecode::Opcode::kCallBuiltin,
       fleaux::vm::builtin_operand(fleaux::vm::BuiltinId::TaskAwait, fleaux::vm::BuiltinCallShape::kDirectValue)},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };
  original.constants = {
      fleaux::bytecode::ConstValue{std::int64_t{1}},
  };
  original.builtin_callsites = {
      fleaux::bytecode::BuiltinCallsiteRecord{
          .owner_kind = fleaux::bytecode::BuiltinCallsiteOwnerKind::kModule,
          .owner_index = 0,
          .instruction_index = 5,
          .builtin_id = fleaux::vm::BuiltinId::ParallelWithOptions,
          .call_shape = fleaux::vm::BuiltinCallShape::kTupleExpanded,
          .static_argument_count = 3,
          .callable_slots = {fleaux::bytecode::BuiltinCallableSlotRecord{
              .slot_index = 1,
              .kind = fleaux::bytecode::BuiltinCallableSlotKind::kFunction,
              .target_index = 0,
              .declared_arity = 1,
              .has_variadic_tail = false,
          }},
          .value_slots = {fleaux::bytecode::BuiltinValueSlotRecord{
              .slot_index = 2,
              .summary_kind = fleaux::bytecode::BuiltinValueSlotSummaryKind::kDictStringAny,
              .source_kind = fleaux::bytecode::BuiltinValueSlotSourceKind::kFunctionReturn,
              .source_index = 1,
          }},
      },
      fleaux::bytecode::BuiltinCallsiteRecord{
          .owner_kind = fleaux::bytecode::BuiltinCallsiteOwnerKind::kModule,
          .owner_index = 0,
          .instruction_index = 8,
          .builtin_id = fleaux::vm::BuiltinId::RandomCreate,
          .call_shape = fleaux::vm::BuiltinCallShape::kDirectValue,
          .static_argument_count = 1,
          .value_slots = {fleaux::bytecode::BuiltinValueSlotRecord{
              .slot_index = 0,
              .summary_kind = fleaux::bytecode::BuiltinValueSlotSummaryKind::kExactUInt64,
              .source_kind = fleaux::bytecode::BuiltinValueSlotSourceKind::kFunctionReturn,
              .source_index = 2,
          }},
      },
      fleaux::bytecode::BuiltinCallsiteRecord{
          .owner_kind = fleaux::bytecode::BuiltinCallsiteOwnerKind::kModule,
          .owner_index = 0,
          .instruction_index = 11,
          .builtin_id = fleaux::vm::BuiltinId::ExitInt64,
          .call_shape = fleaux::vm::BuiltinCallShape::kDirectValue,
          .static_argument_count = 1,
          .value_slots = {fleaux::bytecode::BuiltinValueSlotRecord{
              .slot_index = 0,
              .summary_kind = fleaux::bytecode::BuiltinValueSlotSummaryKind::kExactInt64,
              .source_kind = fleaux::bytecode::BuiltinValueSlotSourceKind::kFunctionReturn,
              .source_index = 3,
          }},
      },
      fleaux::bytecode::BuiltinCallsiteRecord{
          .owner_kind = fleaux::bytecode::BuiltinCallsiteOwnerKind::kModule,
          .owner_index = 0,
          .instruction_index = 14,
          .builtin_id = fleaux::vm::BuiltinId::TaskAwait,
          .call_shape = fleaux::vm::BuiltinCallShape::kDirectValue,
          .static_argument_count = 1,
          .value_slots = {fleaux::bytecode::BuiltinValueSlotRecord{
              .slot_index = 0,
              .summary_kind = fleaux::bytecode::BuiltinValueSlotSummaryKind::kTaskHandle,
              .source_kind = fleaux::bytecode::BuiltinValueSlotSourceKind::kFunctionReturn,
              .source_index = 4,
          }},
      },
  };
  original.functions = {
      fleaux::bytecode::FunctionDef{
          .name = "Inc",
          .arity = 1,
          .has_variadic_tail = false,
      },
      fleaux::bytecode::FunctionDef{
          .name = "BuildOptions",
          .arity = 0,
          .has_variadic_tail = false,
          .return_type_name = "Dict",
          .return_type_is_dict_string_any = true,
      },
      fleaux::bytecode::FunctionDef{
          .name = "BuildSeed",
          .arity = 0,
          .has_variadic_tail = false,
          .return_type_name = "UInt64",
          .return_type_is_exact_uint64 = true,
      },
      fleaux::bytecode::FunctionDef{
          .name = "BuildExitCode",
          .arity = 0,
          .has_variadic_tail = false,
          .return_type_name = "Int64",
          .return_type_is_exact_int64 = true,
      },
      fleaux::bytecode::FunctionDef{
          .name = "BuildTask",
          .arity = 0,
          .has_variadic_tail = false,
          .return_type_name = "TaskHandle",
          .return_type_is_task_handle = true,
      },
  };

  const auto serialized = fleaux::bytecode::serialize_module(original);
  REQUIRE(serialized.has_value());

  const auto deserialized = fleaux::bytecode::deserialize_module(*serialized);
  REQUIRE(deserialized.has_value());
  REQUIRE(deserialized->builtin_callsites.size() == 4U);

  const auto& restored_parallel = deserialized->builtin_callsites[0];
  REQUIRE(restored_parallel.owner_kind == fleaux::bytecode::BuiltinCallsiteOwnerKind::kModule);
  REQUIRE(restored_parallel.owner_index == 0U);
  REQUIRE(restored_parallel.instruction_index == 5U);
  REQUIRE(restored_parallel.builtin_id == fleaux::vm::BuiltinId::ParallelWithOptions);
  REQUIRE(restored_parallel.call_shape == fleaux::vm::BuiltinCallShape::kTupleExpanded);
  REQUIRE(restored_parallel.static_argument_count == 3U);
  REQUIRE(restored_parallel.callable_slots.size() == 1U);
  REQUIRE(restored_parallel.callable_slots[0].kind == fleaux::bytecode::BuiltinCallableSlotKind::kFunction);
  REQUIRE(restored_parallel.callable_slots[0].target_index == 0U);
  REQUIRE(restored_parallel.value_slots.size() == 1U);
  REQUIRE(restored_parallel.value_slots[0].slot_index == 2U);
  REQUIRE(restored_parallel.value_slots[0].summary_kind ==
          fleaux::bytecode::BuiltinValueSlotSummaryKind::kDictStringAny);
  REQUIRE(restored_parallel.value_slots[0].source_index == 1U);

  const auto& restored_random_create = deserialized->builtin_callsites[1];
  REQUIRE(restored_random_create.owner_kind == fleaux::bytecode::BuiltinCallsiteOwnerKind::kModule);
  REQUIRE(restored_random_create.owner_index == 0U);
  REQUIRE(restored_random_create.instruction_index == 8U);
  REQUIRE(restored_random_create.builtin_id == fleaux::vm::BuiltinId::RandomCreate);
  REQUIRE(restored_random_create.call_shape == fleaux::vm::BuiltinCallShape::kDirectValue);
  REQUIRE(restored_random_create.static_argument_count == 1U);
  REQUIRE(restored_random_create.callable_slots.empty());
  REQUIRE(restored_random_create.value_slots.size() == 1U);
  REQUIRE(restored_random_create.value_slots[0].slot_index == 0U);
  REQUIRE(restored_random_create.value_slots[0].summary_kind ==
          fleaux::bytecode::BuiltinValueSlotSummaryKind::kExactUInt64);
  REQUIRE(restored_random_create.value_slots[0].source_index == 2U);

  const auto& restored_exit = deserialized->builtin_callsites[2];
  REQUIRE(restored_exit.owner_kind == fleaux::bytecode::BuiltinCallsiteOwnerKind::kModule);
  REQUIRE(restored_exit.owner_index == 0U);
  REQUIRE(restored_exit.instruction_index == 11U);
  REQUIRE(restored_exit.builtin_id == fleaux::vm::BuiltinId::ExitInt64);
  REQUIRE(restored_exit.call_shape == fleaux::vm::BuiltinCallShape::kDirectValue);
  REQUIRE(restored_exit.static_argument_count == 1U);
  REQUIRE(restored_exit.callable_slots.empty());
  REQUIRE(restored_exit.value_slots.size() == 1U);
  REQUIRE(restored_exit.value_slots[0].slot_index == 0U);
  REQUIRE(restored_exit.value_slots[0].summary_kind == fleaux::bytecode::BuiltinValueSlotSummaryKind::kExactInt64);
  REQUIRE(restored_exit.value_slots[0].source_index == 3U);

  const auto& restored_task = deserialized->builtin_callsites[3];
  REQUIRE(restored_task.owner_kind == fleaux::bytecode::BuiltinCallsiteOwnerKind::kModule);
  REQUIRE(restored_task.owner_index == 0U);
  REQUIRE(restored_task.instruction_index == 14U);
  REQUIRE(restored_task.builtin_id == fleaux::vm::BuiltinId::TaskAwait);
  REQUIRE(restored_task.call_shape == fleaux::vm::BuiltinCallShape::kDirectValue);
  REQUIRE(restored_task.static_argument_count == 1U);
  REQUIRE(restored_task.callable_slots.empty());
  REQUIRE(restored_task.value_slots.size() == 1U);
  REQUIRE(restored_task.value_slots[0].slot_index == 0U);
  REQUIRE(restored_task.value_slots[0].summary_kind == fleaux::bytecode::BuiltinValueSlotSummaryKind::kTaskHandle);
  REQUIRE(restored_task.value_slots[0].source_index == 4U);

  const auto verified = fleaux::bytecode::verify_module_builtin_callsites(*deserialized);
  REQUIRE(verified.has_value());
}

TEST_CASE("Bytecode verifier rejects builtin callsite metadata drift", "[bytecode][verification][builtins]") {
  fleaux::bytecode::Module module;
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kCallBuiltin,
       fleaux::vm::builtin_operand(fleaux::vm::BuiltinId::Help, fleaux::vm::BuiltinCallShape::kDirectValue)},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };
  module.constants = {
      fleaux::bytecode::ConstValue{std::string{"Std.Help"}},
  };
  module.builtin_callsites = {
      fleaux::bytecode::BuiltinCallsiteRecord{
          .owner_kind = fleaux::bytecode::BuiltinCallsiteOwnerKind::kModule,
          .owner_index = 0,
          .instruction_index = 1,
          .builtin_id = fleaux::vm::BuiltinId::Println,
          .call_shape = fleaux::vm::BuiltinCallShape::kTupleExpanded,
          .static_argument_count = 2,
      },
  };

  const auto verified = fleaux::bytecode::verify_module_builtin_callsites(module);
  REQUIRE_FALSE(verified.has_value());
  REQUIRE(verified.error().message.find("Builtin callsite mismatch") != std::string::npos);
}

TEST_CASE("Bytecode verifier rejects builtin callsite arity drift", "[bytecode][verification][builtins]") {
  fleaux::bytecode::Module module;
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kBuildTuple, 1},
      {fleaux::bytecode::Opcode::kCallBuiltin,
       fleaux::vm::builtin_operand(fleaux::vm::BuiltinId::Help, fleaux::vm::BuiltinCallShape::kTupleExpanded)},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };
  module.constants = {
      fleaux::bytecode::ConstValue{std::string{"Std.Help"}},
  };
  module.builtin_callsites = {
      fleaux::bytecode::BuiltinCallsiteRecord{
          .owner_kind = fleaux::bytecode::BuiltinCallsiteOwnerKind::kModule,
          .owner_index = 0,
          .instruction_index = 2,
          .builtin_id = fleaux::vm::BuiltinId::Help,
          .call_shape = fleaux::vm::BuiltinCallShape::kTupleExpanded,
          .static_argument_count = 2,
      },
  };

  const auto verified = fleaux::bytecode::verify_module_builtin_callsites(module);
  REQUIRE_FALSE(verified.has_value());
  REQUIRE(verified.error().message.find("arity mismatch") != std::string::npos);
}

TEST_CASE("Bytecode verifier rejects stale callable slot metadata", "[bytecode][verification][builtins]") {
  fleaux::bytecode::Module module;
  module.functions = {
      fleaux::bytecode::FunctionDef{
          .name = "NeedsTwo",
          .arity = 2,
          .has_variadic_tail = false,
      },
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kMakeUserFuncRef, 0},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kCallBuiltin,
       fleaux::vm::builtin_operand(fleaux::vm::BuiltinId::Apply, fleaux::vm::BuiltinCallShape::kTupleExpanded)},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };
  module.constants = {
      fleaux::bytecode::ConstValue{std::int64_t{1}},
  };
  module.builtin_callsites = {
      fleaux::bytecode::BuiltinCallsiteRecord{
          .owner_kind = fleaux::bytecode::BuiltinCallsiteOwnerKind::kModule,
          .owner_index = 0,
          .instruction_index = 3,
          .builtin_id = fleaux::vm::BuiltinId::Apply,
          .call_shape = fleaux::vm::BuiltinCallShape::kTupleExpanded,
          .static_argument_count = 2,
          .callable_slots = {fleaux::bytecode::BuiltinCallableSlotRecord{
              .slot_index = 1,
              .kind = fleaux::bytecode::BuiltinCallableSlotKind::kFunction,
              .target_index = 0,
              .declared_arity = 1,
              .has_variadic_tail = false,
          }},
      },
  };

  const auto verified = fleaux::bytecode::verify_module_builtin_callsites(module);
  REQUIRE_FALSE(verified.has_value());
  REQUIRE(verified.error().message.find("stale function callable metadata") != std::string::npos);
}

TEST_CASE("Bytecode verifier rejects stale Dict<String, Any> options metadata", "[bytecode][verification][builtins]") {
  fleaux::bytecode::Module module;
  module.functions = {
      fleaux::bytecode::FunctionDef{
          .name = "Inc",
          .arity = 1,
          .has_variadic_tail = false,
      },
      fleaux::bytecode::FunctionDef{
          .name = "BuildOptions",
          .arity = 0,
          .has_variadic_tail = false,
          .return_type_name = "Dict",
          .return_type_is_dict_string_any = false,
      },
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kMakeUserFuncRef, 0},
      {fleaux::bytecode::Opcode::kBuildTuple, 0},
      {fleaux::bytecode::Opcode::kCallUserFunc, 1},
      {fleaux::bytecode::Opcode::kBuildTuple, 3},
      {fleaux::bytecode::Opcode::kCallBuiltin,
       fleaux::vm::builtin_operand(fleaux::vm::BuiltinId::ParallelWithOptions,
                                   fleaux::vm::BuiltinCallShape::kTupleExpanded)},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };
  module.constants = {
      fleaux::bytecode::ConstValue{std::int64_t{1}},
  };
  module.builtin_callsites = {
      fleaux::bytecode::BuiltinCallsiteRecord{
          .owner_kind = fleaux::bytecode::BuiltinCallsiteOwnerKind::kModule,
          .owner_index = 0,
          .instruction_index = 5,
          .builtin_id = fleaux::vm::BuiltinId::ParallelWithOptions,
          .call_shape = fleaux::vm::BuiltinCallShape::kTupleExpanded,
          .static_argument_count = 3,
          .callable_slots = {fleaux::bytecode::BuiltinCallableSlotRecord{
              .slot_index = 1,
              .kind = fleaux::bytecode::BuiltinCallableSlotKind::kFunction,
              .target_index = 0,
              .declared_arity = 1,
              .has_variadic_tail = false,
          }},
          .value_slots = {fleaux::bytecode::BuiltinValueSlotRecord{
              .slot_index = 2,
              .summary_kind = fleaux::bytecode::BuiltinValueSlotSummaryKind::kDictStringAny,
              .source_kind = fleaux::bytecode::BuiltinValueSlotSourceKind::kFunctionReturn,
              .source_index = 1,
          }},
      },
  };

  const auto verified = fleaux::bytecode::verify_module_builtin_callsites(module);
  REQUIRE_FALSE(verified.has_value());
  REQUIRE(verified.error().message.find("stale Dict<String, Any> value metadata") != std::string::npos);
}

TEST_CASE("Bytecode verifier rejects stale exact UInt64 value metadata", "[bytecode][verification][builtins]") {
  fleaux::bytecode::Module module;
  module.functions = {
      fleaux::bytecode::FunctionDef{
          .name = "BuildSeed",
          .arity = 0,
          .has_variadic_tail = false,
          .return_type_name = "UInt64",
          .return_type_is_exact_uint64 = false,
      },
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kBuildTuple, 0},
      {fleaux::bytecode::Opcode::kCallUserFunc, 0},
      {fleaux::bytecode::Opcode::kCallBuiltin,
       fleaux::vm::builtin_operand(fleaux::vm::BuiltinId::RandomCreate, fleaux::vm::BuiltinCallShape::kDirectValue)},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };
  module.builtin_callsites = {
      fleaux::bytecode::BuiltinCallsiteRecord{
          .owner_kind = fleaux::bytecode::BuiltinCallsiteOwnerKind::kModule,
          .owner_index = 0,
          .instruction_index = 2,
          .builtin_id = fleaux::vm::BuiltinId::RandomCreate,
          .call_shape = fleaux::vm::BuiltinCallShape::kDirectValue,
          .static_argument_count = 1,
          .value_slots = {fleaux::bytecode::BuiltinValueSlotRecord{
              .slot_index = 0,
              .summary_kind = fleaux::bytecode::BuiltinValueSlotSummaryKind::kExactUInt64,
              .source_kind = fleaux::bytecode::BuiltinValueSlotSourceKind::kFunctionReturn,
              .source_index = 0,
          }},
      },
  };

  const auto verified = fleaux::bytecode::verify_module_builtin_callsites(module);
  REQUIRE_FALSE(verified.has_value());
  REQUIRE(verified.error().message.find("stale exact UInt64 value metadata") != std::string::npos);
}

TEST_CASE("Bytecode verifier rejects stale exact Int64 value metadata", "[bytecode][verification][builtins]") {
  fleaux::bytecode::Module module;
  module.functions = {
      fleaux::bytecode::FunctionDef{
          .name = "BuildExitCode",
          .arity = 0,
          .has_variadic_tail = false,
          .return_type_name = "Int64",
          .return_type_is_exact_int64 = false,
      },
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kBuildTuple, 0},
      {fleaux::bytecode::Opcode::kCallUserFunc, 0},
      {fleaux::bytecode::Opcode::kCallBuiltin,
       fleaux::vm::builtin_operand(fleaux::vm::BuiltinId::ExitInt64, fleaux::vm::BuiltinCallShape::kDirectValue)},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };
  module.builtin_callsites = {
      fleaux::bytecode::BuiltinCallsiteRecord{
          .owner_kind = fleaux::bytecode::BuiltinCallsiteOwnerKind::kModule,
          .owner_index = 0,
          .instruction_index = 2,
          .builtin_id = fleaux::vm::BuiltinId::ExitInt64,
          .call_shape = fleaux::vm::BuiltinCallShape::kDirectValue,
          .static_argument_count = 1,
          .value_slots = {fleaux::bytecode::BuiltinValueSlotRecord{
              .slot_index = 0,
              .summary_kind = fleaux::bytecode::BuiltinValueSlotSummaryKind::kExactInt64,
              .source_kind = fleaux::bytecode::BuiltinValueSlotSourceKind::kFunctionReturn,
              .source_index = 0,
          }},
      },
  };

  const auto verified = fleaux::bytecode::verify_module_builtin_callsites(module);
  REQUIRE_FALSE(verified.has_value());
  REQUIRE(verified.error().message.find("stale exact Int64 value metadata") != std::string::npos);
}

TEST_CASE("Bytecode verifier rejects stale opaque handle value metadata", "[bytecode][verification][builtins]") {
  fleaux::bytecode::Module module;
  module.functions = {
      fleaux::bytecode::FunctionDef{
          .name = "BuildTask",
          .arity = 0,
          .has_variadic_tail = false,
          .return_type_name = "TaskHandle",
          .return_type_is_task_handle = false,
      },
  };
  module.instructions = {
      {fleaux::bytecode::Opcode::kBuildTuple, 0},
      {fleaux::bytecode::Opcode::kCallUserFunc, 0},
      {fleaux::bytecode::Opcode::kCallBuiltin,
       fleaux::vm::builtin_operand(fleaux::vm::BuiltinId::TaskAwait, fleaux::vm::BuiltinCallShape::kDirectValue)},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };
  module.builtin_callsites = {
      fleaux::bytecode::BuiltinCallsiteRecord{
          .owner_kind = fleaux::bytecode::BuiltinCallsiteOwnerKind::kModule,
          .owner_index = 0,
          .instruction_index = 2,
          .builtin_id = fleaux::vm::BuiltinId::TaskAwait,
          .call_shape = fleaux::vm::BuiltinCallShape::kDirectValue,
          .static_argument_count = 1,
          .value_slots = {fleaux::bytecode::BuiltinValueSlotRecord{
              .slot_index = 0,
              .summary_kind = fleaux::bytecode::BuiltinValueSlotSummaryKind::kTaskHandle,
              .source_kind = fleaux::bytecode::BuiltinValueSlotSourceKind::kFunctionReturn,
              .source_index = 0,
          }},
      },
  };

  const auto verified = fleaux::bytecode::verify_module_builtin_callsites(module);
  REQUIRE_FALSE(verified.has_value());
  REQUIRE(verified.error().message.find("stale TaskHandle value metadata") != std::string::npos);
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

TEST_CASE("Bytecode deserialization rejects invalid opcode values", "[bytecode][serialization][opcodes]") {
  fleaux::bytecode::Module module;
  module.instructions = {{static_cast<fleaux::bytecode::Opcode>(9999), 0}};

  const auto serialized = fleaux::bytecode::serialize_module(module);
  REQUIRE(serialized.has_value());

  const auto result = fleaux::bytecode::deserialize_module(*serialized);
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message.find("Invalid opcode value 9999") != std::string::npos);
}

TEST_CASE("Bytecode module loader accepts empty entry source files", "[bytecode][loader][empty]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_empty_entry_module";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto entry_path = temp_dir / "empty_entry.fleaux";
  {
    std::ofstream out(entry_path);
    REQUIRE(out.good());
  }

  const auto loaded = fleaux::bytecode::load_linked_module(entry_path);
  if (!loaded) {
    INFO(loaded.error().message);
  }
  REQUIRE(loaded.has_value());
  REQUIRE(loaded->header.source_path == entry_path.string());

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto executed = runtime.execute(*loaded, output);
  if (!executed) {
    INFO(executed.error().message);
  }
  REQUIRE(executed.has_value());
  REQUIRE(output.str().empty());
}

TEST_CASE("Bytecode module loader accepts imported empty source files", "[bytecode][loader][empty][imports]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_empty_import_module";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto dep_path = temp_dir / "empty_dep.fleaux";
  {
    std::ofstream out(dep_path);
    REQUIRE(out.good());
  }

  const auto entry_path = temp_dir / "entry.fleaux";
  {
    std::ofstream out(entry_path);
    REQUIRE(out.good());
    out << "import empty_dep;\n";
    REQUIRE(out.good());
  }

  const auto loaded = fleaux::bytecode::load_linked_module(entry_path);
  if (!loaded) {
    INFO(loaded.error().message);
  }
  REQUIRE(loaded.has_value());

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto executed = runtime.execute(*loaded, output);
  if (!executed) {
    INFO(executed.error().message);
  }
  REQUIRE(executed.has_value());
  REQUIRE(output.str().empty());
}

TEST_CASE("Bytecode round-trip: compile -> serialize -> deserialize -> verify", "[bytecode][serialization]") {
  const std::string source_text =
      "import Std;\n"
      "import 20_export;\n"
      "let Local(x: Float64): Any = (y: Float64): Float64 = (x, y) -> Std.Add;\n"
      "(4, 5) -> Std.Add -> Std.Println;\n";
  const auto ir_program = lower_source_to_ir(source_text, "bytecode_roundtrip.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled = compiler.compile(
      ir_program, make_compile_options(false, false, 256, std::filesystem::path("bytecode_roundtrip.fleaux"),
                                       std::string{source_text}, std::string{"bytecode_roundtrip"}));
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
  REQUIRE(loaded.error().hint.has_value());
  REQUIRE(loaded.error().hint->find("Verify module name and file location") != std::string::npos);
  REQUIRE(loaded.error().span.has_value());
  REQUIRE(loaded.error().span->source_name == entry_path.string());
  REQUIRE(loaded.error().span->line == 1);
}

TEST_CASE("Bytecode module loader preserves source span for typed import analysis failures",
          "[bytecode][serialization][imports][contract]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_bytecode_import_type_error_span";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto dep_path = temp_dir / "typed_dep.fleaux";
  const auto entry_path = temp_dir / "typed_entry.fleaux";
  {
    std::ofstream out(dep_path);
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
  REQUIRE(loaded.error().hint.has_value());
  REQUIRE(loaded.error().span.has_value());
  REQUIRE(loaded.error().span->source_name == entry_path.string());
  REQUIRE(loaded.error().span->line == 3);
  REQUIRE(loaded.error().span->source_line().has_value());
  REQUIRE(loaded.error().span->source_line()->find("Add4") != std::string::npos);
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
           "let Add4(x: Float64): Float64 = (4.0, x) -> Std.Add;\n";
  }

  {
    std::ofstream out(entry_path);
    out << "import Std;\n"
           "import typed_dep;\n"
           "(1.0) -> Add4 -> Std.Println;\n";
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
  if (!runtime_result) {
    INFO("vm runtime error: " << runtime_result.error().message);
  }
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
           "let Add4(x: Float64): Float64 = (4.0, x) -> Std.Add;\n";
  }

  {
    std::ofstream out(entry_path);
    out << "import Std;\n"
           "import 20_export;\n"
           "(4.0) -> Add4 -> Std.Println;\n";
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
  if (!runtime_result) {
    INFO("vm runtime error: " << runtime_result.error().message);
  }
  REQUIRE(runtime_result.has_value());
}

TEST_CASE("Bytecode module loader preserves exported user overload link names",
          "[bytecode][serialization][imports][overload]") {
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

  auto dependency_bytes = read_binary_file_bytes(dependency_bytecode_path);

  const auto deserialized = fleaux::bytecode::deserialize_module(dependency_bytes);
  REQUIRE(deserialized.has_value());
  REQUIRE(deserialized->exports.size() == 2U);
  REQUIRE(deserialized->exports[0].name == "Echo");
  REQUIRE(deserialized->exports[0].link_name == "Echo#0");
  REQUIRE(deserialized->exports[1].name == "Echo");
  REQUIRE(deserialized->exports[1].link_name == "Echo#1");

  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(*loaded);
  if (!runtime_result) {
    INFO("vm runtime error: " << runtime_result.error().message);
  }
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
  REQUIRE(loaded.error().hint.has_value());
  REQUIRE(loaded.error().hint->find("Add4 expects argument 0") != std::string::npos);
}

TEST_CASE("Bytecode module loader propagates imported transparent aliases into entry analysis",
          "[bytecode][serialization][imports][aliases]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_bytecode_imported_aliases";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto dependency_path = temp_dir / "alias_dep.fleaux";
  const auto entry_path = temp_dir / "alias_entry.fleaux";

  {
    std::ofstream out(dependency_path);
    out << "import Std;\n"
           "alias Distance = Int64;\n"
           "let EchoDistance(x: Distance): Distance = x;\n";
  }

  {
    std::ofstream out(entry_path);
    out << "import Std;\n"
           "import alias_dep;\n"
           "let NeedsDistance(x: Distance): Distance = x;\n"
           "(1) -> EchoDistance -> NeedsDistance -> Std.Println;\n";
  }

  const auto loaded = fleaux::bytecode::load_linked_module(entry_path);
  if (!loaded.has_value()) {
    INFO("bytecode imported alias load error: " << loaded.error().message);
  }
  REQUIRE(loaded.has_value());

  const fleaux::vm::Runtime runtime;
  std::ostringstream output;
  const auto runtime_result = runtime.execute(*loaded, output);
  if (!runtime_result.has_value()) {
    INFO("vm runtime error: " << runtime_result.error().message);
  }
  REQUIRE(runtime_result.has_value());
  REQUIRE(output.str() == "1\n");
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
           "let Add4(x: Float64): Float64 = (4.0, x) -> Std.Add;\n";
  }

  {
    std::ofstream out(entry_path);
    out << "import Std;\n"
           "import typed_dep;\n"
           "(1.0) -> Add4 -> Std.Println;\n";
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
  REQUIRE(loaded.error().hint.has_value());
  REQUIRE(loaded.error().hint->find("Add4 expects argument 0") != std::string::npos);
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
           "let Add4(x: Float64): Float64 = (4.0, x) -> Std.Add;\n";
  }

  {
    std::ofstream out(entry_path);
    out << "import Std;\n"
           "import typed_dep;\n"
           "(1.0) -> Add4 -> Std.Println;\n";
  }

  const auto initial_load = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE(initial_load.has_value());
  REQUIRE(std::filesystem::exists(entry_bytecode_path));
  REQUIRE(std::filesystem::exists(dependency_bytecode_path));

  auto dependency_bytes = read_binary_file_bytes(dependency_bytecode_path);

  const auto deserialized = fleaux::bytecode::deserialize_module(dependency_bytes);
  REQUIRE(deserialized.has_value());
  auto corrupted_dependency = deserialized.value();

  corrupted_dependency.exports.push_back(fleaux::bytecode::ExportedSymbol{
      .name = "Ghost.Export",
      .link_name = {},
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
  REQUIRE(loaded.error().message.find("Failed to seed typed imports from source") != std::string::npos);
  REQUIRE(loaded.error().hint.has_value());
  REQUIRE(loaded.error().hint->find("Missing exported declaration for typed import seed") != std::string::npos);
  REQUIRE(loaded.error().hint->find("Ghost.Export") != std::string::npos);
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
  REQUIRE(loaded.error().hint.has_value());
  REQUIRE(loaded.error().hint->find("expects argument 0") != std::string::npos);
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
           "let MyMath.Add4(x: Float64): Float64 = (4.0, x) -> Std.Add;\n";
  }

  {
    std::ofstream out(entry_path);
    out << "import Std;\n"
           "import typed_dep_qualified_ok;\n"
           "(1.0) -> MyMath.Add4 -> Std.Println;\n";
  }

  const auto loaded = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE(loaded.has_value());

  const fleaux::vm::Runtime runtime;
  std::ostringstream output;
  const auto runtime_result = runtime.execute(loaded.value(), output);
  if (!runtime_result) {
    INFO("vm runtime error: " << runtime_result.error().message);
  }
  REQUIRE(runtime_result.has_value());
  REQUIRE(output.str().find('5') != std::string::npos);
}

TEST_CASE("Bytecode module loader reconstructs generic typed import seeds from cached bytecode",
          "[bytecode][serialization][imports][stage4g]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_bytecode_typed_imports_generic_seed";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto dependency_path = temp_dir / "generic_dep.fleaux";
  const auto dependency_bytecode_path = temp_dir / "generic_dep.fleaux.bc";
  const auto entry_path = temp_dir / "generic_entry.fleaux";
  const auto entry_bytecode_path = temp_dir / "generic_entry.fleaux.bc";

  {
    std::ofstream out(dependency_path);
    out << "let Identity<T>(x: T): T = x;\n";
  }

  {
    std::ofstream out(entry_path);
    out << "let NeedsInt(x: Int64): Int64 = x;\n"
           "import generic_dep;\n"
           "(1) -> Identity -> NeedsInt;\n";
  }

  const auto initial_load = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE(initial_load.has_value());
  REQUIRE(std::filesystem::exists(dependency_bytecode_path));
  REQUIRE(std::filesystem::exists(entry_bytecode_path));

  std::filesystem::remove(dependency_path);
  std::filesystem::remove(entry_bytecode_path);
  REQUIRE_FALSE(std::filesystem::exists(dependency_path));
  REQUIRE_FALSE(std::filesystem::exists(entry_bytecode_path));

  const auto cached_dependency_load = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE(cached_dependency_load.has_value());

  const fleaux::vm::Runtime runtime;
  const auto runtime_result = runtime.execute(*cached_dependency_load);
  if (!runtime_result) {
    INFO("vm runtime error: " << runtime_result.error().message);
  }
  REQUIRE(runtime_result.has_value());
}

TEST_CASE("Bytecode module loader reconstructs nongeneric typed import seeds from cached bytecode",
          "[bytecode][serialization][imports][stage4g]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_bytecode_typed_imports_nongeneric_seed";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto dependency_path = temp_dir / "nongeneric_dep.fleaux";
  const auto dependency_bytecode_path = temp_dir / "nongeneric_dep.fleaux.bc";
  const auto entry_path = temp_dir / "nongeneric_entry.fleaux";
  const auto entry_bytecode_path = temp_dir / "nongeneric_entry.fleaux.bc";

  {
    std::ofstream out(dependency_path);
    out << "import Std;\n"
           "let Add4(x: Float64): Float64 = (4.0, x) -> Std.Add;\n";
  }

  {
    std::ofstream out(entry_path);
    out << "import Std;\n"
           "import nongeneric_dep;\n"
           "(1.0) -> Add4 -> Std.Println;\n";
  }

  const auto initial_load = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE(initial_load.has_value());
  REQUIRE(std::filesystem::exists(dependency_bytecode_path));
  REQUIRE(std::filesystem::exists(entry_bytecode_path));

  std::filesystem::remove(dependency_path);
  REQUIRE_FALSE(std::filesystem::exists(dependency_path));

  {
    std::ofstream out(entry_path, std::ios::trunc);
    out << "import Std;\n"
           "import nongeneric_dep;\n"
           "(\"oops\") -> Add4 -> Std.Println;\n";
  }

  std::filesystem::remove(entry_bytecode_path);
  REQUIRE_FALSE(std::filesystem::exists(entry_bytecode_path));

  const auto loaded = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE_FALSE(loaded.has_value());
  REQUIRE(loaded.error().message.find("Type mismatch in call target arguments") != std::string::npos);
  REQUIRE(loaded.error().hint.has_value());
  REQUIRE(loaded.error().hint->find("Add4 expects argument 0") != std::string::npos);
}

TEST_CASE("Bytecode module loader reconstructs structured typed import seeds from cached bytecode",
          "[bytecode][serialization][imports]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_bytecode_typed_imports_structured_seed";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto dependency_path = temp_dir / "structured_dep.fleaux";
  const auto dependency_bytecode_path = temp_dir / "structured_dep.fleaux.bc";
  const auto entry_path = temp_dir / "structured_entry.fleaux";
  const auto entry_bytecode_path = temp_dir / "structured_entry.fleaux.bc";

  {
    std::ofstream out(dependency_path);
    out << "import Std;\n"
           "let SumAll(xs: Tuple<Int64...>): Result<Int64, String> =\n"
           "  (xs, 0, (acc: Int64, x: Int64): Int64 = (acc, x) -> Std.Add) -> Std.Tuple.Reduce -> Std.Result.Ok;\n";
  }

  const auto write_entry = [&](const std::string& call_line) {
    std::ofstream out(entry_path, std::ios::trunc);
    out << "import Std;\n"
           "import structured_dep;\n"
        << call_line;
  };

  write_entry("((1, 2, 3)) -> SumAll -> Std.Result.Unwrap -> Std.Println;\n");
  const auto initial_load = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE(initial_load.has_value());
  REQUIRE(std::filesystem::exists(dependency_bytecode_path));

  std::filesystem::remove(dependency_path);
  REQUIRE_FALSE(std::filesystem::exists(dependency_path));

  SECTION("Calls matching the full parameter and return types still type-check") {
    std::filesystem::remove(entry_bytecode_path);
    write_entry(
        "let Total(): Result<Int64, String> = ((4, 5, 6)) -> SumAll;\n"
        "Total -> Std.Result.Unwrap -> Std.Println;\n");
    const auto loaded = fleaux::bytecode::load_linked_module(entry_path);
    if (!loaded) {
      INFO("load error: " << loaded.error().message);
    }
    REQUIRE(loaded.has_value());
  }

  SECTION("Calls with the wrong tuple element type are rejected") {
    std::filesystem::remove(entry_bytecode_path);
    write_entry("((\"a\", \"b\")) -> SumAll -> Std.Result.Unwrap -> Std.Println;\n");
    const auto loaded = fleaux::bytecode::load_linked_module(entry_path);
    REQUIRE_FALSE(loaded.has_value());
    REQUIRE(loaded.error().message.find("Type mismatch in call target arguments") != std::string::npos);
  }
}

TEST_CASE("Bytecode module loader can start from a serialized entry module", "[bytecode][serialization][imports]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_bytecode_serialized_entry";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto dependency_path = temp_dir / "20_export.fleaux";
  const auto entry_path = temp_dir / "21_import.fleaux";
  const auto dependency_bytecode_path = temp_dir / "20_export.fleaux.bc";
  const auto entry_bytecode_path = temp_dir / "21_import.fleaux.bc";

  {
    std::ofstream out(dependency_path);
    out << "import Std;\n"
           "let Add4(x: Float64): Float64 = (4.0, x) -> Std.Add;\n";
  }

  {
    std::ofstream out(entry_path);
    out << "import Std;\n"
           "import 20_export;\n"
           "(4.0) -> Add4 -> Std.Println;\n";
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
  if (!runtime_result) {
    INFO("vm runtime error: " << runtime_result.error().message);
  }
  REQUIRE(runtime_result.has_value());
}

TEST_CASE("Bytecode module loader reports bytecode-only entry corruption when source is unavailable",
          "[bytecode][serialization][imports][contract]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_bytecode_serialized_entry_corrupt";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto dependency_path = temp_dir / "20_export.fleaux";
  const auto entry_path = temp_dir / "21_import.fleaux";
  const auto entry_bytecode_path = temp_dir / "21_import.fleaux.bc";

  {
    std::ofstream out(dependency_path);
    out << "import Std;\n"
           "let Add4(x: Float64): Float64 = (4.0, x) -> Std.Add;\n";
  }

  {
    std::ofstream out(entry_path);
    out << "import Std;\n"
           "import 20_export;\n"
           "(4.0) -> Add4 -> Std.Println;\n";
  }

  const auto initial_load = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE(initial_load.has_value());
  REQUIRE(std::filesystem::exists(entry_bytecode_path));

  auto entry_bytes = read_binary_file_bytes(entry_bytecode_path);
  REQUIRE(entry_bytes.size() > 16U);
  entry_bytes.back() ^= 0x01U;

  {
    std::ofstream out(entry_bytecode_path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out.write(reinterpret_cast<const char*>(entry_bytes.data()), static_cast<std::streamsize>(entry_bytes.size()));
    REQUIRE(out.good());
  }

  std::filesystem::remove(entry_path);
  std::filesystem::remove(dependency_path);
  REQUIRE_FALSE(std::filesystem::exists(entry_path));
  REQUIRE_FALSE(std::filesystem::exists(dependency_path));

  const auto bytecode_only_load = fleaux::bytecode::load_linked_module(entry_bytecode_path);
  REQUIRE_FALSE(bytecode_only_load.has_value());
  REQUIRE(bytecode_only_load.error().message.find("checksum") != std::string::npos);
}

TEST_CASE("Bytecode module loader rejects bytecode-only entry when builtin callsite metadata is missing",
          "[bytecode][serialization][verification][contract]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_bytecode_builtin_callsite_metadata_missing";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto entry_path = temp_dir / "entry.fleaux";
  const auto entry_bytecode_path = temp_dir / "entry.fleaux.bc";

  {
    std::ofstream out(entry_path);
    out << "import Std;\n"
           "(\"Std.Add\") -> Std.Help;\n";
  }

  const auto initial_load = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE(initial_load.has_value());
  REQUIRE(std::filesystem::exists(entry_bytecode_path));

  auto cached = read_binary_file_bytes(entry_bytecode_path);
  const auto deserialized = fleaux::bytecode::deserialize_module(cached);
  REQUIRE(deserialized.has_value());

  auto corrupted = *deserialized;
  corrupted.builtin_callsites.clear();

  const auto corrupted_bytes = fleaux::bytecode::serialize_module(corrupted);
  REQUIRE(corrupted_bytes.has_value());

  {
    std::ofstream out(entry_bytecode_path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.good());
    out.write(reinterpret_cast<const char*>(corrupted_bytes->data()),
              static_cast<std::streamsize>(corrupted_bytes->size()));
    REQUIRE(out.good());
  }

  std::filesystem::remove(entry_path);
  REQUIRE_FALSE(std::filesystem::exists(entry_path));

  const auto bytecode_only_load = fleaux::bytecode::load_linked_module(entry_bytecode_path);
  REQUIRE_FALSE(bytecode_only_load.has_value());
  REQUIRE(bytecode_only_load.error().message.find("Builtin callsite metadata count mismatch") != std::string::npos);
}

TEST_CASE("Bytecode module loader honors optimization-mode cache mismatch without rewriting cache",
          "[bytecode][serialization][imports][options]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_bytecode_cache_mode_mismatch";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto entry_path = temp_dir / "entry.fleaux";
  const auto entry_bytecode_path = temp_dir / "entry.fleaux.bc";

  {
    std::ofstream out(entry_path);
    out << "import Std;\n"
           "(1, 2) -> Std.Add -> Std.Println;\n";
  }

  const auto baseline_load = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE(baseline_load.has_value());
  REQUIRE(std::filesystem::exists(entry_bytecode_path));
  REQUIRE(baseline_load->header.optimization_mode ==
          static_cast<std::uint8_t>(fleaux::bytecode::OptimizationMode::kBaseline));

  auto baseline_bytes = read_binary_file_bytes(entry_bytecode_path);
  const auto baseline_deserialized = fleaux::bytecode::deserialize_module(baseline_bytes);
  REQUIRE(baseline_deserialized.has_value());
  REQUIRE(baseline_deserialized->header.optimization_mode ==
          static_cast<std::uint8_t>(fleaux::bytecode::OptimizationMode::kBaseline));

  const auto extended_load =
      fleaux::bytecode::load_linked_module(entry_path, fleaux::bytecode::ModuleLoadOptions{
                                                           .mode = fleaux::bytecode::OptimizationMode::kExtended,
                                                           .write_bytecode_cache = false,
                                                       });
  REQUIRE(extended_load.has_value());
  REQUIRE(extended_load->header.optimization_mode ==
          static_cast<std::uint8_t>(fleaux::bytecode::OptimizationMode::kExtended));

  auto cache_bytes_after_extended_load = read_binary_file_bytes(entry_bytecode_path);
  const auto cached_after_extended = fleaux::bytecode::deserialize_module(cache_bytes_after_extended_load);
  REQUIRE(cached_after_extended.has_value());
  REQUIRE(cached_after_extended->header.optimization_mode ==
          static_cast<std::uint8_t>(fleaux::bytecode::OptimizationMode::kBaseline));
}

TEST_CASE("Bytecode module loader threads auto value-ref options into compilation and bypasses cache writes",
          "[bytecode][value_ref][auto][loader]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_bytecode_value_ref_loader";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto entry_path = temp_dir / "entry.fleaux";
  const auto entry_bytecode_path = temp_dir / "entry.fleaux.bc";

  {
    std::ofstream out(entry_path);
    REQUIRE(out.good());
    out << "import Std;\n"
           "let Echo(x: String): String = x;\n"
           "(\"abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"
           "\")"
           " -> Echo -> Std.Println;\n";
  }

  const auto loaded = fleaux::bytecode::load_linked_module(entry_path, fleaux::bytecode::ModuleLoadOptions{
                                                                           .enable_value_ref_gate = true,
                                                                           .enable_auto_value_ref = true,
                                                                           .value_ref_byte_cutoff = 32,
                                                                       });
  REQUIRE(loaded.has_value());

  bool saw_make_value_ref = false;
  for (const auto& ins : loaded->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kMakeValueRef) {
      saw_make_value_ref = true;
      break;
    }
  }
  REQUIRE(saw_make_value_ref);
  REQUIRE_FALSE(std::filesystem::exists(entry_bytecode_path));
}

TEST_CASE("Bytecode module loader threads experimental builtin reductions into compilation and bypasses cache writes",
          "[bytecode][optimizer][experimental][loader]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_bytecode_builtin_reductions_loader";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto entry_path = temp_dir / "entry.fleaux";
  const auto entry_bytecode_path = temp_dir / "entry.fleaux.bc";

  {
    std::ofstream out(entry_path);
    REQUIRE(out.good());
    out << "import Std;\n"
           "\"  hi  \" -> Std.String.TrimStart -> Std.String.TrimEnd -> Std.Println;\n";
  }

  const auto loaded =
      fleaux::bytecode::load_linked_module(entry_path, fleaux::bytecode::ModuleLoadOptions{
                                                           .enable_experimental_builtin_reductions = true,
                                                       });
  REQUIRE(loaded.has_value());

  bool saw_builtin_trim = false;
  bool saw_builtin_trim_start = false;
  bool saw_builtin_trim_end = false;
  for (const auto& ins : loaded->instructions) {
    if (ins.opcode == fleaux::bytecode::Opcode::kCallBuiltin) {
      const auto builtin_id = fleaux::vm::builtin_id_from_operand(ins.operand);
      REQUIRE(builtin_id.has_value());
      const auto& builtin_name = fleaux::vm::builtin_name(*builtin_id);
      if (builtin_name == "Std.String.Trim")
        saw_builtin_trim = true;
      if (builtin_name == "Std.String.TrimStart")
        saw_builtin_trim_start = true;
      if (builtin_name == "Std.String.TrimEnd")
        saw_builtin_trim_end = true;
    }
  }

  REQUIRE(saw_builtin_trim);
  REQUIRE_FALSE(saw_builtin_trim_start);
  REQUIRE_FALSE(saw_builtin_trim_end);
  REQUIRE_FALSE(std::filesystem::exists(entry_bytecode_path));
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
           "let Add1(x: Int64): Int64 = (x, 1) -> Std.Add;\n";
  }

  {
    std::ofstream out(left_path);
    out << "import shared;\n"
           "let Left(x: Int64): Int64 = (x) -> Add1;\n";
  }

  {
    std::ofstream out(right_path);
    out << "import shared;\n"
           "let Right(x: Int64): Int64 = (x) -> Add1;\n";
  }

  {
    std::ofstream out(root_path);
    out << "import Std;\n"
           "import left;\n"
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
  if (!runtime_result) {
    INFO("vm runtime error: " << runtime_result.error().message);
  }
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

  REQUIRE_FALSE(std::get<bool>(module.constants[static_cast<std::size_t>(module.instructions[0].operand)].data));
  REQUIRE(std::get<std::int64_t>(module.constants[static_cast<std::size_t>(module.instructions[1].operand)].data) == 5);
  REQUIRE(std::get<bool>(module.constants[static_cast<std::size_t>(module.instructions[2].operand)].data));
}

namespace {

// Builds a module computing `lhs op rhs` for each pair, then runs constant folding.
auto fold_binary_pairs(const fleaux::bytecode::Opcode opcode, const std::vector<fleaux::bytecode::ConstValue>& constants,
                       const std::vector<std::pair<std::int64_t, std::int64_t>>& operand_pairs)
    -> fleaux::bytecode::Module {
  fleaux::bytecode::Module module;
  module.constants = constants;
  for (const auto& [lhs, rhs] : operand_pairs) {
    module.instructions.push_back({fleaux::bytecode::Opcode::kPushConst, lhs});
    module.instructions.push_back({fleaux::bytecode::Opcode::kPushConst, rhs});
    module.instructions.push_back({opcode, 0});
  }
  module.instructions.push_back({fleaux::bytecode::Opcode::kHalt, 0});
  fleaux::bytecode::ConstantFoldingPass{}.run(module);
  return module;
}

auto folded_constant(const fleaux::bytecode::Module& module, const std::size_t instruction_index)
    -> const fleaux::bytecode::ConstValue& {
  REQUIRE(module.instructions[instruction_index].opcode == fleaux::bytecode::Opcode::kPushConst);
  return module.constants[static_cast<std::size_t>(module.instructions[instruction_index].operand)];
}

}  // namespace

TEST_CASE("ConstantFoldingPass folds kDiv with truncating integer and IEEE floating semantics",
          "[bytecode][optimizer]") {
  const auto module = fold_binary_pairs(fleaux::bytecode::Opcode::kDiv,
                                        {
                                            fleaux::bytecode::ConstValue{std::int64_t{10}},
                                            fleaux::bytecode::ConstValue{std::int64_t{4}},
                                            fleaux::bytecode::ConstValue{std::int64_t{0}},
                                            fleaux::bytecode::ConstValue{10.0},
                                            fleaux::bytecode::ConstValue{0.0},
                                        },
                                        {{0, 1}, {0, 2}, {3, 4}});

  // 10 / 4 folds to Int64 2, 10 / 0 is left for the runtime to report, 10.0 / 0.0 folds to infinity.
  REQUIRE(module.instructions.size() == 6);
  REQUIRE(std::get<std::int64_t>(folded_constant(module, 0).data) == 2);
  REQUIRE(module.instructions[3].opcode == fleaux::bytecode::Opcode::kDiv);
  REQUIRE(std::isinf(std::get<double>(folded_constant(module, 4).data)));
  REQUIRE(module.instructions[5].opcode == fleaux::bytecode::Opcode::kHalt);
}

TEST_CASE("ConstantFoldingPass folds kMod exactly and leaves integer mod-by-zero to the runtime",
          "[bytecode][optimizer]") {
  const auto module = fold_binary_pairs(fleaux::bytecode::Opcode::kMod,
                                        {
                                            fleaux::bytecode::ConstValue{std::int64_t{-10}},
                                            fleaux::bytecode::ConstValue{std::int64_t{4}},
                                            fleaux::bytecode::ConstValue{std::int64_t{0}},
                                            fleaux::bytecode::ConstValue{10.0},
                                            fleaux::bytecode::ConstValue{0.0},
                                        },
                                        {{0, 1}, {0, 2}, {3, 4}});

  // -10 % 4 keeps the dividend's sign, -10 % 0 is left for the runtime, 10.0 % 0.0 folds to NaN.
  REQUIRE(module.instructions.size() == 6);
  REQUIRE(std::get<std::int64_t>(folded_constant(module, 0).data) == -2);
  REQUIRE(module.instructions[3].opcode == fleaux::bytecode::Opcode::kMod);
  REQUIRE(std::isnan(std::get<double>(folded_constant(module, 4).data)));
}

TEST_CASE("ConstantFoldingPass folds kPow exactly and leaves negative integer exponents to the runtime",
          "[bytecode][optimizer]") {
  const auto module = fold_binary_pairs(fleaux::bytecode::Opcode::kPow,
                                        {
                                            fleaux::bytecode::ConstValue{std::int64_t{2}},
                                            fleaux::bytecode::ConstValue{std::int64_t{8}},
                                            fleaux::bytecode::ConstValue{std::int64_t{-1}},
                                            fleaux::bytecode::ConstValue{2.0},
                                            fleaux::bytecode::ConstValue{-1.0},
                                        },
                                        {{0, 1}, {0, 2}, {3, 4}});

  REQUIRE(module.instructions.size() == 6);
  REQUIRE(std::get<std::int64_t>(folded_constant(module, 0).data) == 256);
  REQUIRE(module.instructions[3].opcode == fleaux::bytecode::Opcode::kPow);
  REQUIRE(std::get<double>(folded_constant(module, 4).data) == 0.5);
}

TEST_CASE("ConstantFoldingPass leaves integer overflow to the runtime", "[bytecode][optimizer]") {
  const auto module = fold_binary_pairs(fleaux::bytecode::Opcode::kAdd,
                                        {
                                            fleaux::bytecode::ConstValue{std::numeric_limits<std::int64_t>::max()},
                                            fleaux::bytecode::ConstValue{std::int64_t{1}},
                                            fleaux::bytecode::ConstValue{std::uint64_t{1}},
                                            fleaux::bytecode::ConstValue{std::uint64_t{2}},
                                        },
                                        {{0, 1}, {2, 3}});

  // Int64 max + 1 is not folded. UInt64 1 + 2 folds to UInt64 3.
  REQUIRE(module.instructions.size() == 5);
  REQUIRE(module.instructions[2].opcode == fleaux::bytecode::Opcode::kAdd);
  REQUIRE(std::get<std::uint64_t>(folded_constant(module, 3).data) == 3U);
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

TEST_CASE("Bytecode module loader keeps generic bounds for bytecode-only imports", "[bytecode][serialization][imports]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_bytecode_generic_bounds_seed";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);

  const auto dependency_path = temp_dir / "bounded_dep.fleaux";
  const auto dependency_bytecode_path = temp_dir / "bounded_dep.fleaux.bc";
  const auto entry_path = temp_dir / "bounded_entry.fleaux";
  const auto entry_bytecode_path = temp_dir / "bounded_entry.fleaux.bc";

  {
    std::ofstream out(dependency_path);
    out << "import Std;\n"
           "let Twice<T: Numeric>(x: T): T = (x, x) -> Std.Add;\n";
  }

  const auto write_entry = [&](const std::string& call_line) {
    std::ofstream out(entry_path, std::ios::trunc);
    out << "import Std;\n"
           "import bounded_dep;\n"
        << call_line;
  };

  write_entry("21 -> Twice -> Std.Println;\n");
  REQUIRE(fleaux::bytecode::load_linked_module(entry_path).has_value());
  REQUIRE(std::filesystem::exists(dependency_bytecode_path));
  std::filesystem::remove(dependency_path);

  std::filesystem::remove(entry_bytecode_path);
  write_entry("(\"a\") -> Twice -> Std.Println;\n");
  const auto loaded = fleaux::bytecode::load_linked_module(entry_path);
  REQUIRE_FALSE(loaded.has_value());
  REQUIRE(loaded.error().message.find("Type argument does not satisfy its bound") != std::string::npos);
}

TEST_CASE("Bytecode serialization round-trips runtime type checks", "[bytecode][serialization][cast]") {
  using fleaux::common::RuntimeTypeCheck;
  using fleaux::common::RuntimeTypeCheckKind;

  fleaux::bytecode::Module original;
  original.type_checks = {
      RuntimeTypeCheck{.kind = RuntimeTypeCheckKind::kInt64},
      RuntimeTypeCheck{
          .kind = RuntimeTypeCheckKind::kDict,
          .items = {RuntimeTypeCheck{.kind = RuntimeTypeCheckKind::kString},
                    RuntimeTypeCheck{.kind = RuntimeTypeCheckKind::kUnion,
                                     .items = {RuntimeTypeCheck{.kind = RuntimeTypeCheckKind::kNull},
                                               RuntimeTypeCheck{.kind = RuntimeTypeCheckKind::kTuple,
                                                                .items = {RuntimeTypeCheck{.kind = RuntimeTypeCheckKind::kFloat64}},
                                                                .variadic_tail = true}}}}},
  };
  original.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kCheckType, 0},
      {fleaux::bytecode::Opcode::kTryCheckType, 1},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };
  original.constants = {fleaux::bytecode::ConstValue{.data = std::int64_t{1}}};

  const auto serialized = fleaux::bytecode::serialize_module(original);
  REQUIRE(serialized.has_value());
  const auto deserialized = fleaux::bytecode::deserialize_module(*serialized);
  REQUIRE(deserialized.has_value());
  REQUIRE(deserialized->type_checks == original.type_checks);
  REQUIRE(fleaux::common::describe(deserialized->type_checks[1]) == "Dict<String, Null | Tuple<Float64...>>");

  std::ostringstream dump;
  REQUIRE(fleaux::bytecode::disassemble_module(*deserialized, dump).has_value());
  REQUIRE(dump.str().find("type_check[1]=Dict<String, Null | Tuple<Float64...>>") != std::string::npos);
}

TEST_CASE("Bytecode deserialization rejects malformed runtime type checks", "[bytecode][serialization][cast]") {
  using fleaux::common::RuntimeTypeCheck;
  using fleaux::common::RuntimeTypeCheckKind;

  fleaux::bytecode::Module module;
  // A Dict check needs exactly a key and a value check.
  module.type_checks = {RuntimeTypeCheck{.kind = RuntimeTypeCheckKind::kDict,
                                         .items = {RuntimeTypeCheck{.kind = RuntimeTypeCheckKind::kString}}}};
  const auto serialized = fleaux::bytecode::serialize_module(module);
  REQUIRE(serialized.has_value());
  const auto deserialized = fleaux::bytecode::deserialize_module(*serialized);
  REQUIRE_FALSE(deserialized.has_value());
  REQUIRE(deserialized.error().message == "Cannot read type check");
}

TEST_CASE("Module loader links runtime type checks from imported modules", "[bytecode][loader][cast]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_cast_import";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);
  {
    std::ofstream lib(temp_dir / "cast_lib.fleaux");
    lib << "import Std;\n"
           "let Forget(x: Any): Any = x;\n"
           "let AsString(x: Any): String = x -> Std.Cast<String>;\n";
  }
  {
    std::ofstream main(temp_dir / "cast_main.fleaux");
    main << "import Std;\n"
            "import cast_lib;\n"
            "(\"a\" -> Forget -> AsString, 1 -> Forget -> Std.TryCast<Int64> -> Std.Result.Unwrap) -> Std.Println;\n";
  }

  const auto linked = fleaux::bytecode::load_linked_module(temp_dir / "cast_main.fleaux");
  REQUIRE(linked.has_value());
  REQUIRE(linked->type_checks.size() == 2U);

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  REQUIRE(runtime.execute(*linked, output).has_value());
  REQUIRE(output.str() == "a 1\n");
}

namespace {

auto compile_host_module(const std::string& source, const std::string& name) -> fleaux::bytecode::Module {
  const auto ir_program = lower_source_to_ir(source, name);
  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  auto compiled = compiler.compile(ir_program);
  REQUIRE(compiled.has_value());
  return std::move(*compiled);
}

auto host_call(const fleaux::bytecode::Module& module, const std::string& symbol, fleaux::runtime::Value arg)
    -> fleaux::vm::RuntimeValueResult {
  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  return runtime.invoke_symbol(module, symbol, std::move(arg), output);
}

}  // namespace

TEST_CASE("Host calls are checked against the declared parameter types", "[bytecode][host_entry]") {
  using fleaux::runtime::make_float;
  using fleaux::runtime::make_int;
  using fleaux::runtime::make_string;
  using fleaux::runtime::make_tuple;
  using fleaux::runtime::make_uint;

  const auto module = compile_host_module("import Std;\n"
                                          "type UserId :: Int64;\n"
                                          "let App.Double(x: Int64): Int64 = (x, x) -> Std.Add;\n"
                                          "let App.Greet(name: String, times: UInt64): String = name;\n"
                                          "let App.Id(id: UserId): UserId = id;\n"
                                          "let App.Count(items: Int64...): Int64 = items -> Std.Length;\n"
                                          "let App.Apply(f: (Int64) => Int64, x: Int64): Int64 = (x, f) -> Std.Apply;\n",
                                          "host_entry_checks.fleaux");

  SECTION("Matching arguments run") {
    REQUIRE(host_call(module, "App.Double", make_int(21)).value() == make_int(42));
    REQUIRE(host_call(module, "App.Greet", make_tuple(make_string("ada"), make_uint(2))).value() == make_string("ada"));
    REQUIRE(host_call(module, "App.Id", make_int(7)).value() == make_int(7));
    REQUIRE(host_call(module, "App.Count", make_tuple(make_int(1), make_int(2), make_int(3))).value() == make_int(3));
    REQUIRE(host_call(module, "App.Count", make_int(5)).value() == make_int(1));
  }

  SECTION("A mismatched argument is rejected before the function runs") {
    const auto result = host_call(module, "App.Double", make_float(1.5));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message.find("App.Double: argument 0 expected Int64, got Float64") != std::string::npos);
  }

  SECTION("Strong types are checked by their underlying type") {
    const auto result = host_call(module, "App.Id", make_string("seven"));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message.find("argument 0 expected Int64") != std::string::npos);
  }

  SECTION("Argument counts are checked") {
    const auto result = host_call(module, "App.Greet", make_tuple(make_string("ada")));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message.find("App.Greet: expected a tuple of 2 arguments") != std::string::npos);
  }

  SECTION("Each variadic argument is checked") {
    const auto result = host_call(module, "App.Count", make_tuple(make_int(1), make_string("two")));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message.find("App.Count: argument 1 expected Int64, got String") != std::string::npos);
  }

  SECTION("Function parameters must be callables") {
    const auto result = host_call(module, "App.Apply", make_tuple(make_int(1), make_int(2)));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message.find("App.Apply: argument 0 expected a function, got Int64") != std::string::npos);
  }
}

TEST_CASE("Host calls bind bounded generic parameters to one alternative", "[bytecode][host_entry][generics]") {
  using fleaux::runtime::make_float;
  using fleaux::runtime::make_int;
  using fleaux::runtime::make_string;
  using fleaux::runtime::make_tuple;

  const auto module = compile_host_module(
      "import Std;\n"
      "let App.Sum<T: Numeric>(a: T, b: T): T = (a, b) -> Std.Add;\n"
      "let App.Total<T: Numeric>(xs: Tuple<T...>): Int64 = xs -> Std.Length;\n"
      "let App.Pick<T>(c: Bool, a: T, b: T): T = (c, a, b) -> Std.Select;\n"
      "let App.Area<T: Tuple<Int64, Int64> | Tuple<Int64...>>(a: T, b: T): Int64 = 0;\n",
      "host_entry_generics.fleaux");

  SECTION("Arguments matching one alternative run") {
    REQUIRE(host_call(module, "App.Sum", make_tuple(make_int(1), make_int(2))).value() == make_int(3));
    REQUIRE(host_call(module, "App.Sum", make_tuple(make_float(1.5), make_float(2.0))).value() == make_float(3.5));
  }

  SECTION("Arguments matching different alternatives are rejected, in either order") {
    for (const auto& arg : {make_tuple(make_int(1), make_float(2.5)), make_tuple(make_float(2.5), make_int(1))}) {
      const auto result = host_call(module, "App.Sum", arg);
      REQUIRE_FALSE(result.has_value());
      REQUIRE(result.error().message.find("App.Sum: argument 1") != std::string::npos);
      REQUIRE(result.error().message.find("the same alternative as the earlier arguments for T") != std::string::npos);
    }
  }

  SECTION("Uses inside a tuple share the parameter") {
    REQUIRE(host_call(module, "App.Total", make_tuple(make_int(1), make_int(2))).value() == make_int(2));
    const auto result = host_call(module, "App.Total", make_tuple(make_int(1), make_float(2.5)));
    REQUIRE_FALSE(result.has_value());
  }

  SECTION("A value outside the bound is rejected") {
    const auto result = host_call(module, "App.Sum", make_tuple(make_string("a"), make_string("b")));
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message.find("expected Int64 | UInt64 | Float64, got String") != std::string::npos);
  }

  SECTION("Unbounded parameters accept any values") {
    REQUIRE(host_call(module, "App.Pick", make_tuple(fleaux::runtime::make_bool(true), make_int(1), make_string("x")))
                .value() == make_int(1));
  }

  SECTION("Overlapping alternatives use the alternative common to every argument") {
    const auto pair = make_tuple(make_int(2), make_int(3));
    const auto triple = make_tuple(make_int(1), make_int(2), make_int(3));
    REQUIRE(host_call(module, "App.Area", make_tuple(pair, triple)).has_value());
    REQUIRE(host_call(module, "App.Area", make_tuple(triple, pair)).has_value());
  }
}

TEST_CASE("Bytecode serialization round-trips host entry checks", "[bytecode][serialization][host_entry]") {
  const auto module = compile_host_module("import Std;\n"
                                          "let App.Sum<T: Numeric>(a: T, b: T): T = (a, b) -> Std.Add;\n"
                                          "let App.Each(f: (Int64) => Int64, xs: Int64...): Int64 = 0;\n",
                                          "host_entry_serialization.fleaux");
  const auto serialized = fleaux::bytecode::serialize_module(module);
  REQUIRE(serialized.has_value());
  const auto deserialized = fleaux::bytecode::deserialize_module(*serialized);
  REQUIRE(deserialized.has_value());
  REQUIRE(deserialized->exports.size() == module.exports.size());
  for (std::size_t index = 0; index < module.exports.size(); ++index) {
    INFO(module.exports[index].name);
    REQUIRE(module.exports[index].host_entry_check.has_value());
    REQUIRE(deserialized->exports[index].host_entry_check == module.exports[index].host_entry_check);
  }

  // Host-only kinds are not valid in the cast table.
  fleaux::bytecode::Module bad;
  bad.type_checks = {fleaux::common::RuntimeTypeCheck{.kind = fleaux::common::RuntimeTypeCheckKind::kCallable}};
  const auto bad_bytes = fleaux::bytecode::serialize_module(bad);
  REQUIRE(bad_bytes.has_value());
  REQUIRE_FALSE(fleaux::bytecode::deserialize_module(*bad_bytes).has_value());
}

TEST_CASE("Operators find user operator functions from imported modules", "[bytecode][loader][user_operators]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_user_operator_import";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);
  {
    std::ofstream lib(temp_dir / "money.fleaux");
    lib << "import Std;\n"
           "type Money = Int64;\n"
           "let Money.Add(a: Money, b: Money): Money = ((a -> Std.Cast<Int64>, b -> Std.Cast<Int64>) -> +) -> "
           "Std.Cast<Money>;\n"
           "let Money.From(x: Int64): Money = x -> Std.Cast<Money>;\n";
  }
  {
    std::ofstream main(temp_dir / "shop.fleaux");
    main << "import Std;\n"
            "import money;\n"
            "(4 -> Money.From, 6 -> Money.From) -> + -> Std.Println;\n";
  }

  const auto linked = fleaux::bytecode::load_linked_module(temp_dir / "shop.fleaux");
  if (!linked.has_value()) {
    INFO(linked.error().message);
  }
  REQUIRE(linked.has_value());
  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  REQUIRE(runtime.execute(*linked, output).has_value());
  REQUIRE(output.str() == "10\n");
}

TEST_CASE("Host calls pass the operator functions for the bound alternative", "[bytecode][host_entry][operator_requirements]") {
  using fleaux::runtime::make_float;
  using fleaux::runtime::make_int;
  using fleaux::runtime::make_tuple;
  // Money.Add adds 100 so the test can tell which function ran.
  const auto module = compile_host_module(
      "import Std;\n"
      "type Money = Int64;\n"
      "let Money.Add(a: Money, b: Money): Money =\n"
      "  (((a -> Std.Cast<Int64>, b -> Std.Cast<Int64>) -> +, 100) -> +) -> Std.Cast<Money>;\n"
      "let Add2<T: Float64 | Money>(a: T, b: T): T = (a, b) -> +;\n"
      "let Total<T: Float64 | Money>(xs: Tuple<T...>, zero: T): T =\n"
      "  (xs, zero, (acc: T, x: T): T = (acc, x) -> +) -> Std.Tuple.Reduce;\n",
      "host_operator_args.fleaux");

  REQUIRE(host_call(module, "Add2", make_tuple(make_int(1), make_int(2))).value() == make_int(103));
  REQUIRE(host_call(module, "Add2", make_tuple(make_float(1.5), make_float(2.0))).value() == make_float(3.5));
  REQUIRE(host_call(module, "Total", make_tuple(make_tuple(make_float(1.0), make_float(2.0)), make_float(0.0))).value() ==
          make_float(3.0));

  const auto mixed = host_call(module, "Add2", make_tuple(make_int(1), make_float(2.0)));
  REQUIRE_FALSE(mixed.has_value());
}

TEST_CASE("Bytecode keeps operator requirements of functions", "[bytecode][serialization][operator_requirements]") {
  const auto module = compile_host_module(
      "import Std;\n"
      "type Money = Int64;\n"
      "let Money.Add(a: Money, b: Money): Money = ((a -> Std.Cast<Int64>, b -> Std.Cast<Int64>) -> +) -> "
      "Std.Cast<Money>;\n"
      "let Add2<T: Int64 | Money>(a: T, b: T): T = (a, b) -> +;\n",
      "operator_requirements_bytecode.fleaux");
  const auto add2 = std::ranges::find_if(module.functions, [](const auto& fn) { return fn.name == "Add2#0"; });
  REQUIRE(add2 != module.functions.end());
  REQUIRE(add2->hidden_operator_params == 1U);
  REQUIRE(add2->arity == 3U);
  REQUIRE(add2->operator_requirements.size() == 1U);
  REQUIRE(add2->operator_requirements.front().op == "+");
  REQUIRE(add2->operator_requirements.front().alternative_functions.size() == 2U);

  const auto serialized = fleaux::bytecode::serialize_module(module);
  REQUIRE(serialized.has_value());
  const auto deserialized = fleaux::bytecode::deserialize_module(*serialized);
  REQUIRE(deserialized.has_value());
  const auto read_back = std::ranges::find_if(deserialized->functions, [](const auto& fn) { return fn.name == "Add2#0"; });
  REQUIRE(read_back->hidden_operator_params == 1U);
  REQUIRE(read_back->operator_requirements.front().signature == add2->operator_requirements.front().signature);
  REQUIRE(read_back->operator_requirements.front().alternative_functions ==
          add2->operator_requirements.front().alternative_functions);
}

TEST_CASE("Importing modules pass operator functions to imported generic functions",
          "[bytecode][loader][operator_requirements]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_operator_import";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);
  {
    std::ofstream lib(temp_dir / "money.fleaux");
    lib << "import Std;\n"
           "type Money = Int64;\n"
           "let Money.Add(a: Money, b: Money): Money = ((a -> Std.Cast<Int64>, b -> Std.Cast<Int64>) -> +) -> "
           "Std.Cast<Money>;\n"
           "let Money.From(x: Int64): Money = x -> Std.Cast<Money>;\n"
           "alias Summable = Int64 | Money;\n"
           "let Add2<T: Summable>(a: T, b: T): T = (a, b) -> +;\n";
  }
  {
    std::ofstream main(temp_dir / "shop.fleaux");
    main << "import Std;\n"
            "import money;\n"
            "(2, 3) -> Add2 -> Std.Println;\n"
            "(2 -> Money.From, 3 -> Money.From) -> Add2 -> Std.Println;\n"
            "let Twice<T: Summable>(x: T): T = (x, x) -> Add2;\n"
            "(4 -> Money.From) -> Twice -> Std.Println;\n"
            "let SumWith<T>(xs: Tuple<T...>, zero: T, add: (T, T) => T): T = (xs, zero, add) -> Std.Tuple.Reduce;\n"
            "((1 -> Money.From, 5 -> Money.From), 0 -> Money.From, Add2<Money>) -> SumWith -> Std.Println;\n";
  }

  const auto linked = fleaux::bytecode::load_linked_module(temp_dir / "shop.fleaux");
  if (!linked.has_value()) {
    INFO(linked.error().message);
  }
  REQUIRE(linked.has_value());
  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  REQUIRE(runtime.execute(*linked, output).has_value());
  REQUIRE(output.str() == "5\n5\n8\n6\n");
}

TEST_CASE("Bytecode keeps the module's type and alias declarations", "[bytecode][serialization][imports]") {
  const auto module = compile_host_module("import Std;\n"
                                          "type Money = Int64;\n"
                                          "type Id<T> = Int64;\n"
                                          "type Opaque :: __builtin__;\n"
                                          "alias Summable = Int64 | Money;\n",
                                          "type_decls_bytecode.fleaux");
  REQUIRE(module.type_decls.size() == 3U);
  REQUIRE(module.alias_decls.size() == 1U);

  const auto serialized = fleaux::bytecode::serialize_module(module);
  REQUIRE(serialized.has_value());
  const auto deserialized = fleaux::bytecode::deserialize_module(*serialized);
  REQUIRE(deserialized.has_value());
  REQUIRE(deserialized->type_decls.size() == 3U);
  const auto find_type = [&](const std::string& name) {
    return *std::ranges::find_if(deserialized->type_decls, [&](const auto& record) { return record.name == name; });
  };
  REQUIRE(find_type("Money").target == "Int64");
  REQUIRE(find_type("Id").generic_params == std::vector<std::string>{"T"});
  REQUIRE(find_type("Opaque").is_builtin);
  REQUIRE(deserialized->alias_decls.front().name == "Summable");
  REQUIRE(deserialized->alias_decls.front().target == "Int64 | Money");
}

TEST_CASE("Bytecode-only imports know the imported module's types, aliases, and operators",
          "[bytecode][loader][imports][operator_requirements]") {
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_core_tests_bytecode_only_types";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);
  const auto lib_path = temp_dir / "money.fleaux";
  const auto main_path = temp_dir / "shop.fleaux";
  {
    std::ofstream lib(lib_path);
    lib << "import Std;\n"
           "type Money = Int64;\n"
           "let Money.Add(a: Money, b: Money): Money = ((a -> Std.Cast<Int64>, b -> Std.Cast<Int64>) -> +) -> "
           "Std.Cast<Money>;\n"
           "let Money.From(x: Int64): Money = x -> Std.Cast<Money>;\n"
           "alias Summable = Int64 | Money;\n"
           "let Add2<T: Summable>(a: T, b: T): T = (a, b) -> +;\n";
  }
  {
    std::ofstream main(main_path);
    main << "import Std;\nimport money;\n(1, 2) -> Add2 -> Std.Println;\n";
  }
  REQUIRE(fleaux::bytecode::load_linked_module(main_path).has_value());
  REQUIRE(std::filesystem::exists(temp_dir / "money.fleaux.bc"));
  std::filesystem::remove(lib_path);
  std::filesystem::remove(temp_dir / "shop.fleaux.bc");

  {
    std::ofstream main(main_path, std::ios::trunc);
    main << "import Std;\n"
            "import money;\n"
            "let Twice<T: Summable>(x: T): T = (x, x) -> Add2;\n"
            "let Price: Money = 4 -> Money.From;\n"
            "Price -> Twice -> Std.Println;\n"
            "(Price, 1 -> Money.From) -> + -> Std.Println;\n";
  }
  const auto linked = fleaux::bytecode::load_linked_module(main_path);
  if (!linked.has_value()) {
    INFO(linked.error().message);
  }
  REQUIRE(linked.has_value());
  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  REQUIRE(runtime.execute(*linked, output).has_value());
  REQUIRE(output.str() == "8\n5\n");
}
