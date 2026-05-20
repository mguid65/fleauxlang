#include "fleaux/frontend/lexer.hpp"
#include <array>
#include <cctype>
#include <format>
#include <optional>
#include <ranges>

namespace fleaux::frontend::parse {
namespace {

enum class LexMode {
  kStrict,
  kBestEffort,
};

struct LexOutput {
  std::vector<Token> tokens;
  std::optional<ParseError> error{std::nullopt};
};

constexpr std::array<std::string_view, 13> kKeywords = {"let",    "import",  "type",       "alias", "Int64",
                                                        "UInt64", "Float64", "String",     "Bool",  "Null",
                                                        "Any",    "Tuple",   "__builtin__"};

constexpr std::array<std::string_view, 5> kStructuralKeywords = {"let", "import", "type", "alias", "__builtin__"};

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

auto span_from_token(const Token& token, const std::string& source_name, const std::string& source_text)
    -> std::optional<diag::SourceSpan> {
  diag::SourceSpan span;
  span.source_name = source_name;
  span.source_text = source_text;
  span.line = token.line;
  span.col = token.col;
  span.end_line = token.line;
  span.end_col = token.end_col();
  return span;
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
      while (cursor < source.size() && source[cursor] != '\n') {
        ++cursor;
        ++col;
      }
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
  auto [tokens, error] = lex_impl(source, source_name, LexMode::kStrict);
  if (error.has_value()) {
    return tl::unexpected(*error);
  }
  return tokens;
}

auto lex_program_best_effort(const std::string& source, const std::string& source_name) -> std::vector<Token> {
  return lex_impl(source, source_name, LexMode::kBestEffort).tokens;
}

}  // namespace fleaux::frontend::parse