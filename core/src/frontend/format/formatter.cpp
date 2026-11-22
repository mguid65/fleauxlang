#include "fleaux/frontend/format/formatter.hpp"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <format>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "fleaux/common/overloaded.hpp"
#include "fleaux/frontend/ast.hpp"
#include "fleaux/frontend/format/layout.hpp"
#include "fleaux/frontend/lexer.hpp"

namespace fleaux::frontend::format {
namespace {

using parse::Comment;
using parse::Token;
using parse::TokenKind;

// The comments next to one token. A comment on its own line leads the token after it; a comment after code trails the
// token before it on that line.
struct TokenComments {
  std::vector<Comment> leading;
  std::vector<Comment> trailing;
};

// The source's tokens in order. The printer walks the syntax tree in source order and takes every token it prints from
// here, so the output keeps each token's spelling and comments, and any disagreement with the tree is reported.
class TokenCursor {
public:
  TokenCursor(std::vector<Token> tokens, const std::vector<Comment>& comments)
      : tokens_(std::move(tokens)), comments_(tokens_.size()) {
    std::size_t next = 0;
    for (const Comment& comment : comments) {
      while (next + 1 < tokens_.size() && tokens_[next].offset < comment.offset) {
        ++next;
      }
      if (next > 0 && tokens_[next - 1].end_line() == comment.line) {
        comments_[next - 1].trailing.push_back(comment);
      } else {
        comments_[next].leading.push_back(comment);
      }
    }
  }

  [[nodiscard]] auto peek() const -> const Token& { return tokens_[index_]; }
  [[nodiscard]] auto at_end() const -> bool { return peek().kind == TokenKind::kEof; }
  // The last line of the last token taken.
  [[nodiscard]] auto last_line() const -> int { return last_line_; }
  [[nodiscard]] auto error() const -> const std::optional<std::string>& { return error_; }

  void report(std::string message) {
    if (!error_.has_value()) {
      error_ = std::format("{} (line {})", message, peek().line);
    }
  }

  // The comments on their own lines before the next token, for the caller to lay out: a statement's comments and the
  // blank lines around them are printed at statement level.
  auto take_leading_comments() -> std::vector<Comment> { return std::exchange(comments_[index_].leading, {}); }

  // The next token, which must be spelled `expected`.
  auto take(const std::string_view expected) -> Doc {
    if (peek().text != expected) {
      report(std::format("expected '{}' but found '{}'", expected, peek().text));
      return text(std::string(expected));
    }
    return take_current(std::string(expected));
  }

  // The next token, which must be one of the spellings the parser accepts at this point. It keeps its spelling.
  auto take_one_of(const std::initializer_list<std::string_view> spellings) -> Doc {
    if (std::ranges::find(spellings, std::string_view(peek().text)) == spellings.end()) {
      report(std::format("unexpected '{}'", peek().text));
      return text(std::string(*spellings.begin()));
    }
    return take_current(peek().text);
  }

  // A number, string, Bool, or null literal, spelled as in the source.
  auto take_literal() -> Doc {
    if (const TokenKind kind = peek().kind; kind != TokenKind::kNumeric && kind != TokenKind::kString &&
                                            kind != TokenKind::kBool && kind != TokenKind::kNull) {
      report(std::format("expected a literal but found '{}'", peek().text));
      return text(peek().text);
    }
    if (peek().text.find('\n') != std::string::npos) {
      return take_current(multi_line_raw_string(peek().text));
    }
    return take_current(peek().text);
  }

  [[nodiscard]] auto next_is_multi_line_string() const -> bool {
    return peek().kind == TokenKind::kString && peek().text.find('\n') != std::string::npos;
  }

  // The '>' that closes a type argument or generic parameter list. A lexed '>=' splits into '>' and '=', as the parser
  // splits it.
  auto take_type_args_close() -> Doc {
    if (peek().text != ">=") {
      return take(">");
    }
    Doc closing = concat({leading_comments(), text(">")});
    Token& rest = tokens_[index_];
    rest.text = "=";
    rest.value = "=";
    ++rest.offset;
    ++rest.col;
    return closing;
  }

  // The arrow of a function type, written '=>' or '= >'.
  auto take_function_arrow() -> Doc {
    if (peek().text == "=>") {
      return take("=>");
    }
    Doc equals = take("=");
    return concat({std::move(equals), take(">")});
  }

