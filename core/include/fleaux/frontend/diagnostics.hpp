#pragma once

#include <optional>
#include <string>

namespace fleaux::frontend::diag {

struct SourceSpan {
  std::string source_name;
  std::string source_text;
  int line = 1;
  int col = 1;
  int end_line = 1;
  int end_col = 1;

  [[nodiscard]] auto source_line() const -> std::optional<std::string>;
  [[nodiscard]] auto caret_width() const -> int;
};

auto merge_source_spans(const std::optional<SourceSpan>& first, const std::optional<SourceSpan>& last) -> SourceSpan;

[[nodiscard]] auto format_diagnostic(const std::string& stage, const std::string& message,
                                     const std::optional<SourceSpan>& span,
                                     const std::optional<std::string>& hint = std::nullopt,
                                     const std::optional<int>& stage_index = std::nullopt) -> std::string;

}  // namespace fleaux::frontend::diag
