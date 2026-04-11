#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <tl/expected.hpp>

#include "fleaux/frontend/diagnostics.hpp"

namespace fleaux::vm {

struct InterpretError {
  std::string message;
  std::optional<std::string> hint;
  std::optional<frontend::diag::SourceSpan> span;
};

using InterpretResult = tl::expected<void, InterpretError>;

class InterpreterSession {
 public:
  explicit InterpreterSession(const std::vector<std::string>& process_args = {});

  [[nodiscard]] InterpretResult run_snippet(const std::string& snippet_text) const;

 private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

class Interpreter {
 public:
  [[nodiscard]] InterpretResult run_file(const std::filesystem::path& source_file,
                                         const std::vector<std::string>& process_args = {}) const;

  [[nodiscard]] InterpreterSession create_session(const std::vector<std::string>& process_args = {}) const;
};

}  // namespace fleaux::vm

