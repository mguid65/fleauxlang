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

  [[nodiscard]] int end_col() const { return col + static_cast<int>(text.size()); }
};

const std::unordered_set<std::string> kKeywords = {
    "let", "import", "Number", "String", "Bool", "Null", "Any", "Tuple", "__builtin__"};

const std::unordered_set<std::string> kStructuralKeywords = {"let", "import", "__builtin__"};

const std::unordered_set<std::string> kOperators = {
    "^",  "/",  "*",  "%",  "+",  "-",  "==", "!=",
    "<",  ">",  ">=", "<=", "!",  "&&", "||",
};

const std::unordered_set<std::string> kSimpleTypes = {"Number", "String", "Bool", "Null", "Any"};

bool is_ident_start(const char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_'; }

bool is_ident_char(const char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; }

bool starts_with(const std::string& source, const std::size_t index, const std::string& text) {
  if (index + text.size() > source.size()) {
    return false;
  }
  return source.compare(index, text.size(), text) == 0;
}

std::optional<diag::SourceSpan> span_from_token(const Token& token,
                                                const std::string& source_name,
                                                const std::string& source_text) {
  diag::SourceSpan span;
  span.source_name = source_name;
  span.source_text = source_text;
  span.line = token.line;
  span.col = token.col;
  span.end_line = token.line;
  span.end_col = (token.kind == TokenKind::kEof) ? token.col : token.end_col();
  return span;
}

ParseError make_error(const std::string& message,
                      const std::optional<std::string>& hint,
                      const std::optional<diag::SourceSpan>& span) {
  ParseError error;
  error.message = message;
  error.hint = hint;
  error.span = span;
  return error;
}

std::vector<Token> lex_or_throw(const std::string& source,
                                const std::string& source_name,
                                ParseError& error) {
  std::vector<Token> out;
  std::size_t i = 0;
  int line = 1;
  int col = 1;

  auto push = [&](const TokenKind kind, const std::string& value, const std::string& text, const int l, const int c) -> void {
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
    if (matched_multi) {
      continue;
    }

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
          if (j + 1 >= source.size()) {
            break;
          }
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
        if (ch == '\n') {
          break;
        }
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
        error = make_error("Unterminated string literal", std::nullopt,
                           span_from_token(tok, source_name, source));
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

  ParseResult parse() {
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

  [[nodiscard]] const Token& peek() const { return tokens_[i_]; }

  [[nodiscard]] const Token* peek_ahead(std::size_t offset) const {
    const std::size_t index = i_ + offset;
    if (index >= tokens_.size()) {
      return nullptr;
    }
    return &tokens_[index];
  }

  [[nodiscard]] const Token& previous_token() const {
    if (i_ == 0) {
      return tokens_.front();
    }
    return tokens_[i_ - 1];
  }

  [[nodiscard]] bool is(const TokenKind kind) const { return peek().kind == kind; }

  [[nodiscard]] bool is_symbol(const std::string& symbol) const {
    return peek().kind == TokenKind::kSymbol && peek().value == symbol;
  }

  [[nodiscard]] bool is_ident_value(const std::string& ident) const {
    return peek().kind == TokenKind::kIdent && peek().value == ident;
  }

  [[nodiscard]] std::optional<diag::SourceSpan> span_from_token(const Token& token) const {
    return ::fleaux::frontend::parse::span_from_token(token, source_name_, source_);
  }

  [[nodiscard]] std::optional<diag::SourceSpan> span_from_mark(const std::size_t start) const {
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

  [[noreturn]] void err(const std::string& message,
                        const std::optional<Token>& tok = std::nullopt,
                        const std::optional<std::string>& hint = std::nullopt) const {
    const Token& use = tok.has_value() ? *tok : peek();
    throw make_error(message, hint, span_from_token(use));
  }

  const Token& next() {
    const Token& tok = tokens_[i_];
    ++i_;
    return tok;
  }

  bool match_symbol(const std::string& symbol) {
    if (is_symbol(symbol)) {
      ++i_;
      return true;
    }
    return false;
  }

  const Token& eat_symbol(const std::string& symbol) {
    if (const Token& tok = peek(); !is_symbol(symbol)) {
      const std::string got = tok.kind == TokenKind::kEof ? "end of input" : ("'" + tok.value + "'");
      err("Expected '" + symbol + "', got " + got, tok, hint_for_expected_token(symbol, tok));
    }
    return next();
  }

  const Token& eat_ident_token() {
    if (const Token& tok = peek(); tok.kind != TokenKind::kIdent) {
      const std::string got = tok.kind == TokenKind::kEof ? "end of input" : ("'" + tok.value + "'");
      err("Expected 'IDENT', got " + got, tok, hint_for_expected_token("IDENT", tok));
    }
    return next();
  }

  const Token& eat_ident_value(const std::string& ident) {
    if (const Token& tok = peek(); tok.kind != TokenKind::kIdent || tok.value != ident) {
      const std::string got = tok.kind == TokenKind::kEof ? "end of input" : ("'" + tok.value + "'");
      err("Expected '" + ident + "', got " + got, tok,
          "Check keyword spelling and statement structure.");
    }
    return next();
  }

  std::string ident() {
    const Token tok = eat_ident_token();
    if (kKeywords.contains(tok.value)) {
      err("Keyword '" + tok.value + "' cannot be used as an identifier", tok);
    }
    return tok.value;
  }

  std::string ns_ident() {
    const Token tok = eat_ident_token();
    if (kStructuralKeywords.contains(tok.value)) {
      err("Keyword '" + tok.value + "' cannot be used as a namespace segment", tok);
    }
    return tok.value;
  }

  std::variant<std::string, model::QualifiedId> opt_qid() {
    const std::size_t start = i_;
    std::string head = ident();
    std::vector<std::string> parts{head};
    while (match_symbol(".")) {
      parts.push_back(ns_ident());
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

  model::TypeRef type() {
    const std::size_t start = i_;
    auto base = std::make_shared<model::TypeNode>();

    if (is_ident_value("Tuple")) {
      next();
      eat_symbol("(");
      auto type_list = std::make_shared<model::TypeList>();
      if (!is_symbol(")")) {
        while (true) {
          type_list->types.push_back(type());
          if (!match_symbol(",")) {
            break;
          }
        }
      }
      eat_symbol(")");
      type_list->span = span_from_mark(start);
      base->value = type_list;
      base->span = span_from_mark(start);
    } else if (is(TokenKind::kIdent) && kSimpleTypes.contains(peek().value)) {
      const auto token = next();
      base->value = token.value;
      base->span = span_from_mark(start);
    } else if (is(TokenKind::kIdent)) {
      const auto qid = opt_qid();
      if (const auto* simple = std::get_if<std::string>(&qid); simple != nullptr) {
        base->value = *simple;
      } else if (const auto* qualified = std::get_if<model::QualifiedId>(&qid); qualified != nullptr) {
        base->value = *qualified;
      }
      base->span = span_from_mark(start);
    } else {
      err("Expected type");
    }

    if (match_symbol("...")) {
      if (const auto* base_name = std::get_if<std::string>(&base->value); base_name != nullptr) {
        base->value = *base_name + "...";
      } else {
        err("Variadic '...' only supported on simple type names");
      }
    }

    return base;
  }

  [[nodiscard]] bool is_call_target_primary(const model::Primary& primary) const {
    if (primary.base.qualified_var.has_value()) {
      return true;
    }
    if (primary.base.var.has_value()) {
      return true;
    }
    return false;
  }

  model::ExpressionPtr expr(bool allow_ungrouped_closure_stage_split = true) {
    const std::size_t start = i_;
    auto out = std::make_shared<model::Expression>();
    out->expr = flow(allow_ungrouped_closure_stage_split);
    out->span = span_from_mark(start);
    return out;
  }

  model::FlowExpression closure_stage_flow() {
    const std::size_t start = i_;
    model::FlowExpression out;
    out.lhs = primary();

    while (match_symbol("->")) {
      auto stage = primary();
      out.rhs.push_back(stage);
      if (is_call_target_primary(stage)) {
        break;
      }
    }

    out.span = span_from_mark(start);
    return out;
  }

  model::ExpressionPtr closure_stage_expr() {
    const std::size_t start = i_;
    auto out = std::make_shared<model::Expression>();
    out->expr = closure_stage_flow();
    out->span = span_from_mark(start);
    return out;
  }

  bool try_parse_closure_after_open_paren(const std::size_t open_paren_index, model::Atom& out_atom,
                                          const bool allow_ungrouped_closure_stage_split) {
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

        if (!match_symbol(",")) {
          break;
        }
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

    auto closure = std::make_shared<model::ClosureExpression>();
    closure->params.params = std::move(params);
    closure->params.span = span_from_mark(open_paren_index);
    closure->rtype = type();

    if (!match_symbol("=")) {
      i_ = checkpoint;
      return false;
    }

    closure->body = allow_ungrouped_closure_stage_split ? closure_stage_expr() : expr();
    closure->span = span_from_mark(open_paren_index);
    out_atom.closure = std::move(closure);
    out_atom.span = span_from_mark(open_paren_index);
    return true;
  }

  model::FlowExpression flow(bool allow_ungrouped_closure_stage_split = true) {
    const std::size_t start = i_;
    model::FlowExpression out;
    out.lhs = primary();
    while (match_symbol("->")) {
      out.rhs.push_back(primary(allow_ungrouped_closure_stage_split));
    }
    out.span = span_from_mark(start);
    return out;
  }

  model::Primary primary(const bool allow_ungrouped_closure_stage_split = false) {
    const std::size_t start = i_;
    model::Primary out;
    out.base = atom(allow_ungrouped_closure_stage_split);
    out.span = span_from_mark(start);
    return out;
  }

  model::Atom atom(const bool allow_ungrouped_closure_stage_split = false) {
    const std::size_t start = i_;

    if (match_symbol("(")) {
      model::Atom closure_atom;
      if (try_parse_closure_after_open_paren(start, closure_atom, allow_ungrouped_closure_stage_split)) {
        return closure_atom;
      }

      model::Atom out;
      if (match_symbol(")")) {
        out.span = span_from_mark(start);
        return out;
      }

      auto inner = std::make_shared<model::DelimitedExpression>();
      while (true) {
        inner->items.push_back(expr());
        if (!match_symbol(",")) {
          break;
        }
      }
      eat_symbol(")");
      inner->span = span_from_mark(start);
      out.inner = inner;
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
      out.constant = c;
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
      out.constant = c;
      out.span = span_from_mark(start);
      return out;
    }

    if (is(TokenKind::kString)) {
      const Token str = next();
      model::Constant c;
      c.val = str.value;
      c.span = span_from_mark(start);

      model::Atom out;
      out.constant = c;
      out.span = span_from_mark(start);
      return out;
    }

    if (is(TokenKind::kBool)) {
      const Token b = next();
      model::Constant c;
      c.val = (b.value == "True");
      c.span = span_from_mark(start);

      model::Atom out;
      out.constant = c;
      out.span = span_from_mark(start);
      return out;
    }

    if (is(TokenKind::kNull)) {
      next();
      model::Constant c;
      c.val = std::monostate{};
      c.span = span_from_mark(start);

      model::Atom out;
      out.constant = c;
      out.span = span_from_mark(start);
      return out;
    }

    if (is(TokenKind::kSymbol) && kOperators.contains(peek().value)) {
      model::Atom out;
      out.var = next().value;
      out.span = span_from_mark(start);
      return out;
    }

    const auto q = opt_qid();
    model::Atom out;
    if (const auto* qualified = std::get_if<model::QualifiedId>(&q); qualified != nullptr) {
      out.qualified_var = *qualified;
    } else if (const auto* simple = std::get_if<std::string>(&q); simple != nullptr) {
      out.var = *simple;
    }
    out.span = span_from_mark(start);
    return out;
  }

  std::string import_module_name() {
    const Token tok = peek();
    if (tok.kind == TokenKind::kIdent) {
      if (kKeywords.find(tok.value) != kKeywords.end()) {
        err("Keyword '" + tok.value + "' cannot be used as an import module name", tok);
      }
      return next().value;
    }

    if (tok.kind == TokenKind::kNumber) {
      std::vector<Token> parts{next()};
      while (peek().kind == TokenKind::kNumber || peek().kind == TokenKind::kIdent) {
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

    err("Expected import module name", tok,
        "Use a module name like 'Std' or a digit-leading name like '20_export'.");
  }

  model::LetStatement let_stmt() {
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
        if (!match_symbol(",")) {
          break;
        }
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
      out.expr = std::string("__builtin__");
    } else {
      out.expr = expr();
    }

    out.span = span_from_mark(start);
    return out;
  }

  model::Statement stmt() {
    if (is_ident_value("import")) {
      const std::size_t start = i_;
      next();
      model::ImportStatement stmt;
      stmt.module_name = import_module_name();
      stmt.span = span_from_mark(start);
      return stmt;
    }

    if (is_ident_value("let")) {
      return let_stmt();
    }

    const std::size_t start = i_;
    model::ExpressionStatement stmt;
    stmt.expr = expr();
    stmt.span = span_from_mark(start);
    return stmt;
  }

  [[nodiscard]] std::optional<diag::SourceSpan> statement_span(const model::Statement& stmt) const {
    return std::visit([](const auto& s) -> auto { return s.span; }, stmt);
  }

  [[nodiscard]] std::optional<std::string> hint_for_expected_token(const std::string& expected,
                                                     const Token& got_tok) const {
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
        if (prev_tok.kind == TokenKind::kNumber || prev_tok.kind == TokenKind::kString ||
            prev_tok.kind == TokenKind::kBool || prev_tok.kind == TokenKind::kNull ||
            prev_tok.kind == TokenKind::kIdent ||
            (prev_tok.kind == TokenKind::kSymbol && prev_tok.value == ")")) {
          return "Did you forget a comma? Add ',' between tuple elements or function arguments.";
        }
      }
      return "Close the current tuple/parameter list with ')'.";
    }

    if (expected == ":") {
      return "Add ':' before a type annotation (for example: x: Number).";
    }

    if (expected == "IDENT") {
      if (prev_tok.kind == TokenKind::kSymbol && prev_tok.value == "->") {
        return "A pipeline stage is missing after '->'. Use a call target like Std.Add, MyFunc, or +.";
      }
      return "Use a valid identifier (letters/underscore, then letters/digits/underscore).";
    }

    return "Check for a missing or misplaced token earlier in the statement.";
  }

  [[nodiscard]] bool is_expression_start(const Token& tok) const {
    if (tok.kind == TokenKind::kNumber || tok.kind == TokenKind::kString || tok.kind == TokenKind::kBool ||
        tok.kind == TokenKind::kNull || tok.kind == TokenKind::kIdent) {
      return true;
    }
    if (tok.kind == TokenKind::kSymbol && (tok.value == "(" || tok.value == "-" || kOperators.contains(tok.value))) {
      return true;
    }
    return false;
  }

  [[nodiscard]] bool is_statement_start(const Token& tok) const {
    if (tok.kind == TokenKind::kIdent || tok.kind == TokenKind::kNumber || tok.kind == TokenKind::kString ||
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

ParseResult Parser::parse_program(const std::string& source,
                                  const std::string& source_name) const {
  ParseError lex_error;
  auto tokens = lex_or_throw(source, source_name, lex_error);
  if (!lex_error.message.empty()) {
    return tl::unexpected(lex_error);
  }

  try {
    ParserImpl impl(source, source_name, std::move(tokens));
    return impl.parse();
  } catch (const ParseError& parse_error) {
    return tl::unexpected(parse_error);
  }
}

}  // namespace fleaux::frontend::parse

