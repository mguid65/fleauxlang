#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <ranges>
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
#include "fleaux/runtime/value.hpp"
#include "fleaux/vm/interpreter.hpp"
#include "fleaux/vm/runtime.hpp"

namespace {
enum class VmEngine {
  kBytecode,
  kInterpreter,
};

struct CliOptions {
  VmEngine engine = VmEngine::kBytecode;
  std::optional<std::filesystem::path> source;
  std::vector<std::string> process_args;
  bool no_run = false;
  bool all_samples = false;
  bool repl = false;
  bool show_help = false;
};

struct CliError {
  std::string message;
  std::optional<std::string> hint;
  std::optional<fleaux::frontend::diag::SourceSpan> span;
};

auto parse_engine(const std::string_view text) -> std::optional<VmEngine> {
  if (text == "bytecode") { return VmEngine::kBytecode; }
  if (text == "interpreter") { return VmEngine::kInterpreter; }
  return std::nullopt;
}

auto bytecode_runtime_hint_for(const std::string& runtime_message) -> std::optional<std::string> {
  if (constexpr std::string_view kNativeBuiltinGapPrefix = "builtin not implemented natively in bytecode VM:";
      runtime_message.starts_with(kNativeBuiltinGapPrefix)) {
    return "This run is strict bytecode-only. Try --mode interpreter for runtime fallback.";
  }
  return std::nullopt;
}

auto usage_text() -> std::string {
  return "usage: fleaux_vm_cli [--mode bytecode,interpreter] [--repl] [--all-samples] [--no-run] [file.fleaux] [-- "
         "<arg1> <arg2> ...]";
}

void print_help() {
  std::cout << usage_text() << '\n'
            << "\n"
            << "Options:\n"
            << "  -h, --help             Show this help message\n"
            << "  --mode <mode>          Execution mode (bytecode, interpreter)\n"
            << "  --engine <mode>        Alias for --mode\n"
            << "  --repl                 Start interactive interpreter REPL\n"
            << "  --all-samples          Run all .fleaux files under samples/\n"
            << "  --no-run               Skip execution and print what would run\n"
            << "\n"
            << "Notes:\n"
            << "  - Default mode is bytecode\n"
            << "  - REPL forces interpreter mode\n"
            << "  - If no source is provided and --all-samples is not set, defaults to test.fleaux\n"
            << "  - Arguments after '--' are forwarded to runtime entrypoints\n";
}

auto trim_copy(const std::string& text) -> std::string {
  const auto begin_it =
      std::ranges::find_if_not(text, [](const unsigned char ch) -> bool { return std::isspace(ch) != 0; });
  const auto end_it = std::ranges::find_if_not(std::views::reverse(text), [](const unsigned char ch) -> bool {
                        return std::isspace(ch) != 0;
                      }).base();
  if (begin_it >= end_it) { return {}; }
  return {begin_it, end_it};
}

auto buffer_has_complete_statement(const std::string& buffer) -> bool {
  int paren_depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (const char ch : buffer) {
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        in_string = false;
      }
      continue;
    }

    if (ch == '"') {
      in_string = true;
      continue;
    }
    if (ch == '(') {
      ++paren_depth;
      continue;
    }
    if (ch == ')') { --paren_depth; }
  }

  if (paren_depth != 0 || in_string) { return false; }
  return trim_copy(buffer).ends_with(';');
}

auto resolve_samples_dir(const std::filesystem::path& executable_path) -> std::optional<std::filesystem::path> {
  if (auto cwd_samples = std::filesystem::current_path() / "samples"; std::filesystem::is_directory(cwd_samples)) {
    return cwd_samples;
  }

  const auto exe = std::filesystem::weakly_canonical(executable_path);
  if (auto repo_samples = exe.parent_path().parent_path().parent_path() / "samples";
      std::filesystem::is_directory(repo_samples)) {
    return repo_samples;
  }

  return std::nullopt;
}

auto collect_sample_sources(const std::filesystem::path& samples_dir) -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> out;
  for (const auto& entry : std::filesystem::directory_iterator(samples_dir)) {
    if (!entry.is_regular_file()) continue;
    if (entry.path().extension() != ".fleaux") continue;
    out.push_back(entry.path());
  }
  std::ranges::sort(out);
  return out;
}

