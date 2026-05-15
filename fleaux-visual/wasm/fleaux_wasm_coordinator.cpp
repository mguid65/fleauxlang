#include <cctype>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fleaux/bytecode/compiler.hpp"
#include "fleaux/frontend/diagnostics.hpp"
#include "fleaux/frontend/lowering.hpp"
#include "fleaux/frontend/parser.hpp"
#include "fleaux/runtime/value.hpp"
#if FLEAUX_WASM_HAS_RUNTIME
#include "fleaux/vm/runtime.hpp"
#endif

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#define FLEAUX_WASM_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define FLEAUX_WASM_EXPORT
#endif

namespace {
enum class WasmStatus : int {
  kOk = 0,
  kEmptySource = 2,
  kParseError = 3,
  kLowerError = 4,
  kCompileError = 5,
  kRuntimeError = 6,
  kRuntimeUnavailable = 7,
};

struct CoordinatorState {
  std::string last_error;
  std::string last_output;
  int last_exit_code = 0;
  WasmStatus last_status = WasmStatus::kOk;

  void reset() {
    last_error.clear();
    last_output.clear();
    last_exit_code = 0;
    last_status = WasmStatus::kOk;
  }
};

CoordinatorState g_state;

struct StreamRedirectGuard {
  explicit StreamRedirectGuard(std::ostream& source, std::ostream& destination)
      : source_(source), old_buf_(source.rdbuf(destination.rdbuf())) {}
  ~StreamRedirectGuard() { source_.rdbuf(old_buf_); }

