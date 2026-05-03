#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

#include <tl/expected.hpp>

#include "fleaux/bytecode/module.hpp"
#include "fleaux/frontend/box.hpp"
#include "fleaux/frontend/diagnostics.hpp"

namespace fleaux::vm {

struct ExecutionResult {
  std::int64_t exit_code = 0;
};

struct RuntimeError {
  std::string message;
  std::optional<std::string> hint;
  std::optional<frontend::diag::SourceSpan> span;
};

using RuntimeResult = tl::expected<ExecutionResult, RuntimeError>;

class RuntimeSession {
public:
  explicit RuntimeSession(const std::vector<std::string>& process_args = {});
  RuntimeSession(const RuntimeSession& other);
  RuntimeSession(RuntimeSession&& other) noexcept;
  auto operator=(const RuntimeSession& other) -> RuntimeSession&;
  auto operator=(RuntimeSession&& other) noexcept -> RuntimeSession&;
  ~RuntimeSession();

  [[nodiscard]] auto run_snippet(const std::string& snippet_text, std::ostream& output) const -> RuntimeResult;

private:
  struct Impl;
  mutable frontend::Box<Impl> impl_;
};

class Runtime {
public:
  Runtime() = default;

  [[nodiscard]] auto execute(const bytecode::Module& bytecode_module) const -> RuntimeResult;
  auto execute(const bytecode::Module& bytecode_module, std::ostream& output) const -> RuntimeResult;

  [[nodiscard]] auto create_session(const std::vector<std::string>& process_args = {}) const -> RuntimeSession;
};

}  // namespace fleaux::vm
