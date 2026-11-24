#include "fleaux/frontend/lexer.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <optional>
#include <ranges>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace fleaux::frontend::parse {
namespace {

enum class LexMode {
  kStrict,
  kBestEffort,
};

struct LexOutput {
  std::vector<Token> tokens;
  std::vector<Comment> comments;
  std::optional<ParseError> error{std::nullopt};
};

constexpr std::array<std::string_view, 14> kKeywords = {"let",    "import",  "type",        "alias",     "Int64",
                                                        "UInt64", "Float64", "String",      "Bool",      "Null",
                                                        "Any",    "Tuple",   "__builtin__", "__native__"};

constexpr std::array<std::string_view, 6> kStructuralKeywords = {"let",   "import",      "type",
                                                                 "alias", "__builtin__", "__native__"};

constexpr std::array<std::string_view, 15> kOperators = {
    "^", "/",  "*",  "%", "+",  "-", "==", "!=", "<", ">", ">=", "<=", "!", "&&", "||",
};

constexpr std::array<std::string_view, 7> kSimpleTypes = {"Int64", "UInt64", "Float64", "String", "Bool", "Null", "Any"};

constexpr std::array<std::string_view, 9> kMulti = {"...", "->", "::", "==", "!=", ">=", "<=", "&&", "||"};

auto is_ident_start(const char c) -> bool { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_'; }

auto is_ident_char(const char c) -> bool { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

auto starts_with(const std::string_view haystack, const std::size_t index, const std::string_view needle) -> bool {
  if (index + needle.size() > haystack.size()) {
    return false;
  }
  return haystack.compare(index, needle.size(), needle) == 0;
}

// The span of a token the lexer reports an error at. Errors are rare, so each one copies the file for its span.
auto span_from_token(const Token& token, const std::string& source_name, const std::string& source_text)
    -> std::optional<diag::SourceSpan> {
  diag::SourceSpan span;
  span.file = diag::SourceFile{source_name, source_text};
  span.line = token.line;
  span.col = token.col;
  span.end_line = token.end_line();
  span.end_col = token.end_col();
  return span;
}

auto is_space_or_tab(const char ch) -> bool { return ch == ' ' || ch == '\t'; }

// A raw string literal, C# 11 style: three or more quotes open it and the same number close it, and its content is
// taken as written, without escapes. When only whitespace follows the opening quotes on their line, the literal is
// multi-line: its content is the lines between that line and the closing quotes, which stand on a line of their own.
// The whitespace before the closing quotes is removed from every content line, and a line indented less than the
// closing quotes is an error. Lines of only whitespace become empty, and lines end with '\n' whatever the source uses.
struct RawStringScan {
  std::string value{};
  // Offset just past the closing quotes, or where scanning stopped on an error.
  std::size_t end{0};
  std::optional<std::string> error{std::nullopt};
  std::optional<std::string> hint{std::nullopt};
  std::size_t error_offset{0};
};

auto quote_run_length(const std::string_view source, const std::size_t offset) -> std::size_t {
  std::size_t length = 0;
  while (offset + length < source.size() && source[offset + length] == '"') {
    ++length;
  }
  return length;
}

auto raw_string_error(RawStringScan scan, const std::size_t offset, std::string message, std::string hint)
    -> RawStringScan {
  scan.error = std::move(message);
  scan.hint = std::move(hint);
  scan.error_offset = offset;
  scan.end = offset;
  return scan;
}

auto too_many_quotes_hint(const std::size_t run) -> std::string {
  return std::format("Start and end the raw string with at least {} quotes, so that {} quotes in a row can be content.",
                     run + 1, run);
}

auto scan_single_line_raw_string(const std::string_view source, const std::size_t start, const std::size_t quotes)
    -> RawStringScan {
  RawStringScan scan;
  std::size_t cursor = start + quotes;
  while (cursor < source.size() && source[cursor] != '\n') {
    if (source[cursor] != '"') {
      scan.value.push_back(source[cursor]);
      ++cursor;
      continue;
    }
    const std::size_t run = quote_run_length(source, cursor);
    if (run == quotes) {
      scan.end = cursor + run;
      return scan;
    }
    if (run > quotes) {
      return raw_string_error(std::move(scan), cursor, std::format("Raw string contains {} quotes in a row", run),
                              too_many_quotes_hint(run));
    }
    scan.value.append(run, '"');
    cursor += run;
  }
  return raw_string_error(
      std::move(scan), start, "Unterminated raw string literal",
      std::format("Close a one-line raw string with {} quotes on the same line, or end the opening line right after "
                  "the quotes to write a multi-line raw string.",
                  quotes));
}

// Removes the closing line's indentation from the content lines and joins them.
auto finish_multi_line_raw_string(RawStringScan scan,
                                  const std::vector<std::pair<std::size_t, std::string_view>>& content_lines,
                                  const std::string_view closing_indent, const std::size_t end,
                                  const std::size_t start) -> RawStringScan {
  if (content_lines.empty()) {
    return raw_string_error(std::move(scan), start, "Empty multi-line raw string literal",
                            "Put at least one line between the opening and the closing quotes, or write \"\" for an "
                            "empty string.");
  }
  for (std::size_t index = 0; index < content_lines.size(); ++index) {
    const auto& [offset, line] = content_lines[index];
    if (index > 0) {
      scan.value.push_back('\n');
    }
    if (std::ranges::all_of(line, is_space_or_tab)) {
      continue;
    }
    if (!line.starts_with(closing_indent)) {
      return raw_string_error(std::move(scan), offset, "Raw string line is indented less than its closing quotes",
                              "Every line of a multi-line raw string starts with the whitespace before its closing "
                              "quotes, which is removed from each line. Indent this line at least as far.");
    }
    scan.value.append(line.substr(closing_indent.size()));
  }
  scan.end = end;
  return scan;
}

auto scan_multi_line_raw_string(const std::string_view source, const std::size_t start, const std::size_t quotes,
                                const std::size_t first_line) -> RawStringScan {
  RawStringScan scan;
  std::vector<std::pair<std::size_t, std::string_view>> content_lines;
  std::size_t line_start = first_line;
  while (line_start < source.size()) {
    const std::size_t newline = source.find('\n', line_start);
    const std::size_t line_end = newline == std::string_view::npos ? source.size() : newline;
    std::string_view line = source.substr(line_start, line_end - line_start);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }

    std::size_t indent = 0;
    while (indent < line.size() && is_space_or_tab(line[indent])) {
      ++indent;
    }
    if (const std::size_t run = quote_run_length(line, indent); run >= quotes) {
      if (run > quotes) {
        return raw_string_error(std::move(scan), line_start + indent,
                                std::format("Raw string contains {} quotes in a row", run), too_many_quotes_hint(run));
      }
      return finish_multi_line_raw_string(std::move(scan), content_lines, line.substr(0, indent),
                                          line_start + indent + run, start);
    }
    for (std::size_t offset = indent; offset < line.size();) {
      const std::size_t run = quote_run_length(line, offset);
      if (run >= quotes) {
        return raw_string_error(std::move(scan), line_start + offset,
                                std::format("Raw string contains {} quotes in a row", run),
                                "Closing quotes stand on a line of their own. " + too_many_quotes_hint(run));
      }
      offset += run == 0 ? 1 : run;
    }
    content_lines.emplace_back(line_start, line);
    if (newline == std::string_view::npos) {
      break;
    }
    line_start = newline + 1;
  }
  return raw_string_error(std::move(scan), start, "Unterminated raw string literal",
                          std::format("Close the raw string with {} quotes on a line of their own.", quotes));
}

auto scan_raw_string(const std::string_view source, const std::size_t start) -> RawStringScan {
  const std::size_t quotes = quote_run_length(source, start);
  const std::size_t after_open = start + quotes;
  const std::size_t newline = source.find('\n', after_open);
  const std::size_t line_end = newline == std::string_view::npos ? source.size() : newline;
  const std::string_view rest_of_line = source.substr(after_open, line_end - after_open);
  if (rest_of_line.find_first_not_of(" \t\r") != std::string_view::npos) {
    return scan_single_line_raw_string(source, start, quotes);
  }
  if (newline == std::string_view::npos) {
    RawStringScan scan;
    return raw_string_error(std::move(scan), start, "Unterminated raw string literal",
                            std::format("Close the raw string with {} quotes on a line of their own.", quotes));
  }
  return scan_multi_line_raw_string(source, start, quotes, newline + 1);
}

// The line and column of `offset`, counting from a token that starts at `start` on `line`, `col`.
auto position_after(const std::string_view source, const std::size_t start, const int line, const int col,
                    const std::size_t offset) -> std::pair<int, int> {
  const std::string_view between = source.substr(start, offset - start);
  const auto newlines = static_cast<int>(std::ranges::count(between, '\n'));
  if (newlines == 0) {
    return {line, col + static_cast<int>(between.size())};
  }
  return {line + newlines, static_cast<int>(between.size() - between.rfind('\n'))};
}

auto lex_impl(const std::string& source, const std::string& source_name, const LexMode mode) -> LexOutput {
  LexOutput output;
  std::size_t cursor = 0;

  int line = 1;
  int col = 1;

  auto push = [&](const TokenKind kind, const std::string& value, const std::string& text, const std::size_t tok_offset,
                  const int tok_line, const int tok_col) -> void {
    Token token;
    token.kind = kind;
    token.value = value;
    token.offset = tok_offset;
    token.text = text;
    token.line = tok_line;
    token.col = tok_col;
    output.tokens.push_back(std::move(token));
  };

  while (cursor < source.size()) {
    const char ch = source[cursor];
    if (ch == ' ' || ch == '\t' || ch == '\r') {
      ++cursor;
      ++col;
      continue;
    }
    if (ch == '\n') {
      ++cursor;
      ++line;
      col = 1;
      continue;
    }
    if (starts_with(source, cursor, "//")) {
      const std::size_t start = cursor;
      const int start_col = col;
      while (cursor < source.size() && source[cursor] != '\n') {
        ++cursor;
        ++col;
      }
      std::string text = source.substr(start, cursor - start);
      while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.pop_back();
      }
      output.comments.push_back(Comment{.text = std::move(text), .offset = start, .line = line, .col = start_col});
      continue;
    }

    bool matched_multi = false;
    for (const auto& sym : kMulti) {
      if (starts_with(source, cursor, sym)) {
        push(TokenKind::kSymbol, std::string(sym), std::string(sym), cursor, line, col);
        cursor += sym.size();
        col += static_cast<int>(sym.size());
        matched_multi = true;
        break;
      }
    }

    if (matched_multi) {
      continue;
    }

    if (starts_with(source, cursor, R"(""")")) {
      const auto scan = scan_raw_string(source, cursor);
      if (scan.error.has_value() && mode == LexMode::kStrict) {
        const auto [error_line, error_col] = position_after(source, cursor, line, col, scan.error_offset);
        Token token;
        token.kind = TokenKind::kString;
        token.offset = scan.error_offset;
        token.line = error_line;
        token.col = error_col;
        token.text = "\"";
        output.error = ParseError{
            .message = *scan.error,
            .hint = scan.hint,
            .span = span_from_token(token, source_name, source),
        };
        return output;
      }
      // Best effort keeps what was read and goes on after it.
      const std::size_t end = scan.error.has_value() ? source.size() : scan.end;
      const auto text = source.substr(cursor, end - cursor);
      push(TokenKind::kString, scan.value, text, cursor, line, col);
      std::tie(line, col) = position_after(source, cursor, line, col, end);
      cursor = end;
      continue;
    }

    if (ch == '"') {
      const std::size_t start = cursor;
      const int start_line = line;
      const int start_col = col;
      std::string decoded;
      std::size_t str_end = cursor + 1;
      int local_col = col + 1;
      bool closed = false;
      while (str_end < source.size()) {
        char str_ch = source[str_end];
        if (str_ch == '"') {
          closed = true;
          ++str_end;
          ++local_col;
          break;
        }
        if (str_ch == '\\') {
          if (str_end + 1 >= source.size()) {
            break;
          }
          switch (const char esc = source[str_end + 1]) {
            case 'n':
              decoded.push_back('\n');
              break;
            case 't':
              decoded.push_back('\t');
              break;
            case 'r':
              decoded.push_back('\r');
              break;
            case '"':
              decoded.push_back('"');
              break;
            case '\\':
              decoded.push_back('\\');
              break;
            default:
              decoded.push_back(esc);
              break;
          }
          str_end += 2;
          local_col += 2;
          continue;
        }
        if (str_ch == '\n') {
          break;
        }
        decoded.push_back(str_ch);
        ++str_end;
        ++local_col;
      }

      if (!closed) {
        if (mode == LexMode::kStrict) {
          Token token;
          token.kind = TokenKind::kString;
          token.offset = start;
          token.line = start_line;
          token.col = start_col;
          token.text = "\"";
          output.error = ParseError{
              .message = "Unterminated string literal",
              .hint = std::nullopt,
              .span = span_from_token(token, source_name, source),
          };
          return output;
        }
        const std::string text = source.substr(start);
        push(TokenKind::kString, decoded, text, start, start_line, start_col);
        cursor = source.size();
        col = local_col;
        break;
      }
      const auto text = source.substr(start, str_end - start);
      push(TokenKind::kString, decoded, text, start, start_line, start_col);
      col = local_col;
      cursor = str_end;
      continue;
    }

    if (std::isdigit(static_cast<unsigned char>(ch)) != 0) {
      const std::size_t start = cursor;
      const int start_col = col;
      while (cursor < source.size() && std::isdigit(static_cast<unsigned char>(source[cursor])) != 0) {
        ++cursor;
        ++col;
      }
      if (cursor < source.size() && source[cursor] == '.') {
        ++cursor;
        ++col;
        while (cursor < source.size() && std::isdigit(static_cast<unsigned char>(source[cursor])) != 0) {
          ++cursor;
          ++col;
        }
      }
      if (cursor < source.size() && (source[cursor] == 'e' || source[cursor] == 'E')) {
        ++cursor;
        ++col;
        if (cursor < source.size() && (source[cursor] == '+' || source[cursor] == '-')) {
          ++cursor;
          ++col;
        }
        const std::size_t exponent_digits_start = cursor;
        while (cursor < source.size() && std::isdigit(static_cast<unsigned char>(source[cursor])) != 0) {
          ++cursor;
          ++col;
        }
        if (cursor == exponent_digits_start) {
          if (mode == LexMode::kStrict) {
            Token token;
            token.kind = TokenKind::kNumeric;
            token.offset = start;
            token.line = line;
            token.col = start_col;
            token.text = source.substr(start, cursor - start);
            output.error = ParseError{
                .message = "Malformed numeric literal",
                .hint = "Exponent requires at least one digit.",
                .span = span_from_token(token, source_name, source),
            };
            return output;
          }
          const auto text = source.substr(start, cursor - start);
          push(TokenKind::kNumeric, text, text, start, line, start_col);
          continue;
        }
      }
      if (cursor + 3 <= source.size() && source.compare(cursor, 3, "u64") == 0) {
        cursor += 3;
        col += 3;
      }
      const auto text = source.substr(start, cursor - start);
      push(TokenKind::kNumeric, text, text, start, line, start_col);
      continue;
    }
    if (is_ident_start(ch)) {
      const std::size_t start = cursor;
      const int start_col = col;
      while (cursor < source.size() && is_ident_char(source[cursor])) {
        ++cursor;
        ++col;
      }
      if (const auto text = source.substr(start, cursor - start); text == "True" || text == "False") {
        push(TokenKind::kBool, text, text, start, line, start_col);
      } else if (text == "null") {
        push(TokenKind::kNull, text, text, start, line, start_col);
      } else {
        push(TokenKind::kIdent, text, text, start, line, start_col);
      }
      continue;
    }

    static constexpr std::string_view kSingle = "()[]{},:;.=+-*/%^!<>|";

    if (kSingle.find(ch) != std::string::npos) {
      const std::string text(1, ch);
      push(TokenKind::kSymbol, text, text, cursor, line, col);
      ++cursor;
      ++col;
      continue;
    }

    if (mode == LexMode::kStrict) {
      Token token;
      token.kind = TokenKind::kSymbol;
      token.offset = cursor;
      token.line = line;
      token.col = col;
      token.text = std::string(1, ch);
      output.error = ParseError{
          .message = std::format("Unexpected character '{}'", ch),
          .hint = std::nullopt,
          .span = span_from_token(token, source_name, source),
      };
      return output;
    }

    push(TokenKind::kError, std::string(1, ch), std::string(1, ch), cursor, line, col);

    ++cursor;
    ++col;
  }
  push(TokenKind::kEof, "", "", cursor, line, col);

  return output;
}

}  // namespace

