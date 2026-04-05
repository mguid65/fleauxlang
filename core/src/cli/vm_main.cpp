#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "fleaux/bytecode/compiler.hpp"
#include "fleaux/frontend/ast.hpp"
#include "fleaux/frontend/diagnostics.hpp"
#include "fleaux/frontend/lowering.hpp"
#include "fleaux/frontend/parser.hpp"
#include "fleaux/vm/interpreter.hpp"
#include "fleaux/vm/runtime.hpp"

namespace {

enum class VmEngine {
  kHybrid,
  kBytecode,
  kInterpreter,
};

struct CliError {
  std::string message;
  std::optional<std::string> hint;
  std::optional<fleaux::frontend::diag::SourceSpan> span;
};

std::optional<VmEngine> parse_engine(const std::string_view text) {
  if (text == "hybrid") {
    return VmEngine::kHybrid;
  }
  if (text == "bytecode") {
    return VmEngine::kBytecode;
  }
  if (text == "interpreter") {
    return VmEngine::kInterpreter;
  }
  return std::nullopt;
}

std::string read_text_file(const std::filesystem::path& file) {
  std::ifstream in(file);
  if (!in) {
    return {};
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// ── Single-file parse + lower (no import resolution) ─────────────────────────

tl::expected<fleaux::frontend::ir::IRProgram, CliError> parse_and_lower_single(
    const std::filesystem::path& source_file) {
  const auto source_text = read_text_file(source_file);
  if (source_text.empty()) {
    return tl::unexpected(CliError{
        .message = "Failed to read source file.",
        .hint = "Check the file path and ensure it is not empty.",
        .span = std::nullopt,
    });
  }

  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(source_text, source_file.string());
  if (!parsed) {
    return tl::unexpected(CliError{
        .message = parsed.error().message,
        .hint = parsed.error().hint,
        .span = parsed.error().span,
    });
  }

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  if (!lowered) {
    return tl::unexpected(CliError{
        .message = lowered.error().message,
        .hint = lowered.error().hint,
        .span = lowered.error().span,
    });
  }

  return lowered.value();
}

// ── Import resolution (mirrors interpreter's collect_program logic) ───────────

std::filesystem::path resolve_import_path(const std::filesystem::path& current,
                                          const std::string& module_name) {
  if (module_name == "StdBuiltins") return {};

  if (const auto local = current.parent_path() / (module_name + ".fleaux");
      std::filesystem::exists(local)) {
    return std::filesystem::weakly_canonical(local);
  }

  if (module_name == "Std") {
    if (const auto ws_std =
            current.parent_path().parent_path() / "Std.fleaux";
        std::filesystem::exists(ws_std)) {
      return std::filesystem::weakly_canonical(ws_std);
    }
  }

  return {};
}

std::string let_key(const std::optional<std::string>& qualifier,
                    const std::string& name) {
  return qualifier.has_value() ? (*qualifier + "." + name) : name;
}

tl::expected<fleaux::frontend::ir::IRProgram, CliError> collect_ir_program(
    const std::filesystem::path& source_file,
    std::unordered_map<std::string, fleaux::frontend::ir::IRProgram>& cache,
    std::unordered_set<std::string>& in_progress) {
  using namespace fleaux::frontend::ir;

  const std::string key = std::filesystem::weakly_canonical(source_file).string();
  if (cache.count(key)) return cache.at(key);

  if (in_progress.count(key)) {
    return tl::unexpected(CliError{
        .message = "Cyclic import detected.",
        .hint    = "Break the cycle by moving shared definitions to a third module.",
        .span    = std::nullopt,
    });
  }

  in_progress.insert(key);

  auto current = parse_and_lower_single(source_file);
  if (!current) {
    in_progress.erase(key);
    return tl::unexpected(current.error());
  }

  IRProgram merged = current.value();
  std::unordered_set<std::string> seen;
  for (const auto& let : merged.lets) {
    seen.insert(let_key(let.qualifier, let.name));
  }

  std::vector<IRLet>           imported_lets;
  std::vector<IRExprStatement> imported_exprs;

  for (const auto& [module_name, span] : current->imports) {
    const auto import_path = resolve_import_path(source_file, module_name);
    if (import_path.empty()) continue;

    auto imported = collect_ir_program(import_path, cache, in_progress);
    if (!imported) {
      in_progress.erase(key);
      return tl::unexpected(imported.error());
    }

    for (const auto& ilet : imported->lets) {
      if (const auto sym = let_key(ilet.qualifier, ilet.name);
          seen.insert(sym).second) {
        imported_lets.push_back(ilet);
      }
    }
    imported_exprs.insert(imported_exprs.end(),
                          imported->expressions.begin(),
                          imported->expressions.end());
  }

  merged.lets.insert(merged.lets.begin(),
                     imported_lets.begin(), imported_lets.end());
  merged.expressions.insert(merged.expressions.begin(),
                             imported_exprs.begin(), imported_exprs.end());

  cache[key] = merged;
  in_progress.erase(key);
  return merged;
}

// ── collect_and_lower: entry point for the bytecode path ─────────────────────

tl::expected<fleaux::frontend::ir::IRProgram, CliError> collect_and_lower(
    const std::filesystem::path& source_file) {
  std::unordered_map<std::string, fleaux::frontend::ir::IRProgram> cache;
  std::unordered_set<std::string> in_progress;
  return collect_ir_program(source_file, cache, in_progress);
}

// ── Bytecode execution ────────────────────────────────────────────────────────

tl::expected<void, CliError> run_bytecode(const std::filesystem::path& source_file) {
  auto lowered = collect_and_lower(source_file);
  if (!lowered) {
    return tl::unexpected(lowered.error());
  }

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto bytecode = compiler.compile(lowered.value());
  if (!bytecode) {
    return tl::unexpected(CliError{
        .message = bytecode.error().message,
        .hint = "Switch to --engine interpreter for now, or use --engine hybrid.",
        .span = std::nullopt,
    });
  }

  const fleaux::vm::Runtime runtime;
  const auto exec = runtime.execute(bytecode.value());
  if (!exec) {
    return tl::unexpected(CliError{
        .message = exec.error().message,
        .hint = "Bytecode runtime does not yet support this behavior.",
        .span = std::nullopt,
    });
  }

  return {};
}

int print_diag_and_return(const std::string& stage, const CliError& error) {
  std::cerr << fleaux::frontend::diag::format_diagnostic(stage, error.message, error.span, error.hint)
            << '\n';
  return 2;
}

int run_interpreter_and_report(const std::filesystem::path& source,
                               const std::vector<std::string>& process_args,
                               const std::string& stage) {
  const fleaux::vm::Interpreter interpreter;
  const auto result = interpreter.run_file(source, process_args);
  if (!result) {
    return print_diag_and_return(stage,
                                 CliError{
                                     .message = result.error().message,
                                     .hint = result.error().hint,
                                     .span = result.error().span,
                                 });
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: fleaux_vm_cli [--engine hybrid|bytecode|interpreter] <file.fleaux> [-- <arg1> <arg2> ...]\n";
    return 1;
  }

  VmEngine engine = VmEngine::kHybrid;
  std::optional<std::filesystem::path> source;
  std::vector<std::string> process_args;

  bool runtime_args_mode = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view token = argv[i];
    if (!runtime_args_mode && token == "--") {
      runtime_args_mode = true;
      continue;
    }

    if (!runtime_args_mode && token == "--engine") {
      if (i + 1 >= argc) {
        std::cerr << "missing value for --engine\n";
        return 1;
      }
      const std::string_view mode_token = argv[++i];
      const auto parsed_engine = parse_engine(mode_token);
      if (!parsed_engine.has_value()) {
        std::cerr << "unknown --engine value: " << mode_token
                  << " (expected hybrid|bytecode|interpreter)\n";
        return 1;
      }
      engine = parsed_engine.value();
      continue;
    }

    if (!runtime_args_mode && !source.has_value()) {
      source = std::filesystem::path(token);
      continue;
    }

    process_args.emplace_back(argv[i]);
  }

  if (!source.has_value()) {
    std::cerr << "usage: fleaux_vm_cli [--engine hybrid|bytecode|interpreter] <file.fleaux> [-- <arg1> <arg2> ...]\n";
    return 1;
  }

  if (engine == VmEngine::kInterpreter) {
    return run_interpreter_and_report(source.value(), process_args, "vm-run-interpreter");
  }

  if (engine == VmEngine::kBytecode) {
    const auto result = run_bytecode(source.value());
    if (!result) {
      return print_diag_and_return("vm-run-bytecode", result.error());
    }
    return 0;
  }

  const auto bytecode_result = run_bytecode(source.value());
  if (bytecode_result) {
    return 0;
  }

  std::cerr << "[vm] bytecode path unavailable, falling back to interpreter: "
            << bytecode_result.error().message << '\n';
  return run_interpreter_and_report(source.value(), process_args, "vm-run-interpreter");
}
