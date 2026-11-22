#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tl/expected.hpp>

#include "fleaux/frontend/parser.hpp"

namespace fleaux::frontend::parse {

enum class TokenKind {
  kIdent,
  kNumeric,
  kString,
  kBool,
  kNull,
  kSymbol,
  kError,
  kEof,
};

struct Token {
  TokenKind kind = TokenKind::kEof;
  std::string value;
  std::size_t offset = 0;
  int line = 1;
  int col = 1;
  std::string text;

  // The line and column just past the token. A raw string literal spans lines, so both come from its text.
  [[nodiscard]] auto end_line() const -> int { return line + static_cast<int>(std::ranges::count(text, '\n')); }
  [[nodiscard]] auto end_col() const -> int {
    const auto last_newline = text.rfind('\n');
    if (last_newline == std::string::npos) {
      return col + static_cast<int>(text.size());
    }
    return static_cast<int>(text.size() - last_newline);
  }
};

// A '//' line comment. The lexer skips comments; the formatter needs them back.
struct Comment {
  // From '//' to the end of the line, without trailing whitespace.
  std::string text;
  std::size_t offset = 0;
  int line = 1;
  int col = 1;
};

struct LexedSource {
  std::vector<Token> tokens;
  std::vector<Comment> comments;
};

using LexResult = tl::expected<std::vector<Token>, ParseError>;

[[nodiscard]] auto keyword_spellings() -> std::span<const std::string_view>;
[[nodiscard]] auto is_keyword(std::string_view text) -> bool;
[[nodiscard]] auto is_structural_keyword(std::string_view text) -> bool;
[[nodiscard]] auto is_operator_symbol(std::string_view text) -> bool;
[[nodiscard]] auto is_simple_type_name(std::string_view text) -> bool;
[[nodiscard]] auto lex_program(const std::string& source, const std::string& source_name) -> LexResult;
[[nodiscard]] auto lex_program_best_effort(const std::string& source, const std::string& source_name)
    -> std::vector<Token>;
// Like lex_program, and also returns the comments in source order.
[[nodiscard]] auto lex_program_with_comments(const std::string& source, const std::string& source_name)
    -> tl::expected<LexedSource, ParseError>;

}  // namespace fleaux::frontend::parse
