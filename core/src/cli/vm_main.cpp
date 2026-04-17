#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "fleaux/bytecode/compiler.hpp"
#include "fleaux/frontend/diagnostics.hpp"
#include "fleaux/frontend/source_loader.hpp"
#include "fleaux/runtime/value.hpp"
#include "fleaux/vm/interpreter.hpp"
#include "fleaux/vm/runtime.hpp"

namespace {
enum class VmEngine {
  kVm,
  kInterpreter,
};

struct CliOptions {
  VmEngine engine = VmEngine::kVm;
  std::optional<std::filesystem::path> source;
  std::vector<std::string> process_args;
  bool no_run = false;
  bool repl = false;
  bool show_help = false;
};

struct CliError {
  std::string message;
  std::optional<std::string> hint;
  std::optional<fleaux::frontend::diag::SourceSpan> span;
};

auto parse_engine(const std::string_view text) -> std::optional<VmEngine> {
  if (text == "vm") { return VmEngine::kVm; }
  if (text == "interpreter") { return VmEngine::kInterpreter; }
  return std::nullopt;
}

auto vm_runtime_hint_for(const std::string& runtime_message) -> std::optional<std::string> {
  if (constexpr std::string_view kNativeBuiltinGapPrefix = "builtin not implemented natively in VM:";
      runtime_message.starts_with(kNativeBuiltinGapPrefix)) {
    return "This run is strict VM-only. Try --mode interpreter for runtime fallback.";
  }
  return std::nullopt;
}

auto usage_text() -> std::string {
  return "usage: fleaux [--mode vm,interpreter] [--repl] [--no-run] [file.fleaux] [-- "
         "<arg1> <arg2> ...]";
}

void print_help() {
  std::cout << usage_text() << '\n'
            << "\n"
            << "Options:\n"
            << "  -h, --help             Show this help message\n"
            << "  --mode <mode>          Execution mode (vm, interpreter)\n"
            << "  --engine <mode>        Alias for --mode\n"
            << "  --repl                 Start interactive interpreter REPL\n"
            << "  --no-run               Skip execution and print what would run\n"
            << "\n"
            << "Notes:\n"
            << "  - Default mode is vm\n"
            << "  - REPL forces interpreter mode\n"
            << "  - If no source is provided, defaults to test.fleaux\n"
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

auto make_cli_error(const std::string& message, const std::optional<std::string>& hint = std::nullopt,
                    const std::optional<fleaux::frontend::diag::SourceSpan>& span = std::nullopt) -> CliError {
  return CliError{.message = message, .hint = hint, .span = span};
}

auto load_ir_program(const std::filesystem::path& source_file)
    -> tl::expected<fleaux::frontend::ir::IRProgram, CliError> {
  return fleaux::frontend::source_loader::load_ir_program<CliError>(
      source_file, make_cli_error, "Cyclic import detected.", "Break the cycle by moving shared definitions to a third module.");
}

auto run_vm(const std::filesystem::path& source_file, const std::vector<std::string>& process_args)
    -> tl::expected<void, CliError> {
  auto ir_program = load_ir_program(source_file);
  if (!ir_program) { return tl::unexpected(ir_program.error()); }

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  const auto module = compiler.compile(ir_program.value());
  if (!module) {
    return tl::unexpected(CliError{
        .message = module.error().message,
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

  const fleaux::vm::Runtime runtime;
  if (const auto exec = runtime.execute(module.value()); !exec) {
    return tl::unexpected(CliError{
        .message = exec.error().message,
        .hint = vm_runtime_hint_for(exec.error().message),
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
            .hint = "expected vm|interpreter",
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

  if (!options.repl && !options.source.has_value()) {
    options.source = std::filesystem::path("test.fleaux");
  }

  return options;
}

auto run_engine_for_source(const VmEngine engine, const std::filesystem::path& source,
                           const std::vector<std::string>& process_args) -> int {
  if (engine == VmEngine::kInterpreter) {
    return run_interpreter_and_report(source, process_args, "vm-run-interpreter");
  }

  if (const auto result = run_vm(source, process_args); !result) {
    return print_diag_and_return("vm-run-vm", result.error());
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

  const auto& [engine, source_path, process_args, no_run, repl, show_help] = *parsed;
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

  if (no_run) {
    const std::string mode_name = (engine == VmEngine::kInterpreter) ? "interpreter" : "vm";
    std::cout << '[' << mode_name << "] skipped run (--no-run): " << *source_path << '\n';
    return 0;
  }

  return run_engine_for_source(engine, *source_path, process_args);
}