  // An import's module name. A digit-leading name such as '20_export' lexes as several adjacent tokens.
  auto take_module_name(const std::string_view name) -> Doc {
    std::vector<Doc> parts;
    std::string spelled;
    while (spelled.size() < name.size() && !at_end()) {
      spelled += peek().text;
      parts.push_back(take_current(peek().text));
    }
    if (spelled != name) {
      report(std::format("expected module name '{}' but found '{}'", name, spelled));
    }
    return concat(std::move(parts));
  }

private:
  // A multi-line raw string, indented one level past where it is printed. Its lines keep their indentation relative to
  // the closing quotes, which is all its value depends on.
  static auto multi_line_raw_string(const std::string& source_text) -> Doc {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (true) {
      const std::size_t newline = source_text.find('\n', start);
      std::string line = source_text.substr(start, newline == std::string::npos ? std::string::npos : newline - start);
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      lines.push_back(std::move(line));
      if (newline == std::string::npos) {
        break;
      }
      start = newline + 1;
    }

    const std::string& opening = lines.front();
    const std::string& closing = lines.back();
    const std::size_t closing_indent = closing.find_first_not_of(" \t");
    std::vector<Doc> body;
    for (std::size_t index = 1; index + 1 < lines.size(); ++index) {
      const std::string& line = lines[index];
      body.push_back(hard_line());
      if (line.find_first_not_of(" \t") != std::string::npos) {
        body.push_back(text(line.substr(closing_indent)));
      }
    }
    body.push_back(hard_line());
    body.push_back(text(closing.substr(closing_indent)));
    return concat({text(opening.substr(0, opening.find_last_not_of(" \t\r") + 1)), indent(std::move(body))});
  }

  auto take_current(Doc printed) -> Doc {
    std::vector<Doc> parts{leading_comments(), std::move(printed)};
    for (Comment& comment : std::exchange(comments_[index_].trailing, {})) {
      parts.push_back(line_suffix(std::move(comment.text)));
    }
    last_line_ = peek().end_line();
    if (index_ + 1 < tokens_.size()) {
      ++index_;
    }
    return concat(std::move(parts));
  }

  auto take_current(std::string printed) -> Doc { return take_current(text(std::move(printed))); }

  // Comments on their own lines inside an expression, each on a line of its own before the token.
  auto leading_comments() -> Doc {
    std::vector<Doc> parts;
    for (Comment& comment : std::exchange(comments_[index_].leading, {})) {
      parts.push_back(text(std::move(comment.text)));
      parts.push_back(hard_line());
    }
    return concat(std::move(parts));
  }

