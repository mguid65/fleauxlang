#include <algorithm>
#include <charconv>
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
#include "fleaux/frontend/lowering.hpp"
#include "fleaux/frontend/parser.hpp"
#include "fleaux/frontend/source_loader.hpp"
#include "fleaux/runtime/value.hpp"
#include "fleaux/vm/runtime.hpp"

namespace {
struct CliOptions {
  std::optional<std::filesystem::path> source;
  std::vector<std::string> process_args;
  bool no_run = false;
  bool optimize = false;
  bool write_bytecode_cache = true;
  bool enable_auto_value_ref = false;
  std::size_t value_ref_byte_cutoff = 256;
  bool no_color = false;
  bool repl = false;
  bool show_help = false;
  bool disassemble = false;
  bool dump_ast = false;
  bool dump_ir = false;
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
  return "usage: fleaux [--repl] [--no-run] [--disassemble] [--dump-ast] [--dump-ir] [--no-emit-bytecode] [--no-color] "
         "[--auto-value-ref] [--value-ref-byte-cutoff N] "
         "[file.fleaux|file.fleaux.bc] "
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
            << "  --auto-value-ref       Enable automatic by-ref promotion for large safe arguments\n"
            << "  --value-ref-byte-cutoff N\n"
            << "                         Set the byte-size cutoff used by --auto-value-ref (default: 256)\n"
            << "  --disassemble          Print the disassembly for a .fleaux.bc module and exit\n"
            << "  --dump-ast             Parse a .fleaux source file, print the AST, and exit\n"
            << "  --dump-ir              Parse/lower a .fleaux source file, print the IR, and exit\n"
            << "  --no-color             Disable REPL syntax coloring (also honors NO_COLOR)\n";
}

auto load_source_text_for_dump(const std::filesystem::path& source_file, const std::string_view mode_name)
    -> tl::expected<std::string, CliError> {
  if (source_file.extension() == ".bc") {
    return tl::unexpected(CliError{
        .message = std::string(mode_name) + " requires a .fleaux source file",
        .hint = "Pass a source file path rather than a .fleaux.bc bytecode cache.",
        .span = std::nullopt,
    });
  }

  const auto source_text = fleaux::frontend::source_loader::read_text_file(source_file);
  if (!source_text) {
    return tl::unexpected(CliError{
        .message = source_text.error().message,
        .hint = "Check the file path and permissions.",
        .span = std::nullopt,
    });
  }

  return *source_text;
}

auto run_ast_dump(const std::filesystem::path& source_file) -> tl::expected<void, CliError> {
  const auto source_text = load_source_text_for_dump(source_file, "AST dump mode");
  if (!source_text) {
    return tl::unexpected(source_text.error());
  }

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(*source_text, source_file.string());
  if (!parsed) {
    return tl::unexpected(CliError{
        .message = parsed.error().message,
        .hint = parsed.error().hint,
        .span = parsed.error().span,
    });
  }

  std::cout << parser.dump_ast(*parsed);
  if (std::cout.good()) {
    std::cout << '\n';
  }
  return {};
}

auto run_ir_dump(const std::filesystem::path& source_file) -> tl::expected<void, CliError> {
  const auto source_text = load_source_text_for_dump(source_file, "IR dump mode");
  if (!source_text) {
    return tl::unexpected(source_text.error());
  }

  constexpr fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(*source_text, source_file.string());
  if (!parsed) {
    return tl::unexpected(CliError{
        .message = parsed.error().message,
        .hint = parsed.error().hint,
        .span = parsed.error().span,
    });
  }

  constexpr fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower_only(*parsed);
  if (!lowered) {
    return tl::unexpected(CliError{
        .message = lowered.error().message,
        .hint = lowered.error().hint,
        .span = lowered.error().span,
    });
  }

  std::cout << lowerer.dump_ir(*lowered);
  if (std::cout.good()) {
    std::cout << '\n';
  }
  return {};
}

auto run_vm(const std::filesystem::path& source_file, const std::vector<std::string>& process_args, const bool optimize,
            const bool write_bytecode_cache, const bool enable_auto_value_ref, const std::size_t value_ref_byte_cutoff)
    -> tl::expected<void, CliError> {
  auto module_result = fleaux::bytecode::load_linked_module(
      source_file, fleaux::bytecode::ModuleLoadOptions{
                       .mode = optimize ? fleaux::bytecode::OptimizationMode::kExtended
                                        : fleaux::bytecode::OptimizationMode::kBaseline,
                       .write_bytecode_cache = write_bytecode_cache,
                       .enable_value_ref_gate = enable_auto_value_ref,
                       .enable_auto_value_ref = enable_auto_value_ref,
                       .value_ref_byte_cutoff = value_ref_byte_cutoff,
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
  for (auto& arg : args_storage) {
    argv_ptrs.push_back(arg.data());
  }
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

auto run_disassembly(const std::filesystem::path& source_or_bytecode) -> tl::expected<void, CliError> {
  std::filesystem::path bytecode_path = source_or_bytecode;
  if (bytecode_path.extension() != ".bc") {
    bytecode_path += ".bc";
  }

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

  if (const auto& result = fleaux::bytecode::disassemble_module(*module, std::cout); !result) {
    return tl::unexpected(CliError{
        .message = result.error().message,
        .hint = "An unknown error occurred during disassembly.",
        .span = std::nullopt,
    });
  }
  return {};
}

auto print_diag_and_return(const std::string& stage, const CliError& error) -> int {
  std::cerr << fleaux::frontend::diag::format_diagnostic(stage, error.message, error.span, error.hint) << '\n';
  return 2;
}

template <class Runner>
auto run(const std::optional<std::filesystem::path>& source_path, const std::string& stage,
         const std::string& missing_message, const std::string& missing_hint, Runner runner) -> int {
  if (!source_path.has_value()) {
    return print_diag_and_return(stage, CliError{
                                            .message = missing_message,
                                            .hint = missing_hint,
                                            .span = std::nullopt,
                                        });
  }

  if (const auto result = runner(*source_path); !result) {
    return print_diag_and_return(stage, result.error());
  }

  return 0;
}

auto make_repl_compile_options(const bool enable_auto_value_ref, const std::size_t value_ref_byte_cutoff)
    -> fleaux::vm::RuntimeCompileOptions {
  return fleaux::vm::RuntimeCompileOptions{
      .enable_value_ref_gate = enable_auto_value_ref,
      .enable_auto_value_ref = enable_auto_value_ref,
      .value_ref_byte_cutoff = value_ref_byte_cutoff,
  };
}

auto run_repl_mode(const bool no_run, const std::vector<std::string>& process_args, const bool no_color,
                   const bool enable_auto_value_ref, const std::size_t value_ref_byte_cutoff) -> int {
  if (no_run) {
    std::cout << "[vm] skipped run (--no-run): <repl>\n";
    return 0;
  }

  constexpr fleaux::cli::ReplDriver repl_driver;
  return repl_driver.run(process_args, !no_color,
                         make_repl_compile_options(enable_auto_value_ref, value_ref_byte_cutoff));
}

auto parse_cli_args(const int argc, char** argv) -> tl::expected<CliOptions, CliError> {
  CliOptions options;

  const auto parse_size = [](const std::string_view token) -> tl::expected<std::size_t, CliError> {
    std::size_t value = 0;
    const auto* begin = token.data();
    const auto* end = token.data() + token.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
      return tl::unexpected(CliError{
          .message = std::string("invalid value-ref byte cutoff: ") + std::string(token),
          .hint = "pass a non-negative integer to --value-ref-byte-cutoff",
          .span = std::nullopt,
      });
    }
    return value;
  };

  if (argc < 2) {
    options.repl = true;
  }

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

    if (token == "--auto-value-ref") {
      options.enable_auto_value_ref = true;
      continue;
    }

    if (token == "--dump-ast") {
      options.dump_ast = true;
      continue;
    }

    if (token == "--dump-ir") {
      options.dump_ir = true;
      continue;
    }

    if (token == "--value-ref-byte-cutoff") {
      if (arg_index + 1 >= argc) {
        return tl::unexpected(CliError{
            .message = "missing value for --value-ref-byte-cutoff",
            .hint = "pass a non-negative integer after --value-ref-byte-cutoff",
            .span = std::nullopt,
        });
      }
      const auto parsed_cutoff = parse_size(argv[++arg_index]);
      if (!parsed_cutoff) {
        return tl::unexpected(parsed_cutoff.error());
      }
      options.enable_auto_value_ref = true;
      options.value_ref_byte_cutoff = *parsed_cutoff;
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

    if (token == "--disassemble") {
      options.disassemble = true;
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

  const auto primary_mode_count = static_cast<int>(options.repl) + static_cast<int>(options.disassemble) +
                                  static_cast<int>(options.dump_ast) + static_cast<int>(options.dump_ir);
  if (primary_mode_count > 1) {
    return tl::unexpected(CliError{
        .message = "options --repl, --disassemble, --dump-ast, and --dump-ir are mutually exclusive",
        .hint = "Choose exactly one primary mode.",
        .span = std::nullopt,
    });
  }

  return options;
}
}  // namespace

auto main(int argc, char** argv) -> int {
  auto parsed = parse_cli_args(argc, argv);
  if (!parsed) {
    std::cerr << usage_text() << '\n';
    std::cerr << parsed.error().message;
    if (parsed.error().hint.has_value()) {
      std::cerr << " (" << *parsed.error().hint << ")";
    }
    std::cerr << '\n';
    return 1;
  }

  const auto& [source_path, process_args, no_run, optimize, write_bytecode_cache, enable_auto_value_ref,
               value_ref_byte_cutoff, no_color, repl, show_help, disassemble, dump_ast, dump_ir] = *parsed;
  if (show_help) {
    print_help();
    return 0;
  }

  if (disassemble) {
    return run(source_path, "vm-disassemble", "disassembly mode requires a module path",
               "Pass a .fleaux.bc file (or .fleaux with sibling .bc).", run_disassembly);
  }

  if (dump_ast) {
    return run(source_path, "vm-ast", "AST dump mode requires a source path", "Pass a .fleaux source file.",
               run_ast_dump);
  }

  if (dump_ir) {
    return run(source_path, "vm-ir", "IR dump mode requires a source path", "Pass a .fleaux source file.", run_ir_dump);
  }

  if (repl) {
    return run_repl_mode(no_run, process_args, no_color, enable_auto_value_ref, value_ref_byte_cutoff);
  }

  if (!source_path.has_value()) {
    return print_diag_and_return("vm-run", CliError{
                                               .message = "vm mode requires a module path",
                                               .hint = "Pass a .fleaux file, a .fleaux.bc file, or use --repl.",
                                               .span = std::nullopt,
                                           });
  }

  if (no_run) {
    std::cout << "[vm] skipped run (--no-run): " << source_path->string() << '\n';
    return 0;
  }

  if (const auto result = run_vm(*source_path, process_args, optimize, write_bytecode_cache, enable_auto_value_ref,
                                 value_ref_byte_cutoff);
      !result) {
    return print_diag_and_return("vm-run", result.error());
  }
  return 0;
}
