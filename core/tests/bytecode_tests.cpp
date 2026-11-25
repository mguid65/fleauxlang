#include <algorithm>
#include <cmath>
#include <cstring>
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
#include "fleaux/bytecode/local_liveness.hpp"
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

// Whether an instruction calls a builtin: with its argument tuple or value, or with its arguments on the stack.
auto is_builtin_call(const fleaux::bytecode::Opcode opcode) -> bool {
  return opcode == fleaux::bytecode::Opcode::kCallBuiltin || opcode == fleaux::bytecode::Opcode::kCallBuiltinArgs;
}

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
      .file = fleaux::frontend::diag::SourceFile{source_name, {}},
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

auto make_compile_options(std::optional<std::filesystem::path> source_path = std::nullopt,
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
  return options;
}

// Options that compile every call of a named function as a call: the tests of call instructions and of the VM's calls
// use them, since a small function is otherwise inlined.
auto without_inlining() -> fleaux::bytecode::CompileOptions { return fleaux::bytecode::CompileOptions{.inline_functions = false}; }

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

  // Inlined, ShowHelp's body would add a third call site to the module's code.
  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program, without_inlining());
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
      "(5.0, Inc) -> Std.Try -> Std.Println;\n"
      "(5.0, (x: Float64): Float64 = (x, 2.0) -> Std.Add) -> Std.Try -> Std.Println;\n"
      "(5.0, Std.UnaryMinus<Float64>) -> Std.Try -> Std.Println;\n",
      "bytecode_builtin_callable_slot_metadata.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program);
  REQUIRE(result.has_value());

  const auto& module = *result;
  std::vector<fleaux::bytecode::BuiltinCallsiteRecord> try_callsites;
  for (const auto& callsite : module.builtin_callsites) {
    if (callsite.builtin_id == fleaux::vm::BuiltinId::Try) {
      try_callsites.push_back(callsite);
    }
  }

  REQUIRE(try_callsites.size() == 3U);
  REQUIRE(try_callsites[0].callable_slots.size() == 1U);
  REQUIRE(try_callsites[0].callable_slots[0].slot_index == 1U);
  REQUIRE(try_callsites[0].callable_slots[0].kind == fleaux::bytecode::BuiltinCallableSlotKind::kFunction);
  REQUIRE(try_callsites[0].callable_slots[0].declared_arity == 1U);
  REQUIRE_FALSE(try_callsites[0].callable_slots[0].has_variadic_tail);

  REQUIRE(try_callsites[1].callable_slots.size() == 1U);
  REQUIRE(try_callsites[1].callable_slots[0].slot_index == 1U);
  REQUIRE(try_callsites[1].callable_slots[0].kind == fleaux::bytecode::BuiltinCallableSlotKind::kClosure);
  REQUIRE(try_callsites[1].callable_slots[0].declared_arity == 1U);
  REQUIRE_FALSE(try_callsites[1].callable_slots[0].has_variadic_tail);

  REQUIRE(try_callsites[2].callable_slots.size() == 1U);
  REQUIRE(try_callsites[2].callable_slots[0].slot_index == 1U);
  REQUIRE(try_callsites[2].callable_slots[0].kind == fleaux::bytecode::BuiltinCallableSlotKind::kBuiltin);
  REQUIRE(try_callsites[2].callable_slots[0].target_index ==
          static_cast<std::uint32_t>(fleaux::vm::BuiltinId::UnaryMinus));
  REQUIRE(try_callsites[2].callable_slots[0].declared_arity == 1U);
  REQUIRE_FALSE(try_callsites[2].callable_slots[0].has_variadic_tail);

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
  const auto result = compiler.compile(ir_program, without_inlining());
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
      if (!is_builtin_call(instruction.opcode)) {
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
      if (is_builtin_call(ins.opcode)) {
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
    if (is_builtin_call(ins.opcode)) {
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
    if (is_builtin_call(ins.opcode)) {
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
      ir_program, make_compile_options(std::nullopt, std::nullopt, std::nullopt, {}, true));

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
    if (is_builtin_call(ins.opcode)) {
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
      ir_program, make_compile_options(std::nullopt, std::nullopt, std::nullopt, {}, true));

  REQUIRE(result.has_value());

  bool found_builtin_os_has_env = false;
  bool found_builtin_os_env = false;
  for (const auto& ins : result->instructions) {
    if (is_builtin_call(ins.opcode)) {
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
    if (is_builtin_call(ins.opcode)) {
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

namespace {

auto count_opcode(const std::vector<fleaux::bytecode::Instruction>& instructions,
                  const fleaux::bytecode::Opcode opcode) -> std::size_t {
  return static_cast<std::size_t>(std::ranges::count_if(
      instructions, [opcode](const fleaux::bytecode::Instruction& instruction) { return instruction.opcode == opcode; }));
}

auto calls_builtin(const std::vector<fleaux::bytecode::Instruction>& instructions, const std::string_view name) -> bool {
  return std::ranges::any_of(instructions, [name](const fleaux::bytecode::Instruction& instruction) {
    if (!is_builtin_call(instruction.opcode)) {
      return false;
    }
    const auto builtin_id = fleaux::vm::builtin_id_from_operand(instruction.operand);
    return builtin_id.has_value() && fleaux::vm::builtin_name(*builtin_id) == name;
  });
}

// The compiled function a let became; its name is the let's name, followed by '#' and an overload number.
auto compiled_function(const fleaux::bytecode::Module& bytecode_module, const std::string& let_name)
    -> const fleaux::bytecode::FunctionDef& {
  const auto found = std::ranges::find_if(bytecode_module.functions, [&](const fleaux::bytecode::FunctionDef& function) {
    return function.name == let_name || function.name.starts_with(let_name + "#");
  });
  REQUIRE(found != bytecode_module.functions.end());
  return *found;
}

auto compile_and_run(const std::string& source, const std::string& source_name,
                     const fleaux::bytecode::CompileOptions& options = fleaux::bytecode::CompileOptions{})
    -> std::pair<fleaux::bytecode::Module, std::string> {
  const auto ir_program = lower_source_to_ir(source, source_name);
  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  auto compiled = compiler.compile(ir_program, options);
  REQUIRE(compiled.has_value());
  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto executed = runtime.execute(*compiled, output);
  if (!executed.has_value()) {
    INFO("runtime error: " << executed.error().message);
  }
  REQUIRE(executed.has_value());
  return {std::move(*compiled), output.str()};
}

}  // namespace

TEST_CASE("Bytecode compiler runs Std.Loop with named functions in place", "[bytecode][in_place]") {
  const auto [bytecode_module, output] = compile_and_run(R"(
let Continue(n: Int64): Bool = (n, 0) -> Std.GreaterThan;
let Step(n: Int64): Int64 = (n, 1) -> Std.Subtract;
(10, Continue, Step) -> Std.Loop -> Std.Println;
)",
                                                         "bytecode_loop_in_place_named.fleaux");

  REQUIRE(output == "0\n");
  REQUIRE(count_opcode(bytecode_module.instructions, fleaux::bytecode::Opcode::kLoopCall) == 0U);
  REQUIRE(count_opcode(bytecode_module.instructions, fleaux::bytecode::Opcode::kJumpIfNot) == 1U);
  // Continue and Step are inlined in the loop; without inlining, the loop calls them.
  REQUIRE(count_opcode(bytecode_module.instructions, fleaux::bytecode::Opcode::kCallUserFunc) == 0U);
  const auto [called, called_output] = compile_and_run(R"(
let Continue(n: Int64): Bool = (n, 0) -> Std.GreaterThan;
let Step(n: Int64): Int64 = (n, 1) -> Std.Subtract;
(10, Continue, Step) -> Std.Loop -> Std.Println;
)",
                                                       "bytecode_loop_in_place_named_calls.fleaux", without_inlining());
  REQUIRE(called_output == "0\n");
  REQUIRE(count_opcode(called.instructions, fleaux::bytecode::Opcode::kCallUserFunc) == 2U);
}

TEST_CASE("Bytecode compiler runs Std.Loop with inline closures in place", "[bytecode][in_place]") {
  const auto [bytecode_module, output] =
      compile_and_run("(0, (i: Int64): Bool = (i, 5) -> <, (i: Int64): Int64 = (i, 1) -> +) -> Std.Loop -> Std.Println;\n",
                      "bytecode_loop_in_place_closures.fleaux");

  REQUIRE(output == "5\n");
  REQUIRE(bytecode_module.closures.empty());
  REQUIRE(count_opcode(bytecode_module.instructions, fleaux::bytecode::Opcode::kLoopCall) == 0U);
  REQUIRE(count_opcode(bytecode_module.instructions, fleaux::bytecode::Opcode::kJump) == 1U);
}

TEST_CASE("Bytecode compiler emits kLoopCall for Std.Loop with function-value arguments", "[bytecode]") {
  const auto [bytecode_module, output] = compile_and_run(R"(
let RunLoop(start: Int64, keep_going: (Int64) => Bool, step: (Int64) => Int64): Int64 =
  (start, keep_going, step) -> Std.Loop;
(10, (n: Int64): Bool = (n, 0) -> >, (n: Int64): Int64 = (n, 1) -> -) -> RunLoop -> Std.Println;
)",
                                                         "bytecode_loop_function_values.fleaux");

  REQUIRE(output == "0\n");
  REQUIRE(count_opcode(compiled_function(bytecode_module, "RunLoop").instructions,
                       fleaux::bytecode::Opcode::kLoopCall) == 1U);
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

TEST_CASE("One-argument builtins receive their argument without a wrapping tuple", "[bytecode][builtins]") {
  // The closure is a value, so that it is compiled as a function of its own rather than run in place.
  const auto [bytecode_module, output] = compile_and_run(R"(
let Root: (Int64) => Float64 = (x: Int64): Float64 = x -> Std.ToFloat64 -> Std.Math.Sqrt;
(4) -> Root -> Std.Println;
(9.0) -> Std.Math.Sqrt -> Std.Println;
)",
                                                         "bytecode_one_argument_builtins.fleaux");

  REQUIRE(output == "2\n3\n");
  const auto& closure = bytecode_module.functions.at(bytecode_module.closures.front().function_index).instructions;
  REQUIRE(count_opcode(closure, fleaux::bytecode::Opcode::kBuildTuple) == 0U);
  REQUIRE(count_opcode(closure, fleaux::bytecode::Opcode::kCallBuiltin) == 2U);
}

TEST_CASE("Std.ElementAt on a tuple and an index compiles to kElementAt", "[bytecode][builtins]") {
  const auto [bytecode_module, output] = compile_and_run(R"(
let Pick(items: Tuple<Int64...>, at: UInt64): Int64 = (items, at) -> Std.ElementAt;
((10, 20, 30), 1u64) -> Pick -> Std.Println;
)",
                                                         "bytecode_element_at_opcode.fleaux");

  REQUIRE(output == "20\n");
  const auto& pick = compiled_function(bytecode_module, "Pick").instructions;
  REQUIRE(count_opcode(pick, fleaux::bytecode::Opcode::kElementAt) == 1U);
  REQUIRE_FALSE(calls_builtin(pick, "Std.ElementAt"));
}

TEST_CASE("kElementAt reports an index past the end", "[bytecode][builtins]") {
  const auto ir_program =
      lower_source_to_ir("((10, 20), 5u64) -> Std.ElementAt -> Std.Println;\n", "bytecode_element_at_range.fleaux");
  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled = compiler.compile(ir_program);
  REQUIRE(compiled.has_value());
  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto executed = runtime.execute(*compiled, output);
  REQUIRE_FALSE(executed.has_value());
  REQUIRE(executed.error().message.find("ElementAt: index out of range") != std::string::npos);
}

TEST_CASE("A one-argument builtin treats one-item tuples the same in every call", "[bytecode][builtins]") {
  // Called directly and through a function value, Std.ToString gets the one-item tuple itself.
  const auto [bytecode_module, output] = compile_and_run(R"(
(((1,), (2,)), Std.ToString) -> Std.Tuple.Map -> Std.Println;
(1,) -> Std.ToString -> Std.Println;
)",
                                                         "bytecode_one_argument_singletons.fleaux");

  REQUIRE(output == "((1,), (2,))\n(1,)\n");
}

TEST_CASE("Std.Task.AwaitAll accepts a tuple of one task", "[bytecode][builtins][concurrency]") {
  const auto [bytecode_module, output] = compile_and_run(R"(
let Id(x: Int64): Int64 = x;
((Id, 7) -> Std.Task.Spawn,) -> Std.Task.AwaitAll -> Std.Println;
)",
                                                         "bytecode_await_all_one_task.fleaux");

  REQUIRE(output == "(True, (7,))\n");
}

TEST_CASE("Bytecode compiler runs Std.Branch with named functions in place", "[bytecode][in_place]") {
  const auto [bytecode_module, output] = compile_and_run(R"(
let Inc(x: Float64): Float64 = (x, 1.0) -> Std.Add;
let Dec(x: Float64): Float64 = (x, 1.0) -> Std.Subtract;
(True, 10.0, Inc, Dec) -> Std.Branch -> Std.Println;
(False, 10.0, Inc, Dec) -> Std.Branch -> Std.Println;
)",
                                                         "bytecode_branch_in_place_named.fleaux");

  REQUIRE(output == "11\n9\n");
  REQUIRE(count_opcode(bytecode_module.instructions, fleaux::bytecode::Opcode::kBranchCall) == 0U);
  REQUIRE_FALSE(calls_builtin(bytecode_module.instructions, "Std.Branch"));
  // Inc and Dec are inlined in both branches; without inlining, each branch calls its function.
  REQUIRE(count_opcode(bytecode_module.instructions, fleaux::bytecode::Opcode::kCallUserFunc) == 0U);
  const auto [called, called_output] = compile_and_run(R"(
let Inc(x: Float64): Float64 = (x, 1.0) -> Std.Add;
let Dec(x: Float64): Float64 = (x, 1.0) -> Std.Subtract;
(True, 10.0, Inc, Dec) -> Std.Branch -> Std.Println;
(False, 10.0, Inc, Dec) -> Std.Branch -> Std.Println;
)",
                                                       "bytecode_branch_in_place_named_calls.fleaux", without_inlining());
  REQUIRE(called_output == "11\n9\n");
  REQUIRE(count_opcode(called.instructions, fleaux::bytecode::Opcode::kCallUserFunc) == 4U);
}