  std::vector<Token> tokens_;
  std::vector<TokenComments> comments_;
  std::size_t index_ = 0;
  int last_line_ = 1;
  std::optional<std::string> error_{std::nullopt};
};

// A pipeline stage the parser takes as a call target: a name, a qualified name, or an operator.
auto is_call_target(const model::Primary& primary) -> bool {
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

auto is_bare_block(const model::Expression& expression) -> bool {
  return expression.expr.rhs.empty() && std::holds_alternative<model::BlockExpressionBox>(expression.expr.lhs.base.value);
}

auto is_bare_literal(const model::Expression& expression) -> bool {
  return expression.expr.rhs.empty() && std::holds_alternative<model::Constant>(expression.expr.lhs.base.value);
}

auto split_qualifier(const std::string_view qualifier) -> std::vector<std::string_view> {
  std::vector<std::string_view> segments;
  std::size_t start = 0;
  while (start <= qualifier.size()) {
    const std::size_t dot = qualifier.find('.', start);
    const std::size_t end = dot == std::string_view::npos ? qualifier.size() : dot;
    segments.push_back(qualifier.substr(start, end - start));
    if (dot == std::string_view::npos) {
      break;
    }
    start = dot + 1;
  }
  return segments;
}

// Builds the document for a program from its syntax tree, taking every token from the cursor in source order.
//
// Layout rules:
// - Statements and block items each start a line; one blank line is kept wherever the source had blank lines.
// - A let, closure, or block-local binding keeps its body on the '=' line when it fits, otherwise the body moves to the
//   next line, indented. A block body stays on the '=' line and breaks inside itself.
// - A pipeline keeps its stages on one line when they fit. Otherwise every call starts a line with '->', where a call
//   is a run of stages up to a call target, as in '-> (_, 1) -> Std.Add'.
// - Tuples, parameter lists, and blocks stay on one line when they fit, otherwise one item per line.
class Printer {
public:
  explicit Printer(TokenCursor& cursor) : cursor_(cursor) {}

  auto program(const model::Program& program) -> Doc {
    std::vector<Doc> parts;
    for (std::size_t index = 0; index < program.statements.size(); ++index) {
      begin_item(parts, index > 0, hard_line());
      parts.push_back(statement(program.statements[index]));
      parts.push_back(cursor_.take(";"));
    }

    // Comments after the last statement.
    const std::vector<Comment> comments = cursor_.take_leading_comments();
    for (std::size_t index = 0; index < comments.size(); ++index) {
      const int previous_line = index == 0 ? cursor_.last_line() : comments[index - 1].line;
      if (!parts.empty()) {
        parts.push_back(hard_line());
        if (comments[index].line > previous_line + 1) {
          parts.push_back(hard_line());
        }
      }
      parts.push_back(text(comments[index].text));
    }
    return concat(std::move(parts));
  }

private:
  // Starts the next item of a sequence of statements or block items: the separator from the item before, one blank
  // line where the source had any, and the comments on their own lines above the item, keeping the blank lines
  // between them.
  void begin_item(std::vector<Doc>& parts, const bool has_previous, const Doc& separator) {
    const std::vector<Comment> comments = cursor_.take_leading_comments();
    const int first_line = comments.empty() ? cursor_.peek().line : comments.front().line;
    if (has_previous) {
      parts.push_back(separator);
      if (first_line > cursor_.last_line() + 1) {
        parts.push_back(hard_line());
      }
    }
    for (std::size_t index = 0; index < comments.size(); ++index) {
      if (index > 0 && comments[index].line > comments[index - 1].line + 1) {
        parts.push_back(hard_line());
      }
      parts.push_back(text(comments[index].text));
      parts.push_back(hard_line());
    }
    if (!comments.empty() && cursor_.peek().line > comments.back().line + 1) {
      parts.push_back(hard_line());
    }
  }

  auto statement(const model::Statement& stmt) -> Doc {
    return std::visit(common::overloaded{
                          [this](const model::ImportStatement& import_stmt) -> Doc { return import_statement(import_stmt); },
                          [this](const model::TypeStatement& type_decl) -> Doc { return type_statement(type_decl); },
                          [this](const model::AliasStatement& alias) -> Doc { return alias_statement(alias); },
                          [this](const model::LetStatement& let) -> Doc { return let_statement(let); },
                          [this](const model::ExpressionStatement& expr) -> Doc { return expression(expr.expr); },
                      },
                      stmt);
  }

  auto import_statement(const model::ImportStatement& import_stmt) -> Doc {
    return concat({cursor_.take("import"), text(" "), cursor_.take_module_name(import_stmt.module_name)});
  }

  auto type_statement(const model::TypeStatement& type_decl) -> Doc {
    std::vector<Doc> parts{cursor_.take("type"), text(" "), cursor_.take(type_decl.name)};
    if (!type_decl.generic_params.empty()) {
      parts.push_back(generic_params(type_decl.generic_params, {}));
    }
    parts.push_back(text(" "));
    parts.push_back(cursor_.take_one_of({"::", "="}));
    parts.push_back(text(" "));
    parts.push_back(type_decl.is_builtin ? cursor_.take("__builtin__") : type(type_decl.target));
    return concat(std::move(parts));
  }

  auto alias_statement(const model::AliasStatement& alias) -> Doc {
    return concat({cursor_.take("alias"), text(" "), cursor_.take(alias.name), text(" "), cursor_.take("="), text(" "),
                   type(alias.target)});
  }

  auto let_statement(const model::LetStatement& let) -> Doc {
    std::vector<Doc> parts{cursor_.take("let"), text(" "), name_path(let.id)};
    if (!let.generic_params.empty()) {
      parts.push_back(generic_params(let.generic_params, let.generic_bounds));
    }
    if (let.is_value_binding) {
      parts.push_back(cursor_.take(":"));
      parts.push_back(text(" "));
      parts.push_back(type(let.rtype));
    } else {
      parts.push_back(signature(let.params, let.rtype));
    }
    parts.push_back(text(" "));
    parts.push_back(let.is_value_binding ? cursor_.take("=") : cursor_.take_one_of({"::", "="}));
    if (let.is_builtin || let.is_native) {
      parts.push_back(text(" "));
      parts.push_back(cursor_.take(let.is_builtin ? "__builtin__" : "__native__"));
    } else if (let.expr.has_value()) {
      append_body(parts, *let.expr);
    }
    return group(std::move(parts));
  }

  // The body after '=': on the '=' line when it fits there, otherwise on the next line, indented. The body is a group
  // of its own, so a header whose parameters broke across lines still keeps a short body on its '=' line. A block, or
  // a multi-line raw string, stays on the '=' line and breaks inside itself.
  void append_body(std::vector<Doc>& parts, const model::Expression& body) {
    if (is_bare_block(body) || (is_bare_literal(body) && cursor_.next_is_multi_line_string())) {
      parts.push_back(text(" "));
      parts.push_back(expression(body));
      return;
    }
    parts.push_back(group({indent({line(), expression(body)})}));
  }

  auto name_path(const std::variant<std::string, model::QualifiedId>& name) -> Doc {
    return std::visit(common::overloaded{
                          [this](const std::string& simple) -> Doc { return cursor_.take(simple); },
                          [this](const model::QualifiedId& qualified) -> Doc { return qualified_id(qualified); },
                      },
                      name);
  }

  auto qualified_id(const model::QualifiedId& qualified) -> Doc {
    std::vector<Doc> parts;
    for (const std::string_view segment : split_qualifier(qualified.qualifier.qualifier)) {
      parts.push_back(cursor_.take(segment));
      parts.push_back(cursor_.take("."));
    }
    parts.push_back(cursor_.take(qualified.id));
    return concat(std::move(parts));
  }

  // The parts of a comma-separated list after `open`, for a group: on one line when it fits, otherwise one item a
  // line, indented, with the closing token on a line of its own. `item` takes the tokens of the item at an index, and
  // `close` takes the closing token.
  auto comma_list_parts(Doc open, const std::size_t count, const std::invocable<std::size_t> auto& item,
                        const std::invocable<> auto& close) -> std::vector<Doc> {
    if (count == 0) {
      return {std::move(open), close()};
    }
    std::vector<Doc> items{soft_line()};
    for (std::size_t index = 0; index < count; ++index) {
      if (index > 0) {
        items.push_back(cursor_.take(","));
        items.push_back(line());
      }
      items.push_back(item(index));
    }
    Doc closing = close();
    return {std::move(open), indent(std::move(items)), soft_line(), std::move(closing)};
  }

  auto comma_list(Doc open, const std::size_t count, const std::invocable<std::size_t> auto& item,
                  const std::invocable<> auto& close) -> Doc {
    return group(comma_list_parts(std::move(open), count, item, close));
  }

  // '<T, U: Bound>'. `bounds` runs parallel to `names`; a missing or empty entry is an unbounded parameter.
  auto generic_params(const std::vector<std::string>& names, const std::vector<model::TypeBox>& bounds) -> Doc {
    Doc open = cursor_.take("<");
    return comma_list(
        std::move(open), names.size(),
        [&](const std::size_t index) -> Doc {
          Doc name = cursor_.take(names[index]);
          if (index >= bounds.size() || !bounds[index].has_value()) {
            return name;
          }
          Doc colon = cursor_.take(":");
          return concat({std::move(name), std::move(colon), text(" "), type(*bounds[index])});
        },
        [this]() -> Doc { return cursor_.take_type_args_close(); });
  }

  // '(a: A, b: B): R'. The return type belongs to the parameters' group, so a signature too long for its line breaks
  // its parameters first; the return type's own type arguments break only when it is still too long after that.
  auto signature(const model::ParameterDeclList& list, const model::TypeNode& return_type) -> Doc {
    Doc open = cursor_.take("(");
    std::vector<Doc> parts = comma_list_parts(
        std::move(open), list.params.size(),
        [&](const std::size_t index) -> Doc {
          const model::Parameter& param = list.params[index];
          return concat({cursor_.take(param.param_name), cursor_.take(":"), text(" "), type(param.type)});
        },
        [this]() -> Doc { return cursor_.take(")"); });
    parts.push_back(cursor_.take(":"));
    parts.push_back(text(" "));
    parts.push_back(type(return_type));
    return group(std::move(parts));
  }

  // '<A, B>' after 'Tuple', a type name, or a call target.
  auto type_args(const std::vector<model::TypeBox>& types) -> Doc {
    Doc open = cursor_.take("<");
    return comma_list(
        std::move(open), types.size(), [&](const std::size_t index) -> Doc { return type(*types[index]); },
        [this]() -> Doc { return cursor_.take_type_args_close(); });
  }

  auto type(const model::TypeNode& node) -> Doc {
    Doc base = std::visit(
        common::overloaded{
            [this](const std::string& name) -> Doc { return cursor_.take(name); },
            [this](const model::QualifiedId& qualified) -> Doc { return qualified_id(qualified); },
            [this](const model::TypeListBox& tuple) -> Doc {
              Doc keyword = cursor_.take("Tuple");
              return concat({std::move(keyword), type_args(tuple->types)});
            },
            [this](const model::UnionTypeListBox& alternatives) -> Doc {
              std::vector<Doc> parts;
              for (std::size_t index = 0; index < alternatives->alternatives.size(); ++index) {
                if (index > 0) {
                  parts.push_back(text(" "));
                  parts.push_back(cursor_.take("|"));
                  parts.push_back(text(" "));
                }
                parts.push_back(type(*alternatives->alternatives[index]));
              }
              return concat(std::move(parts));
            },
            [this](const model::AppliedTypeNodeBox& applied) -> Doc {
              Doc name = cursor_.take(applied->name);
              return concat({std::move(name), type_args(applied->args.types)});
            },
            [this](const model::FunctionTypeNodeBox& function) -> Doc {
              const std::vector<model::TypeBox>& params = function->params.types;
              Doc open = cursor_.take("(");
              Doc param_list = comma_list(
                  std::move(open), params.size(),
                  [&](const std::size_t index) -> Doc { return type(*params[index]); },
                  [this]() -> Doc { return cursor_.take(")"); });
              Doc arrow = cursor_.take_function_arrow();
              return concat({std::move(param_list), text(" "), std::move(arrow), text(" "), type(*function->return_type)});
            },
        },
        node.value);
    if (node.variadic) {
      return concat({std::move(base), cursor_.take("...")});
    }
    return base;
  }

  auto expression(const model::Expression& expr) -> Doc { return flow(expr.expr); }

  auto flow(const model::FlowExpression& flow_expr) -> Doc {
    Doc head = primary(flow_expr.lhs);
    if (flow_expr.rhs.empty()) {
      return head;
    }
    // When the stages break, every call starts a line: a run of stages up to a call target stays together.
    std::vector<Doc> stages;
    bool inside_call = false;
    for (const model::Primary& stage : flow_expr.rhs) {
      stages.push_back(inside_call ? text(" ") : line());
      stages.push_back(cursor_.take("->"));
      stages.push_back(text(" "));
      stages.push_back(primary(stage));
      inside_call = !is_call_target(stage);
    }
    return concat({std::move(head), group({indent(std::move(stages))})});
  }

  auto primary(const model::Primary& primary_expr) -> Doc {
    if (!primary_expr.extra.empty()) {
      cursor_.report("a primary with extra segments has no source form");
    }
    return atom(primary_expr.base);
  }

  auto atom(const model::Atom& atom_expr) -> Doc {
    return std::visit(common::overloaded{
                          [this](const std::monostate&) -> Doc {
                            Doc open = cursor_.take("(");
                            return concat({std::move(open), cursor_.take(")")});
                          },
                          [this](const model::DelimitedExpressionBox& items) -> Doc { return delimited(*items); },
                          [this](const model::ClosureExpressionBox& closure_expr) -> Doc {
                            return closure(*closure_expr);
                          },
                          [this](const model::BlockExpressionBox& block_expr) -> Doc { return block(*block_expr); },
                          [this](const model::Constant&) -> Doc { return literal(); },
                          [this](const model::QualifiedId& qualified) -> Doc { return qualified_id(qualified); },
                          [this](const std::string& operator_symbol) -> Doc { return cursor_.take(operator_symbol); },
                          [this](const model::NamedTargetBox& target) -> Doc { return named_target(*target); },
                      },
                      atom_expr.value);
  }

  auto delimited(const model::DelimitedExpression& items) -> Doc {
    Doc open = cursor_.take("(");
    std::vector<Doc> parts{soft_line()};
    for (std::size_t index = 0; index < items.items.size(); ++index) {
      if (index > 0) {
        parts.push_back(cursor_.take(","));
        parts.push_back(line());
      }
      parts.push_back(expression(*items.items[index]));
      if (index < items.spread_items.size() && items.spread_items[index]) {
        parts.push_back(cursor_.take("..."));
      }
    }
    if (items.trailing_comma) {
      parts.push_back(cursor_.take(","));
    }
    return group({std::move(open), indent(std::move(parts)), soft_line(), cursor_.take(")")});
  }

  auto closure(const model::ClosureExpression& closure_expr) -> Doc {
    std::vector<Doc> parts;
    if (!closure_expr.generic_params.empty()) {
      parts.push_back(generic_params(closure_expr.generic_params, closure_expr.generic_bounds));
    }
    parts.push_back(signature(closure_expr.params, closure_expr.rtype));
    parts.push_back(text(" "));
    parts.push_back(cursor_.take_one_of({"::", "="}));
    if (closure_expr.body.has_value()) {
      append_body(parts, *closure_expr.body);
    }
    return group(std::move(parts));
  }

  auto block(const model::BlockExpression& block_expr) -> Doc {
    Doc open = cursor_.take("{");
    std::vector<Doc> items{line()};
    for (std::size_t index = 0; index < block_expr.items.size(); ++index) {
      begin_item(items, index > 0, line());
      items.push_back(block_item(block_expr.items[index]));
    }
    begin_item(items, !block_expr.items.empty(), line());
    if (block_expr.result.has_value()) {
      items.push_back(expression(*block_expr.result));
    }
    items.push_back(cursor_.take(";"));
    return group({std::move(open), indent(std::move(items)), line(), cursor_.take("}")});
  }

  auto block_item(const model::BlockItem& item) -> Doc {
    return std::visit(common::overloaded{
                          [this](const model::LocalLetBinding& binding) -> Doc {
                            std::vector<Doc> parts{cursor_.take("let"), text(" "), cursor_.take(binding.name),
                                                   cursor_.take(":"),   text(" "), type(binding.type),
                                                   text(" "),           cursor_.take("=")};
                            if (binding.expr.has_value()) {
                              append_body(parts, *binding.expr);
                            }
                            return concat({group(std::move(parts)), cursor_.take(";")});
                          },
                          [this](const model::ExpressionStatement& stmt) -> Doc {
                            return concat({expression(stmt.expr), cursor_.take(";")});
                          },
                      },
                      item);
  }

  // A literal as written, including the '-' of a negative number.
  auto literal() -> Doc {
    if (cursor_.peek().text == "-") {
      Doc minus = cursor_.take("-");
      return concat({std::move(minus), cursor_.take_literal()});
    }
    return cursor_.take_literal();
  }

  auto named_target(const model::NamedTarget& target) -> Doc {
    Doc name = name_path(target.target);
    if (target.explicit_type_args.empty()) {
      return name;
    }
    return concat({std::move(name), type_args(target.explicit_type_args)});
  }

  TokenCursor& cursor_;
};

auto formatter_error(const std::string& detail) -> parse::ParseError {
  return parse::ParseError{
      .message = "Formatter error: " + detail,
      .hint = "This is a bug in the formatter. The source was left unchanged.",
      .span = std::nullopt,
  };
}

}  // namespace

auto format_source(const std::string& source, const std::string& source_name, const FormatOptions& options)
    -> tl::expected<std::string, parse::ParseError> {
  constexpr parse::Parser parser;
  const auto parsed = parser.parse_program(source, source_name);
  if (!parsed) {
    return tl::unexpected(parsed.error());
  }
  auto lexed = parse::lex_program_with_comments(source, source_name);
  if (!lexed) {
    return tl::unexpected(lexed.error());
  }

  TokenCursor cursor(std::move(lexed->tokens), lexed->comments);
  const Doc doc = Printer(cursor).program(*parsed);
  if (cursor.error().has_value()) {
    return tl::unexpected(formatter_error(*cursor.error()));
  }
  if (!cursor.at_end()) {
    return tl::unexpected(formatter_error(std::format("tokens left over from line {}", cursor.peek().line)));
  }

  std::string formatted = render(doc, RenderOptions{
                                          .width = options.width,
                                          .indent = options.indent,
                                          .newline = source.find("\r\n") != std::string::npos ? "\r\n" : "\n",
                                      });

  // The formatted source must be the same program: it parses, to the same tree.
  const auto reparsed = parser.parse_program(formatted, source_name);
  if (!reparsed) {
    return tl::unexpected(formatter_error("the formatted source does not parse: " + reparsed.error().message));
  }
  if (parser.dump_ast(*reparsed) != parser.dump_ast(*parsed)) {
    return tl::unexpected(formatter_error("the formatted source parses to a different program"));
  }
  return formatted;
}

}  // namespace fleaux::frontend::format
