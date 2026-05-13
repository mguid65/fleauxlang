#pragma once

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

  [[nodiscard]] auto end_col() const -> int { return col + static_cast<int>(text.size()); }
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

}  // namespace fleaux::frontend::parse