TEST_CASE("Bytecode compiler runs Std.Branch with inline closures in place", "[bytecode][in_place]") {
  const auto [bytecode_module, output] = compile_and_run(R"(
let Abs(n: Int64): Int64 = ((n, 0) -> <, n, (k: Int64): Int64 = (0, k) -> -, (k: Int64): Int64 = k) -> Std.Branch;
(-5) -> Abs -> Std.Println;
(7) -> Abs -> Std.Println;
)",
                                                         "bytecode_branch_in_place_closures.fleaux");

  REQUIRE(output == "5\n7\n");
  REQUIRE(bytecode_module.closures.empty());
  const auto& abs = compiled_function(bytecode_module, "Abs").instructions;
  REQUIRE(count_opcode(abs, fleaux::bytecode::Opcode::kJumpIfNot) == 1U);
  REQUIRE(count_opcode(abs, fleaux::bytecode::Opcode::kJump) == 1U);
  REQUIRE(count_opcode(abs, fleaux::bytecode::Opcode::kMakeClosureRef) == 0U);
  REQUIRE_FALSE(calls_builtin(abs, "Std.Branch"));
}

TEST_CASE("Std.Branch run in place keeps closure scoping", "[bytecode][in_place]") {
  // The first arm's parameter shadows the let's own `n`, captures `factor`, and is captured by a closure that
  // Std.Apply runs in place too. The second arm holds another Std.Branch run in place.
  const auto [bytecode_module, output] = compile_and_run(R"(
let Scale(n: Int64, factor: Int64): Int64 =
  (
    (n, 0) -> >,
    (n, 2) -> *,
    (n: Int64): Int64 = ((n, factor) -> *, (x: Int64): Int64 = (x, n) -> +) -> Std.Apply,
    (k: Int64): Int64 = ((k, 0) -> ==, k, (z: Int64): Int64 = 100, (z: Int64): Int64 = (z, factor) -> -) -> Std.Branch
  ) -> Std.Branch;
(3, 10) -> Scale -> Std.Println;
(0, 10) -> Scale -> Std.Println;
(-4, 10) -> Scale -> Std.Println;
)",
                                                         "bytecode_branch_in_place_scoping.fleaux");

  REQUIRE(output == "66\n100\n-18\n");
  const auto& scale = compiled_function(bytecode_module, "Scale").instructions;
  REQUIRE(count_opcode(scale, fleaux::bytecode::Opcode::kJumpIfNot) == 2U);
  REQUIRE(count_opcode(scale, fleaux::bytecode::Opcode::kMakeClosureRef) == 0U);
  REQUIRE_FALSE(calls_builtin(scale, "Std.Branch"));
  REQUIRE_FALSE(calls_builtin(scale, "Std.Apply"));
}

TEST_CASE("Bytecode compiler runs Std.Tuple.Map with an inline closure in place", "[bytecode][in_place]") {
  const auto [bytecode_module, output] = compile_and_run(R"(
((1, 2, 3), (x: Int64): Int64 = (x, 10) -> *) -> Std.Tuple.Map -> Std.Println;
(0 -> Std.Tuple.Range, (x: Int64): Int64 = x) -> Std.Tuple.Map -> Std.Println;
)",
                                                         "bytecode_map_in_place_closure.fleaux");

  REQUIRE(output == "(10, 20, 30)\n()\n");
  REQUIRE(bytecode_module.closures.empty());
  REQUIRE_FALSE(calls_builtin(bytecode_module.instructions, "Std.Tuple.Map"));
  REQUIRE(count_opcode(bytecode_module.instructions, fleaux::bytecode::Opcode::kBuildTupleCounted) == 2U);
}

TEST_CASE("Bytecode compiler runs Std.Tuple.Map with a named function in place", "[bytecode][in_place]") {
  const auto [bytecode_module, output] = compile_and_run(R"(
let Twice(x: Int64): Int64 = (x, 2) -> *;
((1, 2, 3), Twice) -> Std.Tuple.Map -> Std.Println;
)",
                                                         "bytecode_map_in_place_named.fleaux");

  REQUIRE(output == "(2, 4, 6)\n");
  REQUIRE_FALSE(calls_builtin(bytecode_module.instructions, "Std.Tuple.Map"));
  // Twice is inlined in the loop's body; without inlining, the body calls it.
  REQUIRE(count_opcode(bytecode_module.instructions, fleaux::bytecode::Opcode::kCallUserFunc) == 0U);
  const auto [called, called_output] = compile_and_run(R"(
let Twice(x: Int64): Int64 = (x, 2) -> *;
((1, 2, 3), Twice) -> Std.Tuple.Map -> Std.Println;
)",
                                                       "bytecode_map_in_place_named_calls.fleaux", without_inlining());
  REQUIRE(called_output == "(2, 4, 6)\n");
  REQUIRE(count_opcode(called.instructions, fleaux::bytecode::Opcode::kCallUserFunc) == 1U);
}

TEST_CASE("Bytecode compiler calls Std.Tuple.Map for a function-valued argument", "[bytecode][in_place]") {
  const auto [bytecode_module, output] = compile_and_run(R"(
let MapWith(items: Tuple<Int64...>, f: (Int64) => Int64): Tuple<Int64...> = (items, f) -> Std.Tuple.Map;
((1, 2), (x: Int64): Int64 = (x, 1) -> +) -> MapWith -> Std.Println;
)",
                                                         "bytecode_map_function_value.fleaux");

  REQUIRE(output == "(2, 3)\n");
  REQUIRE(calls_builtin(compiled_function(bytecode_module, "MapWith").instructions, "Std.Tuple.Map"));
}

TEST_CASE("Std.Tuple.Map run in place nests with the other in-place forms", "[bytecode][in_place]") {
  // A map inside a map body that reads the outer item and a parameter, a Branch and a Loop inside map bodies, and a
  // map inside a Branch arm.
  const auto [bytecode_module, output] = compile_and_run(R"(
let Grid(n: Int64, scale: Int64): Tuple<Tuple<Int64...>...> =
  (
    n -> Std.Tuple.Range,
    (row: Int64): Tuple<Int64...> =
      (n -> Std.Tuple.Range, (column: Int64): Int64 = (((row, n) -> *, column) -> +, scale) -> *) -> Std.Tuple.Map
  ) -> Std.Tuple.Map;
let Signs(items: Tuple<Int64...>): Tuple<Int64...> =
  (items, (x: Int64): Int64 = ((x, 0) -> <, x, (k: Int64): Int64 = -1, (k: Int64): Int64 = 1) -> Std.Branch)
    -> Std.Tuple.Map;
let Counts(n: Int64): Tuple<Int64...> =
  (
    n -> Std.Tuple.Range,
    (limit: Int64): Int64 = (0, (i: Int64): Bool = (i, limit) -> <, (i: Int64): Int64 = (i, 1) -> +) -> Std.Loop
  ) -> Std.Tuple.Map;
let MaybeBump(bump: Bool, items: Tuple<Int64...>): Tuple<Int64...> =
  (
    bump,
    items,
    (all: Tuple<Int64...>): Tuple<Int64...> = (all, (x: Int64): Int64 = (x, 1) -> +) -> Std.Tuple.Map,
    (all: Tuple<Int64...>): Tuple<Int64...> = all
  ) -> Std.Branch;
(2, 10) -> Grid -> Std.Println;
((-3, 0, 5)) -> Signs -> Std.Println;
3 -> Counts -> Std.Println;
(True, (1, 2)) -> MaybeBump -> Std.Println;
)",
                                                         "bytecode_map_in_place_nesting.fleaux");

  REQUIRE(output == "((0, 10), (20, 30))\n(-1, 1, 1)\n(0, 1, 2)\n(2, 3)\n");
  for (const auto* name : {"Grid", "Signs", "Counts", "MaybeBump"}) {
    INFO(name);
    REQUIRE_FALSE(calls_builtin(compiled_function(bytecode_module, name).instructions, "Std.Tuple.Map"));
  }
}

TEST_CASE("An error in a Std.Tuple.Map body run in place stops the program", "[bytecode][in_place]") {
  const auto ir_program =
      lower_source_to_ir("((1, 0), (x: Int64): Int64 = (10, x) -> /) -> Std.Tuple.Map -> Std.Println;\n",
                         "bytecode_map_in_place_error.fleaux");
  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled = compiler.compile(ir_program);
  REQUIRE(compiled.has_value());
  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto executed = runtime.execute(*compiled, output);
  REQUIRE_FALSE(executed.has_value());
  REQUIRE(executed.error().message.find("division by zero") != std::string::npos);
  REQUIRE(output.str().empty());
}

TEST_CASE("Recursion through Std.Branch does not use the native stack", "[bytecode][in_place]") {
  // Each level used to call back into the interpreter from C++, so a recursion this deep overflowed the native
  // stack. Run in place, its frames are on the VM's own frame stack.
  const auto [bytecode_module, output] = compile_and_run(R"(
let Sum(n: Int64): Int64 =
  ((n, 0) -> ==, n, (k: Int64): Int64 = 0, (k: Int64): Int64 = ((k, 1) -> - -> Sum, k) -> +) -> Std.Branch;
(200000) -> Sum -> Std.Println;
)",
                                                         "bytecode_branch_deep_recursion.fleaux");

  REQUIRE(output == "20000100000\n");
}

TEST_CASE("Bytecode compiler runs Std.Match with constant patterns in place", "[bytecode][in_place]") {
  const auto [bytecode_module, output] = compile_and_run(R"(
let Name(n: Int64): String = (n, (0, (): String = "zero"), (1, (): String = "one"), (_, (): String = "many")) -> Std.Match;
(0) -> Name -> Std.Println;
(1) -> Name -> Std.Println;
(7) -> Name -> Std.Println;
)",
                                                         "bytecode_match_in_place_constants.fleaux");

  REQUIRE(output == "zero\none\nmany\n");
  REQUIRE(bytecode_module.closures.empty());
  const auto& name = compiled_function(bytecode_module, "Name").instructions;
  REQUIRE_FALSE(calls_builtin(name, "Std.Match"));
  REQUIRE(count_opcode(name, fleaux::bytecode::Opcode::kCmpEq) == 2U);
  REQUIRE(count_opcode(name, fleaux::bytecode::Opcode::kMakeClosureRef) == 0U);
  REQUIRE(count_opcode(name, fleaux::bytecode::Opcode::kMatchFailed) == 0U);
}

TEST_CASE("Std.Match run in place tests every pattern form", "[bytecode][in_place]") {
  // A named predicate, an inline predicate, and `_`, with handlers that take the subject inline and by name; then
  // String, Bool, and Float64 constants, the Bool match without `_`.
  const auto [bytecode_module, output] = compile_and_run(R"(
let IsEven(n: Int64): Bool = ((n, 2) -> Std.Mod, 0) -> ==;
let Negate(n: Int64): Int64 = (0, n) -> -;
let Describe(n: Int64): Int64 =
  (n,
   (IsEven, (k: Int64): Int64 = (k, 2) -> /),
   ((k: Int64): Bool = (k, 0) -> <, Negate),
   (_, (k: Int64): Int64 = (k, 100) -> +)
  ) -> Std.Match;
(8) -> Describe -> Std.Println;
(-3) -> Describe -> Std.Println;
(5) -> Describe -> Std.Println;
("b", ("a", (): Int64 = 1), ("b", (): Int64 = 2), (_, (): Int64 = 0)) -> Std.Match -> Std.Println;
(False, (True, (): String = "yes"), (False, (): String = "no")) -> Std.Match -> Std.Println;
(2.5, (2.5, (): String = "exact"), (_, (): String = "other")) -> Std.Match -> Std.Println;
)",
                                                         "bytecode_match_in_place_forms.fleaux");

  REQUIRE(output == "4\n3\n105\n2\nno\nexact\n");
  REQUIRE(bytecode_module.closures.empty());
  REQUIRE_FALSE(calls_builtin(compiled_function(bytecode_module, "Describe").instructions, "Std.Match"));
  REQUIRE_FALSE(calls_builtin(bytecode_module.instructions, "Std.Match"));
  REQUIRE(count_opcode(bytecode_module.instructions, fleaux::bytecode::Opcode::kMatchFailed) == 1U);
}

TEST_CASE("Std.Match run in place keeps closure scoping", "[bytecode][in_place]") {
  // Pick's first handler takes no parameter, captures nothing, and has a local of its own, while `n` and `before` are
  // still needed after the Match. Scale nests a Match in a handler whose cases read the outer handler's parameter and
  // the function's own.
  const auto [bytecode_module, output] = compile_and_run(R"(
let Pick(n: Int64): Int64 = {
  let before: Int64 = (n, 10) -> *;
  let chosen: Int64 = (n, (0, (): Int64 = { let t: Int64 = 7; (t, 1) -> +; }), (_, (): Int64 = 3)) -> Std.Match;
  ((before, chosen) -> +, n) -> +;
};
let Scale(n: Int64, factor: Int64): Int64 =
  (n,
   (0, (): Int64 = factor),
   (_, (k: Int64): Int64 =
         ((k, 0) -> <, (True, (): Int64 = (0, (k, factor) -> *) -> -), (_, (): Int64 = (k, factor) -> *)) -> Std.Match)
  ) -> Std.Match;
(0) -> Pick -> Std.Println;
(2) -> Pick -> Std.Println;
(0, 3) -> Scale -> Std.Println;
(-2, 3) -> Scale -> Std.Println;
(4, 3) -> Scale -> Std.Println;
)",
                                                         "bytecode_match_in_place_scoping.fleaux");

  REQUIRE(output == "8\n25\n3\n6\n12\n");
  REQUIRE(bytecode_module.closures.empty());
  for (const auto* name : {"Pick", "Scale"}) {
    INFO(name);
    REQUIRE_FALSE(calls_builtin(compiled_function(bytecode_module, name).instructions, "Std.Match"));
  }
}

TEST_CASE("A Std.Match run in place with no matching case stops the program", "[bytecode][in_place]") {
  const auto ir_program = lower_source_to_ir("(3, (1, (): Int64 = 10), (2, (): Int64 = 20)) -> Std.Match -> Std.Println;\n",
                                             "bytecode_match_in_place_no_case.fleaux");
  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled = compiler.compile(ir_program);
  REQUIRE(compiled.has_value());
  REQUIRE(count_opcode(compiled->instructions, fleaux::bytecode::Opcode::kMatchFailed) == 1U);
  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto executed = runtime.execute(*compiled, output);
  REQUIRE_FALSE(executed.has_value());
  REQUIRE(executed.error().message.find("no case matched and no wildcard case provided") != std::string::npos);
  REQUIRE(output.str().empty());
}

