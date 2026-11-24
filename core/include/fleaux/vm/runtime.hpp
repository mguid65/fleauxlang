#pragma once

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <tl/expected.hpp>

#include "fleaux/bytecode/module.hpp"
#include "fleaux/common/indirect_optional.hpp"
#include "fleaux/frontend/diagnostics.hpp"
#include "fleaux/runtime/value.hpp"

namespace fleaux::vm {

struct ExecutionResult {
  std::int64_t exit_code{0};
};

struct RuntimeError {
  std::string message{};
  std::optional<std::string> hint{std::nullopt};
  std::optional<frontend::diag::SourceSpan> span{std::nullopt};
};

using RuntimeResult = tl::expected<ExecutionResult, RuntimeError>;
using RuntimeValueResult = tl::expected<runtime::Value, RuntimeError>;

struct RuntimeCompileOptions {
  bool enable_experimental_builtin_reductions{false};
};

// Finds the extension function for a native function symbol, or nothing when no loaded extension provides it.
using NativeFunctionResolver = std::function<std::optional<runtime::NativeFunction>(std::string_view symbol)>;

struct RuntimeInvocationOptions {
  std::string entry_label{};
  // Binds the module's native functions. Without one, a module that declares native functions cannot run.
  NativeFunctionResolver native_resolver{};
  std::optional<std::reference_wrapper<std::istream>> input{std::nullopt};
  std::optional<std::reference_wrapper<std::ostream>> output{std::nullopt};
};

class RuntimeSession {
public:
  explicit RuntimeSession(const std::vector<std::string>& process_args = {},
                          const RuntimeCompileOptions& compile_options = {});
  RuntimeSession(const RuntimeSession& other);
  RuntimeSession(RuntimeSession&& other) noexcept;
  auto operator=(const RuntimeSession& other) -> RuntimeSession&;
  auto operator=(RuntimeSession&& other) noexcept -> RuntimeSession&;
  ~RuntimeSession();

  [[nodiscard]] auto run_snippet(const std::string& snippet_text, std::ostream& output) const -> RuntimeResult;
  [[nodiscard]] auto run_snippet(const std::string& snippet_text, std::ostream& output, std::istream& input) const
      -> RuntimeResult;

private:
  struct Impl;
  mutable common::IndirectOptional<Impl> impl_;
};

class Runtime {
public:
  explicit Runtime(std::vector<std::string> process_args = {});

  [[nodiscard]] auto execute(const bytecode::Module& bytecode_module) const -> RuntimeResult;
  auto execute(const bytecode::Module& bytecode_module, std::ostream& output) const -> RuntimeResult;
  [[nodiscard]] auto execute(const bytecode::Module& bytecode_module, const RuntimeInvocationOptions& options) const
      -> RuntimeResult;
  auto execute(const bytecode::Module& bytecode_module, std::ostream& output, const RuntimeInvocationOptions& options) const
      -> RuntimeResult;
  [[nodiscard]] auto invoke_symbol(const bytecode::Module& bytecode_module, std::string_view qualified_symbol,
                                   runtime::Value arg) const -> RuntimeValueResult;
  [[nodiscard]] auto invoke_symbol(const bytecode::Module& bytecode_module, std::string_view qualified_symbol,
                                   runtime::Value arg, std::ostream& output) const -> RuntimeValueResult;
  [[nodiscard]] auto invoke_symbol(const bytecode::Module& bytecode_module, std::string_view qualified_symbol,
                                   runtime::Value arg, const RuntimeInvocationOptions& options) const
      -> RuntimeValueResult;
  [[nodiscard]] auto invoke_symbol(const bytecode::Module& bytecode_module, std::string_view qualified_symbol,
                                   runtime::Value arg, std::ostream& output,
                                   const RuntimeInvocationOptions& options) const -> RuntimeValueResult;

  [[nodiscard]] auto create_session(const std::vector<std::string>& process_args = {},
                                    const RuntimeCompileOptions& compile_options = {}) const -> RuntimeSession;

private:
  std::vector<std::string> process_args_{};
};

}  // namespace fleaux::vm
