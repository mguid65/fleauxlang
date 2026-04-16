#include <sstream>
#include <string>
#include <vector>
#include <iostream>

#include "fleaux/bytecode/compiler.hpp"
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
std::string g_last_error;
std::string g_last_output;
int g_last_exit_code = 0;

struct CoutRedirectGuard {
  explicit CoutRedirectGuard(std::ostream& destination) : old_buf(std::cout.rdbuf(destination.rdbuf())) {}
  ~CoutRedirectGuard() { std::cout.rdbuf(old_buf); }
  std::streambuf* old_buf;
};

void set_error(std::string msg) { g_last_error = std::move(msg); }

const char* safe_text(const char* text, const char* fallback) {
  return text != nullptr ? text : fallback;
}

auto compile_module(const std::string& source, const std::string& name)
    -> tl::expected<fleaux::bytecode::Module, int> {
  const fleaux::frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(source, name);
  if (!parsed) {
    set_error(parsed.error().message);
    return tl::unexpected(3);
  }

  const fleaux::frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  if (!lowered) {
    set_error(lowered.error().message);
    return tl::unexpected(4);
  }

  const fleaux::bytecode::BytecodeCompiler compiler;
  const auto bytecode = compiler.compile(lowered.value());
  if (!bytecode) {
    set_error(bytecode.error().message);
    return tl::unexpected(5);
  }

  return bytecode.value();
}
}  // namespace

extern "C" {

FLEAUX_WASM_EXPORT const char* fleaux_wasm_version() {
  return "fleaux-wasm-coordinator/0.1";
}

FLEAUX_WASM_EXPORT const char* fleaux_wasm_last_error() {
  return g_last_error.c_str();
}

FLEAUX_WASM_EXPORT const char* fleaux_wasm_last_output() {
  return g_last_output.c_str();
}

FLEAUX_WASM_EXPORT int fleaux_wasm_last_exit_code() {
  return g_last_exit_code;
}

// Skeleton coordinator entrypoint for smoke-testing the Emscripten toolchain.
// Returns 0 on success, non-zero on parse/lower/bytecode-compile errors.
FLEAUX_WASM_EXPORT int fleaux_wasm_parse_and_lower(const char* source_text, const char* source_name) {
  g_last_error.clear();
  g_last_output.clear();
  g_last_exit_code = 0;

  const std::string source = safe_text(source_text, "");
  if (source.empty()) {
    set_error("empty source text");
    return 2;
  }

  const std::string name = safe_text(source_name, "visual_graph.fleaux");

  const auto compiled = compile_module(source, name);
  if (!compiled) {
    return compiled.error();
  }

  return 0;
}

FLEAUX_WASM_EXPORT int fleaux_wasm_run_source(const char* source_text, const char* source_name) {
  g_last_error.clear();
  g_last_output.clear();
  g_last_exit_code = 0;

  const std::string source = safe_text(source_text, "");
  if (source.empty()) {
    set_error("empty source text");
    return 2;
  }

  const std::string name = safe_text(source_name, "visual_graph.fleaux");

  const auto compiled = compile_module(source, name);
  if (!compiled) {
    return compiled.error();
  }

#if FLEAUX_WASM_HAS_RUNTIME
  std::vector<std::string> args_storage{name};
  std::vector<char*> argv_ptrs;
  argv_ptrs.reserve(args_storage.size());
  for (auto& arg : args_storage) {
    argv_ptrs.push_back(arg.data());
  }
  fleaux::runtime::set_process_args(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());

  std::ostringstream output;
  CoutRedirectGuard cout_guard(output);
  const fleaux::vm::Runtime runtime(fleaux::vm::RuntimeOptions{.allow_runtime_fallback = false});
  const auto exec = runtime.execute(compiled.value(), output);
  g_last_output = output.str();

  if (!exec) {
    set_error(exec.error().message);
    return 6;
  }

  g_last_exit_code = static_cast<int>(exec->exit_code);
  return 0;
#else
  set_error("WASM runtime execution is unavailable in this build.");
  return 7;
#endif
}

}  // extern "C"

int main() { return 0; }