TEST_CASE("Recursion through Std.Match does not use the native stack", "[bytecode][in_place]") {
  const auto [bytecode_module, output] = compile_and_run(R"(
let Sum(n: Int64): Int64 = (n, (0, (): Int64 = 0), (_, (): Int64 = ((n, 1) -> - -> Sum, n) -> +)) -> Std.Match;
(200000) -> Sum -> Std.Println;
)",
                                                         "bytecode_match_deep_recursion.fleaux");

  REQUIRE(output == "20000100000\n");
}

TEST_CASE("Bytecode compiler calls Std.Match for patterns and handlers it cannot run in place", "[bytecode][in_place]") {
  // A handler that is a parameter, and a pattern that is a value let: either could hold a function or a value.
  const auto [bytecode_module, output] = compile_and_run(R"(
let Zero: Int64 = 0;
let Run(n: Int64, handler: (Int64) => Int64): Int64 = (n, (0, handler), (_, (k: Int64): Int64 = k)) -> Std.Match;
let IsZero(n: Int64): String = (n, (Zero, (): String = "zero"), (_, (): String = "other")) -> Std.Match;
(0, (k: Int64): Int64 = 99) -> Run -> Std.Println;
(5, (k: Int64): Int64 = 99) -> Run -> Std.Println;
(0) -> IsZero -> Std.Println;
(1) -> IsZero -> Std.Println;
)",
                                                         "bytecode_match_builtin_fallback.fleaux");

  REQUIRE(output == "99\n5\nzero\nother\n");
  REQUIRE(calls_builtin(compiled_function(bytecode_module, "Run").instructions, "Std.Match"));
  REQUIRE(calls_builtin(compiled_function(bytecode_module, "IsZero").instructions, "Std.Match"));
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
      // Passed to Std.Apply or Std.Tuple.Filter the closure would run in place; Std.Tuple.Any takes it as a value.
      lower_source_to_ir(
          "((10.0,), (x: Float64): Bool = (x, 1.0) -> Std.GreaterThan) -> Std.Tuple.Any -> Std.Println;\n",
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
    // The capture is the parameter's only read, so it moves the value out of the slot.
    if (ins.opcode == fleaux::bytecode::Opcode::kTakeLocal) {
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
    if (is_builtin_call(ins.opcode)) {
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
      // The imported function as a value. Passed to Std.Apply it would run in place as a direct call.
      fleaux::frontend::ir::IRExprStatement{
          .expr = make_ir_expr(fleaux::frontend::ir::IRTupleExpr{
              .items = {make_int_constant_box(5), make_name_ref_box(std::nullopt, "Identity")},
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
      ir_program, make_compile_options(std::nullopt, std::nullopt, std::nullopt, {imported_module}));

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
      compiler.compile(ir_program, make_compile_options(std::filesystem::path("main.fleaux")));
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
    if (is_builtin_call(ins.opcode)) {
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
  const auto result = compiler.compile(ir_program, without_inlining());

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

TEST_CASE("Calls with three or more arguments bind them from the stack", "[bytecode][calls]") {
  // The subtractions show the order the arguments arrive in.
  const auto [bytecode_module, output] = compile_and_run(R"(
let Pick3(a: Int64, b: Int64, c: Int64): Int64 = ((a, b) -> -, c) -> *;
let Pick5(a: Int64, b: Int64, c: Int64, d: Int64, e: Int64): Int64 = ((((a, b) -> -, c) -> -, d) -> -, e) -> -;
let Spread(rest: Tuple<Int64, Int64>): Int64 = (1, rest...) -> Pick3;
(10, 4, 3) -> Pick3 -> Std.Println;
(100, 1, 2, 3, 4) -> Pick5 -> Std.Println;
((2, 3)) -> Spread -> Std.Println;
)",
                                                         "bytecode_call_args.fleaux", without_inlining());

  REQUIRE(output == "18\n90\n-3\n");
  REQUIRE(count_opcode(bytecode_module.instructions, fleaux::bytecode::Opcode::kCallUserFuncArgs) == 2U);
  REQUIRE(count_opcode(bytecode_module.instructions, fleaux::bytecode::Opcode::kCallUserFunc) == 1U);
  for (const auto& instruction : bytecode_module.instructions) {
    if (instruction.opcode == fleaux::bytecode::Opcode::kBuildTuple) {
      REQUIRE(instruction.operand < 3);
    }
  }
}

TEST_CASE("Recursion through a five-parameter function binds each call's arguments", "[bytecode][calls]") {
  const auto [bytecode_module, output] = compile_and_run(R"(
let Count(n: Int64, a: Int64, b: Int64, c: Int64, total: Int64): Int64 =
  ((n, 0) -> ==, n, (k: Int64): Int64 = total,
   (k: Int64): Int64 = ((k, 1) -> -, a, (b, 1) -> +, c, ((total, a) -> +, c) -> -) -> Count) -> Std.Branch;
(100000, 3, 0, 1, 0) -> Count -> Std.Println;
)",
                                                         "bytecode_call_args_recursion.fleaux");

  REQUIRE(output == "200000\n");
  REQUIRE(count_opcode(compiled_function(bytecode_module, "Count").instructions,
                       fleaux::bytecode::Opcode::kCallUserFuncArgs) == 1U);
}

TEST_CASE("Std.Bit operations compile to their own opcodes", "[bytecode][builtins]") {
  const auto [bytecode_module, output] = compile_and_run(R"(
((12, 10) -> Std.Bit.And, (12, 10) -> Std.Bit.Or, (12, 10) -> Std.Bit.Xor, (1, 4u64) -> Std.Bit.ShiftLeft,
 (256, 4u64) -> Std.Bit.ShiftRight) -> Std.Println;
)",
                                                         "bytecode_bit_opcodes.fleaux");

  REQUIRE(output == "8 14 6 16 16\n");
  for (const auto opcode : {fleaux::bytecode::Opcode::kBitAnd, fleaux::bytecode::Opcode::kBitOr,
                            fleaux::bytecode::Opcode::kBitXor, fleaux::bytecode::Opcode::kShiftLeft,
                            fleaux::bytecode::Opcode::kShiftRight}) {
    INFO(fleaux::bytecode::stringify_opcode(opcode));
    REQUIRE(count_opcode(bytecode_module.instructions, opcode) == 1U);
  }
  for (const auto* name : {"Std.Bit.And", "Std.Bit.Or", "Std.Bit.Xor", "Std.Bit.ShiftLeft", "Std.Bit.ShiftRight"}) {
    INFO(name);
    REQUIRE_FALSE(calls_builtin(bytecode_module.instructions, name));
  }
}

TEST_CASE("String constants keep their value however often they run", "[bytecode][constants]") {
  // Map runs the constant once per item; the tasks run it on other threads.
  const auto [bytecode_module, output] = compile_and_run(R"(
let Label(n: Int64): String = "a label long enough to need its own buffer";
(3 -> Std.Tuple.Range, (i: Int64): String = "ab") -> Std.Tuple.Map -> Std.Println;
((Label, 1) -> Std.Task.Spawn, (Label, 2) -> Std.Task.Spawn,) -> Std.Task.AwaitAll -> Std.Println;
)",
                                                         "bytecode_string_constants.fleaux");

  REQUIRE(output == "(ab, ab, ab)\n(True, (a label long enough to need its own buffer, a label long enough to need its "
                    "own buffer))\n");
}

TEST_CASE("Calls of function values compile to kCallFunctionValue", "[bytecode][function_values]") {
  // Named functions, closures, and builtins as values, called with one argument and with two; then closures that
  // builtins call (Map with one argument, Reduce with two).
  const auto [bytecode_module, output] = compile_and_run(R"(
let Twice(f: (Int64) => Int64, x: Int64): Int64 = x -> f -> f;
let Combine(f: (Int64, Int64) => Int64, a: Int64, b: Int64): Int64 = (a, b) -> f;
let Inc(x: Int64): Int64 = (x, 1) -> +;
let Sub(a: Int64, b: Int64): Int64 = (a, b) -> -;
let MakeAdder(n: Int64): (Int64) => Int64 = (k: Int64): Int64 = (k, n) -> +;
let MakeScaledSub(s: Int64): (Int64, Int64) => Int64 = (a: Int64, b: Int64): Int64 = ((a, b) -> -, s) -> *;
(Inc, 5) -> Twice -> Std.Println;
((10) -> MakeAdder, 5) -> Twice -> Std.Println;
(Std.UnaryMinus<Int64>, 5) -> Twice -> Std.Println;
(Sub, 10, 4) -> Combine -> Std.Println;
((3) -> MakeScaledSub, 10, 4) -> Combine -> Std.Println;
(Std.Add, 10, 4) -> Combine -> Std.Println;
((1, 2, 3), (10) -> MakeAdder) -> Std.Tuple.Map -> Std.Println;
((1, 2, 3), 0, (2) -> MakeScaledSub) -> Std.Tuple.Reduce -> Std.Println;
)",
                                                         "bytecode_function_value_calls.fleaux");

  REQUIRE(output == "7\n25\n5\n6\n18\n14\n(11, 12, 13)\n-22\n");
  const auto& twice = compiled_function(bytecode_module, "Twice").instructions;
  REQUIRE(count_opcode(twice, fleaux::bytecode::Opcode::kCallFunctionValue) == 2U);
  REQUIRE_FALSE(calls_builtin(twice, "Std.Apply"));
  const auto& combine = compiled_function(bytecode_module, "Combine").instructions;
  REQUIRE(count_opcode(combine, fleaux::bytecode::Opcode::kCallFunctionValueArgs) == 1U);
  REQUIRE(count_opcode(combine, fleaux::bytecode::Opcode::kBuildTuple) == 0U);
  REQUIRE_FALSE(calls_builtin(combine, "Std.Apply"));
  for (const auto& instruction : combine) {
    if (instruction.opcode == fleaux::bytecode::Opcode::kCallFunctionValueArgs) {
      REQUIRE(instruction.operand == 2);
    }
  }
}

TEST_CASE("Std.Apply runs named functions and inline closures in place", "[bytecode][in_place]") {
  // A named function; closures of two, one, and no parameters that capture locals; and a closure whose second
  // argument runs a Std.Match in place, which uses the first free slot while the first argument waits on the stack.
  const auto [bytecode_module, output] = compile_and_run(R"(
let Inc(x: Int64): Int64 = (x, 1) -> +;
let Run(n: Int64): Int64 = {
  let base: Int64 = (n, 10) -> *;
  let a: Int64 = (n, Inc) -> Std.Apply;
  let b: Int64 = (base, n) -> (x: Int64, y: Int64): Int64 = ((x, y) -> -, base) -> +;
  let c: Int64 = n -> (x: Int64): Int64 = (x, a) -> *;
  let d: Int64 = ((), (): Int64 = { let t: Int64 = 7; (t, b) -> +; }) -> Std.Apply;
  let e: Int64 = ((n, 100) -> +, (n, (2, (): Int64 = 5), (_, (): Int64 = 6)) -> Std.Match) ->
      (x: Int64, y: Int64): Int64 = (x, y) -> -;
  ((((a, b) -> +, c) -> +, d) -> +, e) -> +;
};
(2) -> Run -> Std.Println;
)",
                                                         "bytecode_apply_in_place.fleaux");

  // a = 3, b = 38, c = 6, d = 45, e = 97.
  REQUIRE(output == "189\n");
  REQUIRE(bytecode_module.closures.empty());
  const auto& run = compiled_function(bytecode_module, "Run").instructions;
  REQUIRE_FALSE(calls_builtin(run, "Std.Apply"));
  REQUIRE(count_opcode(run, fleaux::bytecode::Opcode::kCallFunctionValue) == 0U);
  REQUIRE(count_opcode(run, fleaux::bytecode::Opcode::kCallFunctionValueArgs) == 0U);
  REQUIRE(count_opcode(run, fleaux::bytecode::Opcode::kCallUserFunc) == 1U);
}

TEST_CASE("Recursion through a function value does not use the native stack", "[bytecode][function_values]") {
  // Step is a value of function type, so each call through it is a call of a function value.
  const auto [bytecode_module, output] = compile_and_run(R"(
let Down(n: Int64): Int64 = (n, (0, (): Int64 = 0), (_, (): Int64 = (((n, 1) -> -) -> Step, 1) -> +)) -> Std.Match;
let Step: (Int64) => Int64 = Down;
(200000) -> Down -> Std.Println;
)",
                                                         "bytecode_function_value_deep_recursion.fleaux");

  REQUIRE(output == "200000\n");
  REQUIRE(count_opcode(compiled_function(bytecode_module, "Down").instructions,
                       fleaux::bytecode::Opcode::kCallFunctionValue) == 1U);
}

TEST_CASE("Variadic closures receive their extra arguments once", "[bytecode][function_values]") {
  // Each result counts the extra arguments (2), adds the first (1), and the closure adds what it captured (5).
  const auto [bytecode_module, output] = compile_and_run(R"(
let Use(f: (Int64, Int64...) => Int64): Int64 = (1, 2, 3) -> f;
let Make(n: Int64): (Int64, Int64...) => Int64 =
    (a: Int64, rest: Int64...): Int64 = ((rest -> Std.Length -> Std.ToInt64, a) -> +, n) -> +;
let Plain(a: Int64, rest: Int64...): Int64 = (rest -> Std.Length -> Std.ToInt64, a) -> +;
(5) -> Make -> Use -> Std.Println;
(Plain) -> Use -> Std.Println;
)",
                                                         "bytecode_variadic_closure_arguments.fleaux");

  REQUIRE(output == "8\n3\n");
}

TEST_CASE("An error inside a function value reads as from a direct call", "[bytecode][function_values]") {
  const auto ir_program = lower_source_to_ir(R"(
import Std;
let Fail(x: Int64): Int64 = (x, 0) -> /;
let Call(f: (Int64) => Int64): Int64 = 1 -> f;
(Fail) -> Call -> Std.Println;
)",
                                             "bytecode_function_value_error.fleaux");
  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled = compiler.compile(ir_program);
  REQUIRE(compiled.has_value());
  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto executed = runtime.execute(*compiled, output);
  REQUIRE_FALSE(executed.has_value());
  REQUIRE(executed.error().message == "native 'div' threw: Divide: Int64 division by zero");
}

