#include "fleaux/frontend/diagnostics.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <vector>

namespace fleaux::frontend::diag {

auto SourceSpan::source_line() const -> std::optional<std::string> {
  if (source_text.empty()) {
    return std::nullopt;
  }

  std::istringstream in(source_text);
  std::string line_text;
  int current = 1;
  while (std::getline(in, line_text)) {
    if (current == line) {
      return line_text;
    }
    ++current;
  }
  return std::nullopt;
}

auto SourceSpan::caret_width() const -> int {
  if (end_line == line && end_col >= col) {
    return std::max(end_col - col, 1);
  }
  return 1;
}

auto merge_source_spans(const std::optional<SourceSpan>& first, const std::optional<SourceSpan>& last) -> SourceSpan {
  if (first && last) {
    SourceSpan merged = *first;
    merged.end_line = last->end_line;
    merged.end_col = last->end_col;
    return merged;
  }
  if (first) {
    return *first;
  }
  if (last) {
    return *last;
  }
  return SourceSpan{};
}

namespace {

auto split_source_lines(const std::string& source) -> std::vector<std::string> {
  std::vector<std::string> lines;
  std::istringstream in(source);
  std::string ln;
  while (std::getline(in, ln)) {
    lines.push_back(ln);
  }
  return lines;
}

auto num_digits(int value) -> int {
  if (value <= 0) {
    return 1;
  }
  int count = 0;
  while (value > 0) {
    value /= 10;
    ++count;
  }
  return count;
}

auto pad_left(const std::string& text, const int width) -> std::string {
  if (width <= 0) {
    return text;
  }
  const auto target_width = static_cast<std::size_t>(width);
  if (text.size() >= target_width) {
    return text;
  }
  return std::string(target_width - text.size(), ' ') + text;
}

void append_diagnostic_header(std::ostringstream& out, const std::string& stage, const std::string& message,
                              const std::optional<int>& stage_index) {
  out << "error";
  if (!stage.empty()) {
    out << "[" << stage << "]";
  }
  out << ": " << message;
  if (stage_index.has_value()) {
    out << " (stage " << *stage_index << ")";
  }
}

void append_diagnostic_note(std::ostringstream& out, const std::optional<std::string>& hint) {
  if (hint.has_value()) {
    out << "\n   = note: " << *hint;
  }
}

void append_diagnostic_location(std::ostringstream& out, const SourceSpan& span) {
  out << "\n   --> ";
  if (!span.source_name.empty()) {
    out << span.source_name << ":";
  }
  out << span.line << ":" << span.col;
}

void emit_source_context_line(std::ostringstream& out, const std::vector<std::string>& lines, const int gutter_width,
                              const int line_number) {
  if (line_number < 1 || line_number > static_cast<int>(lines.size())) {
    return;
  }
  out << "\n"
      << pad_left(std::to_string(line_number), gutter_width) << " | "
      << lines[static_cast<std::size_t>(line_number - 1)];
}

void append_diagnostic_source_context(std::ostringstream& out, const SourceSpan& span) {
  if (span.source_text.empty() || span.line < 1) {
    return;
  }

  const auto lines = split_source_lines(span.source_text);
  const int error_line = span.line;
  const int line_count = static_cast<int>(lines.size());

  const int max_shown = std::min(error_line + 1, line_count);
  const int gutter_width = num_digits(max_shown);
  const auto blank_gutter = std::string(static_cast<std::size_t>(gutter_width), ' ');

  out << "\n" << blank_gutter << " |";

  if (error_line > 1) {
    emit_source_context_line(out, lines, gutter_width, error_line - 1);
  }

  emit_source_context_line(out, lines, gutter_width, error_line);

  const int caret_col = std::max(span.col - 1, 0);
  const int caret_len = span.caret_width();
  out << "\n"
      << blank_gutter << " | " << std::string(static_cast<std::size_t>(caret_col), ' ') << "^"
      << std::string(static_cast<std::size_t>(std::max(caret_len - 1, 0)), '~');

  if (error_line < line_count) {
    emit_source_context_line(out, lines, gutter_width, error_line + 1);
  }

  out << "\n" << blank_gutter << " |";
}

}  // namespace

auto format_diagnostic(const std::string& stage, const std::string& message, const std::optional<SourceSpan>& span,
                       const std::optional<std::string>& hint, const std::optional<int>& stage_index) -> std::string {
  std::ostringstream out;

  append_diagnostic_header(out, stage, message, stage_index);

  if (!span.has_value()) {
    append_diagnostic_note(out, hint);
    return out.str();
  }

  append_diagnostic_location(out, *span);
  append_diagnostic_source_context(out, *span);
  append_diagnostic_note(out, hint);

  return out.str();
}

}  // namespace fleaux::frontend::diag
