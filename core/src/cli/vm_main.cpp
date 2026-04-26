#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "fleaux/bytecode/module_loader.hpp"
#include "fleaux/bytecode/serialization.hpp"
#include "fleaux/cli/repl_driver.hpp"
#include "fleaux/frontend/diagnostics.hpp"
#include "fleaux/runtime/value.hpp"
#include "fleaux/vm/runtime.hpp"

namespace {
struct CliOptions {
  std::optional<std::filesystem::path> source;
  std::vector<std::string> process_args;
  bool no_run = false;
  bool optimize = false;
  bool write_bytecode_cache = true;
  bool inspect = false;
  bool no_color = false;
  bool repl = false;
  bool show_help = false;
};

struct CliError {
  std::string message;
  std::optional<std::string> hint;
  std::optional<fleaux::frontend::diag::SourceSpan> span;
};

auto vm_runtime_hint_for(const std::string& runtime_message) -> std::optional<std::string> {
  if (constexpr std::string_view kNativeBuiltinGapPrefix = "builtin not implemented natively in VM:";
      runtime_message.starts_with(kNativeBuiltinGapPrefix)) {
    return "This run is strict VM-only. Implement the missing builtin in the VM runtime, then retry.";
  }
  return std::nullopt;
}

auto vm_loader_hint_for(const std::string& load_message) -> std::optional<std::string> {
  if (load_message.starts_with("import-unresolved:")) {
    return "Resolve the missing module path or module name, then retry.";
  }
  if (load_message.starts_with("import-cycle:")) {
    return "Break the cycle by extracting shared declarations into a separate module.";
  }
  if (load_message.find("Type mismatch in call target arguments") != std::string::npos ||
      load_message.find("Unresolved symbol") != std::string::npos) {
    return "Imported module API typing must match consuming usage across module boundaries.";
  }
  return "Resolve the VM load or typing failure, then retry.";
}

auto usage_text() -> std::string {
  return "usage: fleaux [--repl] [--no-run] [--inspect] [--no-emit-bytecode] [--no-color] [file.fleaux|file.fleaux.bc] "
         "[-- "
         "<arg1> <arg2> ...]";
}

void print_help() {
  std::cout << usage_text() << '\n'
            << "\n"
            << "Options:\n"
            << "  -h, --help             Show this help message\n"
            << "  --repl                 Start the interactive REPL\n"
            << "  --no-run               Skip execution and print what would run\n"
            << "  --optimize             Enable extended optimizer passes (baseline passes always run)\n"
            << "  --no-emit-bytecode     Do not write/refresh .fleaux.bc cache files while loading modules\n"
            << "  --inspect              Print header/dependency/export info for a .fleaux.bc module and exit\n"
            << "  --no-color             Disable REPL syntax coloring (also honors NO_COLOR)\n"
            << "\n"
            << "Notes:\n"
            << "  - VM mode writes/refreshes .fleaux.bc cache files by default\n"
            << "  - .fleaux.bc entry modules are supported for execution and --inspect\n"
            << "  - REPL imports are symbolic-only: Std, StdBuiltins\n"
            << "  - In REPL, use :help (or :?) to list REPL commands\n"
            << "  - If no source is provided, defaults to test.fleaux\n"
            << "  - --inspect expects a .fleaux.bc file (or a .fleaux file with sibling .fleaux.bc)\n"
            << "  - Arguments after '--' are forwarded to runtime entrypoints\n";
}

auto run_vm(const std::filesystem::path& source_file, const std::vector<std::string>& process_args, const bool optimize,
            const bool write_bytecode_cache) -> tl::expected<void, CliError> {
  auto module_result = fleaux::bytecode::load_linked_module(
      source_file, fleaux::bytecode::ModuleLoadOptions{
                       .mode = optimize ? fleaux::bytecode::OptimizationMode::kExtended
                                        : fleaux::bytecode::OptimizationMode::kBaseline,
                       .write_bytecode_cache = write_bytecode_cache,
                   });
  if (!module_result) {
    return tl::unexpected(CliError{
        .message = module_result.error().message,
        .hint = vm_loader_hint_for(module_result.error().message),
        .span = std::nullopt,
    });
  }
  auto module = std::move(module_result.value());

  std::vector<std::string> args_storage;
  args_storage.reserve(process_args.size() + 1U);
  args_storage.push_back(source_file.string());
  args_storage.insert(args_storage.end(), process_args.begin(), process_args.end());

  std::vector<char*> argv_ptrs;
  argv_ptrs.reserve(args_storage.size());
  for (auto& arg : args_storage) { argv_ptrs.push_back(arg.data()); }
  fleaux::runtime::set_process_args(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());

  constexpr fleaux::vm::Runtime runtime;
  if (const auto exec = runtime.execute(module); !exec) {
    return tl::unexpected(CliError{
        .message = exec.error().message,
        .hint = exec.error().hint.has_value() ? exec.error().hint : vm_runtime_hint_for(exec.error().message),
        .span = exec.error().span,
    });
  }

  return {};
}

void print_module_summary(const fleaux::bytecode::Module& module) {
  std::cout << "module: " << module.header.module_name << '\n';
  std::cout << "source_path: " << module.header.source_path << '\n';
  std::cout << "source_hash: " << module.header.source_hash << '\n';
  std::cout << "payload_checksum: " << module.header.payload_checksum << '\n';
  std::cout << "optimization_mode: " << static_cast<int>(module.header.optimization_mode) << '\n';

  std::cout << "dependencies(" << module.dependencies.size() << "):\n";
  for (const auto& dependency : module.dependencies) {
    std::cout << "  - " << dependency.module_name;
    if (dependency.is_symbolic) { std::cout << " [symbolic]"; }
    std::cout << '\n';
  }

  std::cout << "exports(" << module.exports.size() << "):\n";
  for (const auto& exported_symbol : module.exports) {
    std::cout << "  - " << exported_symbol.name << " kind="
              << (exported_symbol.kind == fleaux::bytecode::ExportKind::kFunction ? "function" : "builtin_alias")
              << " index=" << exported_symbol.index;
    if (exported_symbol.kind == fleaux::bytecode::ExportKind::kBuiltinAlias) {
      std::cout << " builtin=" << exported_symbol.builtin_name;
    }
    std::cout << '\n';
  }
}

auto run_inspect(const std::filesystem::path& source_or_bytecode) -> tl::expected<void, CliError> {
  std::filesystem::path bytecode_path = source_or_bytecode;
  if (bytecode_path.extension() != ".bc") { bytecode_path += ".bc"; }

  std::ifstream in(bytecode_path, std::ios::binary);
  if (!in) {
    return tl::unexpected(CliError{
        .message = "Failed to read bytecode file: " + bytecode_path.string(),
        .hint = "Provide a .fleaux.bc file or a source path with a sibling .fleaux.bc.",
        .span = std::nullopt,
    });
  }

  const std::vector<std::uint8_t> buffer((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto module = fleaux::bytecode::deserialize_module(buffer);
  if (!module) {
    return tl::unexpected(CliError{
        .message = module.error().message,
        .hint = "The file may be corrupted or from an incompatible bytecode schema.",
        .span = std::nullopt,
    });
  }

  print_module_summary(*module);
  return {};
}

auto print_diag_and_return(const std::string& stage, const CliError& error) -> int {
  std::cerr << fleaux::frontend::diag::format_diagnostic(stage, error.message, error.span, error.hint) << '\n';
  return 2;
}

auto parse_cli_args(int argc, char** argv) -> tl::expected<CliOptions, CliError> {
  CliOptions options;

  if (argc < 2) { options.repl = true; }

  bool runtime_args_mode = false;
  for (int arg_index = 1; arg_index < argc; ++arg_index) {
    const std::string_view token = argv[arg_index];
    if (!runtime_args_mode && token == "--") {
      runtime_args_mode = true;
      continue;
    }

    if (runtime_args_mode) {
      options.process_args.emplace_back(argv[arg_index]);
      continue;
    }

    if (token == "-h" || token == "--help") {
      options.show_help = true;
      continue;
    }

    if (token == "--no-run") {
      options.no_run = true;
      continue;
    }

    if (token == "--optimize") {
      options.optimize = true;
      continue;
    }

    if (token == "--no-emit-bytecode") {
      options.write_bytecode_cache = false;
      continue;
    }

    if (token == "--inspect") {
      options.inspect = true;
      continue;
    }

    if (token == "--no-color") {
      options.no_color = true;
      continue;
    }

    if (token == "--repl") {
      options.repl = true;
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

  if (!options.repl && !options.inspect && !options.source.has_value()) {
    options.source = std::filesystem::path("test.fleaux");
  }

  return options;
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

  const auto& [source_path, process_args, no_run, optimize, write_bytecode_cache, inspect, no_color, repl, show_help] =
      *parsed;
  if (show_help) {
    print_help();
    return 0;
  }

  if (inspect) {
    if (!source_path.has_value()) {
      return print_diag_and_return("vm-inspect", CliError{
                                                     .message = "inspect mode requires a module path",
                                                     .hint = "Pass a .fleaux.bc file (or .fleaux with sibling .bc).",
                                                     .span = std::nullopt,
                                                 });
    }
    if (const auto result = run_inspect(*source_path); !result) {
      return print_diag_and_return("vm-inspect", result.error());
    }
    return 0;
  }

  if (repl) {
    if (no_run) {
      std::cout << "[vm] skipped run (--no-run): <repl>\n";
      return 0;
    }
    constexpr fleaux::cli::ReplDriver repl_driver;
    return repl_driver.run(process_args, !no_color);
  }

  if (no_run) {
    std::cout << "[vm] skipped run (--no-run): " << *source_path << '\n';
    return 0;
  }

  if (const auto result = run_vm(*source_path, process_args, optimize, write_bytecode_cache); !result) {
    return print_diag_and_return("vm-run", result.error());
  }
  return 0;
}
