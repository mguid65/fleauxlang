#include "fleaux/frontend/diagnostics.hpp"

#include <sstream>
#include <vector>

namespace fleaux::frontend::diag {

auto SourceSpan::source_line() const -> std::optional<std::string> {
  if (source_text.empty()) { return std::nullopt; }

  std::istringstream in(source_text);
  std::string line_text;
  int current = 1;
  while (std::getline(in, line_text)) {
    if (current == line) { return line_text; }
    ++current;
  }
  return std::nullopt;
}

auto SourceSpan::caret_width() const -> int {
  if (end_line == line && end_col >= col) { return std::max(end_col - col, 1); }
  return 1;
}

auto merge_source_spans(const std::optional<SourceSpan>& first, const std::optional<SourceSpan>& last) -> SourceSpan {
  if (first && last) {
    SourceSpan merged = *first;
    merged.end_line = last->end_line;
    merged.end_col = last->end_col;
    return merged;
  }
  if (first) { return *first; }
  if (last) { return *last; }
  return SourceSpan{};
}

auto format_diagnostic(const std::string& stage, const std::string& message, const std::optional<SourceSpan>& span,
                       const std::optional<std::string>& hint, const std::optional<int>& stage_index) -> std::string {
  std::ostringstream out;
  out << "[" << stage << "] " << message;
  if (stage_index.has_value()) { out << " (stage " << *stage_index << ")"; }
  if (span.has_value()) {
    if (!span->source_name.empty()) { out << " in " << span->source_name; }
    out << " at line " << span->line << ", column " << span->col << ".";
  }

  if (span.has_value()) {
    if (const auto source_line = span->source_line(); source_line.has_value()) {
      out << "\n" << *source_line;
      out << "\n"
          << std::string(static_cast<std::size_t>(std::max(span->col - 1, 0)), ' ') << "^"
          << std::string(static_cast<std::size_t>(std::max(span->caret_width() - 1, 0)), '~');
    }
  }

  if (hint.has_value()) { out << "\n" << *hint; }

  return out.str();
}

}  // namespace fleaux::frontend::diag