TEST_CASE("Bytecode compiler emits kCallUserFuncArgs for direct binary user calls", "[bytecode]") {
  const auto ir_program = lower_source_to_ir(R"(
import Std;
let AddPair(a: Int64, b: Int64): Int64 = (a, b) -> Std.Add;
(7, 8) -> AddPair -> Std.Println;
)",
                                             "bytecode_user_func_binary.fleaux");

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto result = compiler.compile(ir_program, without_inlining());

  REQUIRE(result.has_value());
  REQUIRE(result->functions.size() == 1U);
  REQUIRE(result->functions[0].name == "AddPair#0");
  REQUIRE(result->functions[0].arity == 2U);

  bool found_binary_call = false;
  bool found_generic_call = false;
  bool found_pair_build = false;
  for (const auto& ins : result->instructions) {
    found_binary_call = found_binary_call || ins.opcode == fleaux::bytecode::Opcode::kCallUserFuncArgs;
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
  bool found_take_local = false;
  for (const auto& ins : compute_it->instructions) {
    found_store_local = found_store_local || ins.opcode == fleaux::bytecode::Opcode::kStoreLocal;
    // Each let is read once, so its one read moves the value out of the slot.
    found_take_local = found_take_local || ins.opcode == fleaux::bytecode::Opcode::kTakeLocal;
  }

  REQUIRE(found_store_local);
  REQUIRE(found_take_local);
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

TEST_CASE("Optimizer passes move jumps along with the code they target", "[bytecode][optimizer][jumps]") {
  using fleaux::bytecode::Opcode;
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.constants = {fleaux::bytecode::ConstValue{true}, fleaux::bytecode::ConstValue{std::int64_t{1}},
                               fleaux::bytecode::ConstValue{std::int64_t{2}}};

  SECTION("NoOpEliminationPass") {
    bytecode_module.instructions = {
        {.opcode = Opcode::kPushConst, .operand = 0}, {.opcode = Opcode::kJumpIfNot, .operand = 5},
        {.opcode = Opcode::kNoOp, .operand = 0},      {.opcode = Opcode::kPushConst, .operand = 1},
        {.opcode = Opcode::kJump, .operand = 7},      {.opcode = Opcode::kNoOp, .operand = 0},
        {.opcode = Opcode::kPushConst, .operand = 2}, {.opcode = Opcode::kHalt, .operand = 0},
    };
    fleaux::bytecode::NoOpEliminationPass{}.run(bytecode_module);

    const auto& instructions = bytecode_module.instructions;
    REQUIRE(instructions.size() == 6U);
    // The removed kNoOp a jump landed on is replaced by the instruction after it.
    REQUIRE(instructions[1].operand == 4);
    REQUIRE(instructions[4].opcode == Opcode::kPushConst);
    REQUIRE(instructions[3].operand == 5);
    REQUIRE(instructions[5].opcode == Opcode::kHalt);
  }

  SECTION("ConstantFoldingPass") {
    bytecode_module.instructions = {
        {.opcode = Opcode::kPushConst, .operand = 0}, {.opcode = Opcode::kJumpIfNot, .operand = 6},
        {.opcode = Opcode::kPushConst, .operand = 1}, {.opcode = Opcode::kPushConst, .operand = 2},
        {.opcode = Opcode::kAdd, .operand = 0},       {.opcode = Opcode::kJump, .operand = 7},
        {.opcode = Opcode::kPushConst, .operand = 1}, {.opcode = Opcode::kHalt, .operand = 0},
    };
    fleaux::bytecode::ConstantFoldingPass{}.run(bytecode_module);

    const auto& instructions = bytecode_module.instructions;
    REQUIRE(instructions.size() == 6U);
    REQUIRE(instructions[1].operand == 4);
    REQUIRE(instructions[3].operand == 5);
    REQUIRE(bytecode_module.constants[static_cast<std::size_t>(instructions[2].operand)].data ==
            fleaux::bytecode::ConstValue{std::int64_t{3}}.data);
  }
}

TEST_CASE("Optimizer passes leave instructions a jump lands among", "[bytecode][optimizer][jumps]") {
  using fleaux::bytecode::Opcode;
  fleaux::bytecode::Module bytecode_module;
  bytecode_module.constants = {fleaux::bytecode::ConstValue{true}, fleaux::bytecode::ConstValue{std::int64_t{1}},
                               fleaux::bytecode::ConstValue{std::int64_t{2}}};

  SECTION("DeadPushEliminationPass keeps a pop that another path jumps to") {
    // Both paths meet at the kPop: removing it with the push before it would leave the other path's value.
    bytecode_module.instructions = {
        {.opcode = Opcode::kPushConst, .operand = 0}, {.opcode = Opcode::kJumpIfNot, .operand = 4},
        {.opcode = Opcode::kPushConst, .operand = 1}, {.opcode = Opcode::kJump, .operand = 5},
        {.opcode = Opcode::kPushConst, .operand = 2}, {.opcode = Opcode::kPop, .operand = 0},
        {.opcode = Opcode::kHalt, .operand = 0},
    };
    const auto before = bytecode_module.instructions;
    fleaux::bytecode::DeadPushEliminationPass{}.run(bytecode_module);
    REQUIRE(bytecode_module.instructions == before);
  }

  SECTION("ConstantFoldingPass keeps an operation that another path jumps to") {
    bytecode_module.instructions = {
        {.opcode = Opcode::kPushConst, .operand = 0}, {.opcode = Opcode::kJumpIfNot, .operand = 5},
        {.opcode = Opcode::kPushConst, .operand = 1}, {.opcode = Opcode::kPushConst, .operand = 2},
        {.opcode = Opcode::kJump, .operand = 7},      {.opcode = Opcode::kPushConst, .operand = 2},
        {.opcode = Opcode::kPushConst, .operand = 1}, {.opcode = Opcode::kAdd, .operand = 0},
        {.opcode = Opcode::kHalt, .operand = 0},
    };
    const auto before = bytecode_module.instructions;
    fleaux::bytecode::ConstantFoldingPass{}.run(bytecode_module);
    REQUIRE(bytecode_module.instructions == before);
  }
}

TEST_CASE("LocalItemReadFusionPass reads items of local tuples in one instruction", "[bytecode][optimizer]") {
  using fleaux::bytecode::ConstValue;
  using fleaux::bytecode::local_item_operand;
  using fleaux::bytecode::Opcode;
  fleaux::bytecode::Module bytecode_module;
  // 0: UInt64 1, 1: Int64 1, 2: Float64 1.0, 3: UInt64 2^31, 4: the largest index that fits, 5: true.
  bytecode_module.constants = {ConstValue{std::uint64_t{1}},
                               ConstValue{std::int64_t{1}},
                               ConstValue{1.0},
                               ConstValue{std::uint64_t{fleaux::bytecode::kMaxLocalItemIndex} + 1U},
                               ConstValue{std::uint64_t{fleaux::bytecode::kMaxLocalItemIndex}},
                               ConstValue{true}};

  SECTION("A read and a last read with a UInt64 index") {
    bytecode_module.instructions = {
        {.opcode = Opcode::kLoadLocal, .operand = 3}, {.opcode = Opcode::kPushConst, .operand = 0},
        {.opcode = Opcode::kElementAt, .operand = 0}, {.opcode = Opcode::kTakeLocal, .operand = 3},
        {.opcode = Opcode::kPushConst, .operand = 4}, {.opcode = Opcode::kElementAt, .operand = 0},
        {.opcode = Opcode::kHalt, .operand = 0},
    };
    fleaux::bytecode::LocalItemReadFusionPass{}.run(bytecode_module);
    REQUIRE(bytecode_module.instructions ==
            std::vector<fleaux::bytecode::Instruction>{
                {.opcode = Opcode::kLoadLocalItem, .operand = local_item_operand(3U, 1U)},
                {.opcode = Opcode::kTakeLocalItem, .operand = local_item_operand(3U, fleaux::bytecode::kMaxLocalItemIndex)},
                {.opcode = Opcode::kHalt, .operand = 0},
            });
  }

  SECTION("Reads it cannot fuse stay as they are") {
    // An Int64 index, a Float64 index, an index too large for the operand, and a slot too large for it.
    bytecode_module.instructions = {
        {.opcode = Opcode::kLoadLocal, .operand = 0}, {.opcode = Opcode::kPushConst, .operand = 1},
        {.opcode = Opcode::kElementAt, .operand = 0}, {.opcode = Opcode::kLoadLocal, .operand = 0},
        {.opcode = Opcode::kPushConst, .operand = 2}, {.opcode = Opcode::kElementAt, .operand = 0},
        {.opcode = Opcode::kTakeLocal, .operand = 0}, {.opcode = Opcode::kPushConst, .operand = 3},
        {.opcode = Opcode::kElementAt, .operand = 0}, {.opcode = Opcode::kLoadLocal, .operand = std::int64_t{1} << 32},
        {.opcode = Opcode::kPushConst, .operand = 0}, {.opcode = Opcode::kElementAt, .operand = 0},
        {.opcode = Opcode::kHalt, .operand = 0},
    };
    const auto before = bytecode_module.instructions;
    fleaux::bytecode::LocalItemReadFusionPass{}.run(bytecode_module);
    REQUIRE(bytecode_module.instructions == before);
  }

  SECTION("A read that a jump lands inside stays as it is") {
    for (const std::int64_t target : {3, 4}) {
      INFO("jump target " << target);
      bytecode_module.instructions = {
          {.opcode = Opcode::kPushConst, .operand = 5}, {.opcode = Opcode::kJumpIfNot, .operand = target},
          {.opcode = Opcode::kLoadLocal, .operand = 0}, {.opcode = Opcode::kPushConst, .operand = 0},
          {.opcode = Opcode::kElementAt, .operand = 0}, {.opcode = Opcode::kHalt, .operand = 0},
      };
      const auto before = bytecode_module.instructions;
      fleaux::bytecode::LocalItemReadFusionPass{}.run(bytecode_module);
      REQUIRE(bytecode_module.instructions == before);
    }
  }

  SECTION("Jumps onto a read and past one keep their targets") {
    bytecode_module.instructions = {
        {.opcode = Opcode::kPushConst, .operand = 5}, {.opcode = Opcode::kJumpIfNot, .operand = 5},
        {.opcode = Opcode::kLoadLocal, .operand = 0}, {.opcode = Opcode::kPushConst, .operand = 0},
        {.opcode = Opcode::kElementAt, .operand = 0}, {.opcode = Opcode::kLoadLocal, .operand = 0},
        {.opcode = Opcode::kPushConst, .operand = 0}, {.opcode = Opcode::kElementAt, .operand = 0},
        {.opcode = Opcode::kJump, .operand = 9},      {.opcode = Opcode::kHalt, .operand = 0},
    };
    fleaux::bytecode::LocalItemReadFusionPass{}.run(bytecode_module);
    REQUIRE(bytecode_module.instructions == std::vector<fleaux::bytecode::Instruction>{
                                                {.opcode = Opcode::kPushConst, .operand = 5},
                                                {.opcode = Opcode::kJumpIfNot, .operand = 3},
                                                {.opcode = Opcode::kLoadLocalItem, .operand = local_item_operand(0U, 1U)},
                                                {.opcode = Opcode::kLoadLocalItem, .operand = local_item_operand(0U, 1U)},
                                                {.opcode = Opcode::kJump, .operand = 5},
                                                {.opcode = Opcode::kHalt, .operand = 0},
                                            });
  }
}

TEST_CASE("BytecodeOptimizer fuses local item reads in both modes", "[bytecode][optimizer]") {
  using fleaux::bytecode::ConstValue;
  using fleaux::bytecode::local_item_operand;
  using fleaux::bytecode::Opcode;
  using fleaux::bytecode::OptimizationMode;
  constexpr fleaux::bytecode::BytecodeOptimizer optimizer;

  for (const auto mode : {OptimizationMode::kBaseline, OptimizationMode::kExtended}) {
    INFO("extended mode " << (mode == OptimizationMode::kExtended));
    fleaux::bytecode::Module bytecode_module;
    bytecode_module.constants = {ConstValue{std::uint64_t{1}}};
    bytecode_module.instructions = {
        {.opcode = Opcode::kLoadLocal, .operand = 0},
        {.opcode = Opcode::kPushConst, .operand = 0},
        {.opcode = Opcode::kElementAt, .operand = 0},
        {.opcode = Opcode::kHalt, .operand = 0},
    };
    REQUIRE(optimizer.optimize(bytecode_module, fleaux::bytecode::OptimizerConfig{.mode = mode}).has_value());
    REQUIRE(bytecode_module.instructions == std::vector<fleaux::bytecode::Instruction>{
                                                {.opcode = Opcode::kLoadLocalItem, .operand = local_item_operand(0U, 1U)},
                                                {.opcode = Opcode::kHalt, .operand = 0},
                                            });
  }

  // Folding (1u64, 1u64) -> + makes the index the constant 2.
  fleaux::bytecode::Module folded;
  folded.constants = {ConstValue{std::uint64_t{1}}};
  folded.instructions = {
      {.opcode = Opcode::kLoadLocal, .operand = 0}, {.opcode = Opcode::kPushConst, .operand = 0},
      {.opcode = Opcode::kPushConst, .operand = 0}, {.opcode = Opcode::kAdd, .operand = 0},
      {.opcode = Opcode::kElementAt, .operand = 0}, {.opcode = Opcode::kHalt, .operand = 0},
  };
  REQUIRE(optimizer.optimize(folded, fleaux::bytecode::OptimizerConfig{.mode = OptimizationMode::kExtended}).has_value());
  REQUIRE(folded.instructions == std::vector<fleaux::bytecode::Instruction>{
                                     {.opcode = Opcode::kLoadLocalItem, .operand = local_item_operand(0U, 2U)},
                                     {.opcode = Opcode::kHalt, .operand = 0},
                                 });
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
      ir_program, make_compile_options(std::filesystem::path("bytecode_roundtrip.fleaux"),
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
  REQUIRE(loaded.error().span->source_name() == entry_path.string());
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
  REQUIRE(loaded.error().span->source_name() == entry_path.string());
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
  REQUIRE(loaded.error().hint->find("Add4 expects argument 1") != std::string::npos);
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
  REQUIRE(loaded.error().hint->find("Add4 expects argument 1") != std::string::npos);
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
  REQUIRE(loaded.error().hint->find("expects argument 1") != std::string::npos);
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
  REQUIRE(loaded.error().hint->find("Add4 expects argument 1") != std::string::npos);
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
    if (is_builtin_call(ins.opcode)) {
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

TEST_CASE("A native function compiles to kCallNative with a native function record", "[bytecode][native]") {
  const auto ir = lower_source_to_ir("let Ext.Add(a: Int64, b: Int64): Int64 :: __native__;\n"
                                     "(2, 3) -> Ext.Add;\n",
                                     "native_compile.fleaux");
  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled = compiler.compile(ir);
  REQUIRE(compiled.has_value());

  REQUIRE(compiled->native_functions.size() == 1);
  CHECK(compiled->native_functions[0].symbol == "Ext.Add");
  CHECK(compiled->native_functions[0].return_check.kind == fleaux::common::RuntimeTypeCheckKind::kInt64);
  const auto has_native_call = std::ranges::any_of(compiled->functions, [](const fleaux::bytecode::FunctionDef& fn) {
    return std::ranges::any_of(fn.instructions, [](const fleaux::bytecode::Instruction& instruction) {
      return instruction.opcode == fleaux::bytecode::Opcode::kCallNative && instruction.operand == 0;
    });
  });
  CHECK(has_native_call);

  const auto serialized = fleaux::bytecode::serialize_module(*compiled);
  REQUIRE(serialized.has_value());
  const auto deserialized = fleaux::bytecode::deserialize_module(*serialized);
  REQUIRE(deserialized.has_value());
  CHECK(deserialized->native_functions == compiled->native_functions);
}

namespace {

// The opcodes of a stream after the last-read pass.
auto opcodes_after_last_reads(std::vector<fleaux::bytecode::Instruction> instructions)
    -> std::vector<fleaux::bytecode::Opcode> {
  fleaux::bytecode::move_last_local_reads(instructions);
  std::vector<fleaux::bytecode::Opcode> opcodes;
  for (const auto& instruction : instructions) {
    opcodes.push_back(instruction.opcode);
  }
  return opcodes;
}

}  // namespace

TEST_CASE("The last read of a local slot moves its value", "[bytecode][liveness]") {
  using fleaux::bytecode::Opcode;

  SECTION("A slot read twice moves only at the second read") {
    REQUIRE(opcodes_after_last_reads({{Opcode::kLoadLocal, 0}, {Opcode::kPop, 0}, {Opcode::kLoadLocal, 0},
                                      {Opcode::kReturn, 0}}) ==
            std::vector{Opcode::kLoadLocal, Opcode::kPop, Opcode::kTakeLocal, Opcode::kReturn});
  }

  SECTION("A write ends the life of the value before it") {
    REQUIRE(opcodes_after_last_reads({{Opcode::kLoadLocal, 0}, {Opcode::kPushConst, 0}, {Opcode::kStoreLocal, 0},
                                      {Opcode::kLoadLocal, 0}, {Opcode::kReturn, 0}}) ==
            std::vector{Opcode::kTakeLocal, Opcode::kPushConst, Opcode::kStoreLocal, Opcode::kTakeLocal,
                        Opcode::kReturn});
  }

  SECTION("A read inside a loop copies while the loop can read the slot again") {
    REQUIRE(opcodes_after_last_reads({{Opcode::kLoadLocal, 0}, {Opcode::kPop, 0}, {Opcode::kLoadLocal, 1},
                                      {Opcode::kJumpIf, 0}, {Opcode::kLoadLocal, 0}, {Opcode::kReturn, 0}}) ==
            std::vector{Opcode::kLoadLocal, Opcode::kPop, Opcode::kLoadLocal, Opcode::kJumpIf, Opcode::kTakeLocal,
                        Opcode::kReturn});
  }

  SECTION("Each arm of a branch has its own last read") {
    REQUIRE(opcodes_after_last_reads({{Opcode::kLoadLocal, 1}, {Opcode::kJumpIfNot, 4}, {Opcode::kLoadLocal, 0},
                                      {Opcode::kJump, 5}, {Opcode::kLoadLocal, 0}, {Opcode::kReturn, 0}}) ==
            std::vector{Opcode::kTakeLocal, Opcode::kJumpIfNot, Opcode::kTakeLocal, Opcode::kJump, Opcode::kTakeLocal,
                        Opcode::kReturn});
    // A read after the arms join keeps the reads in the arms copies.
    REQUIRE(opcodes_after_last_reads({{Opcode::kLoadLocal, 1}, {Opcode::kJumpIfNot, 4}, {Opcode::kLoadLocal, 0},
                                      {Opcode::kJump, 5}, {Opcode::kLoadLocal, 0}, {Opcode::kLoadLocal, 0},
                                      {Opcode::kReturn, 0}}) ==
            std::vector{Opcode::kTakeLocal, Opcode::kJumpIfNot, Opcode::kLoadLocal, Opcode::kJump, Opcode::kLoadLocal,
                        Opcode::kTakeLocal, Opcode::kReturn});
  }

  SECTION("kIterate writes the item slot after its index, so the body's read of it is its last") {
    REQUIRE(opcodes_after_last_reads({{Opcode::kPushConst, 0}, {Opcode::kStoreLocal, 0}, {Opcode::kPushConst, 1},
                                      {Opcode::kStoreLocal, 1}, {Opcode::kJump, 6}, {Opcode::kLoadLocal, 2},
                                      {Opcode::kIterate, 0}, {Opcode::kJumpIf, 5}, {Opcode::kLoadLocal, 1},
                                      {Opcode::kReturn, 0}}) ==
            std::vector{Opcode::kPushConst, Opcode::kStoreLocal, Opcode::kPushConst, Opcode::kStoreLocal, Opcode::kJump,
                        Opcode::kTakeLocal, Opcode::kIterate, Opcode::kJumpIf, Opcode::kTakeLocal, Opcode::kReturn});
  }

  SECTION("kIterate reads its tuple slot and its index slot") {
    REQUIRE(opcodes_after_last_reads({{Opcode::kPushConst, 0}, {Opcode::kStoreLocal, 0}, {Opcode::kIterate, 0},
                                      {Opcode::kJumpIfNot, 6}, {Opcode::kLoadLocal, 0}, {Opcode::kJump, 2},
                                      {Opcode::kLoadLocal, 1}, {Opcode::kReturn, 0}}) ==
            std::vector{Opcode::kPushConst, Opcode::kStoreLocal, Opcode::kIterate, Opcode::kJumpIfNot,
                        Opcode::kLoadLocal, Opcode::kJump, Opcode::kTakeLocal, Opcode::kReturn});
  }

  SECTION("An item read reads the slot in the low bits of its operand") {
    // The operand of slot 0, item 1 is 2^32, not 0, so the read would be missed if it were taken as the slot.
    REQUIRE(opcodes_after_last_reads({{Opcode::kLoadLocal, 0},
                                      {Opcode::kPop, 0},
                                      {Opcode::kLoadLocalItem, fleaux::bytecode::local_item_operand(0U, 1U)},
                                      {Opcode::kReturn, 0}}) ==
            std::vector{Opcode::kLoadLocal, Opcode::kPop, Opcode::kLoadLocalItem, Opcode::kReturn});
    REQUIRE(opcodes_after_last_reads({{Opcode::kLoadLocal, 0},
                                      {Opcode::kPop, 0},
                                      {Opcode::kTakeLocalItem, fleaux::bytecode::local_item_operand(0U, 1U)},
                                      {Opcode::kReturn, 0}}) ==
            std::vector{Opcode::kLoadLocal, Opcode::kPop, Opcode::kTakeLocalItem, Opcode::kReturn});
  }

  SECTION("kMarkStack writes its slot and kBuildTupleSinceMark reads it") {
    REQUIRE(opcodes_after_last_reads({{Opcode::kLoadLocal, 0},
                                      {Opcode::kPop, 0},
                                      {Opcode::kMarkStack, 0},
                                      {Opcode::kBuildTupleSinceMark, 0},
                                      {Opcode::kReturn, 0}}) ==
            std::vector{Opcode::kTakeLocal, Opcode::kPop, Opcode::kMarkStack, Opcode::kBuildTupleSinceMark, Opcode::kReturn});
    REQUIRE(opcodes_after_last_reads({{Opcode::kMarkStack, 0},
                                      {Opcode::kLoadLocal, 0},
                                      {Opcode::kPop, 0},
                                      {Opcode::kBuildTupleSinceMark, 0},
                                      {Opcode::kReturn, 0}}) ==
            std::vector{Opcode::kMarkStack, Opcode::kLoadLocal, Opcode::kPop, Opcode::kBuildTupleSinceMark, Opcode::kReturn});
  }

  SECTION("A return ends the frame") {
    REQUIRE(opcodes_after_last_reads({{Opcode::kLoadLocal, 0}, {Opcode::kReturn, 0}, {Opcode::kLoadLocal, 0},
                                      {Opcode::kReturn, 0}}) ==
            std::vector{Opcode::kTakeLocal, Opcode::kReturn, Opcode::kTakeLocal, Opcode::kReturn});
  }
}

TEST_CASE("Updates leave a value that is read again as it was", "[bytecode][liveness][in_place]") {
  const auto [bytecode_module, output] = compile_and_run(R"(
// Every update reads `a` while a later read still needs it, so each one must leave `a` as it was.
let Updates(a: Tuple<Int64...>): Tuple<Int64...> = {
  ((a, 0u64, 9) -> Std.Array.SetAt) -> Std.Println;
  ((a, 1u64, 2u64, 7) -> Std.Array.Fill) -> Std.Println;
  ((a, 4) -> Std.Tuple.Append) -> Std.Println;
  ((a, 0) -> Std.Tuple.Prepend) -> Std.Println;
  ((a, 1u64, 5) -> Std.Array.InsertAt) -> Std.Println;
  ((a, 1u64) -> Std.Array.RemoveAt) -> Std.Println;
  ((a, a) -> Std.Array.Concat) -> Std.Println;
  ((a, 2u64) -> Std.Take) -> Std.Println;
  ((a, 1u64) -> Std.Drop) -> Std.Println;
  ((a, 0u64, 3u64, 2u64) -> Std.Slice) -> Std.Println;
  (a -> Std.Tuple.Sort) -> Std.Println;
  ((a, (x: Int64): Int64 = (x, 10) -> *) -> Std.Tuple.Map) -> Std.Println;
  a;
};

let Grid(g: Tuple<Tuple<Int64...>...>): Tuple<Tuple<Int64...>...> = {
  ((g, 0u64, 1u64, 8) -> Std.Array.SetAt2D) -> Std.Println;
  g;
};

let Pair(p: Tuple<Tuple<Int64...>, Int64>): Tuple<Tuple<Int64...>, Int64> = {
  ((p -> Std.First, 0u64, 6) -> Std.Array.SetAt) -> Std.Println;
  p;
};

(3, 1, 2,) -> Updates -> Std.Println;
((1, 2), (3, 4),) -> Grid -> Std.Println;
((1, 2), 5,) -> Pair -> Std.Println;
)",
                                                         "bytecode_updates_of_shared_values.fleaux");

  REQUIRE(output ==
          "(9, 1, 2)\n(3, 7, 7)\n(3, 1, 2, 4)\n(0, 3, 1, 2)\n(3, 5, 1, 2)\n(3, 2)\n(3, 1, 2, 3, 1, 2)\n(3, 1)\n"
          "(1, 2)\n(3, 2)\n(1, 2, 3)\n(30, 10, 20)\n(3, 1, 2)\n((1, 8), (3, 4))\n((1, 2), (3, 4))\n(6, 2)\n"
          "((1, 2), 5)\n");
  // Every read of the parameter but the last copies it.
  const auto& updates = compiled_function(bytecode_module, "Updates").instructions;
  REQUIRE(count_opcode(updates, fleaux::bytecode::Opcode::kTakeLocal) >= 1U);
  REQUIRE(count_opcode(updates, fleaux::bytecode::Opcode::kLoadLocal) >= 12U);
}

TEST_CASE("Updates take the only reference to a value", "[bytecode][liveness][in_place]") {
  const auto [bytecode_module, output] = compile_and_run(R"(
// Each update takes the only reference to the tuple before it.
let Chain(a: Tuple<Int64...>): Tuple<Int64...> = {
  let b: Tuple<Int64...> = (a, 0u64, 9) -> Std.Array.SetAt;
  let c: Tuple<Int64...> = (b, 1u64, 1u64, 7) -> Std.Array.Fill;
  let d: Tuple<Int64...> = (c, 4) -> Std.Tuple.Append;
  let e: Tuple<Int64...> = (d, 0) -> Std.Tuple.Prepend;
  let f: Tuple<Int64...> = (e, 1u64, 5) -> Std.Array.InsertAt;
  let g: Tuple<Int64...> = (f, 0u64) -> Std.Array.RemoveAt;
  let h: Tuple<Int64...> = (g, (1, 1)) -> Std.Array.Concat;
  let i: Tuple<Int64...> = (h, 6u64) -> Std.Take;
  let j: Tuple<Int64...> = (i, 1u64) -> Std.Drop;
  let k: Tuple<Int64...> = (j, 0u64, 5u64, 2u64) -> Std.Slice;
  k -> Std.Tuple.Sort;
};

let Rows(rows: Tuple<Tuple<Int64...>...>): Tuple<Tuple<Int64...>...> =
  (rows, (row: Tuple<Int64...>): Tuple<Int64...> = (row, 0u64, 0) -> Std.Array.SetAt) -> Std.Tuple.Map;

let Count(n: Int64): Int64 =
  ((n -> Std.Tuple.Range, 0 -> Std.Tuple.Range, (acc: Tuple<Int64...>, x: Int64): Tuple<Int64...> = (acc, x) -> Std.Tuple.Append)
    -> Std.Tuple.Reduce) -> Std.Length;

let Lookup(n: Int64): Int64 = {
  let built: Dict<Int64, Int64> =
    (n -> Std.Tuple.Range, () -> Std.Dict.Create, (d: Dict<Int64, Int64>, x: Int64): Dict<Int64, Int64> = (d, x, (x, x) -> *) -> Std.Dict.Set)
      -> Std.Tuple.Reduce;
  (built, 30) -> Std.Dict.Get;
};

(3, 1, 2,) -> Chain -> Std.Println;
((1, 2), (3, 4),) -> Rows -> Std.Println;
1000 -> Count -> Std.Println;
100 -> Lookup -> Std.Println;
)",
                                                         "bytecode_updates_in_place.fleaux");

  REQUIRE(output == "(1, 2, 9)\n((0, 2), (0, 4))\n1000\n900\n");
  // Each name in the chain is read once, so every read moves the value on.
  const auto& chain = compiled_function(bytecode_module, "Chain").instructions;
  REQUIRE(count_opcode(chain, fleaux::bytecode::Opcode::kLoadLocal) == 0U);
  REQUIRE(count_opcode(chain, fleaux::bytecode::Opcode::kTakeLocal) == 11U);
}

TEST_CASE("Printing writes every kind of value as before", "[bytecode][printing]") {
  // Values are written into a string, not a stream; the text is the same as the stream wrote, character for
  // character.
  const auto [bytecode_module, output] = compile_and_run(R"(
// Every kind of value, printed and turned into text.
let Show(v: Any): Any = {
  v -> Std.Println;
  v -> Std.ToString -> Std.Println;
};
let Inc(x: Int64): Int64 = (x, 1) -> +;

0 -> Show;
-1 -> Show;
9223372036854775807 -> Show;
((0, 9223372036854775807) -> -, 1) -> - -> Show;
18446744073709551615u64 -> Show;
1.0 -> Show;
0.1 -> Show;
-2.5 -> Show;
2.5e300 -> Show;
1e-300 -> Show;
(1.0, 0.0) -> / -> Show;
(-1.0, 0.0) -> / -> Show;
"text" -> Show;
"" -> Show;
(1, 1) -> == -> Show;
(1, 2) -> == -> Show;
(1, (2, 3),) -> Show;
((1,)) -> Show;
(()) -> Show;
((), ("a",),) -> Show;
() -> Std.Dict.Create -> (_, "a", 1) -> Std.Dict.Set -> (_, "b", 2) -> Std.Dict.Set -> Show;
Inc -> Show;
(1, "two", 3.5) -> Std.Println;
(", ", (1, "two", 3.5, (4, 5),)) -> Std.String.Join -> Std.Println;
("", ("a", "b", 1, 2.5,)) -> Std.String.Join -> Std.Println;
("-", ()) -> Std.String.Join -> Std.Println;
)",
                                                         "bytecode_printing.fleaux");

  REQUIRE(output ==
          "0\n0\n-1\n-1\n9223372036854775807\n9223372036854775807\n-9223372036854775808\n-9223372036854775808\n"
          "18446744073709551615\n18446744073709551615\n1\n1\n0.1\n0.1\n-2.5\n-2.5\n2.5e+300\n2.5e+300\n1e-300\n"
          "1e-300\ninf\ninf\n-inf\n-inf\ntext\ntext\n\n\nTrue\nTrue\nFalse\nFalse\n(1, (2, 3))\n(1, (2, 3))\n(1"
          ",)\n(1,)\n()\n()\n((), (a,))\n((), (a,))\n{a: 1, b: 2}\n{a: 1, b: 2}\n<function>\n<function>\n1 two "
          "3.5\n1, two, 3.5, (4, 5)\nab12.5\n\n");
}

TEST_CASE("Builtins take their arguments from the stack", "[bytecode][builtins][arguments]") {
  const auto [bytecode_module, output] = compile_and_run(R"(
// Builtins called with no arguments, several, a variadic list, a spread, and as function values.
let Pair: Tuple<String, String> = ("left", "right");
let Words: Tuple<Tuple<Int64...>...> = ((1, 2), (3,), (4, 5));
let Lookup(key: String): Int64 =
  (() -> Std.Dict.Create -> (_, "a", 1) -> Std.Dict.Set -> (_, "b", 2) -> Std.Dict.Set, key, 0) -> Std.Dict.GetDefault;

() -> Std.Dict.Create -> Std.Dict.Length -> Std.Println;
("a" -> Lookup, "b" -> Lookup, "c" -> Lookup) -> Std.Println;
("fleaux", "eau") -> Std.String.Contains -> Std.Println;
"plain" -> Std.String.Format -> Std.Println;
("{} and {}", 1, "two") -> Std.String.Format -> Std.Println;
("{} | {}", Pair...) -> Std.String.Format -> Std.Println;
(Words, (), Std.Array.Concat) -> Std.Tuple.Reduce -> Std.Println;
Words -> Std.Println;
((1, 2), (3, 4)) -> Std.Array.Concat -> Std.Println;
Std.Pi -> Std.Println;
)",
                                                         "bytecode_builtin_arguments.fleaux");

  REQUIRE(output == "0\n1 2 0\nTrue\nplain\n1 and two\nleft | right\n(1, 2, 3, 4, 5)\n((1, 2), (3,), (4, 5))\n(1, 2, 3, 4)\n3.141592653589793\n");

  // No argument tuple is built for the dict calls: Create takes none, Set and GetDefault take theirs from the stack.
  const auto& lookup = compiled_function(bytecode_module, "Lookup").instructions;
  REQUIRE(count_opcode(lookup, fleaux::bytecode::Opcode::kCallBuiltinArgs) == 4U);
  REQUIRE(count_opcode(lookup, fleaux::bytecode::Opcode::kBuildTuple) == 0U);
  std::vector<std::size_t> counts;
  for (const auto& instruction : lookup) {
    if (instruction.opcode == fleaux::bytecode::Opcode::kCallBuiltinArgs) {
      counts.push_back(fleaux::vm::builtin_argument_count_from_operand(instruction.operand));
    }
  }
  REQUIRE(counts == std::vector<std::size_t>{0U, 3U, 3U, 3U});

  // A spread builds the argument tuple, and Std.Println takes its tuple whole.
  REQUIRE(count_opcode(bytecode_module.instructions, fleaux::bytecode::Opcode::kConcatTuples) == 1U);
  REQUIRE(calls_builtin(bytecode_module.instructions, "Std.Println"));
  for (const auto& instruction : bytecode_module.instructions) {
    if (instruction.opcode == fleaux::bytecode::Opcode::kCallBuiltinArgs) {
      const auto builtin_id = fleaux::vm::builtin_id_from_operand(instruction.operand);
      REQUIRE(builtin_id.has_value());
      REQUIRE(fleaux::vm::builtin_takes_arguments(*builtin_id));
    }
  }
  REQUIRE(fleaux::bytecode::verify_module_builtin_callsites(bytecode_module).has_value());
}

