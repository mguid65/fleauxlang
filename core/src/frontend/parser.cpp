#include "fleaux/frontend/parser.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace fleaux::frontend::parse {
namespace {

template <typename T>
using PResult = tl::expected<T, ParseError>;

#define FLEAUX_TRY_ASSIGN(name, expr)             \
  auto name##_result = (expr);                    \
  if (!name##_result)                             \
    return tl::unexpected(name##_result.error()); \
  auto name = std::move(*name##_result)

#define FLEAUX_TRYV(expr)                            \
  do {                                               \
    auto _fleaux_result = (expr);                    \
    if (!_fleaux_result)                             \
      return tl::unexpected(_fleaux_result.error()); \
  } while (false)

enum class TokenKind {
  kIdent,
  kNumeric,
  kString,
  kBool,
  kNull,
  kSymbol,
  kEof,
};

struct Token {
  TokenKind kind = TokenKind::kEof;
  std::string value;
  int line = 1;
  int col = 1;
  std::string text;

  [[nodiscard]] auto end_col() const -> int { return col + static_cast<int>(text.size()); }
};

const std::unordered_set<std::string> kKeywords = {"let",    "import", "type",      "alias", "Int64",
                                                   "UInt64", "Float64", "String",    "Bool",  "Null",
                                                   "Any",    "Tuple",  "__builtin__"};

const std::unordered_set<std::string> kStructuralKeywords = {"let", "import", "type", "alias", "__builtin__"};

const std::unordered_set<std::string> kOperators = {
    "^", "/", "*", "%", "+", "-", "==", "!=", "<", ">", ">=", "<=", "!", "&&", "||",
};

const std::unordered_set<std::string> kSimpleTypes = {"Int64", "UInt64", "Float64", "String", "Bool", "Null", "Any"};

auto is_ident_start(const char c) -> bool { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_'; }

auto is_ident_char(const char c) -> bool { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

auto starts_with(const std::string& source, const std::size_t index, const std::string& text) -> bool {
  if (index + text.size() > source.size()) {
    return false;
  }
  return source.compare(index, text.size(), text) == 0;
}

auto span_from_token(const Token& token, const std::string& source_name, const std::string& source_text)
    -> std::optional<diag::SourceSpan> {
  diag::SourceSpan span;
  span.source_name = source_name;
  span.source_text = source_text;
  span.line = token.line;
  span.col = token.col;
  span.end_line = token.line;
  span.end_col = (token.kind == TokenKind::kEof) ? token.col : token.end_col();
  return span;
}

auto make_error(const std::string& message, const std::optional<std::string>& hint,
                const std::optional<diag::SourceSpan>& span) -> ParseError {
  ParseError error;
  error.message = message;
  error.hint = hint;
  error.span = span;
  return error;
}

auto lex(const std::string& source, const std::string& source_name) -> PResult<std::vector<Token>> {
  std::vector<Token> out;
  std::size_t cursor = 0;
  int line = 1;
  int col = 1;

  auto push = [&](const TokenKind kind, const std::string& value, const std::string& text, const int tok_line,
                  const int tok_col) -> void {
    Token token;
    token.kind = kind;
    token.value = value;
    token.text = text;
    token.line = tok_line;
    token.col = tok_col;
    out.push_back(std::move(token));
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

    static const std::vector<std::string> kMulti = {"...", "->", "::", "==", "!=", ">=", "<=", "&&", "||"};
    bool matched_multi = false;
    for (const auto& sym : kMulti) {
      if (starts_with(source, cursor, sym)) {
        push(TokenKind::kSymbol, sym, sym, line, col);
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
        Token token;
        token.kind = TokenKind::kString;
        token.line = start_line;
        token.col = start_col;
        token.text = "\"";
        return tl::unexpected(
            make_error("Unterminated string literal", std::nullopt, span_from_token(token, source_name, source)));
      }

      const auto text = source.substr(cursor, str_end - cursor);
      push(TokenKind::kString, decoded, text, start_line, start_col);
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
          Token token;
          token.kind = TokenKind::kNumeric;
          token.line = line;
          token.col = start_col;
          token.text = source.substr(start, cursor - start);
          return tl::unexpected(make_error("Malformed numeric literal", "Exponent requires at least one digit.",
                                           span_from_token(token, source_name, source)));
        }
      }

      if (cursor + 3 <= source.size() && source.compare(cursor, 3, "u64") == 0) {
        cursor += 3;
        col += 3;
      }

      const auto text = source.substr(start, cursor - start);
      push(TokenKind::kNumeric, text, text, line, start_col);
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
        push(TokenKind::kBool, text, text, line, start_col);
      } else if (text == "null") {
        push(TokenKind::kNull, text, text, line, start_col);
      } else {
        push(TokenKind::kIdent, text, text, line, start_col);
      }
      continue;
    }

    static const std::string kSingle = "()[],:;.=+-*/%^!<>|";
    if (kSingle.find(ch) != std::string::npos) {
      const std::string text(1, ch);
      push(TokenKind::kSymbol, text, text, line, col);
      ++cursor;
      ++col;
      continue;
    }

    Token token;
    token.kind = TokenKind::kSymbol;
    token.line = line;
    token.col = col;
    token.text = std::string(1, ch);
    return tl::unexpected(make_error(std::format("Unexpected character '{}'", ch), std::nullopt,
                                     span_from_token(token, source_name, source)));
  }

  push(TokenKind::kEof, "", "", line, col);
  return out;
}

class ParserImpl {
public:
  ParserImpl(std::string source, std::string source_name, std::vector<Token> tokens)
      : source_(std::move(source)), source_name_(std::move(source_name)), tokens_(std::move(tokens)) {
    classify_source_lines();
  }

  auto parse() -> ParseResult {
    model::Program program;
    program.source_text = source_;
    program.source_name = source_name_;

    while (!is(TokenKind::kEof)) {
      FLEAUX_TRY_ASSIGN(parsed_stmt, stmt());
      program.statements.push_back(std::move(parsed_stmt));
      FLEAUX_TRYV(eat_symbol(";"));
    }

    if (!program.statements.empty()) {
      const auto first_span = statement_span(program.statements.front());
      if (const auto last_span = statement_span(program.statements.back()); first_span || last_span) {
        program.span = diag::merge_source_spans(first_span, last_span);
      }
    }

    return program;
  }

private:
  std::string source_;
  std::string source_name_;
  std::vector<Token> tokens_;
  std::size_t i_ = 0;
  std::vector<bool> source_line_is_blank_;
  std::vector<std::optional<std::string>> source_line_comment_;

  [[nodiscard]] static auto trim_copy(const std::string_view text) -> std::string {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
      return {};
    }
    const auto last = text.find_last_not_of(" \t\r");
    return std::string{text.substr(first, last - first + 1)};
  }

  void classify_source_lines() {
    source_line_is_blank_.assign(1, false);
    source_line_comment_.assign(1, std::nullopt);

    std::size_t line_start = 0;
    while (line_start <= source_.size()) {
      const std::size_t newline = source_.find('\n', line_start);
      const std::size_t line_end = newline == std::string::npos ? source_.size() : newline;
      const std::string_view raw_line(source_.data() + line_start, line_end - line_start);
      const std::string trimmed = trim_copy(raw_line);

      source_line_is_blank_.push_back(trimmed.empty());
      if (trimmed.starts_with("//")) {
        std::string comment_text = trim_copy(std::string_view(trimmed).substr(2));
        source_line_comment_.emplace_back(std::move(comment_text));
      } else {
        source_line_comment_.emplace_back(std::nullopt);
      }

      if (newline == std::string::npos) {
        break;
      }
      line_start = newline + 1;
    }
  }

  [[nodiscard]] auto doc_comments_for_line(const int line) const -> std::vector<std::string> {
    if (line <= 1 || static_cast<std::size_t>(line) >= source_line_is_blank_.size()) {
      return {};
    }

    int cursor = line - 1;
    if (source_line_is_blank_[static_cast<std::size_t>(cursor)]) {
      return {};
    }
    if (!source_line_comment_[static_cast<std::size_t>(cursor)].has_value()) {
      return {};
    }

    std::vector<std::string> comments;
    while (cursor >= 1) {
      const auto idx = static_cast<std::size_t>(cursor);
      if (source_line_comment_[idx].has_value()) {
        comments.push_back(*source_line_comment_[idx]);
        --cursor;
        continue;
      }
      if (source_line_is_blank_[idx]) {
        comments.clear();
        break;
      }
      break;
    }

    std::ranges::reverse(comments);
    return comments;
  }

  [[nodiscard]] auto peek() const -> const Token& { return tokens_[i_]; }

  [[nodiscard]] auto peek_ahead(const std::size_t offset) const -> const Token* {
    const std::size_t index = i_ + offset;
    if (index >= tokens_.size()) {
      return nullptr;
    }
    return &tokens_[index];
  }

  [[nodiscard]] auto previous_token() const -> const Token& {
    if (i_ == 0) {
      return tokens_.front();
    }
    return tokens_[i_ - 1];
  }

  [[nodiscard]] auto is(const TokenKind kind) const -> bool { return peek().kind == kind; }

  [[nodiscard]] auto is_symbol(const std::string& symbol) const -> bool {
    return is(TokenKind::kSymbol) && peek().value == symbol;
  }

  [[nodiscard]] auto is_ident_value(const std::string& ident) const -> bool {
    return is(TokenKind::kIdent) && peek().value == ident;
  }

  [[nodiscard]] auto span_from_token(const Token& token) const -> std::optional<diag::SourceSpan> {
    return ::fleaux::frontend::parse::span_from_token(token, source_name_, source_);
  }

  [[nodiscard]] auto span_from_mark(const std::size_t start) const -> std::optional<diag::SourceSpan> {
    const Token& start_tok = tokens_[start];
    const Token& end_tok = (i_ > start) ? previous_token() : start_tok;

    diag::SourceSpan span;
    span.source_name = source_name_;
    span.source_text = source_;
    span.line = start_tok.line;
    span.col = start_tok.col;
    span.end_line = end_tok.line;
    span.end_col = (end_tok.kind == TokenKind::kEof) ? end_tok.col : end_tok.end_col();
    return span;
  }

  [[nodiscard]] auto err(const std::string& message, const std::optional<Token>& tok = std::nullopt,
                         const std::optional<std::string>& hint = std::nullopt) const -> ParseError {
    const Token& use = tok.has_value() ? *tok : peek();
    return make_error(message, hint, span_from_token(use));
  }

  auto next() -> const Token& {
    const Token& tok = tokens_[i_];
    ++i_;
    return tok;
  }

  auto match_symbol(const std::string& symbol) -> bool {
    if (is_symbol(symbol)) {
      ++i_;
      return true;
    }
    return false;
  }

  // Lookahead to check if current position looks like a function type: (TypeList) =>
  // We need to scan ahead to find matching ) and then check for => (either as one token or = + >).
  [[nodiscard]] auto is_function_type_ahead() const -> bool {
    if (!is_symbol("("))
      return false;

    // Quick heuristic: scan forward for ) followed by => or = >
    std::size_t scan_pos = i_ + 1;
    int paren_depth = 1;

    while (scan_pos < tokens_.size() && paren_depth > 0) {
      if (const auto& tok = tokens_[scan_pos]; tok.kind == TokenKind::kSymbol) {
        if (tok.value == "(") {
          ++paren_depth;
        } else if (tok.value == ")") {
          --paren_depth;
          if (paren_depth == 0) {
            // Found the matching ). Check if next token is => (single token) or = > (two tokens)
            if (scan_pos + 1 < tokens_.size()) {
              const auto& next_tok = tokens_[scan_pos + 1];
              if (next_tok.kind == TokenKind::kSymbol && next_tok.value == "=>") {
                return true;
              }
              // Check for = followed by >
              if (next_tok.kind == TokenKind::kSymbol && next_tok.value == "=" && scan_pos + 2 < tokens_.size()) {
                const auto& after_eq = tokens_[scan_pos + 2];
                return after_eq.kind == TokenKind::kSymbol && after_eq.value == ">";
              }
            }
            return false;
          }
        }
      }
      ++scan_pos;
    }
    return false;
  }

  auto eat_symbol(const std::string& symbol) -> PResult<Token> {
    if (const Token& tok = peek(); !is_symbol(symbol)) {
      const std::string got = tok.kind == TokenKind::kEof ? "end of input" : std::format("'{}'", tok.value);
      return tl::unexpected(
          err(std::format("Expected '{}', got {}", symbol, got), tok, hint_for_expected_token(symbol, tok)));
    }
    return next();
  }

  template <typename... Symbols>
  auto eat_one_of(Symbols&&... symbols) -> PResult<Token> {
    if (const Token& tok = peek(); !(is_symbol(symbols) || ...)) {
      const std::string got = tok.kind == TokenKind::kEof ? "end of input" : std::format("'{}'", tok.value);
      const auto symbol_list_str = [&]() -> std::string {
        std::string result;
        ((result += (result.empty() ? "" : ", ") + std::format("'{}'", symbols)), ...);
        return result;
      }();

      return tl::unexpected(err(std::format("Expected one of [{}], got {}", symbol_list_str, got), tok));
    }
    return next();
  }

  auto eat_ident_token() -> PResult<Token> {
    if (const Token& tok = peek(); tok.kind != TokenKind::kIdent) {
      const std::string got = tok.kind == TokenKind::kEof ? "end of input" : std::format("'{}'", tok.value);
      return tl::unexpected(
          err(std::format("Expected 'IDENT', got {}", got), tok, hint_for_expected_token("IDENT", tok)));
    }
    return next();
  }

  auto eat_ident_value(const std::string& ident) -> PResult<Token> {
    if (const Token& tok = peek(); tok.kind != TokenKind::kIdent || tok.value != ident) {
      const std::string got = tok.kind == TokenKind::kEof ? "end of input" : std::format("'{}'", tok.value);
      return tl::unexpected(err(std::format("Expected '{}', got {}", ident, got), tok,
                                "Check keyword spelling and statement structure."));
    }
    return next();
  }

  auto ident() -> PResult<std::string> {
    FLEAUX_TRY_ASSIGN(tok, eat_ident_token());
    if (kKeywords.contains(tok.value)) {
      return tl::unexpected(err(std::format("Keyword '{}' cannot be used as an identifier", tok.value), tok));
    }
    return tok.value;
  }

  auto ns_ident() -> PResult<std::string> {
    FLEAUX_TRY_ASSIGN(tok, eat_ident_token());
    if (kStructuralKeywords.contains(tok.value)) {
      return tl::unexpected(err(std::format("Keyword '{}' cannot be used as a namespace segment", tok.value), tok));
    }
    return tok.value;
  }

  auto opt_qid() -> PResult<std::variant<std::string, model::QualifiedId>> {
    const std::size_t start = i_;
    FLEAUX_TRY_ASSIGN(head, ident());
    std::vector<std::string> parts{head};
    while (match_symbol(".")) {
      FLEAUX_TRY_ASSIGN(segment, ns_ident());
      parts.push_back(std::move(segment));
    }

    if (parts.size() == 1U) {
      return head;
    }

    model::Qualifier q;
    q.qualifier.reserve(parts.size() * 2U);
    for (std::size_t idx = 0; idx + 1U < parts.size(); ++idx) {
      if (idx > 0U) {
        q.qualifier += ".";
      }
      q.qualifier += parts[idx];
    }
    q.span = span_from_mark(start);

    model::QualifiedId out;
    out.qualifier = q;
    out.id = parts.back();
    out.span = span_from_mark(start);
    return out;
  }

  auto generic_param_list() -> PResult<std::vector<std::string>> {
    FLEAUX_TRYV(eat_symbol("<"));

    std::vector<std::string> params;
    if (is_symbol(">")) {
      return tl::unexpected(err("Generic parameter list cannot be empty.", std::nullopt,
                                "Add at least one parameter name, for example: <T>."));
    }

    while (true) {
      FLEAUX_TRY_ASSIGN(param_name, ident());
      params.push_back(std::move(param_name));
      if (!match_symbol(",")) {
        break;
      }
      if (is_symbol(">")) {
        return tl::unexpected(err("Trailing comma in generic parameter list.", std::nullopt,
                                  "Remove the trailing comma or add another parameter name."));
      }
    }

    FLEAUX_TRYV(eat_symbol(">"));
    return params;
  }

  auto explicit_type_arg_list() -> PResult<std::vector<model::TypeNode>> {
    FLEAUX_TRYV(eat_symbol("<"));

    std::vector<model::TypeNode> args;
    if (is_symbol(">")) {
      return tl::unexpected(err("Explicit type argument list cannot be empty.", std::nullopt,
                                "Add at least one type, for example: <Int64>."));
    }

    while (true) {
      FLEAUX_TRY_ASSIGN(type_arg, type());
      args.push_back(std::move(type_arg));
      if (!match_symbol(",")) {
        break;
      }
      if (is_symbol(">")) {
        return tl::unexpected(err("Trailing comma in explicit type argument list.", std::nullopt,
                                  "Remove the trailing comma or add another type argument."));
      }
    }

    FLEAUX_TRYV(eat_symbol(">"));
    return args;
  }

  auto type() -> PResult<model::TypeNode> {
    const std::size_t start = i_;
    model::TypeNode base;

    // Check for function type: (TypeList) => ReturnType
    if (is_symbol("(") && is_function_type_ahead()) {
      next();  // consume '('
      model::TypeList param_types;
      if (!is_symbol(")")) {
        while (true) {
          FLEAUX_TRY_ASSIGN(t, type());
          param_types.types.emplace_back(std::move(t));
          if (!match_symbol(",")) {
            break;
          }
        }
      }
      FLEAUX_TRYV(eat_symbol(")"));
      // Handle both => (single token) and = > (two tokens)
      if (is_symbol("=>")) {
        next();  // consume '=>'
      } else {
        FLEAUX_TRYV(eat_symbol("="));
        FLEAUX_TRYV(eat_symbol(">"));
      }
      FLEAUX_TRY_ASSIGN(return_type, type());

      model::FunctionTypeNode func;
      func.params = param_types;
      func.params.span = span_from_mark(start);
      func.return_type = model::TypeBox(std::move(return_type));
      func.span = span_from_mark(start);
      base.value = std::move(func);
      base.span = span_from_mark(start);
      return base;
    }

    if (is_ident_value("Tuple")) {
      next();
      FLEAUX_TRYV(eat_symbol("("));
      model::TypeList type_list;
      if (!is_symbol(")")) {
        while (true) {
          FLEAUX_TRY_ASSIGN(t, type());
          type_list.types.emplace_back(std::move(t));
          if (!match_symbol(",")) {
            break;
          }
        }
      }
      FLEAUX_TRYV(eat_symbol(")"));
      type_list.span = span_from_mark(start);
      base.value = std::move(type_list);
      base.span = span_from_mark(start);
    } else if (is(TokenKind::kIdent) && kSimpleTypes.contains(peek().value)) {
      const auto token = next();
      base.value = token.value;
      base.span = span_from_mark(start);
    } else if (is(TokenKind::kIdent)) {
      FLEAUX_TRY_ASSIGN(qid, opt_qid());
      if (const auto* simple = std::get_if<std::string>(&qid); simple != nullptr) {
        // Check for applied type syntax: Name(TypeArg, ...) e.g. Dict(String, Any)
        if (is_symbol("(")) {
          next();  // consume '('
          model::AppliedTypeNode applied;
          applied.name = *simple;
          if (!is_symbol(")")) {
            while (true) {
              FLEAUX_TRY_ASSIGN(t, type());
              applied.args.types.emplace_back(std::move(t));
              if (!match_symbol(",")) {
                break;
              }
            }
          }
          FLEAUX_TRYV(eat_symbol(")"));
          applied.span = span_from_mark(start);
          applied.args.span = applied.span;
          base.value = std::move(applied);
        } else {
          base.value = *simple;
        }
      } else if (const auto* qualified = std::get_if<model::QualifiedId>(&qid); qualified != nullptr) {
        base.value = *qualified;
      }
      base.span = span_from_mark(start);
    } else {
      return tl::unexpected(err("Expected type"));
    }

    if (match_symbol("...")) {
      base.variadic = true;
      return base;
    }

    // Union type: T | U | V
    if (is_symbol("|")) {
      model::UnionTypeList union_list;
      union_list.alternatives.emplace_back(base);
      while (match_symbol("|")) {
        FLEAUX_TRY_ASSIGN(t, type());
        union_list.alternatives.emplace_back(std::move(t));
      }
      union_list.span = span_from_mark(start);
      model::TypeNode union_node;
      union_node.value = std::move(union_list);
      union_node.span = span_from_mark(start);
      return union_node;
    }

    return base;
  }

  [[nodiscard]] static auto is_call_target_primary(const model::Primary& primary) -> bool {
    if (std::holds_alternative<model::QualifiedId>(primary.base.value) ||
        std::holds_alternative<std::string>(primary.base.value)) {
      return true;
    }

    if (const auto* named_target = std::get_if<model::NamedTargetBox>(&primary.base.value); named_target != nullptr) {
      return std::holds_alternative<model::QualifiedId>((*named_target)->target) ||
             std::holds_alternative<std::string>((*named_target)->target);
    }

    return false;
  }

  auto expr(const bool allow_ungrouped_closure_stage_split = true) -> PResult<model::Expression> {
    const std::size_t start = i_;
    model::Expression out;
    FLEAUX_TRY_ASSIGN(parsed_flow, flow(allow_ungrouped_closure_stage_split));
    out.expr = std::move(parsed_flow);
    out.span = span_from_mark(start);
    return out;
  }

  auto closure_stage_flow() -> PResult<model::FlowExpression> {
    const std::size_t start = i_;
    model::FlowExpression out;
    FLEAUX_TRY_ASSIGN(lhs, primary());
    out.lhs = std::move(lhs);

    while (match_symbol("->")) {
      FLEAUX_TRY_ASSIGN(stage, primary());
      out.rhs.push_back(stage);
      if (is_call_target_primary(stage)) {
        break;
      }
    }

    out.span = span_from_mark(start);
    return out;
  }

  auto closure_stage_expr() -> PResult<model::Expression> {
    const std::size_t start = i_;
    model::Expression out;
    FLEAUX_TRY_ASSIGN(parsed_flow, closure_stage_flow());
    out.expr = std::move(parsed_flow);
    out.span = span_from_mark(start);
    return out;
  }

  auto parse_closure_after_open_paren(const std::size_t closure_start_index, const std::size_t open_paren_index,
                                      std::vector<std::string> generic_params,
                                      const bool allow_ungrouped_closure_stage_split, bool& committed)
      -> PResult<model::Atom> {
    model::Atom out_atom;

    std::vector<model::Parameter> params;
    if (!is_symbol(")")) {
      while (true) {
        if (peek().kind != TokenKind::kIdent) {
          return tl::unexpected(err("Expected closure parameter name", peek(),
                                    "Closure parameters must be declared as name/type pairs, for example: (x: T)."));
        }

        const std::size_t param_start = i_;
        model::Parameter p;
        auto parsed_ident = ident();
        if (!parsed_ident) {
          return tl::unexpected(parsed_ident.error());
        }
        p.param_name = std::move(*parsed_ident);
        if (!match_symbol(":")) {
          return tl::unexpected(err("Expected ':' in closure parameter list", peek(),
                                    "Declare parameters as name/type pairs, for example: (x: Float64)."));
        }
        auto parsed_type = type();
        if (!parsed_type) {
          return tl::unexpected(parsed_type.error());
        }
        p.type = std::move(*parsed_type);
        p.span = span_from_mark(param_start);
        params.push_back(std::move(p));

        if (!match_symbol(",")) {
          break;
        }
      }
    }

    FLEAUX_TRYV(eat_symbol(")"));
    FLEAUX_TRYV(eat_symbol(":"));

    model::ClosureExpression closure;
    closure.generic_params = std::move(generic_params);
    closure.params.params = std::move(params);
    closure.params.span = span_from_mark(open_paren_index);
    auto parsed_rtype = type();
    if (!parsed_rtype) {
      return tl::unexpected(parsed_rtype.error());
    }
    closure.rtype = std::move(*parsed_rtype);

    // Committed: we have parsed a full closure signature; any failure after this is a hard error.
    committed = true;

    FLEAUX_TRYV(eat_one_of("::", "="));

    if (allow_ungrouped_closure_stage_split) {
      auto parsed_body = closure_stage_expr();
      if (!parsed_body) {
        return tl::unexpected(parsed_body.error());
      }
      closure.body = std::move(*parsed_body);
    } else {
      auto parsed_body = expr();
      if (!parsed_body) {
        return tl::unexpected(parsed_body.error());
      }
      closure.body = std::move(*parsed_body);
    }
    closure.span = span_from_mark(closure_start_index);
    out_atom.value = std::move(closure);
    out_atom.span = span_from_mark(closure_start_index);
    return out_atom;
  }

  // Returns true if a closure was parsed into out_atom, false if the token stream does not
  // look like a closure (speculative mismatch), or an error if a closure signature was
  // committed (params + ':' + rtype fully parsed) but the body separator or body was malformed.
  auto try_parse_closure_after_open_paren(const std::size_t open_paren_index, model::Atom& out_atom,
                                          const bool allow_ungrouped_closure_stage_split) -> PResult<bool> {
    const std::size_t checkpoint = i_;
    bool committed = false;
    auto parsed = parse_closure_after_open_paren(open_paren_index, open_paren_index, {},
                                                 allow_ungrouped_closure_stage_split, committed);
    if (!parsed) {
      if (committed) {
        // Signature was complete; propagate the hard error rather than silently backtracking.
        return tl::unexpected(parsed.error());
      }
      i_ = checkpoint;
      return false;
    }
    out_atom = std::move(*parsed);
    return true;
  }

  auto flow(const bool allow_ungrouped_closure_stage_split = true) -> PResult<model::FlowExpression> {
    const std::size_t start = i_;
    model::FlowExpression out;
    FLEAUX_TRY_ASSIGN(lhs, primary());
    out.lhs = std::move(lhs);
    while (match_symbol("->")) {
      FLEAUX_TRY_ASSIGN(stage, primary(allow_ungrouped_closure_stage_split));
      out.rhs.push_back(std::move(stage));
    }
    out.span = span_from_mark(start);
    return out;
  }

  auto primary(const bool allow_ungrouped_closure_stage_split = false) -> PResult<model::Primary> {
    const std::size_t start = i_;
    model::Primary out;
    FLEAUX_TRY_ASSIGN(parsed_atom, atom(allow_ungrouped_closure_stage_split));
    out.base = std::move(parsed_atom);
    out.span = span_from_mark(start);
    return out;
  }

  auto atom(const bool allow_ungrouped_closure_stage_split = false) -> PResult<model::Atom> {
    const std::size_t start = i_;

    if (is_symbol("<") && peek_ahead(1) != nullptr &&
        (peek_ahead(1)->kind == TokenKind::kIdent || peek_ahead(1)->value == ">")) {
      FLEAUX_TRY_ASSIGN(generic_params, generic_param_list());
      FLEAUX_TRYV(eat_symbol("("));
      bool committed_generic = true;  // already committed by consuming the generic param list
      FLEAUX_TRY_ASSIGN(prefixed_closure,
                        parse_closure_after_open_paren(start, i_ - 1, std::move(generic_params),
                                                       allow_ungrouped_closure_stage_split, committed_generic));
      return prefixed_closure;
    }

    auto parse_number_constant = [&](const Token& num, const bool negate) -> PResult<model::Constant> {
      model::Constant c;
      const bool is_u64 = num.value.ends_with("u64");
      const std::string digits = is_u64 ? num.value.substr(0, num.value.size() - 3) : num.value;
      try {
        if (is_u64) {
          if (negate) {
            return tl::unexpected(
                err("UInt64 literal cannot be negative", num, "Remove the unary '-' or use an Int64/Float64 literal."));
          }
          if (digits.find_first_of(".eE") != std::string::npos) {
            return tl::unexpected(
                err("Invalid UInt64 literal", num, "UInt64 literals must be whole numbers (for example: 42u64)."));
          }
          const auto parsed = std::stoull(digits);
          if (parsed > std::numeric_limits<std::uint64_t>::max()) {
            return tl::unexpected(err("Numeric literal is out of range", num, "Use a smaller UInt64 literal."));
          }
          c.val = static_cast<std::uint64_t>(parsed);
        } else if (digits.find_first_of(".eE") != std::string::npos) {
          const double parsed = std::stod(digits);
          c.val = negate ? -parsed : parsed;
        } else {
          const std::int64_t parsed = std::stoll(digits);
          c.val = negate ? -parsed : parsed;
        }
      } catch (const std::out_of_range&) {
        return tl::unexpected(err("Numeric literal is out of range", num,
                                  "Use a smaller value or write the literal as Float64 if appropriate."));
      } catch (const std::invalid_argument&) {
        return tl::unexpected(err("Invalid numeric literal", num, "Check number formatting."));
      }
      c.span = span_from_mark(start);
      return c;
    };

    if (match_symbol("(")) {
      model::Atom closure_atom;
      FLEAUX_TRY_ASSIGN(is_closure,
                        try_parse_closure_after_open_paren(start, closure_atom, allow_ungrouped_closure_stage_split));
      if (is_closure) {
        return closure_atom;
      }

      model::Atom out;
      if (match_symbol(")")) {
        out.value = std::monostate{};
        out.span = span_from_mark(start);
        return out;
      }

      model::DelimitedExpression inner;
      while (true) {
        FLEAUX_TRY_ASSIGN(item_expr, expr());
        inner.items.emplace_back(std::move(item_expr));
        if (!match_symbol(",")) {
          break;
        }
      }
      FLEAUX_TRYV(eat_symbol(")"));
      inner.span = span_from_mark(start);
      out.value = std::move(inner);
      out.span = span_from_mark(start);
      return out;
    }

    if (is_symbol("-") && peek_ahead(1) != nullptr && peek_ahead(1)->kind == TokenKind::kNumeric) {
      next();
      const Token num = next();
      FLEAUX_TRY_ASSIGN(c, parse_number_constant(num, true));

      model::Atom out;
      out.value = std::move(c);
      out.span = span_from_mark(start);
      return out;
    }

    if (is(TokenKind::kNumeric)) {
      const Token num = next();
      FLEAUX_TRY_ASSIGN(c, parse_number_constant(num, false));

      model::Atom out;
      out.value = std::move(c);
      out.span = span_from_mark(start);
      return out;
    }

    if (is(TokenKind::kString)) {
      const Token str = next();
      model::Constant c;
      c.val = str.value;
      c.span = span_from_mark(start);

      model::Atom out;
      out.value = std::move(c);
      out.span = span_from_mark(start);
      return out;
    }

    if (is(TokenKind::kBool)) {
      const Token b = next();
      model::Constant c;
      c.val = (b.value == "True");
      c.span = span_from_mark(start);

      model::Atom out;
      out.value = std::move(c);
      out.span = span_from_mark(start);
      return out;
    }

    if (is(TokenKind::kNull)) {
      next();
      model::Constant c;
      c.val = std::monostate{};
      c.span = span_from_mark(start);

      model::Atom out;
      out.value = std::move(c);
      out.span = span_from_mark(start);
      return out;
    }

    if (is(TokenKind::kSymbol) && kOperators.contains(peek().value)) {
      model::Atom out;
      out.value = next().value;
      out.span = span_from_mark(start);
      return out;
    }

    if (!is(TokenKind::kIdent)) {
      const Token& tok = peek();
      const std::string got = tok.kind == TokenKind::kEof ? "end of input" : std::format("'{}'", tok.value);
      return tl::unexpected(
          err(std::format("expected an expression, got {}", got), tok, hint_for_expected_expression(tok)));
    }

    FLEAUX_TRY_ASSIGN(q, opt_qid());
    model::NamedTarget named_target;
    named_target.target = std::move(q);
    if (is_symbol("<")) {
      FLEAUX_TRY_ASSIGN(explicit_type_args, explicit_type_arg_list());
      for (auto& type_arg : explicit_type_args) {
        named_target.explicit_type_args.emplace_back(std::move(type_arg));
      }
    }
    named_target.span = span_from_mark(start);

    model::Atom out;
    out.value = model::NamedTargetBox(std::move(named_target));
    out.span = span_from_mark(start);
    return out;
  }

  auto import_module_name() -> PResult<std::string> {
    const Token tok = peek();
    if (tok.kind == TokenKind::kIdent) {
      if (kKeywords.contains(tok.value)) {
        return tl::unexpected(err(std::format("Keyword '{}' cannot be used as an import module name", tok.value), tok));
      }
      return next().value;
    }

    if (tok.kind == TokenKind::kNumeric) {
      std::vector<Token> parts{next()};
      while (peek().kind == TokenKind::kNumeric || peek().kind == TokenKind::kIdent) {
        const Token nxt = peek();
        if (const Token prev = parts.back(); nxt.line != prev.line || nxt.col != prev.end_col()) {
          break;
        }
        parts.push_back(next());
      }

      std::string name;
      for (const auto& part : parts) {
        name += part.value;
      }
      return name;
    }

    return tl::unexpected(err("Expected import module name", tok,
                              "Use a module name like 'Std' or a digit-leading name like '20_export'."));
  }

  auto type_stmt() -> PResult<model::TypeStatement> {
    const std::size_t start = i_;
    FLEAUX_TRYV(eat_ident_value("type"));

    model::TypeStatement out;
    FLEAUX_TRY_ASSIGN(type_name, ident());
    out.name = std::move(type_name);
    FLEAUX_TRYV(eat_one_of("::", "="));
    FLEAUX_TRY_ASSIGN(target_type, type());
    out.target = std::move(target_type);
    out.span = span_from_mark(start);
    return out;
  }

  auto alias_stmt() -> PResult<model::AliasStatement> {
    const std::size_t start = i_;
    FLEAUX_TRYV(eat_ident_value("alias"));

    model::AliasStatement out;
    FLEAUX_TRY_ASSIGN(alias_name, ident());
    out.name = std::move(alias_name);

    if (is_symbol("<")) {
      return tl::unexpected(err("Transparent aliases do not support generic parameter lists.", peek(),
                                "Remove '<...>' from the alias declaration. Generic aliases are not supported in phase 1."));
    }
    if (is_symbol("::")) {
      return tl::unexpected(err("Transparent aliases use '=' as their declaration separator.", peek(),
                                "Write aliases as 'alias Name = Type;'."));
    }

    FLEAUX_TRYV(eat_symbol("="));
    FLEAUX_TRY_ASSIGN(target_type, type());
    out.target = std::move(target_type);
    out.span = span_from_mark(start);
    return out;
  }

  auto let_stmt() -> PResult<model::LetStatement> {
    const std::size_t start = i_;
    FLEAUX_TRYV(eat_ident_value("let"));

    model::LetStatement out;
    out.doc_comments = doc_comments_for_line(tokens_[start].line);
    FLEAUX_TRY_ASSIGN(let_id, opt_qid());
    out.id = std::move(let_id);

    if (is_symbol("<")) {
      FLEAUX_TRY_ASSIGN(generic_params, generic_param_list());
      out.generic_params = std::move(generic_params);
    }

    FLEAUX_TRYV(eat_symbol("("));
    std::vector<model::Parameter> params;
    const std::size_t params_paren_start = i_ - 1;

    if (!is_symbol(")")) {
      while (true) {
        const std::size_t param_start = i_;
        model::Parameter p;
        FLEAUX_TRY_ASSIGN(param_name, ident());
        p.param_name = std::move(param_name);
        FLEAUX_TRYV(eat_symbol(":"));
        FLEAUX_TRY_ASSIGN(param_type, type());
        p.type = std::move(param_type);
        p.span = span_from_mark(param_start);
        params.push_back(std::move(p));
        if (!match_symbol(",")) {
          break;
        }
      }
    }
    FLEAUX_TRYV(eat_symbol(")"));

    out.params.params = std::move(params);
    out.params.span = span_from_mark(params_paren_start);

    FLEAUX_TRYV(eat_symbol(":"));
    FLEAUX_TRY_ASSIGN(rtype, type());
    out.rtype = std::move(rtype);

    FLEAUX_TRYV(eat_one_of("::", "="));

    if (is_ident_value("__builtin__")) {
      next();
      out.is_builtin = true;
      out.expr = std::nullopt;
    } else {
      FLEAUX_TRY_ASSIGN(let_expr, expr());
      out.expr = std::move(let_expr);
    }

    out.span = span_from_mark(start);
    return out;
  }

  auto stmt() -> PResult<model::Statement> {
    if (is_ident_value("import")) {
      const std::size_t start = i_;
      next();
      model::ImportStatement stmt;
      FLEAUX_TRY_ASSIGN(module_name, import_module_name());
      stmt.module_name = std::move(module_name);
      stmt.span = span_from_mark(start);
      return stmt;
    }

    if (is_ident_value("let")) {
      return let_stmt();
    }

    if (is_ident_value("type")) {
      return type_stmt();
    }

    if (is_ident_value("alias")) {
      return alias_stmt();
    }

    const std::size_t start = i_;
    model::ExpressionStatement stmt;
    FLEAUX_TRY_ASSIGN(statement_expr, expr());
    stmt.expr = std::move(statement_expr);
    stmt.span = span_from_mark(start);
    return stmt;
  }

  [[nodiscard]] static auto statement_span(const model::Statement& stmt) -> std::optional<diag::SourceSpan> {
    return std::visit([](const auto& s) -> auto { return s.span; }, stmt);
  }

  [[nodiscard]] auto hint_for_expected_expression(const Token& tok) const -> std::optional<std::string> {
    if (tok.kind == TokenKind::kSymbol) {
      if (tok.value == "->") {
        if (const Token& prev = previous_token(); prev.kind == TokenKind::kSymbol && prev.value == "=") {
          return "The function body is missing after '='. Add an expression, e.g. '= (a, b) -> Std.Add'.";
        }
        return "The left-hand side of '->' is missing. Add an expression before the pipeline operator.";
      }
      if (tok.value == ")") {
        return "Unexpected ')'. Check for an extra closing parenthesis or a missing expression.";
      }
      if (tok.value == ",") {
        return "Unexpected ','. A tuple element is missing before this comma.";
      }
      if (tok.value == ";") {
        return "Unexpected ';'. Remove the extra semicolon or add an expression.";
      }
      if (tok.value == ":") {
        return "Unexpected ':'. Type annotations must be inside parameter or let definitions.";
      }
      if (tok.value == "=") {
        return "Unexpected '='. Assignment is not an expression; use '::' or '=' in a let definition.";
      }
    }
    return "Valid expressions start with a literal, an identifier, a tuple '( )', or an operator.";
  }

  [[nodiscard]] auto hint_for_expected_token(const std::string& expected, const Token& got_tok) const
      -> std::optional<std::string> {
    const Token& prev_tok = previous_token();

    if (expected == ";") {
      if (got_tok.kind == TokenKind::kEof) {
        return "Add ';' to terminate the final statement. Every statement in Fleaux must end with a semicolon.";
      }
      if (is_statement_start(got_tok)) {
        return "Add ';' before this next statement. Fleaux requires semicolons between statements.";
      }
      return "Terminate the current statement with ';'.";
    }

    if (expected == ",") {
      if (is_expression_start(got_tok)) {
        return "Add ',' between tuple elements or argument entries.";
      }
      return "Separate entries with ','.";
    }

    if (expected == ")") {
      if (got_tok.kind == TokenKind::kSymbol && got_tok.value == "->") {
        return "Close the current tuple/parameter list with ')' before continuing the pipeline.";
      }
      if (is_expression_start(got_tok)) {
        if (prev_tok.kind == TokenKind::kNumeric || prev_tok.kind == TokenKind::kString ||
            prev_tok.kind == TokenKind::kBool || prev_tok.kind == TokenKind::kNull ||
            prev_tok.kind == TokenKind::kIdent || (prev_tok.kind == TokenKind::kSymbol && prev_tok.value == ")")) {
          return "Did you forget a comma? Add ',' between tuple elements or function arguments.";
        }
      }
      return "Close the current tuple/parameter list with ')'.";
    }

    if (expected == ":") {
      return "Add ':' before a type annotation (for example: x: Float64).";
    }

    if (expected == "IDENT") {
      if (prev_tok.kind == TokenKind::kSymbol && prev_tok.value == "->") {
        return "A pipeline stage is missing after '->'. Use a call target like Std.Add, MyFunc, or +.";
      }
      return "Use a valid identifier (letters/underscore, then letters/digits/underscore).";
    }

    return "Check for a missing or misplaced token earlier in the statement.";
  }

  [[nodiscard]] static auto is_expression_start(const Token& tok) -> bool {
    if (tok.kind == TokenKind::kNumeric || tok.kind == TokenKind::kString || tok.kind == TokenKind::kBool ||
        tok.kind == TokenKind::kNull || tok.kind == TokenKind::kIdent) {
      return true;
    }
    if (tok.kind == TokenKind::kSymbol && (tok.value == "(" || tok.value == "-" || kOperators.contains(tok.value))) {
      return true;
    }
    return false;
  }

  [[nodiscard]] static auto is_statement_start(const Token& tok) -> bool {
    if (tok.kind == TokenKind::kIdent || tok.kind == TokenKind::kNumeric || tok.kind == TokenKind::kString ||
        tok.kind == TokenKind::kBool || tok.kind == TokenKind::kNull) {
      return true;
    }
    if (tok.kind == TokenKind::kSymbol && (tok.value == "(" || kOperators.contains(tok.value))) {
      return true;
    }
    return false;
  }
};

}  // namespace

auto Parser::parse_program(const std::string& source, const std::string& source_name) const -> ParseResult {
  auto tokens = lex(source, source_name);
  if (!tokens) {
    return tl::unexpected(tokens.error());
  }

  ParserImpl impl(source, source_name, std::move(*tokens));
  return impl.parse();
}

}  // namespace fleaux::frontend::parse