auto keyword_spellings() -> std::span<const std::string_view> { return kKeywords; }

auto is_keyword(const std::string_view text) -> bool { return std::ranges::find(kKeywords, text) != kKeywords.end(); }

auto is_structural_keyword(const std::string_view text) -> bool {
  return std::ranges::find(kStructuralKeywords, text) != kStructuralKeywords.end();
}

auto is_operator_symbol(const std::string_view text) -> bool {
  return std::ranges::find(kOperators, text) != kOperators.end();
}

auto is_simple_type_name(const std::string_view text) -> bool {
  return std::ranges::find(kSimpleTypes, text) != kSimpleTypes.end();
}

auto lex_program(const std::string& source, const std::string& source_name) -> LexResult {
  auto output = lex_impl(source, source_name, LexMode::kStrict);
  if (output.error.has_value()) {
    return tl::unexpected(*output.error);
  }
  return std::move(output.tokens);
}

auto lex_program_best_effort(const std::string& source, const std::string& source_name) -> std::vector<Token> {
  return lex_impl(source, source_name, LexMode::kBestEffort).tokens;
}

auto lex_program_with_comments(const std::string& source, const std::string& source_name)
    -> tl::expected<LexedSource, ParseError> {
  auto output = lex_impl(source, source_name, LexMode::kStrict);
  if (output.error.has_value()) {
    return tl::unexpected(*output.error);
  }
  return LexedSource{.tokens = std::move(output.tokens), .comments = std::move(output.comments)};
}

}  // namespace fleaux::frontend::parse