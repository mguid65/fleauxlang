#pragma once

#include <memory>
#include <optional>
#include <string>

namespace fleaux::frontend::diag {

// The name and text of one source file. Every span in a file holds one of these, so copies share the file's
// contents instead of copying them: a copy costs a reference-count increment. A default-constructed SourceFile
// stands for no file, and its name and text are empty.
class SourceFile {
public:
  SourceFile() = default;
  SourceFile(std::string name, std::string text);

  [[nodiscard]] auto name() const noexcept -> const std::string&;
  [[nodiscard]] auto text() const noexcept -> const std::string&;

private:
  struct Contents;
  std::shared_ptr<const Contents> contents_;
};

struct SourceSpan {
  SourceFile file;
  int line = 1;
  int col = 1;
  int end_line = 1;
  int end_col = 1;

  [[nodiscard]] auto source_name() const noexcept -> const std::string& { return file.name(); }
  [[nodiscard]] auto source_text() const noexcept -> const std::string& { return file.text(); }
  [[nodiscard]] auto source_line() const -> std::optional<std::string>;
  [[nodiscard]] auto caret_width() const -> int;
};

auto merge_source_spans(const std::optional<SourceSpan>& first, const std::optional<SourceSpan>& last) -> SourceSpan;

[[nodiscard]] auto format_diagnostic(const std::string& stage, const std::string& message,
                                     const std::optional<SourceSpan>& span,
                                     const std::optional<std::string>& hint = std::nullopt,
                                     const std::optional<int>& stage_index = std::nullopt) -> std::string;

}  // namespace fleaux::frontend::diag