TEST_CASE("An error in a builtin called with stack arguments reads as before", "[bytecode][builtins][arguments]") {
  const auto ir_program = lower_source_to_ir(R"(
import Std;
(() -> Std.Dict.Create -> (_, "a", 1) -> Std.Dict.Set, "missing") -> Std.Dict.Get -> Std.Println;
)",
                                             "bytecode_builtin_arguments_error.fleaux");
  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled = compiler.compile(ir_program);
  REQUIRE(compiled.has_value());
  REQUIRE(count_opcode(compiled->instructions, fleaux::bytecode::Opcode::kCallBuiltinArgs) == 3U);
  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto executed = runtime.execute(*compiled, output);
  REQUIRE_FALSE(executed.has_value());
  REQUIRE(executed.error().message == "builtin 'Std.Dict.Get' threw: DictGet: key not found");
}

TEST_CASE("The verifier refuses stack arguments for a builtin of one parameter", "[bytecode][verification][arguments]") {
  const auto [bytecode_module, output] = compile_and_run(R"(
("fleaux", "eau") -> Std.String.Contains -> Std.Println;
)",
                                                         "bytecode_builtin_arguments_verifier.fleaux");
  REQUIRE(output == "True\n");
  auto module = bytecode_module;
  const auto call = std::ranges::find_if(module.instructions, [](const fleaux::bytecode::Instruction& instruction) {
    return instruction.opcode == fleaux::bytecode::Opcode::kCallBuiltinArgs;
  });
  REQUIRE(call != module.instructions.end());
  REQUIRE(fleaux::bytecode::verify_module_builtin_callsites(module).has_value());

  // Std.String.Upper takes one value, not stack arguments.
  call->operand = fleaux::vm::builtin_operand_with_arguments(fleaux::vm::BuiltinId::StringUpper, 1U);
  for (auto& record : module.builtin_callsites) {
    if (record.builtin_id == fleaux::vm::BuiltinId::StringContains) {
      record.builtin_id = fleaux::vm::BuiltinId::StringUpper;
      record.static_argument_count = 1U;
    }
  }
  const auto verified = fleaux::bytecode::verify_module_builtin_callsites(module);
  REQUIRE_FALSE(verified.has_value());
  REQUIRE(verified.error().message.find("passes stack arguments to a builtin that takes one value") !=
          std::string::npos);
}

