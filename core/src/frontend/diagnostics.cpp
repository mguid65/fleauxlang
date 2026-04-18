#include "fleaux/frontend/diagnostics.hpp"

#include <algorithm>
#include <sstream>
#include <string>
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

namespace {

auto split_source_lines(const std::string& source) -> std::vector<std::string> {
  std::vector<std::string> lines;
  std::istringstream in(source);
  std::string ln;
  while (std::getline(in, ln)) { lines.push_back(ln); }
  return lines;
}

auto num_digits(int value) -> int {
  if (value <= 0) { return 1; }
  int count = 0;
  while (value > 0) {
    value /= 10;
    ++count;
  }
  return count;
}

auto pad_left(const std::string& text, const int width) -> std::string {
  if (width <= 0) { return text; }
  const auto target_width = static_cast<std::size_t>(width);
  if (text.size() >= target_width) { return text; }
  return std::string(target_width - text.size(), ' ') + text;
}

}  // namespace

auto format_diagnostic(const std::string& stage, const std::string& message, const std::optional<SourceSpan>& span,
                       const std::optional<std::string>& hint, const std::optional<int>& stage_index) -> std::string {
  std::ostringstream out;

  // Header: error[stage]: message
  out << "error";
  if (!stage.empty()) { out << "[" << stage << "]"; }
  out << ": " << message;
  if (stage_index.has_value()) { out << " (stage " << *stage_index << ")"; }

  if (!span.has_value()) {
    if (hint.has_value()) { out << "\n   = note: " << *hint; }
    return out.str();
  }

  // Location: --> file:line:col
  out << "\n   --> ";
  if (!span->source_name.empty()) { out << span->source_name << ":"; }
  out << span->line << ":" << span->col;

  if (const bool has_source = !span->source_text.empty() && span->line >= 1; has_source) {
    const auto lines = split_source_lines(span->source_text);
    const int error_line = span->line;
    const int line_count = static_cast<int>(lines.size());

    // Gutter width: enough digits for the highest line number shown (at most error_line + 1)
    const int max_shown = std::min(error_line + 1, line_count);
    const int gw = num_digits(max_shown);
    const auto blank_gutter = std::string(static_cast<std::size_t>(gw), ' ');

    auto emit_line = [&](const int line_number) -> void {
      if (line_number < 1 || line_number > line_count) { return; }
      out << "\n"
          << pad_left(std::to_string(line_number), gw)
          << " | "
          << lines[static_cast<std::size_t>(line_number - 1)];
    };

    out << "\n" << blank_gutter << " |";

    // One context line before the error
    if (error_line > 1) { emit_line(error_line - 1); }

    // The error line itself
    emit_line(error_line);

    // Caret annotation
    const int caret_col = std::max(span->col - 1, 0);
    const int caret_len = span->caret_width();
    out << "\n" << blank_gutter << " | "
        << std::string(static_cast<std::size_t>(caret_col), ' ') << "^"
        << std::string(static_cast<std::size_t>(std::max(caret_len - 1, 0)), '~');

    // One context line after the error
    if (error_line < line_count) { emit_line(error_line + 1); }

    out << "\n" << blank_gutter << " |";
  }

  if (hint.has_value()) { out << "\n   = note: " << *hint; }

  return out.str();
}

}  // namespace fleaux::frontend::diag