 private:
  std::ostream& source_;
  std::streambuf* old_buf_;
};

constexpr auto kDefaultSourceName = "visual_graph.fleaux";

[[nodiscard]] auto is_blank_text(const std::string_view text) -> bool {
  for (const unsigned char ch : text) {
    if (!std::isspace(ch)) {
      return false;
    }
  }
  return true;
}

constexpr auto to_status_code(const WasmStatus status) -> int { return static_cast<int>(status); }

void set_status(const WasmStatus status) { g_state.last_status = status; }

void set_error(std::string msg) { g_state.last_error = std::move(msg); }

void set_output(std::string text) { g_state.last_output = std::move(text); }

auto fail(const WasmStatus status, std::string message) -> WasmStatus {
  set_status(status);
  set_error(std::move(message));
  return status;
}

auto fail_with_diagnostic(const WasmStatus status, const std::string_view stage, const std::string& message,
                          const std::optional<fleaux::frontend::diag::SourceSpan>& span = std::nullopt,
                          const std::optional<std::string>& hint = std::nullopt) -> WasmStatus {
  return fail(status, fleaux::frontend::diag::format_diagnostic(std::string(stage), message, span, hint));
}

const char* safe_text(const char* text, const char* fallback) {
  return text != nullptr ? text : fallback;
}

[[nodiscard]] auto normalized_source_name(const char* source_name) -> std::string {
  const std::string name = safe_text(source_name, kDefaultSourceName);
  if (is_blank_text(name)) {
    return kDefaultSourceName;
  }
  return name;
}

#if FLEAUX_WASM_HAS_RUNTIME
void set_process_args_for_source_name(const std::string& name) {
  std::vector<std::string> args_storage{name};
  std::vector<char*> argv_ptrs;
  argv_ptrs.reserve(args_storage.size());
  for (auto& arg : args_storage) {
    argv_ptrs.push_back(arg.data());
  }
  fleaux::runtime::set_process_args(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());
}

auto execute_runtime_module(fleaux::bytecode::Module& module, std::ostringstream& output)
    -> tl::expected<int, std::string> {
  try {
    const fleaux::vm::Runtime runtime;
    const auto exec = runtime.execute(module, output);
    if (!exec) {
      return tl::unexpected(exec.error().message);
    }
    return static_cast<int>(exec->exit_code);
  } catch (const std::exception& ex) {
    return tl::unexpected(std::string(ex.what()));
  } catch (...) {
    return tl::unexpected(std::string("Unknown runtime failure"));
  }
}
#endif

auto compile_module(const std::string& source, const std::string& name)
    -> tl::expected<fleaux::bytecode::Module, WasmStatus> {
  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(source, name);
  if (!parsed) {
    return tl::unexpected(
        fail_with_diagnostic(WasmStatus::kParseError, "parse", parsed.error().message, parsed.error().span,
                             parsed.error().hint));
  }

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  if (!lowered) {
    return tl::unexpected(
        fail_with_diagnostic(WasmStatus::kLowerError, "lower", lowered.error().message, lowered.error().span,
                             lowered.error().hint));
  }

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto bytecode = compiler.compile(lowered.value());
  if (!bytecode) {
    return tl::unexpected(fail_with_diagnostic(WasmStatus::kCompileError, "compiler", bytecode.error().message));
  }

  return bytecode.value();
}

auto load_compile_unit(const char* source_text, const char* source_name)
    -> tl::expected<std::pair<fleaux::bytecode::Module, std::string>, WasmStatus> {
  const std::string source = safe_text(source_text, "");
  if (is_blank_text(source)) {
    return tl::unexpected(fail(WasmStatus::kEmptySource, "empty source text"));
  }

  const std::string name = normalized_source_name(source_name);
  const auto compiled = compile_module(source, name);
  if (!compiled) {
    return tl::unexpected(compiled.error());
  }

  return std::make_pair(std::move(compiled.value()), name);
}

template <class Handler>
int run_with_compile_unit(const char* source_text, const char* source_name, Handler&& handler) {
  g_state.reset();

  const auto compile_unit = load_compile_unit(source_text, source_name);
  if (!compile_unit) {
    return to_status_code(compile_unit.error());
  }

  return std::forward<Handler>(handler)(std::move(compile_unit.value()));
}

auto run_module(fleaux::bytecode::Module module, const std::string& name) -> WasmStatus {
#if FLEAUX_WASM_HAS_RUNTIME
  set_process_args_for_source_name(name);

  std::ostringstream output;
  StreamRedirectGuard cout_guard(std::cout, output);
  StreamRedirectGuard cerr_guard(std::cerr, output);
  const auto exit_code = execute_runtime_module(module, output);
  set_output(output.str());

  if (!exit_code) {
    return fail_with_diagnostic(WasmStatus::kRuntimeError, "runtime", exit_code.error());
  }

  g_state.last_exit_code = *exit_code;
  set_status(WasmStatus::kOk);
  return WasmStatus::kOk;
#else
  (void)module;
  (void)name;
  return fail(WasmStatus::kRuntimeUnavailable, "WASM runtime execution is unavailable in this build.");
#endif
}
}  // namespace

extern "C" {

FLEAUX_WASM_EXPORT const char* fleaux_wasm_version() {
  return "fleaux-wasm-coordinator/0.1";
}

FLEAUX_WASM_EXPORT const char* fleaux_wasm_last_error() {
  return g_state.last_error.c_str();
}

FLEAUX_WASM_EXPORT const char* fleaux_wasm_last_output() {
  return g_state.last_output.c_str();
}

FLEAUX_WASM_EXPORT int fleaux_wasm_last_exit_code() {
  return g_state.last_exit_code;
}

FLEAUX_WASM_EXPORT int fleaux_wasm_last_status() {
  return to_status_code(g_state.last_status);
}

// Coordinator entrypoint for smoke-testing the Emscripten toolchain.
FLEAUX_WASM_EXPORT int fleaux_wasm_parse_and_lower(const char* source_text, const char* source_name) {
  return run_with_compile_unit(source_text, source_name, [](auto&&) { return to_status_code(WasmStatus::kOk); });
}

FLEAUX_WASM_EXPORT int fleaux_wasm_run_source(const char* source_text, const char* source_name) {
  return run_with_compile_unit(source_text, source_name, [](auto&& compile_unit) {
    auto [module, name] = std::move(compile_unit);
    return to_status_code(run_module(std::move(module), name));
  });
}

}  // extern "C"

int main() { return 0; }