TEST_CASE("Failures reach Std.Try, tasks, and the parallel builtins as before", "[bytecode][try]") {
  // The VM's function values hand their failure over as a value (Callable::call_or_fail); the Err texts and item
  // indices are the ones the exceptions gave.
  const auto [bytecode_module, output] = compile_and_run(R"(
// Failures that Std.Try, tasks, and the parallel builtins turn into an Err.
let Half(x: Int64): Int64 = (x, 0) -> /;
let Pick(x: Int64): Int64 = (x, (1, (): Int64 = 2), (3, (): Int64 = 4)) -> Std.Match;
let Inner(x: Int64): Int64 = (x, Half) -> Std.Try -> Std.Result.IsOk -> (_, 1, 0) -> Std.Select;
let Outer(x: Int64): Int64 = (x, Inner) -> Std.Try -> Std.Result.Unwrap;
let Failing(x: Int64): Int64 = (x, 0) -> /;
let Sum(acc: Int64, x: Int64): Int64 = (acc, (x, (x, 3) -> -) -> /) -> +;

("oops", Std.String.ParseInt64) -> Std.Try -> Std.Println;
(7, Half) -> Std.Try -> Std.Println;
(5, Pick) -> Std.Try -> Std.Println;
(3, (x: Int64): Int64 = (x, 0) -> Std.Mod) -> Std.Try -> Std.Println;
(9, Outer) -> Std.Try -> Std.Println;
("8080", Std.String.ParseInt64) -> Std.Try -> Std.Println;
(Failing, 4) -> Std.Task.Spawn -> Std.Task.Await -> Std.Println;
((1, 2, 3), Failing) -> Std.Parallel.Map -> Std.Println;
((1, 2, 3), Failing) -> Std.Parallel.ForEach -> Std.Println;
((1, 2, 3, 4), 0, Sum) -> Std.Parallel.Reduce -> Std.Println;
)",
                                                         "bytecode_failing_calls.fleaux");

  REQUIRE(output ==
          "(False, ParseInt64: invalid numeric input)\n"
          "(False, Divide: Int64 division by zero)\n"
          "(False, Match: no case matched and no wildcard case provided)\n"
          "(False, Mod: Int64 division by zero)\n"
          "(True, 0)\n"
          "(True, 8080)\n"
          "(False, Divide: Int64 division by zero)\n"
          "(False, (0, Divide: Int64 division by zero))\n"
          "(False, (0, Divide: Int64 division by zero))\n"
          "(False, (2, Divide: Int64 division by zero))\n");
}

TEST_CASE("Calls return to their caller's frame and position while the frames grow", "[bytecode][loop]") {
  // The VM loop holds the running frame and its position, and enters the frame on top again after each call and
  // return. Std.Loop runs in place as a loop that calls More and Next and jumps back after each returns; Std.Tuple.Map
  // runs in place as a kIterate loop whose calls recurse deep enough to grow the frame storage; Count passes two
  // arguments (kCallUserFuncArgs).
  const auto [bytecode_module, output] = compile_and_run(R"(
let More(n: Int64): Bool = (n, 5000) -> <;
let Next(n: Int64): Int64 = (n, 1) -> +;
let Depth(n: Int64): Int64 = (n, (0, (): Int64 = 0), (_, (): Int64 = (((n, 1) -> -) -> Depth, 1) -> +)) -> Std.Match;
let Count(n: Int64, total: Int64): Int64 =
    (n, (0, (): Int64 = total), (_, (): Int64 = ((n, 1) -> -, (total, 1) -> +) -> Count)) -> Std.Match;
(0, More, Next) -> Std.Loop -> Std.Println;
((10, 3000, 20), Depth) -> Std.Tuple.Map -> Std.Println;
(3000, 0) -> Count -> Std.Println;
(4000) -> Depth -> Std.Println;
)",
                                                         "bytecode_loop_frames.fleaux", without_inlining());

  REQUIRE(output == "5000\n(10, 3000, 20)\n3000\n4000\n");
  REQUIRE(count_opcode(bytecode_module.instructions, fleaux::bytecode::Opcode::kJump) >= 2U);
  REQUIRE(count_opcode(bytecode_module.instructions, fleaux::bytecode::Opcode::kIterate) == 1U);
  REQUIRE(count_opcode(bytecode_module.instructions, fleaux::bytecode::Opcode::kCallUserFunc) >= 3U);
  REQUIRE(count_opcode(compiled_function(bytecode_module, "Count").instructions,
                       fleaux::bytecode::Opcode::kCallUserFuncArgs) == 1U);
}

TEST_CASE("An in-place Std.Tuple.Map is a loop tested at the bottom", "[bytecode][in_place][map]") {
  // kIterate stores each item in its slot and the loop jumps back from the bottom:
  //   Jump C  B: <func x>  C: Iterate s  JumpIf B
  const auto [bytecode_module, output] = compile_and_run(R"(
let Twice(x: Int64): Int64 = (x, 2) -> *;
let Run(items: Tuple<Int64...>): Tuple<Int64...> = (items, Twice) -> Std.Tuple.Map;
(1, 2, 3,) -> Run -> Std.Println;
)",
                                                         "bytecode_map_bottom_tested.fleaux");

  REQUIRE(output == "(2, 4, 6)\n");
  const auto& run = compiled_function(bytecode_module, "Run").instructions;
  REQUIRE(count_opcode(run, fleaux::bytecode::Opcode::kIterate) == 1U);
  REQUIRE(count_opcode(run, fleaux::bytecode::Opcode::kJumpIfNot) == 0U);
  const auto iterate = static_cast<std::size_t>(std::ranges::find_if(run, [](const auto& instruction) {
                                                  return instruction.opcode == fleaux::bytecode::Opcode::kIterate;
                                                }) -
                                                run.begin());
  REQUIRE(iterate + 1U < run.size());
  const auto& test = run[iterate + 1U];
  REQUIRE(test.opcode == fleaux::bytecode::Opcode::kJumpIf);
  const auto body = static_cast<std::size_t>(test.operand);
  REQUIRE(body > 0U);
  REQUIRE(body < iterate);
  // The jump into the loop, just before the body, goes to the test.
  REQUIRE(run[body - 1U].opcode == fleaux::bytecode::Opcode::kJump);
  REQUIRE(run[body - 1U].operand == static_cast<std::int64_t>(iterate));
}

