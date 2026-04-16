#include "fleaux/frontend/parser.hpp"

#include <cctype>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace fleaux::frontend::parse {
namespace {

enum class TokenKind {
  kIdent,
  kNumber,
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

const std::unordered_set<std::string> kKeywords = {"let",  "import", "Number", "String",     "Bool",
                                                   "Null", "Any",    "Tuple",  "__builtin__"};

const std::unordered_set<std::string> kStructuralKeywords = {"let", "import", "__builtin__"};

const std::unordered_set<std::string> kOperators = {
    "^", "/", "*", "%", "+", "-", "==", "!=", "<", ">", ">=", "<=", "!", "&&", "||",
};

const std::unordered_set<std::string> kSimpleTypes = {"Number", "String", "Bool", "Null", "Any"};

auto is_ident_start(const char c) -> bool { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_'; }

auto is_ident_char(const char c) -> bool { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

auto starts_with(const std::string& source, const std::size_t index, const std::string& text) -> bool {
  if (index + text.size() > source.size()) { return false; }
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

auto lex_or_throw(const std::string& source, const std::string& source_name, ParseError& error) -> std::vector<Token> {
  std::vector<Token> out;
  std::size_t i = 0;
  int line = 1;
  int col = 1;

  auto push = [&](const TokenKind kind, const std::string& value, const std::string& text, const int l,
                  const int c) -> void {
    Token t;
    t.kind = kind;
    t.value = value;
    t.text = text;
    t.line = l;
    t.col = c;
    out.push_back(std::move(t));
  };

  while (i < source.size()) {
    const char c = source[i];

    if (c == ' ' || c == '\t' || c == '\r') {
      ++i;
      ++col;
      continue;
    }
    if (c == '\n') {
      ++i;
      ++line;
      col = 1;
      continue;
    }
    if (starts_with(source, i, "//")) {
      while (i < source.size() && source[i] != '\n') {
        ++i;
        ++col;
      }
      continue;
    }

    static const std::vector<std::string> kMulti = {"...", "->", "::", "==", "!=", ">=", "<=", "&&", "||"};
    bool matched_multi = false;
    for (const auto& sym : kMulti) {
      if (starts_with(source, i, sym)) {
        push(TokenKind::kSymbol, sym, sym, line, col);
        i += sym.size();
        col += static_cast<int>(sym.size());
        matched_multi = true;
        break;
      }
    }
    if (matched_multi) { continue; }

    if (c == '"') {
      const int start_line = line;
      const int start_col = col;
      std::string decoded;
      std::size_t j = i + 1;
      int local_col = col + 1;
      bool closed = false;

      while (j < source.size()) {
        char ch = source[j];
        if (ch == '"') {
          closed = true;
          ++j;
          ++local_col;
          break;
        }
        if (ch == '\\') {
          if (j + 1 >= source.size()) { break; }
          switch (const char esc = source[j + 1]) {
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
          j += 2;
          local_col += 2;
          continue;
        }
        if (ch == '\n') { break; }
        decoded.push_back(ch);
        ++j;
        ++local_col;
      }

      if (!closed) {
        Token tok;
        tok.kind = TokenKind::kString;
        tok.line = start_line;
        tok.col = start_col;
        tok.text = "\"";
        error = make_error("Unterminated string literal", std::nullopt, span_from_token(tok, source_name, source));
        return {};
      }

      const auto text = source.substr(i, j - i);
      push(TokenKind::kString, decoded, text, start_line, start_col);
      col = local_col;
      i = j;
      continue;
    }

    if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
      const std::size_t start = i;
      const int start_col = col;

      while (i < source.size() && std::isdigit(static_cast<unsigned char>(source[i])) != 0) {
        ++i;
        ++col;
      }
      if (i < source.size() && source[i] == '.') {
        ++i;
        ++col;
        while (i < source.size() && std::isdigit(static_cast<unsigned char>(source[i])) != 0) {
          ++i;
          ++col;
        }
      }
      if (i < source.size() && (source[i] == 'e' || source[i] == 'E')) {
        ++i;
        ++col;
        if (i < source.size() && (source[i] == '+' || source[i] == '-')) {
          ++i;
          ++col;
        }
        while (i < source.size() && std::isdigit(static_cast<unsigned char>(source[i])) != 0) {
          ++i;
          ++col;
        }
      }

      const auto text = source.substr(start, i - start);
      push(TokenKind::kNumber, text, text, line, start_col);
      continue;
    }

    if (is_ident_start(c)) {
      const std::size_t start = i;
      const int start_col = col;
      while (i < source.size() && is_ident_char(source[i])) {
        ++i;
        ++col;
      }
      if (const auto text = source.substr(start, i - start); text == "True" || text == "False") {
        push(TokenKind::kBool, text, text, line, start_col);
      } else if (text == "null") {
        push(TokenKind::kNull, text, text, line, start_col);
      } else {
        push(TokenKind::kIdent, text, text, line, start_col);
      }
      continue;
    }

    static const std::string kSingle = "()[],:;.=+-*/%^!<>";
    if (kSingle.find(c) != std::string::npos) {
      const std::string text(1, c);
      push(TokenKind::kSymbol, text, text, line, col);
      ++i;
      ++col;
      continue;
    }

    Token tok;
    tok.kind = TokenKind::kSymbol;
    tok.line = line;
    tok.col = col;
    tok.text = std::string(1, c);
    error = make_error("Unexpected character '" + std::string(1, c) + "'", std::nullopt,
                       span_from_token(tok, source_name, source));
    return {};
  }

  push(TokenKind::kEof, "", "", line, col);
  return out;
}

class ParserImpl {
public:
  ParserImpl(std::string source, std::string source_name, std::vector<Token> tokens)
      : source_(std::move(source)), source_name_(std::move(source_name)), tokens_(std::move(tokens)) {}

  auto parse() -> ParseResult {
    model::Program program;
    program.source_text = source_;
    program.source_name = source_name_;

    while (!is(TokenKind::kEof)) {
      program.statements.push_back(stmt());
      eat_symbol(";");
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

  [[nodiscard]] auto peek() const -> const Token& { return tokens_[i_]; }

  [[nodiscard]] auto peek_ahead(std::size_t offset) const -> const Token* {
    const std::size_t index = i_ + offset;
    if (index >= tokens_.size()) { return nullptr; }
    return &tokens_[index];
  }

  [[nodiscard]] auto previous_token() const -> const Token& {
    if (i_ == 0) { return tokens_.front(); }
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

  [[noreturn]] void err(const std::string& message, const std::optional<Token>& tok = std::nullopt,
                        const std::optional<std::string>& hint = std::nullopt) const {
    const Token& use = tok.has_value() ? *tok : peek();
    throw make_error(message, hint, span_from_token(use));
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

  auto eat_symbol(const std::string& symbol) -> const Token& {
    if (const Token& tok = peek(); !is_symbol(symbol)) {
      const std::string got = tok.kind == TokenKind::kEof ? "end of input" : ("'" + tok.value + "'");
      err("Expected '" + symbol + "', got " + got, tok, hint_for_expected_token(symbol, tok));
    }
    return next();
  }

  auto eat_ident_token() -> const Token& {
    if (const Token& tok = peek(); tok.kind != TokenKind::kIdent) {
      const std::string got = tok.kind == TokenKind::kEof ? "end of input" : ("'" + tok.value + "'");
      err("Expected 'IDENT', got " + got, tok, hint_for_expected_token("IDENT", tok));
    }
    return next();
  }

  auto eat_ident_value(const std::string& ident) -> const Token& {
    if (const Token& tok = peek(); tok.kind != TokenKind::kIdent || tok.value != ident) {
      const std::string got = tok.kind == TokenKind::kEof ? "end of input" : ("'" + tok.value + "'");
      err("Expected '" + ident + "', got " + got, tok, "Check keyword spelling and statement structure.");
    }
    return next();
  }

  auto ident() -> std::string {
    const Token tok = eat_ident_token();
    if (kKeywords.contains(tok.value)) { err("Keyword '" + tok.value + "' cannot be used as an identifier", tok); }
    return tok.value;
  }

  auto ns_ident() -> std::string {
    const Token tok = eat_ident_token();
    if (kStructuralKeywords.contains(tok.value)) {
      err("Keyword '" + tok.value + "' cannot be used as a namespace segment", tok);
    }
    return tok.value;
  }

  auto opt_qid() -> std::variant<std::string, model::QualifiedId> {
    const std::size_t start = i_;
    std::string head = ident();
    std::vector<std::string> parts{head};
    while (match_symbol(".")) { parts.push_back(ns_ident()); }

    if (parts.size() == 1U) { return head; }

    model::Qualifier q;
    q.qualifier.reserve(parts.size() * 2U);
    for (std::size_t idx = 0; idx + 1U < parts.size(); ++idx) {
      if (idx > 0U) { q.qualifier += "."; }
      q.qualifier += parts[idx];
    }
    q.span = span_from_mark(start);

    model::QualifiedId out;
    out.qualifier = q;
    out.id = parts.back();
    out.span = span_from_mark(start);
    return out;
  }

  auto type() -> model::TypeNode {
    const std::size_t start = i_;
    model::TypeNode base;

    if (is_ident_value("Tuple")) {
      next();
      eat_symbol("(");
      model::TypeList type_list;
      if (!is_symbol(")")) {
        while (true) {
          type_list.types.emplace_back(type());
          if (!match_symbol(",")) { break; }
        }
      }
      eat_symbol(")");
      type_list.span = span_from_mark(start);
      base.value = std::move(type_list);
      base.span = span_from_mark(start);
    } else if (is(TokenKind::kIdent) && kSimpleTypes.contains(peek().value)) {
      const auto token = next();
      base.value = token.value;
      base.span = span_from_mark(start);
    } else if (is(TokenKind::kIdent)) {
      const auto qid = opt_qid();
      if (const auto* simple = std::get_if<std::string>(&qid); simple != nullptr) {
        base.value = *simple;
      } else if (const auto* qualified = std::get_if<model::QualifiedId>(&qid); qualified != nullptr) {
        base.value = *qualified;
      }
      base.span = span_from_mark(start);
    } else {
      err("Expected type");
    }

    if (match_symbol("...")) {
      if (const auto* base_name = std::get_if<std::string>(&base.value); base_name != nullptr) {
        base.value = *base_name + "...";
      } else {
        err("Variadic '...' only supported on simple type names");
      }
    }

    return base;
  }

  [[nodiscard]] static auto is_call_target_primary(const model::Primary& primary) -> bool {
    return std::holds_alternative<model::QualifiedId>(primary.base.value) ||
           std::holds_alternative<std::string>(primary.base.value);
  }

  auto expr(bool allow_ungrouped_closure_stage_split = true) -> model::Expression {
    const std::size_t start = i_;
    model::Expression out;
    out.expr = flow(allow_ungrouped_closure_stage_split);
    out.span = span_from_mark(start);
    return out;
  }

  auto closure_stage_flow() -> model::FlowExpression {
    const std::size_t start = i_;
    model::FlowExpression out;
    out.lhs = primary();

    while (match_symbol("->")) {
      auto stage = primary();
      out.rhs.push_back(stage);
      if (is_call_target_primary(stage)) { break; }
    }

    out.span = span_from_mark(start);
    return out;
  }

  auto closure_stage_expr() -> model::Expression {
    const std::size_t start = i_;
    model::Expression out;
    out.expr = closure_stage_flow();
    out.span = span_from_mark(start);
    return out;
  }

  auto try_parse_closure_after_open_paren(const std::size_t open_paren_index, model::Atom& out_atom,
                                          const bool allow_ungrouped_closure_stage_split) -> bool {
    const std::size_t checkpoint = i_;

    std::vector<model::Parameter> params;
    if (!is_symbol(")")) {
      while (true) {
        if (peek().kind != TokenKind::kIdent) {
          i_ = checkpoint;
          return false;
        }

        const std::size_t param_start = i_;
        model::Parameter p;
        p.param_name = ident();
        if (!match_symbol(":")) {
          i_ = checkpoint;
          return false;
        }
        p.type = type();
        p.span = span_from_mark(param_start);
        params.push_back(std::move(p));

        if (!match_symbol(",")) { break; }
      }
    }

    if (!match_symbol(")")) {
      i_ = checkpoint;
      return false;
    }
    if (!match_symbol(":")) {
      i_ = checkpoint;
      return false;
    }

    model::ClosureExpression closure;
    closure.params.params = std::move(params);
    closure.params.span = span_from_mark(open_paren_index);
    closure.rtype = type();

    if (!match_symbol("=")) {
      i_ = checkpoint;
      return false;
    }

    closure.body = allow_ungrouped_closure_stage_split ? closure_stage_expr() : expr();
    closure.span = span_from_mark(open_paren_index);
    out_atom.value = std::move(closure);
    out_atom.span = span_from_mark(open_paren_index);
    return true;
  }

  auto flow(bool allow_ungrouped_closure_stage_split = true) -> model::FlowExpression {
    const std::size_t start = i_;
    model::FlowExpression out;
    out.lhs = primary();
    while (match_symbol("->")) { out.rhs.push_back(primary(allow_ungrouped_closure_stage_split)); }
    out.span = span_from_mark(start);
    return out;
  }

  auto primary(const bool allow_ungrouped_closure_stage_split = false) -> model::Primary {
    const std::size_t start = i_;
    model::Primary out;
    out.base = atom(allow_ungrouped_closure_stage_split);
    out.span = span_from_mark(start);
    return out;
  }

  auto atom(const bool allow_ungrouped_closure_stage_split = false) -> model::Atom {
    const std::size_t start = i_;

    if (match_symbol("(")) {
      model::Atom closure_atom;
      if (try_parse_closure_after_open_paren(start, closure_atom, allow_ungrouped_closure_stage_split)) {
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
        inner.items.emplace_back(expr());
        if (!match_symbol(",")) { break; }
      }
      eat_symbol(")");
      inner.span = span_from_mark(start);
      out.value = std::move(inner);
      out.span = span_from_mark(start);
      return out;
    }

    if (is_symbol("-") && peek_ahead(1) != nullptr && peek_ahead(1)->kind == TokenKind::kNumber) {
      next();
      const Token num = next();
      model::Constant c;
      if (num.value.find_first_of(".eE") != std::string::npos) {
        c.val = -std::stod(num.value);
      } else {
        c.val = -std::stoll(num.value);
      }
      c.span = span_from_mark(start);

      model::Atom out;
      out.value = std::move(c);
      out.span = span_from_mark(start);
      return out;
    }

    if (is(TokenKind::kNumber)) {
      const Token num = next();
      model::Constant c;
      if (num.value.find_first_of(".eE") != std::string::npos) {
        c.val = std::stod(num.value);
      } else {
        c.val = std::stoll(num.value);
      }
      c.span = span_from_mark(start);

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
      const std::string got = tok.kind == TokenKind::kEof ? "end of input" : ("'" + tok.value + "'");
      err("expected an expression, got " + got, tok, hint_for_expected_expression(tok));
    }

    const auto q = opt_qid();
    model::Atom out;
    if (const auto* qualified = std::get_if<model::QualifiedId>(&q); qualified != nullptr) {
      out.value = *qualified;
    } else if (const auto* simple = std::get_if<std::string>(&q); simple != nullptr) {
      out.value = *simple;
    }
    out.span = span_from_mark(start);
    return out;
  }

  auto import_module_name() -> std::string {
    const Token tok = peek();
    if (tok.kind == TokenKind::kIdent) {
      if (kKeywords.contains(tok.value)) {
        err("Keyword '" + tok.value + "' cannot be used as an import module name", tok);
      }
      return next().value;
    }

    if (tok.kind == TokenKind::kNumber) {
      std::vector<Token> parts{next()};
      while (peek().kind == TokenKind::kNumber || peek().kind == TokenKind::kIdent) {
        const Token nxt = peek();
        if (const Token prev = parts.back(); nxt.line != prev.line || nxt.col != prev.end_col()) { break; }
        parts.push_back(next());
      }

      std::string name;
      for (const auto& part : parts) { name += part.value; }
      return name;
    }

    err("Expected import module name", tok, "Use a module name like 'Std' or a digit-leading name like '20_export'.");
  }

  auto let_stmt() -> model::LetStatement {
    const std::size_t start = i_;
    eat_ident_value("let");

    model::LetStatement out;
    out.id = opt_qid();

    eat_symbol("(");
    std::vector<model::Parameter> params;
    const std::size_t params_paren_start = i_ - 1;

    if (!is_symbol(")")) {
      while (true) {
        const std::size_t param_start = i_;
        model::Parameter p;
        p.param_name = ident();
        eat_symbol(":");
        p.type = type();
        p.span = span_from_mark(param_start);
        params.push_back(std::move(p));
        if (!match_symbol(",")) { break; }
      }
    }
    eat_symbol(")");

    out.params.params = std::move(params);
    out.params.span = span_from_mark(params_paren_start);

    eat_symbol(":");
    out.rtype = type();

    if (!(match_symbol("::") || match_symbol("="))) {
      err("Expected '::' or '='", std::nullopt,
          "After the return type, use '=' for a normal body or ':: __builtin__' for runtime-provided functions.");
    }

    if (is_ident_value("__builtin__")) {
      next();
      out.is_builtin = true;
      out.expr = std::nullopt;
    } else {
      out.expr = expr();
    }

    out.span = span_from_mark(start);
    return out;
  }

  auto stmt() -> model::Statement {
    if (is_ident_value("import")) {
      const std::size_t start = i_;
      next();
      model::ImportStatement stmt;
      stmt.module_name = import_module_name();
      stmt.span = span_from_mark(start);
      return stmt;
    }

    if (is_ident_value("let")) { return let_stmt(); }

    const std::size_t start = i_;
    model::ExpressionStatement stmt;
    stmt.expr = expr();
    stmt.span = span_from_mark(start);
    return stmt;
  }

  [[nodiscard]] static auto statement_span(const model::Statement& stmt) -> std::optional<diag::SourceSpan> {
    return std::visit([](const auto& s) -> auto { return s.span; }, stmt);
  }

  [[nodiscard]] auto hint_for_expected_expression(const Token& tok) const -> std::optional<std::string> {
    if (tok.kind == TokenKind::kSymbol) {
      if (tok.value == "->") {
        const Token& prev = previous_token();
        if (prev.kind == TokenKind::kSymbol && prev.value == "=") {
          return "The function body is missing after '='. Add an expression, e.g. '= (a, b) -> Std.Add'.";
        }
        return "The left-hand side of '->' is missing. Add an expression before the pipeline operator.";
      }
      if (tok.value == ")") { return "Unexpected ')'. Check for an extra closing parenthesis or a missing expression."; }
      if (tok.value == ",") { return "Unexpected ','. A tuple element is missing before this comma."; }
      if (tok.value == ";") { return "Unexpected ';'. Remove the extra semicolon or add an expression."; }
      if (tok.value == ":") { return "Unexpected ':'. Type annotations must be inside parameter or let definitions."; }
      if (tok.value == "=") { return "Unexpected '='. Assignment is not an expression; use '::' or '=' in a let definition."; }
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
      if (is_expression_start(got_tok)) { return "Add ',' between tuple elements or argument entries."; }
      return "Separate entries with ','.";
    }

    if (expected == ")") {
      if (got_tok.kind == TokenKind::kSymbol && got_tok.value == "->") {
        return "Close the current tuple/parameter list with ')' before continuing the pipeline.";
      }
      if (is_expression_start(got_tok)) {
        if (prev_tok.kind == TokenKind::kNumber || prev_tok.kind == TokenKind::kString ||
            prev_tok.kind == TokenKind::kBool || prev_tok.kind == TokenKind::kNull ||
            prev_tok.kind == TokenKind::kIdent || (prev_tok.kind == TokenKind::kSymbol && prev_tok.value == ")")) {
          return "Did you forget a comma? Add ',' between tuple elements or function arguments.";
        }
      }
      return "Close the current tuple/parameter list with ')'.";
    }

    if (expected == ":") { return "Add ':' before a type annotation (for example: x: Number)."; }

    if (expected == "IDENT") {
      if (prev_tok.kind == TokenKind::kSymbol && prev_tok.value == "->") {
        return "A pipeline stage is missing after '->'. Use a call target like Std.Add, MyFunc, or +.";
      }
      return "Use a valid identifier (letters/underscore, then letters/digits/underscore).";
    }

    return "Check for a missing or misplaced token earlier in the statement.";
  }

  [[nodiscard]] static auto is_expression_start(const Token& tok) -> bool {
    if (tok.kind == TokenKind::kNumber || tok.kind == TokenKind::kString || tok.kind == TokenKind::kBool ||
        tok.kind == TokenKind::kNull || tok.kind == TokenKind::kIdent) {
      return true;
    }
    if (tok.kind == TokenKind::kSymbol && (tok.value == "(" || tok.value == "-" || kOperators.contains(tok.value))) {
      return true;
    }
    return false;
  }

  [[nodiscard]] static auto is_statement_start(const Token& tok) -> bool {
    if (tok.kind == TokenKind::kIdent || tok.kind == TokenKind::kNumber || tok.kind == TokenKind::kString ||
        tok.kind == TokenKind::kBool || tok.kind == TokenKind::kNull) {
      return true;
    }
    if (tok.kind == TokenKind::kSymbol && (tok.value == "(" || kOperators.contains(tok.value))) { return true; }
    return false;
  }
};

}  // namespace

auto Parser::parse_program(const std::string& source, const std::string& source_name) const -> ParseResult {
  ParseError lex_error;
  auto tokens = lex_or_throw(source, source_name, lex_error);
  if (!lex_error.message.empty()) { return tl::unexpected(lex_error); }

  try {
    ParserImpl impl(source, source_name, std::move(tokens));
    return impl.parse();
  } catch (const ParseError& parse_error) { return tl::unexpected(parse_error); }
}

}  // namespace fleaux::frontend::parse