auto read_text_file(const std::filesystem::path& file) -> std::string {
  std::ifstream in(file);
  if (!in) { return {}; }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

// Single-file parse + lower (no import resolution)

auto parse_and_lower_single(const std::filesystem::path& source_file)
    -> tl::expected<fleaux::frontend::ir::IRProgram, CliError> {
  const auto source_text = read_text_file(source_file);
  if (source_text.empty()) {
    return tl::unexpected(CliError{
        .message = "Failed to read source file.",
        .hint = "Check the file path and ensure it is not empty.",
        .span = std::nullopt,
    });
  }

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(source_text, source_file.string());
  if (!parsed) {
    return tl::unexpected(CliError{
        .message = parsed.error().message,
        .hint = parsed.error().hint,
        .span = parsed.error().span,
    });
  }

  constexpr fleaux::frontend::lowering::Lowerer lowerer;
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

// Import resolution (mirrors interpreter's collect_program logic)

auto resolve_import_path(const std::filesystem::path& current, const std::string& module_name)
    -> std::filesystem::path {
  if (module_name == "Std" || module_name == "StdBuiltins") return {};

  if (const auto local = current.parent_path() / (module_name + ".fleaux"); std::filesystem::exists(local)) {
    return std::filesystem::weakly_canonical(local);
  }

  return {};
}

auto let_key(const std::optional<std::string>& qualifier, const std::string& name) -> std::string {
  return qualifier.has_value() ? (*qualifier + "." + name) : name;
}

auto collect_ir_program(const std::filesystem::path& source_file,
                        std::unordered_map<std::string, fleaux::frontend::ir::IRProgram>& cache,
                        std::unordered_set<std::string>& in_progress)
    -> tl::expected<fleaux::frontend::ir::IRProgram, CliError> {
  using namespace fleaux::frontend::ir;

  const std::string key = std::filesystem::weakly_canonical(source_file).string();
  if (cache.contains(key)) return cache.at(key);

  if (in_progress.contains(key)) {
    return tl::unexpected(CliError{
        .message = "Cyclic import detected.",
        .hint = "Break the cycle by moving shared definitions to a third module.",
        .span = std::nullopt,
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
  for (const auto& let : merged.lets) { seen.insert(let_key(let.qualifier, let.name)); }

  std::vector<IRLet> imported_lets;
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
      if (const auto sym = let_key(ilet.qualifier, ilet.name); seen.insert(sym).second) {
        imported_lets.push_back(ilet);
      }
    }
    imported_exprs.insert(imported_exprs.end(), imported->expressions.begin(), imported->expressions.end());
  }

  merged.lets.insert(merged.lets.begin(), imported_lets.begin(), imported_lets.end());
  merged.expressions.insert(merged.expressions.begin(), imported_exprs.begin(), imported_exprs.end());

  cache[key] = merged;
  in_progress.erase(key);
  return merged;
}

// collect_and_lower: entry point for the bytecode path

auto collect_and_lower(const std::filesystem::path& source_file)
    -> tl::expected<fleaux::frontend::ir::IRProgram, CliError> {
  std::unordered_map<std::string, fleaux::frontend::ir::IRProgram> cache;
  std::unordered_set<std::string> in_progress;
  return collect_ir_program(source_file, cache, in_progress);
}

// Bytecode execution

auto run_bytecode(const std::filesystem::path& source_file, const std::vector<std::string>& process_args)
    -> tl::expected<void, CliError> {
  auto lowered = collect_and_lower(source_file);
  if (!lowered) { return tl::unexpected(lowered.error()); }

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto bytecode = compiler.compile(lowered.value());
  if (!bytecode) {
    return tl::unexpected(CliError{
        .message = bytecode.error().message,
        .hint = "Switch to --mode interpreter for this program.",
        .span = std::nullopt,
    });
  }

  std::vector<std::string> args_storage;
  args_storage.reserve(process_args.size() + 1U);
  args_storage.push_back(source_file.string());
  args_storage.insert(args_storage.end(), process_args.begin(), process_args.end());

  std::vector<char*> argv_ptrs;
  argv_ptrs.reserve(args_storage.size());
  for (auto& arg : args_storage) { argv_ptrs.push_back(arg.data()); }
  fleaux::runtime::set_process_args(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());

  const fleaux::vm::Runtime runtime(fleaux::vm::RuntimeOptions{});
  if (const auto exec = runtime.execute(bytecode.value()); !exec) {
    return tl::unexpected(CliError{
        .message = exec.error().message,
        .hint = bytecode_runtime_hint_for(exec.error().message),
        .span = std::nullopt,
    });
  }

  return {};
}

auto print_diag_and_return(const std::string& stage, const CliError& error) -> int {
  std::cerr << fleaux::frontend::diag::format_diagnostic(stage, error.message, error.span, error.hint) << '\n';
  return 2;
}

auto run_interpreter_and_report(const std::filesystem::path& source, const std::vector<std::string>& process_args,
                                const std::string& stage) -> int {
  constexpr fleaux::vm::Interpreter interpreter;
  if (const auto result = interpreter.run_file(source, process_args); !result) {
    return print_diag_and_return(stage, CliError{
                                            .message = result.error().message,
                                            .hint = result.error().hint,
                                            .span = result.error().span,
                                        });
  }
  return 0;
}

auto run_repl_loop(const std::vector<std::string>& process_args) -> int {
  constexpr fleaux::vm::Interpreter interpreter;
  const auto session = interpreter.create_session(process_args);

  std::cout << "Fleaux interpreter REPL\n"
            << "Type :help for commands, :quit to exit.\n";

  std::string buffer;
  while (true) {
    std::cout << (buffer.empty() ? ">>> " : "... ");
    std::string line;
    if (!std::getline(std::cin, line)) {
      std::cout << '\n';
      break;
    }

    if (buffer.empty()) {
      const auto command = trim_copy(line);
      if (command == ":quit" || command == ":q") { break; }
      if (command == ":help") {
        std::cout << "Commands:\n"
                  << "  :help    Show this help\n"
                  << "  :quit    Exit REPL\n"
                  << "Notes:\n"
                  << "  - End snippets with ';'\n"
                  << "  - Multi-line input is supported until a complete statement\n";
        continue;
      }
      if (command.empty()) { continue; }
    }

    if (!buffer.empty()) { buffer.push_back('\n'); }
    buffer += line;

    if (!buffer_has_complete_statement(buffer)) { continue; }

    if (const auto result = session.run_snippet(buffer); !result) {
      std::cerr << fleaux::frontend::diag::format_diagnostic("vm-repl", result.error().message, result.error().span,
                                                             result.error().hint)
                << '\n';
    }
    buffer.clear();
  }

  return 0;
}

auto parse_cli_args(int argc, char** argv) -> tl::expected<CliOptions, CliError> {
  CliOptions options;

  bool runtime_args_mode = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view token = argv[i];
    if (!runtime_args_mode && token == "--") {
      runtime_args_mode = true;
      continue;
    }

    if (runtime_args_mode) {
      options.process_args.emplace_back(argv[i]);
      continue;
    }

    if (token == "-h" || token == "--help") {
      options.show_help = true;
      continue;
    }

    if (token == "--engine" || token == "--mode") {
      if (i + 1 >= argc) {
        return tl::unexpected(CliError{.message = std::string("missing value for ") + std::string(token),
                                       .hint = std::nullopt,
                                       .span = std::nullopt});
      }
      const std::string_view mode_token = argv[++i];
      const auto parsed_engine = parse_engine(mode_token);
      if (!parsed_engine.has_value()) {
        return tl::unexpected(CliError{
            .message = std::string("unknown mode value: ") + std::string(mode_token),
            .hint = "expected bytecode|interpreter",
            .span = std::nullopt,
        });
      }
      options.engine = parsed_engine.value();
      continue;
    }

    if (token == "--no-run") {
      options.no_run = true;
      continue;
    }

    if (token == "--repl") {
      options.repl = true;
      options.engine = VmEngine::kInterpreter;
      continue;
    }

    if (token == "--all-samples") {
      options.all_samples = true;
      continue;
    }

    if (!token.empty() && token[0] == '-') {
      return tl::unexpected(CliError{
          .message = std::string("unknown option: ") + std::string(token),
          .hint = "use --help to list supported options",
          .span = std::nullopt,
      });
    }

    if (!options.source.has_value()) {
      options.source = std::filesystem::path(token);
      continue;
    }

    return tl::unexpected(CliError{
        .message = std::string("unexpected extra positional argument: ") + std::string(token),
        .hint = "use --help for usage",
        .span = std::nullopt,
    });
  }

  if (options.repl) {
    if (options.all_samples) {
      return tl::unexpected(CliError{
          .message = "--repl cannot be used with --all-samples",
          .hint = "Choose interactive mode or batch sample execution.",
          .span = std::nullopt,
      });
    }
  } else if (!options.all_samples && !options.source.has_value()) {
    options.source = std::filesystem::path("test.fleaux");
  }

  return options;
}

auto run_engine_for_source(const VmEngine engine, const std::filesystem::path& source,
                           const std::vector<std::string>& process_args) -> int {
  if (engine == VmEngine::kInterpreter) {
    return run_interpreter_and_report(source, process_args, "vm-run-interpreter");
  }

  if (const auto result = run_bytecode(source, process_args); !result) {
    return print_diag_and_return("vm-run-bytecode", result.error());
  }
  return 0;
}
}  // namespace

auto main(int argc, char** argv) -> int {
  auto parsed = parse_cli_args(argc, argv);
  if (!parsed) {
    std::cerr << usage_text() << '\n';
    std::cerr << parsed.error().message;
    if (parsed.error().hint.has_value()) { std::cerr << " (" << *parsed.error().hint << ")"; }
    std::cerr << '\n';
    return 1;
  }

  const auto& [engine, source_path, process_args, no_run, all_samples, repl, show_help] = *parsed;
  if (show_help) {
    print_help();
    return 0;
  }

  if (repl) {
    if (no_run) {
      std::cout << "[interpreter] skipped run (--no-run): <repl>\n";
      return 0;
    }
    return run_repl_loop(process_args);
  }

  std::vector<std::filesystem::path> sources;
  if (all_samples) {
    const auto samples_dir = resolve_samples_dir(argv[0]);
    if (!samples_dir.has_value()) {
      std::cerr << "samples directory not found\n";
      return 2;
    }
    sources = collect_sample_sources(*samples_dir);
  } else {
    sources.push_back(*source_path);
  }

  for (const auto& source : sources) {
    if (no_run) {
      const std::string mode_name = (engine == VmEngine::kInterpreter) ? "interpreter" : "bytecode";
      std::cout << '[' << mode_name << "] skipped run (--no-run): " << source << '\n';
      continue;
    }

    if (const int rc = run_engine_for_source(engine, source, process_args); rc != 0) { return rc; }
  }

  return 0;
}