TEST_CASE("In-place maps keep their results across the loop's cases", "[bytecode][in_place][map]") {
  // An empty source, one item, many; a source another value also holds (its items are copied, and it stays as it was);
  // a closure that reads a captured local on every item; nested maps; a function that recurses deeply enough to grow
  // the frame storage inside the body; and a map inside a Std.Loop.
  const auto [bytecode_module, output] = compile_and_run(R"(
let Twice(x: Int64): Int64 = (x, 2) -> *;
let Deep(n: Int64): Int64 = (n, (0, (): Int64 = 0), (_, (): Int64 = (((n, 1) -> -) -> Deep, 1) -> +)) -> Std.Match;
let Empty(): Tuple<Int64...> = (0) -> Std.Tuple.Range;
let Shared(items: Tuple<Int64...>): Tuple<Tuple<Int64...>, Tuple<Int64...>> = ((items, Twice) -> Std.Tuple.Map, items);
let Scaled(items: Tuple<Int64...>, k: Int64): Tuple<Int64...> = (items, (x: Int64): Int64 = (x, k) -> *) -> Std.Tuple.Map;
let Nested(rows: Tuple<Tuple<Int64...>...>): Tuple<Tuple<Int64...>...> =
    (rows, (row: Tuple<Int64...>): Tuple<Int64...> = (row, Twice) -> Std.Tuple.Map) -> Std.Tuple.Map;
let Grow(n: Int64): Int64 = (((n, n), Twice) -> Std.Tuple.Map -> Std.Length, n) -> +;
(() -> Empty, Twice) -> Std.Tuple.Map -> Std.Println;
((5,), Twice) -> Std.Tuple.Map -> Std.Println;
((1, 2, 3), Twice) -> Std.Tuple.Map -> Std.Println;
(1, 2, 3,) -> Shared -> Std.Println;
((1, 2, 3), 10) -> Scaled -> Std.Println;
((1, 2), (3, 4),) -> Nested -> Std.Println;
((3000, 10), Deep) -> Std.Tuple.Map -> Std.Println;
(0, (n: Int64): Bool = (n, 3) -> <, Grow) -> Std.Loop -> Std.Println;
)",
                                                         "bytecode_map_loop_cases.fleaux");

  REQUIRE(output ==
          "()\n"
          "(10,)\n"
          "(2, 4, 6)\n"
          "((2, 4, 6), (1, 2, 3))\n"
          "(10, 20, 30)\n"
          "((2, 4), (6, 8))\n"
          "(3000, 10)\n"
          "4\n");
}

TEST_CASE("An in-place Std.Tuple.Filter collects the kept items above a stack mark", "[bytecode][in_place][filter]") {
  //   MarkStack m  Jump C  B: <pred x>  JumpIfNot C  LoadLocal x  C: Iterate s  JumpIf B  BuildTupleSinceMark m
  const auto [bytecode_module, output] = compile_and_run(R"(
let Even(items: Tuple<Int64...>): Tuple<Int64...> = (items, (x: Int64): Bool = (x, 2) -> Std.Mod -> (_, 0) -> ==) -> Std.Tuple.Filter;
(1, 2, 3, 4,) -> Even -> Std.Println;
)",
                                                         "bytecode_filter_in_place.fleaux");

  REQUIRE(output == "(2, 4)\n");
  const auto& even = compiled_function(bytecode_module, "Even").instructions;
  REQUIRE_FALSE(calls_builtin(even, "Std.Tuple.Filter"));
  REQUIRE(count_opcode(even, fleaux::bytecode::Opcode::kIterate) == 1U);
  REQUIRE(count_opcode(even, fleaux::bytecode::Opcode::kMarkStack) == 1U);
  REQUIRE(count_opcode(even, fleaux::bytecode::Opcode::kBuildTupleSinceMark) == 1U);
  REQUIRE(count_opcode(even, fleaux::bytecode::Opcode::kBuildTupleCounted) == 0U);
  // The mark is in the slot below the loop's three.
  const auto mark =
      std::ranges::find_if(even, [](const auto& instruction) { return instruction.opcode == fleaux::bytecode::Opcode::kMarkStack; });
  const auto iterate =
      std::ranges::find_if(even, [](const auto& instruction) { return instruction.opcode == fleaux::bytecode::Opcode::kIterate; });
  REQUIRE(mark->operand + 1 == iterate->operand);
}

TEST_CASE("An in-place Std.Tuple.Reduce keeps its accumulator in a slot", "[bytecode][in_place][reduce]") {
  //   StoreLocal a  StoreLocal s ... Jump C  B: <func a x>  StoreLocal a  C: Iterate s  JumpIf B  LoadLocal a
  const auto [bytecode_module, output] = compile_and_run(R"(
let Sum(items: Tuple<Int64...>): Int64 = (items, 0, (acc: Int64, x: Int64): Int64 = (acc, x) -> +) -> Std.Tuple.Reduce;
(1, 2, 3, 4,) -> Sum -> Std.Println;
)",
                                                         "bytecode_reduce_in_place.fleaux");

  REQUIRE(output == "10\n");
  const auto& sum = compiled_function(bytecode_module, "Sum").instructions;
  REQUIRE_FALSE(calls_builtin(sum, "Std.Tuple.Reduce"));
  REQUIRE(count_opcode(sum, fleaux::bytecode::Opcode::kIterate) == 1U);
  REQUIRE(count_opcode(sum, fleaux::bytecode::Opcode::kMakeClosureRef) == 0U);
}

TEST_CASE("Std.Tuple.Filter and Std.Tuple.Reduce call the builtin with a named function", "[bytecode][in_place]") {
  // A named function run in place costs a kCallUserFunc per item, as much as the builtin's call of it.
  const auto [bytecode_module, output] = compile_and_run(R"(
let Odd(x: Int64): Bool = (x, 2) -> Std.Mod -> (_, 1) -> ==;
let Plus(acc: Int64, x: Int64): Int64 = (acc, x) -> +;
let Run(items: Tuple<Int64...>): Tuple<Tuple<Int64...>, Int64> = ((items, Odd) -> Std.Tuple.Filter, (items, 0, Plus) -> Std.Tuple.Reduce);
(1, 2, 3,) -> Run -> Std.Println;
)",
                                                         "bytecode_filter_reduce_named.fleaux");

  REQUIRE(output == "((1, 3), 6)\n");
  const auto& run = compiled_function(bytecode_module, "Run").instructions;
  REQUIRE(calls_builtin(run, "Std.Tuple.Filter"));
  REQUIRE(calls_builtin(run, "Std.Tuple.Reduce"));
  REQUIRE(count_opcode(run, fleaux::bytecode::Opcode::kIterate) == 0U);
}

TEST_CASE("In-place filters and reduces keep their results across the loop's cases", "[bytecode][in_place][filter]") {
  // Filter: an empty source, none kept, all kept, some kept, a source another value also holds (left as it was), a
  // captured local, and tuples as items. Reduce: an empty source, one item, many, a captured local, and a tuple
  // accumulator that grows on each item. Then each inside the other and inside a Map's closure, a predicate that
  // recurses deeply enough to grow the frame storage, and a filter inside a Std.Loop's step.
  const auto [bytecode_module, output] = compile_and_run(R"(
let Deep(n: Int64): Int64 = (n, (0, (): Int64 = 0), (_, (): Int64 = (((n, 1) -> -) -> Deep, 1) -> +)) -> Std.Match;
let Empty(): Tuple<Int64...> = (0) -> Std.Tuple.Range;
let Shared(items: Tuple<Int64...>): Tuple<Tuple<Int64...>, Tuple<Int64...>> =
    ((items, (x: Int64): Bool = (x, 1) -> >) -> Std.Tuple.Filter, items);
let Above(items: Tuple<Int64...>, k: Int64): Tuple<Int64...> = (items, (x: Int64): Bool = (x, k) -> >) -> Std.Tuple.Filter;
let Scaled(items: Tuple<Int64...>, k: Int64): Int64 =
    (items, 0, (acc: Int64, x: Int64): Int64 = (acc, (x, k) -> *) -> +) -> Std.Tuple.Reduce;
let RowSums(rows: Tuple<Tuple<Int64...>...>): Tuple<Int64...> =
    (rows, (row: Tuple<Int64...>): Int64 = (row, 0, (acc: Int64, x: Int64): Int64 = (acc, x) -> +) -> Std.Tuple.Reduce)
        -> Std.Tuple.Map;
let WithBig(rows: Tuple<Tuple<Int64...>...>): Tuple<Tuple<Int64...>...> =
    (rows, (row: Tuple<Int64...>): Bool = ((row, (x: Int64): Bool = (x, 2) -> >) -> Std.Tuple.Filter -> Std.Length, 0) -> >)
        -> Std.Tuple.Filter;
let Kept(rows: Tuple<Tuple<Int64...>...>): Int64 =
    (rows, 0, (acc: Int64, row: Tuple<Int64...>): Int64 =
        (acc, (row, (x: Int64): Bool = (x, 2) -> >) -> Std.Tuple.Filter -> Std.Length) -> +) -> Std.Tuple.Reduce;
let Step(n: Int64): Int64 = ((n -> Std.Tuple.Range, (x: Int64): Bool = (x, 0) -> >=) -> Std.Tuple.Filter -> Std.Length, 1) -> +;
(() -> Empty, (x: Int64): Bool = (x, 0) -> >) -> Std.Tuple.Filter -> Std.Println;
((1, 2, 3), (x: Int64): Bool = (x, 9) -> >) -> Std.Tuple.Filter -> Std.Println;
((1, 2, 3), (x: Int64): Bool = (x, 0) -> >) -> Std.Tuple.Filter -> Std.Println;
((1, 2, 3, 4), (x: Int64): Bool = (x, 2) -> Std.Mod -> (_, 0) -> ==) -> Std.Tuple.Filter -> Std.Println;
(1, 2, 3,) -> Shared -> Std.Println;
((1, 5, 10), 4) -> Above -> Std.Println;
(((1, 2), (3, 4), (5, 6)), (p: Tuple<Int64, Int64>): Bool = ((p, 0u64) -> Std.ElementAt, 1) -> >) -> Std.Tuple.Filter
    -> Std.Println;
(() -> Empty, 7, (acc: Int64, x: Int64): Int64 = (acc, x) -> +) -> Std.Tuple.Reduce -> Std.Println;
((5,), 1, (acc: Int64, x: Int64): Int64 = (acc, x) -> *) -> Std.Tuple.Reduce -> Std.Println;
((1, 2, 3, 4), 0, (acc: Int64, x: Int64): Int64 = (acc, x) -> +) -> Std.Tuple.Reduce -> Std.Println;
((1, 2, 3), 10) -> Scaled -> Std.Println;
(5 -> Std.Tuple.Range, 0 -> Std.Tuple.Range, (acc: Tuple<Int64...>, x: Int64): Tuple<Int64...> = (acc, (x, x) -> *) -> Std.Tuple.Append)
    -> Std.Tuple.Reduce -> Std.Println;
((1, 2), (3, 4),) -> RowSums -> Std.Println;
((1, 2), (3, 4), (0, 5),) -> WithBig -> Std.Println;
((1, 2), (3, 4), (0, 5),) -> Kept -> Std.Println;
((3000, 10), (x: Int64): Bool = (x -> Deep, 100) -> >) -> Std.Tuple.Filter -> Std.Println;
(0, (n: Int64): Bool = (n, 3) -> <, Step) -> Std.Loop -> Std.Println;
)",
                                                         "bytecode_filter_reduce_cases.fleaux");

  REQUIRE(output ==
          "()\n"
          "()\n"
          "(1, 2, 3)\n"
          "(2, 4)\n"
          "((2, 3), (1, 2, 3))\n"
          "(5, 10)\n"
          "((3, 4), (5, 6))\n"
          "7\n"
          "5\n"
          "10\n"
          "60\n"
          "(0, 1, 4, 9, 16)\n"
          "(3, 7)\n"
          "((3, 4), (0, 5))\n"
          "3\n"
          "(3000,)\n"
          "3\n");
  REQUIRE_FALSE(calls_builtin(bytecode_module.instructions, "Std.Tuple.Filter"));
  REQUIRE_FALSE(calls_builtin(bytecode_module.instructions, "Std.Tuple.Reduce"));
}

TEST_CASE("Bytecode of earlier versions is refused", "[bytecode][serialization]") {
  // Version 41's kIterate pushed the item, where version 42's stores it in its slot. Version 43 compiles constant
  // item reads of local tuples to kLoadLocalItem and kTakeLocalItem, version 44 runs Std.Tuple.Filter and
  // Std.Tuple.Reduce in place with kMarkStack and kBuildTupleSinceMark, and version 45 inlines small named functions.
  fleaux::bytecode::Module module;
  module.header.module_name = "OldVersion";
  module.instructions = {{fleaux::bytecode::Opcode::kHalt, 0}};
  const auto serialized = fleaux::bytecode::serialize_module(module);
  REQUIRE(serialized.has_value());
  REQUIRE(fleaux::bytecode::deserialize_module(*serialized).has_value());

  for (const std::uint32_t old_version : {41U, 42U, 43U, 44U}) {
    INFO("version " << old_version);
    // The version follows the four bytes of the magic number.
    std::vector<std::uint8_t> bytes = *serialized;
    std::memcpy(bytes.data() + sizeof(std::uint32_t), &old_version, sizeof old_version);
    const auto result = fleaux::bytecode::deserialize_module(bytes);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().message.find("Version") != std::string::npos);
  }
}

namespace {

// The module `source` compiles to after the optimizer's baseline passes, as the module loader makes it, and what it
// prints or the message it fails with.
auto compile_optimize_and_run(const std::string& source, const std::string& source_name)
    -> std::pair<fleaux::bytecode::Module, tl::expected<std::string, std::string>> {
  const auto ir_program = lower_source_to_ir(source, source_name);
  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  auto compiled = compiler.compile(ir_program);
  REQUIRE(compiled.has_value());
  constexpr fleaux::bytecode::BytecodeOptimizer optimizer;
  REQUIRE(optimizer.optimize(*compiled).has_value());
  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto executed = runtime.execute(*compiled, output);
  if (!executed.has_value()) {
    return {std::move(*compiled), tl::unexpected(executed.error().message)};
  }
  return {std::move(*compiled), output.str()};
}

}  // namespace

