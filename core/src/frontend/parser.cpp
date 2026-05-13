#include "fleaux/frontend/parser.hpp"
#include "fleaux/frontend/lexer.hpp"
#include "fleaux/common/overloaded.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
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
    diag::SourceSpan span;
    span.source_name = source_name_;
    span.source_text = source_;
    span.line = token.line;
    span.col = token.col;
    span.end_line = token.line;
    span.end_col = (token.kind == TokenKind::kEof) ? token.col : token.end_col();
    return span;
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
    return ParseError{
        .message = message,
        .hint = hint,
        .span = span_from_token(use),
    };
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
    if (is_keyword(tok.value)) {
      return tl::unexpected(err(std::format("Keyword '{}' cannot be used as an identifier", tok.value), tok));
    }
    return tok.value;
  }

  auto ns_ident() -> PResult<std::string> {
    FLEAUX_TRY_ASSIGN(tok, eat_ident_token());
    if (is_structural_keyword(tok.value)) {
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

      model::FunctionTypeNode func{};
      func.params = std::move(param_types);
      func.return_type = model::TypeBox(std::move(return_type));
      func.params.span = span_from_mark(start);
      func.span = span_from_mark(start);
      base.value = model::FunctionTypeNodeBox(std::move(func));
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
      base.value = model::TypeListBox(std::move(type_list));
      base.span = span_from_mark(start);
    } else if (is(TokenKind::kIdent) && is_simple_type_name(peek().value)) {
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
          base.value = model::AppliedTypeNodeBox(std::move(applied));
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
      union_node.value = model::UnionTypeListBox(std::move(union_list));
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

    auto parsed_rtype = type();
    if (!parsed_rtype) {
      return tl::unexpected(parsed_rtype.error());
    }
    model::ParameterDeclList closure_params{
        .params = std::move(params),
        .span = span_from_mark(open_paren_index),
    };
    model::TypeNode closure_rtype = std::move(*parsed_rtype);

    // Committed: we have parsed a full closure signature; any failure after this is a hard error.
    committed = true;

    FLEAUX_TRYV(eat_one_of("::", "="));

    model::Expression parsed_body_expr;

    if (allow_ungrouped_closure_stage_split) {
      auto parsed_body = closure_stage_expr();
      if (!parsed_body) {
        return tl::unexpected(parsed_body.error());
      }
      parsed_body_expr = std::move(*parsed_body);
    } else {
      auto parsed_body = expr();
      if (!parsed_body) {
        return tl::unexpected(parsed_body.error());
      }
      parsed_body_expr = std::move(*parsed_body);
    }

    model::ClosureExpression closure{
        .generic_params = std::move(generic_params),
        .params = std::move(closure_params),
        .rtype = std::move(closure_rtype),
        .body = model::ExpressionBox(std::move(parsed_body_expr)),
        .span = span_from_mark(closure_start_index),
    };
    out_atom.value = model::make_closure_expression_box(std::move(closure));
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
      const auto parse_uint64_literal = [&](const std::string& text) -> PResult<std::uint64_t> {
        std::uint64_t result{0};
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), result);
        if (ec == std::errc::invalid_argument || ptr != text.data() + text.size()) {
          return tl::unexpected(err("Invalid numeric literal", num, "Use a valid UInt64 literal."));
        }
        if (ec == std::errc::result_out_of_range) {
          return tl::unexpected(err("Numeric literal is out of range", num, "Use a value within the range of UInt64."));
        }
        return result;
      };
      const auto parse_int64_literal = [&](const std::string& text) -> PResult<std::int64_t> {
        std::int64_t result{0};
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), result);
        if (ec == std::errc::invalid_argument || ptr != text.data() + text.size()) {
          return tl::unexpected(err("Invalid numeric literal", num, "Use a valid Int64 literal."));
        }
        if (ec == std::errc::result_out_of_range) {
          return tl::unexpected(err("Numeric literal is out of range", num, "Use a value within the range of Int64."));
        }
        return result;
      };
      const auto parse_float64_literal = [&](const std::string& text) -> PResult<double> {
        double result{0.0};
        const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), result);
        if (ec == std::errc::invalid_argument || ptr != text.data() + text.size()) {
          return tl::unexpected(err("Invalid numeric literal", num, "Use a valid Float64 literal."));
        }
        if (ec == std::errc::result_out_of_range) {
          return tl::unexpected(
              err("Numeric literal is out of range", num, "Use a value within the range of Float64."));
        }
        return result;
      };

      if (is_u64) {
        if (negate) {
          return tl::unexpected(
              err("UInt64 literal cannot be negative", num, "Remove the unary '-' or use an Int64/Float64 literal."));
        }
        if (digits.find_first_of(".eE") != std::string::npos) {
          return tl::unexpected(
              err("Invalid UInt64 literal", num, "UInt64 literals must be whole numbers (for example: 42u64)."));
        }
        FLEAUX_TRY_ASSIGN(result, parse_uint64_literal(digits));
        c.val = result;
      } else if (digits.find_first_of(".eE") != std::string::npos) {
        FLEAUX_TRY_ASSIGN(result, parse_float64_literal(digits));
        c.val = negate ? -result : result;
      } else if (negate) {
        FLEAUX_TRY_ASSIGN(unsigned_result, parse_uint64_literal(digits));
        if (constexpr auto kInt64AbsMin = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U;
            unsigned_result == kInt64AbsMin) {
          c.val = std::numeric_limits<std::int64_t>::min();
        } else if (unsigned_result <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
          c.val = -static_cast<std::int64_t>(unsigned_result);
        } else {
          return tl::unexpected(err("Numeric literal is out of range", num, "Use a value within the range of Int64."));
        }
      } else {
        FLEAUX_TRY_ASSIGN(result, parse_int64_literal(digits));
        c.val = negate ? -result : result;
      }
      c.span = span_from_mark(start);
      return c;
    };

    if (match_symbol("(")) {
      model::Atom closure_atom;
      // I prefer to just use this ugly macro here so ignore the clang-tidy warning about moving it into the if-init
      // NOLINTNEXTLINE
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
      out.value = model::DelimitedExpressionBox(std::move(inner));
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

    if (is(TokenKind::kSymbol) && is_operator_symbol(peek().value)) {
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
      // I prefer to just use this ugly macro here so ignore the clang-tidy warning about moving it into the for-init
      // NOLINTNEXTLINE
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
      if (is_keyword(tok.value)) {
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
      return tl::unexpected(
          err("Transparent aliases do not support generic parameter lists.", peek(),
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
    return tok.kind == TokenKind::kSymbol && (tok.value == "(" || tok.value == "-" || is_operator_symbol(tok.value));
  }

  [[nodiscard]] static auto is_statement_start(const Token& tok) -> bool {
    if (tok.kind == TokenKind::kIdent || tok.kind == TokenKind::kNumeric || tok.kind == TokenKind::kString ||
        tok.kind == TokenKind::kBool || tok.kind == TokenKind::kNull) {
      return true;
    }
    return tok.kind == TokenKind::kSymbol && (tok.value == "(" || is_operator_symbol(tok.value));
  }
};

class AstDumper {
public:
  [[nodiscard]] static auto dump(const model::Program& program) -> std::string { return format_program(program, 0); }

private:
  [[nodiscard]] static auto indent(const int level) -> std::string {
    // The suggestion emitted here is incorrect, because the brace init gives the wrong overload
    // NOLINTNEXTLINE
    return std::string(static_cast<std::size_t>(level) * 2U, ' ');
  }

  [[nodiscard]] static auto escape_string(const std::string_view text) -> std::string {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char ch : text) {
      switch (ch) {
        case '\\':
          escaped += "\\\\";
          break;
        case '\"':
          escaped += "\\\"";
          break;
        case '\n':
          escaped += "\\n";
          break;
        case '\r':
          escaped += "\\r";
          break;
        case '\t':
          escaped += "\\t";
          break;
        default:
          escaped.push_back(ch);
          break;
      }
    }
    return escaped;
  }

  [[nodiscard]] static auto quote(const std::string_view text) -> std::string {
    return std::format("\"{}\"", escape_string(text));
  }

  [[nodiscard]] static auto format_string_list(const std::vector<std::string>& items, const int level) -> std::string {
    std::vector<std::string> formatted;
    formatted.reserve(items.size());
    for (const auto& item : items) {
      formatted.push_back(quote(item));
    }
    return format_list(formatted, level);
  }

  [[nodiscard]] static auto format_list(const std::vector<std::string>& items, const int level) -> std::string {
    if (items.empty()) {
      return "[]";
    }

    std::string out = "[\n";
    for (std::size_t idx = 0; idx < items.size(); ++idx) {
      out += indent(level + 1) + items[idx];
      if (idx + 1 < items.size()) {
        out += ',';
      }
      out += '\n';
    }
    out += indent(level) + ']';
    return out;
  }

  [[nodiscard]] static auto format_block(const std::string_view name, const std::vector<std::string>& fields,
                                         const int level) -> std::string {
    if (fields.empty()) {
      return std::format("{} {{}}", name);
    }

    std::string out = std::format("{} {{\n", name);
    for (std::size_t idx = 0; idx < fields.size(); ++idx) {
      out += indent(level + 1) + fields[idx];
      if (idx + 1 < fields.size()) {
        out += ',';
      }
      out += '\n';
    }
    out += indent(level) + '}';
    return out;
  }

  [[nodiscard]] static auto format_program(const model::Program& program, const int level) -> std::string {
    std::vector<std::string> fields;
    fields.push_back(std::format("source_name: {}", quote(program.source_name)));

    std::vector<std::string> statements;
    statements.reserve(program.statements.size());
    for (const auto& stmt : program.statements) {
      statements.push_back(format_statement(stmt, level + 1));
    }
    fields.push_back(std::format("statements: {}", format_list(statements, level + 1)));

    return format_block("Program", fields, level);
  }

  [[nodiscard]] static auto format_statement(const model::Statement& stmt, const int level) -> std::string {
    return std::visit([&](const auto& concrete) -> std::string { return format_statement_impl(concrete, level); },
                      stmt);
  }

  [[nodiscard]] static auto format_statement_impl(const model::ImportStatement& stmt, const int level) -> std::string {
    return format_block("ImportStatement", {std::format("module_name: {}", quote(stmt.module_name))}, level);
  }

  [[nodiscard]] static auto format_statement_impl(const model::TypeStatement& stmt, const int level) -> std::string {
    return format_block(
        "TypeStatement",
        {std::format("name: {}", quote(stmt.name)), std::format("target: {}", format_type(stmt.target, level + 1))},
        level);
  }

  [[nodiscard]] static auto format_statement_impl(const model::AliasStatement& stmt, const int level) -> std::string {
    return format_block(
        "AliasStatement",
        {std::format("name: {}", quote(stmt.name)), std::format("target: {}", format_type(stmt.target, level + 1))},
        level);
  }

  [[nodiscard]] static auto format_statement_impl(const model::LetStatement& stmt, const int level) -> std::string {
    std::vector<std::string> fields;
    fields.push_back(std::format("id: {}", format_let_id(stmt.id, level + 1)));
    fields.push_back(std::format("generic_params: {}", format_string_list(stmt.generic_params, level + 1)));
    fields.push_back(std::format("params: {}", format_parameter_decl_list(stmt.params, level + 1)));
    fields.push_back(std::format("return_type: {}", format_type(stmt.rtype, level + 1)));
    fields.push_back(std::format("doc_comments: {}", format_string_list(stmt.doc_comments, level + 1)));
    fields.push_back(std::format("is_builtin: {}", stmt.is_builtin));
    fields.push_back(
        std::format("expr: {}", stmt.expr ? format_expression(*stmt.expr, level + 1) : std::string{"null"}));
    return format_block("LetStatement", fields, level);
  }

  [[nodiscard]] static auto format_statement_impl(const model::ExpressionStatement& stmt, const int level)
      -> std::string {
    return format_block("ExpressionStatement", {std::format("expr: {}", format_expression(stmt.expr, level + 1))},
                        level);
  }

  [[nodiscard]] static auto format_let_id(const std::variant<std::string, model::QualifiedId>& id, const int level)
      -> std::string {
    return std::visit(common::overloaded{[](const std::string& name) -> std::string { return quote(name); },
                                         [&](const model::QualifiedId& qualified) -> std::string {
                                           return format_qualified_id(qualified, level);
                                         }},
                      id);
  }

  [[nodiscard]] static auto format_qualified_id(const model::QualifiedId& qualified, const int level) -> std::string {
    return format_block("QualifiedId",
                        {std::format("qualifier: {}", quote(qualified.qualifier.qualifier)),
                         std::format("id: {}", quote(qualified.id))},
                        level);
  }

  [[nodiscard]] static auto format_type(const model::TypeNode& type_node, const int level) -> std::string {
    std::vector<std::string> fields;
    fields.push_back(std::format("kind: {}", quote(type_kind_name(type_node.value))));
    fields.push_back(std::format("value: {}", format_type_value(type_node.value, level + 1)));
    fields.push_back(std::format("variadic: {}", type_node.variadic));
    return format_block("TypeNode", fields, level);
  }

  [[nodiscard]] static auto type_kind_name(
      const std::variant<std::string, model::QualifiedId, model::TypeListBox, model::UnionTypeListBox,
                         model::AppliedTypeNodeBox, model::FunctionTypeNodeBox>& value) -> std::string_view {
    return std::visit(
        common::overloaded{[](const std::string&) -> std::string_view { return "RawType"; },
                           [](const model::QualifiedId&) -> std::string_view { return "QualifiedType"; },
                           [](const model::TypeListBox&) -> std::string_view { return "TupleType"; },
                           [](const model::UnionTypeListBox&) -> std::string_view { return "UnionType"; },
                           [](const model::AppliedTypeNodeBox&) -> std::string_view { return "AppliedType"; },
                           [](const model::FunctionTypeNodeBox&) -> std::string_view { return "FunctionType"; }},
        value);
  }

  [[nodiscard]] static auto format_type_value(
      const std::variant<std::string, model::QualifiedId, model::TypeListBox, model::UnionTypeListBox,
                         model::AppliedTypeNodeBox, model::FunctionTypeNodeBox>& value,
      const int level) -> std::string {
    return std::visit(
        common::overloaded{
            [](const std::string& raw_type) -> std::string { return quote(raw_type); },
            [&](const model::QualifiedId& qualified) -> std::string { return format_qualified_id(qualified, level); },
            [&](const model::TypeListBox& type_list) -> std::string {
              if (!type_list) {
                return std::string{"null"};
              }
              std::vector<std::string> items;
              items.reserve(type_list->types.size());
              for (const auto& item : type_list->types) {
                items.push_back(item ? format_type(*item, level + 1) : std::string{"null"});
              }
              return format_block("TypeList", {std::format("items: {}", format_list(items, level + 1))}, level);
            },
            [&](const model::UnionTypeListBox& union_type) -> std::string {
              if (!union_type) {
                return std::string{"null"};
              }
              std::vector<std::string> items;
              items.reserve(union_type->alternatives.size());
              for (const auto& alternative : union_type->alternatives) {
                items.push_back(alternative ? format_type(*alternative, level + 1) : std::string{"null"});
              }
              return format_block("UnionTypeList", {std::format("alternatives: {}", format_list(items, level + 1))},
                                  level);
            },
            [&](const model::AppliedTypeNodeBox& applied_type) -> std::string {
              if (!applied_type) {
                return std::string{"null"};
              }
              std::vector<std::string> args;
              args.reserve(applied_type->args.types.size());
              for (const auto& arg : applied_type->args.types) {
                args.push_back(arg ? format_type(*arg, level + 1) : std::string{"null"});
              }
              return format_block("AppliedTypeNode",
                                  {std::format("name: {}", quote(applied_type->name)),
                                   std::format("args: {}", format_list(args, level + 1))},
                                  level);
            },
            [&](const model::FunctionTypeNodeBox& function_type) -> std::string {
              if (!function_type) {
                return std::string{"null"};
              }
              std::vector<std::string> params;
              params.reserve(function_type->params.types.size());
              for (const auto& param : function_type->params.types) {
                params.push_back(param ? format_type(*param, level + 1) : std::string{"null"});
              }
              return format_block(
                  "FunctionTypeNode",
                  {std::format("params: {}", format_list(params, level + 1)),
                   std::format("return_type: {}", function_type->return_type
                                                      ? format_type(*function_type->return_type, level + 1)
                                                      : std::string{"null"})},
                  level);
            }},
        value);
  }

  [[nodiscard]] static auto format_parameter_decl_list(const model::ParameterDeclList& params, const int level)
      -> std::string {
    std::vector<std::string> items;
    items.reserve(params.params.size());
    for (const auto& param : params.params) {
      items.push_back(format_parameter(param, level + 1));
    }
    return format_block("ParameterDeclList", {std::format("params: {}", format_list(items, level + 1))}, level);
  }

  [[nodiscard]] static auto format_parameter(const model::Parameter& param, const int level) -> std::string {
    return format_block(
        "Parameter",
        {std::format("name: {}", quote(param.param_name)), std::format("type: {}", format_type(param.type, level + 1))},
        level);
  }

  [[nodiscard]] static auto format_expression(const model::Expression& expr, const int level) -> std::string {
    return format_block("Expression", {std::format("flow: {}", format_flow(expr.expr, level + 1))}, level);
  }

  [[nodiscard]] static auto format_flow(const model::FlowExpression& flow, const int level) -> std::string {
    std::vector<std::string> rhs;
    rhs.reserve(flow.rhs.size());
    for (const auto& stage : flow.rhs) {
      rhs.push_back(format_primary(stage, level + 1));
    }
    return format_block("FlowExpression",
                        {std::format("lhs: {}", format_primary(flow.lhs, level + 1)),
                         std::format("rhs: {}", format_list(rhs, level + 1))},
                        level);
  }

  [[nodiscard]] static auto format_primary(const model::Primary& primary, const int level) -> std::string {
    return format_block("Primary",
                        {std::format("base: {}", format_atom(primary.base, level + 1)),
                         std::format("extra: {}", format_string_list(primary.extra, level + 1))},
                        level);
  }

  [[nodiscard]] static auto format_atom(const model::Atom& atom, const int level) -> std::string {
    std::vector<std::string> fields;
    fields.push_back(std::format("kind: {}", quote(atom_kind_name(atom.value))));
    fields.push_back(std::format("value: {}", format_atom_value(atom.value, level + 1)));
    return format_block("Atom", fields, level);
  }

  [[nodiscard]] static auto atom_kind_name(
      const std::variant<std::monostate, model::DelimitedExpressionBox, model::ClosureExpressionBox, model::Constant,
                         model::QualifiedId, std::string, model::NamedTargetBox>& value) -> std::string_view {
    return std::visit(
        common::overloaded{
            [](const std::monostate&) -> std::string_view { return "Unit"; },
            [](const model::DelimitedExpressionBox&) -> std::string_view { return "DelimitedExpression"; },
            [](const model::ClosureExpressionBox&) -> std::string_view { return "ClosureExpression"; },
            [](const model::Constant&) -> std::string_view { return "Constant"; },
            [](const model::QualifiedId&) -> std::string_view { return "QualifiedId"; },
            [](const std::string&) -> std::string_view { return "RawString"; },
            [](const model::NamedTargetBox&) -> std::string_view { return "NamedTarget"; }},
        value);
  }

  [[nodiscard]] static auto format_atom_value(
      const std::variant<std::monostate, model::DelimitedExpressionBox, model::ClosureExpressionBox, model::Constant,
                         model::QualifiedId, std::string, model::NamedTargetBox>& value,
      const int level) -> std::string {
    return std::visit(
        common::overloaded{
            [](const std::monostate&) -> std::string { return std::string{"null"}; },
            [&](const model::DelimitedExpressionBox& delimited) -> std::string {
              if (!delimited) {
                return std::string{"null"};
              }
              std::vector<std::string> items;
              items.reserve(delimited->items.size());
              for (const auto& item : delimited->items) {
                items.push_back(item ? format_expression(*item, level + 1) : std::string{"null"});
              }
              return format_block("DelimitedExpression", {std::format("items: {}", format_list(items, level + 1))},
                                  level);
            },
            [&](const model::ClosureExpressionBox& closure) -> std::string {
              if (!closure) {
                return std::string{"null"};
              }
              return format_block(
                  "ClosureExpression",
                  {std::format("generic_params: {}", format_string_list(closure->generic_params, level + 1)),
                   std::format("params: {}", format_parameter_decl_list(closure->params, level + 1)),
                   std::format("return_type: {}", format_type(closure->rtype, level + 1)),
                   std::format("body: {}",
                               closure->body ? format_expression(*closure->body, level + 1) : std::string{"null"})},
                  level);
            },
            [&](const model::Constant& constant) -> std::string { return format_constant(constant, level); },
            [&](const model::QualifiedId& qualified) -> std::string { return format_qualified_id(qualified, level); },
            [](const std::string& raw) -> std::string { return quote(raw); },
            [&](const model::NamedTargetBox& named_target) -> std::string {
              if (!named_target) {
                return std::string{"null"};
              }
              return format_named_target(*named_target, level);
            }},
        value);
  }

  [[nodiscard]] static auto format_named_target(const model::NamedTarget& named_target, const int level)
      -> std::string {
    std::vector<std::string> type_args;
    type_args.reserve(named_target.explicit_type_args.size());
    for (const auto& type_arg : named_target.explicit_type_args) {
      type_args.push_back(type_arg ? format_type(*type_arg, level + 1) : std::string{"null"});
    }

    return format_block(
        "NamedTarget",
        {std::format("target: {}",
                     std::visit(common::overloaded{[](const std::string& name) -> std::string { return quote(name); },
                                                   [&](const model::QualifiedId& qualified) -> std::string {
                                                     return format_qualified_id(qualified, level + 1);
                                                   }},
                                named_target.target)),
         std::format("explicit_type_args: {}", format_list(type_args, level + 1))},
        level);
  }

  [[nodiscard]] static auto format_constant(const model::Constant& constant, const int level) -> std::string {
    return format_block(
        "Constant",
        {std::format(
            "value: {}",
            std::visit(
                common::overloaded{[](const std::int64_t value) -> std::string { return std::format("{}", value); },
                                   [](const std::uint64_t value) -> std::string { return std::format("{}u64", value); },
                                   [](const double value) -> std::string { return std::format("{}", value); },
                                   [](const bool value) -> std::string { return value ? "true" : "false"; },
                                   [](const std::string& value) -> std::string { return quote(value); },
                                   [](const std::monostate&) -> std::string { return std::string{"null"}; }},
                constant.val))},
        level);
  }
};

}  // namespace

auto Parser::parse_program(const std::string& source, const std::string& source_name) const -> ParseResult {
  auto tokens = lex_program(source, source_name);
  if (!tokens) {
    return tl::unexpected(tokens.error());
  }

  ParserImpl impl(source, source_name, std::move(*tokens));
  return impl.parse();
}

auto Parser::dump_ast(const model::Program& program) const -> std::string { return AstDumper{}.dump(program); }

}  // namespace fleaux::frontend::parse
