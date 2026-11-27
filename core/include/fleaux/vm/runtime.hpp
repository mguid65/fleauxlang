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

// One call in the chain of a failed run: the function it was made in, and where.
struct RuntimeCall {
  // The function's name without its overload number, or empty for the top level of a module.
  std::string function{};
  // For a call of a builtin that called the function of the call before it in the chain back (Std.Tuple.Any, say),
  // the builtin's name; empty otherwise.
  std::string through{};
  // Where the call is, or for the first call of a chain, where the run failed. An empty path when the instruction has
  // no location.
  std::string file{};
  std::uint64_t file_hash{0};
  std::uint32_t line{0};
  std::uint32_t column{0};
  std::uint32_t end_line{0};
  std::uint32_t end_column{0};
};

struct RuntimeError {
  std::string message{};
  std::optional<std::string> hint{std::nullopt};
  std::optional<frontend::diag::SourceSpan> span{std::nullopt};
  // For a failed run, the calls it failed in, innermost first; empty for an error before the run.
  std::vector<RuntimeCall> calls{};
};

// The notes that describe a failed run's calls after its first, as diagnostics show them: one line per call, with
// consecutive calls from one place collapsed into one line, and a chain of more than kRuntimeCallNoteLimit lines cut
// to its first and last lines.
inline constexpr std::size_t kRuntimeCallNoteLimit = 20U;
[[nodiscard]] auto describe_runtime_calls(const std::vector<RuntimeCall>& calls) -> std::vector<std::string>;

// The span of a failed run's first call, with the text of its file when `read_text` finds the file and its hash is the
// one compiled, and without the text otherwise; nothing when the call has no location. By default the file is read
// from disk.
using SourceTextReader = std::function<std::optional<std::string>(const std::string& path)>;
[[nodiscard]] auto runtime_failure_span(const RuntimeError& error, const SourceTextReader& read_text = {})
    -> std::optional<frontend::diag::SourceSpan>;

using RuntimeResult = tl::expected<ExecutionResult, RuntimeError>;
using RuntimeValueResult = tl::expected<runtime::Value, RuntimeError>;

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
  explicit RuntimeSession(const std::vector<std::string>& process_args = {});
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

  [[nodiscard]] auto create_session(const std::vector<std::string>& process_args = {}) const -> RuntimeSession;

private:
  std::vector<std::string> process_args_{};
};

}  // namespace fleaux::vm