TEST_CASE("Constant item reads of local tuples read the item in one instruction", "[bytecode][optimizer][locals]") {
  using fleaux::bytecode::Opcode;
  // A tuple-state Std.Loop, a function's parameter read twice, and the closure of an in-place Std.Tuple.Map.
  const auto [bytecode_module, output] = compile_optimize_and_run(R"(
let Swap(p: Tuple<Int64, Int64>): Tuple<Int64, Int64> = ((p, 1u64) -> Std.ElementAt, (p, 0u64) -> Std.ElementAt);
((0, 0), (s: Tuple<Int64, Int64>): Bool = ((s, 0u64) -> Std.ElementAt, 10) -> <,
 (s: Tuple<Int64, Int64>): Tuple<Int64, Int64> =
     (((s, 0u64) -> Std.ElementAt, 1) -> +, ((s, 1u64) -> Std.ElementAt, (s, 0u64) -> Std.ElementAt) -> +)
) -> Std.Loop -> Std.Println;
(1, 2,) -> Swap -> Std.Println;
(((1, 2), (3, 4)), (row: Tuple<Int64, Int64>): Int64 = ((row, 0u64) -> Std.ElementAt, (row, 1u64) -> Std.ElementAt) -> *)
    -> Std.Tuple.Map -> Std.Println;
)",
                                                                  "bytecode_local_item_reads.fleaux");

  REQUIRE(output.has_value());
  REQUIRE(*output == "(10, 45)\n(2, 1)\n(2, 12)\n");
  // The loop and the map run in place in the top-level code: three reads that copy and a last one in the loop, and one
  // of each in the map's closure and in Swap's body, which the call inlines.
  const auto& top_level = bytecode_module.instructions;
  REQUIRE(count_opcode(top_level, Opcode::kElementAt) == 0U);
  REQUIRE(count_opcode(top_level, Opcode::kLoadLocalItem) == 5U);
  REQUIRE(count_opcode(top_level, Opcode::kTakeLocalItem) == 3U);
  const auto& swap = compiled_function(bytecode_module, "Swap").instructions;
  REQUIRE(count_opcode(swap, Opcode::kElementAt) == 0U);
  REQUIRE(count_opcode(swap, Opcode::kLoadLocalItem) == 1U);
  REQUIRE(count_opcode(swap, Opcode::kTakeLocalItem) == 1U);
}

TEST_CASE("A constant item read past the end of a local tuple fails as before", "[bytecode][optimizer][locals]") {
  const std::string source = R"(
let At(t: Tuple<Int64...>): Int64 = (t, 5u64) -> Std.ElementAt;
(10, 20,) -> At -> Std.Println;
)";
  const auto [optimized, optimized_run] = compile_optimize_and_run(source, "bytecode_local_item_range.fleaux");
  REQUIRE(count_opcode(compiled_function(optimized, "At").instructions, fleaux::bytecode::Opcode::kTakeLocalItem) == 1U);
  REQUIRE_FALSE(optimized_run.has_value());

  // The same program without the optimizer reads the item with kElementAt.
  const auto ir_program = lower_source_to_ir(source, "bytecode_local_item_range.fleaux");
  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto compiled = compiler.compile(ir_program);
  REQUIRE(compiled.has_value());
  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto executed = runtime.execute(*compiled, output);
  REQUIRE_FALSE(executed.has_value());
  REQUIRE(executed.error().message.find("ElementAt: index out of range") != std::string::npos);
  REQUIRE(optimized_run.error() == executed.error().message);
}

namespace {

// Whether `instructions` call the user function `name` (any overload) with kCallUserFunc or kCallUserFuncArgs.
auto calls_user_function(const fleaux::bytecode::Module& bytecode_module, const std::vector<fleaux::bytecode::Instruction>& instructions,
                         const std::string& name) -> bool {
  return std::ranges::any_of(instructions, [&](const fleaux::bytecode::Instruction& instruction) {
    if (instruction.opcode != fleaux::bytecode::Opcode::kCallUserFunc &&
        instruction.opcode != fleaux::bytecode::Opcode::kCallUserFuncArgs) {
      return false;
    }
    const auto& function = bytecode_module.functions[static_cast<std::size_t>(instruction.operand)];
    return function.name == name || function.name.starts_with(name + "#");
  });
}

auto user_call_count(const std::vector<fleaux::bytecode::Instruction>& instructions) -> std::size_t {
  return count_opcode(instructions, fleaux::bytecode::Opcode::kCallUserFunc) +
         count_opcode(instructions, fleaux::bytecode::Opcode::kCallUserFuncArgs);
}

}  // namespace

TEST_CASE("Calls of small named functions are inlined", "[bytecode][inline]") {
  // Every body is emitted in place of its call: in Quad, in Run, and in the top-level code, which inlines Run as well.
  // Sum3 takes its arguments one per parameter.
  const auto [bytecode_module, output] = compile_and_run(R"(
let Double(x: Int64): Int64 = (x, x) -> +;
let Sum3(a: Int64, b: Int64, c: Int64): Int64 = ((a, b) -> +, c) -> +;
let Quad(x: Int64): Int64 = (x -> Double) -> Double;
let Run(n: Int64): Int64 = ((n -> Quad, 1, 2) -> Sum3, n) -> -;
(5) -> Run -> Std.Println;
(3, 4, 5) -> Sum3 -> Std.Println;
)",
                                                         "bytecode_inline_calls.fleaux");

  REQUIRE(output == "18\n12\n");
  REQUIRE(user_call_count(bytecode_module.instructions) == 0U);
  REQUIRE(user_call_count(compiled_function(bytecode_module, "Run").instructions) == 0U);
  REQUIRE(user_call_count(compiled_function(bytecode_module, "Quad").instructions) == 0U);
}

TEST_CASE("Inlining stops three functions deep", "[bytecode][inline]") {
  // The top-level code inlines A, A's body B, and B's body C; C's call of D stays a call.
  const auto [bytecode_module, output] = compile_and_run(R"(
let D(x: Int64): Int64 = (x, 1) -> +;
let C(x: Int64): Int64 = (x -> D, 1) -> +;
let B(x: Int64): Int64 = (x -> C, 1) -> +;
let A(x: Int64): Int64 = (x -> B, 1) -> +;
(0) -> A -> Std.Println;
)",
                                                         "bytecode_inline_depth.fleaux");

  REQUIRE(output == "4\n");
  REQUIRE(user_call_count(bytecode_module.instructions) == 1U);
  REQUIRE(calls_user_function(bytecode_module, bytecode_module.instructions, "D"));
}

TEST_CASE("Calls that cannot be inlined stay calls", "[bytecode][inline]") {
  // A recursive function, two mutually recursive ones, a body over the size limit, a variadic function, a call with an
  // argument tuple, and a function passed as a value. A recursive function's first call is inlined; the call in its
  // body never is.
  const auto [bytecode_module, output] = compile_and_run(R"(
let Down(n: Int64): Int64 = (n, (0, (): Int64 = 0), (_, (): Int64 = ((n, 1) -> -) -> Down)) -> Std.Match;
let Even(n: Int64): Bool = (n, (0, (): Bool = True), (_, (): Bool = ((n, 1) -> -) -> Odd)) -> Std.Match;
let Odd(n: Int64): Bool = (n, (0, (): Bool = False), (_, (): Bool = ((n, 1) -> -) -> Even)) -> Std.Match;
let Big(x: Int64): Int64 =
    ((((((((((((x, 1) -> +, 2) -> +, 3) -> +, 4) -> +, 5) -> +, 6) -> +, 7) -> +, 8) -> +, 9) -> +, 10) -> +, 11) -> +, 12) -> +;
let Count(items: Int64...): Int64 = items -> Std.Length;
let Add2(a: Int64, b: Int64): Int64 = (a, b) -> +;
let FromPair(p: Tuple<Int64, Int64>): Int64 = (p...) -> Add2;
let Double(x: Int64): Int64 = (x, x) -> +;
let ApplyTwice(f: (Int64) => Int64, x: Int64): Int64 = (x -> f) -> f;
(3) -> Down -> Std.Println;
(10) -> Even -> Std.Println;
(0) -> Big -> Std.Println;
(1, 2, 3) -> Count -> Std.Println;
(4, 5,) -> FromPair -> Std.Println;
(Double, 3) -> ApplyTwice -> Std.Println;
)",
                                                         "bytecode_inline_not.fleaux");

  REQUIRE(output == "0\nTrue\n78\n3\n9\n12\n");
  REQUIRE(calls_user_function(bytecode_module, compiled_function(bytecode_module, "Down").instructions, "Down"));
  REQUIRE(calls_user_function(bytecode_module, bytecode_module.instructions, "Down"));
  REQUIRE(calls_user_function(bytecode_module, compiled_function(bytecode_module, "Even").instructions, "Even"));
  REQUIRE(calls_user_function(bytecode_module, bytecode_module.instructions, "Big"));
  REQUIRE(calls_user_function(bytecode_module, bytecode_module.instructions, "Count"));
  REQUIRE(calls_user_function(bytecode_module, bytecode_module.instructions, "Add2"));
  REQUIRE(count_opcode(bytecode_module.instructions, fleaux::bytecode::Opcode::kMakeUserFuncRef) == 1U);
}

TEST_CASE("Calls of generic functions with operator requirements stay calls", "[bytecode][inline]") {
  // Each call passes the operator functions for the type it binds as hidden arguments.
  const auto [bytecode_module, output] = compile_and_run(R"(
type Money = Int64;
let Money.LessThan(a: Money, b: Money): Bool = (a -> Std.Cast<Int64>, b -> Std.Cast<Int64>) -> <;
let M(x: Int64): Money = x -> Std.Cast<Money>;
alias Ordered = Int64 | Money;
let Smaller<T: Ordered>(a: T, b: T): T = ((a, b) -> <, a, b) -> Std.Select;
(5 -> M, 4 -> M) -> Smaller -> Std.Println;
(5, 4) -> Smaller -> Std.Println;
)",
                                                         "bytecode_inline_generic.fleaux");

  REQUIRE(output == "4\n4\n");
  REQUIRE(calls_user_function(bytecode_module, bytecode_module.instructions, "Smaller"));
}

TEST_CASE("Inlined calls behave as calls", "[bytecode][inline]") {
  // The arguments are evaluated in order before the body; a parameter read twice; a body with its own locals, an
  // in-place Map and Reduce, and a closure that captures a parameter; a call inside an in-place loop; and an inlined
  // body inside another.
  const auto [bytecode_module, output] = compile_and_run(R"(
let Diff(a: Int64, b: Int64): Int64 = (a, b) -> -;
let Square(x: Int64): Int64 = (x, x) -> *;
let SumSquares(n: Int64): Int64 = {
  let items: Tuple<Int64...> = n -> Std.Tuple.Range;
  let squares: Tuple<Int64...> = (items, (x: Int64): Int64 = x -> Square) -> Std.Tuple.Map;
  (squares, 0, (acc: Int64, x: Int64): Int64 = (acc, x) -> +) -> Std.Tuple.Reduce;
};
let Scaled(k: Int64): Tuple<Int64...> = ((1, 2, 3), (x: Int64): Int64 = (x, k) -> *) -> Std.Tuple.Map;
let Adder(k: Int64): (Int64) => Int64 = (x: Int64): Int64 = (x, k) -> +;
("first" -> Std.Println -> Std.Length, ("second", "x") -> Std.Println -> Std.Length) -> Diff -> Std.Println;
(4) -> SumSquares -> Std.Println;
(10) -> Scaled -> Std.Println;
(5, (3) -> Adder) -> Std.Apply -> Std.Println;
(1, (n: Int64): Bool = (n, 100) -> <, (n: Int64): Int64 = (n -> Square, 1) -> +) -> Std.Loop -> Std.Println;
)",
                                                         "bytecode_inline_behaviour.fleaux");

  REQUIRE(output == "first\nsecond x\n-1\n14\n(10, 20, 30)\n8\n677\n");
  REQUIRE(user_call_count(bytecode_module.instructions) == 0U);
}

TEST_CASE("A host call runs a function whose calls are inlined elsewhere", "[bytecode][inline][host_entry]") {
  const auto module = compile_host_module(
      "import Std;\n"
      "let App.Square(x: Int64): Int64 = (x, x) -> *;\n"
      "let App.Twice(x: Int64): Int64 = (x -> App.Square, 2) -> *;\n",
      "host_inline.fleaux");

  REQUIRE(user_call_count(compiled_function(module, "App.Twice").instructions) == 0U);
  REQUIRE(host_call(module, "App.Twice", fleaux::runtime::make_int(5)).value() == fleaux::runtime::make_int(50));
  REQUIRE(host_call(module, "App.Square", fleaux::runtime::make_int(4)).value() == fleaux::runtime::make_int(16));
}

TEST_CASE("Calls of imported functions are not inlined", "[bytecode][inline][loader]") {
  // An imported function's IR is in the module that defines it.
  const auto temp_dir = std::filesystem::temp_directory_path() / "fleaux_inline_import";
  std::filesystem::remove_all(temp_dir);
  std::filesystem::create_directories(temp_dir);
  {
    std::ofstream library(temp_dir / "inline_library.fleaux");
    library << "import Std;\nlet Triple(x: Int64): Int64 = (x, 3) -> *;\n";
    std::ofstream entry(temp_dir / "inline_entry.fleaux");
    entry << "import Std;\nimport inline_library;\n(4) -> Triple -> Std.Println;\n";
  }

  const auto loaded = fleaux::bytecode::load_linked_module(temp_dir / "inline_entry.fleaux");
  if (!loaded) {
    INFO(loaded.error().message);
  }
  REQUIRE(loaded.has_value());
  REQUIRE(calls_user_function(*loaded, loaded->instructions, "Triple"));
  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  REQUIRE(runtime.execute(*loaded, output).has_value());
  REQUIRE(output.str() == "12\n");
  std::filesystem::remove_all(temp_dir);
}

TEST_CASE("Bools that operations make are the same values as Bool constants", "[bytecode][values]") {
  // A Bool is stored as 0 or 1 in the whole payload; the constant True, a comparison's result and a builtin's are the
  // same value, and the same dict key.
  const auto [bytecode_module, output] = compile_and_run(R"(
((1, 2) -> <, (2, 1) -> <, ((1, 2) -> <, True) -> Std.And, True -> Std.Not, (1, 1) -> ==) -> Std.Println;
(((1, 2) -> <, True) -> ==, ((2, 1) -> <, False) -> ==, ((1, 2) -> <, False) -> ==) -> Std.Println;
((() -> Std.Dict.Create, (1, 2) -> <, "less") -> Std.Dict.Set, True) -> Std.Dict.Get -> Std.Println;
((1, 2, 3), (x: Int64): Bool = (x, 2) -> >) -> Std.Tuple.Any -> Std.ToString -> Std.Println;
)",
                                                         "bytecode_bool_values.fleaux");

  REQUIRE(output == "True False True False True\nTrue True False\nless\nTrue\n");
}
