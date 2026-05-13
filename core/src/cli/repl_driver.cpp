#include "fleaux/cli/repl_driver.hpp"

#include <cctype>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "fleaux/cli/line_editor.hpp"
#include "fleaux/frontend/diagnostics.hpp"
#include "fleaux/frontend/lexer.hpp"
#include "fleaux/vm/builtin_catalog.hpp"
#include "fleaux/vm/runtime.hpp"
#include <algorithm>

namespace fleaux::cli {
namespace {

enum class ReplCommand {
  kNone,
  kQuit,
  kHelp,
  kClear,
  kUnknown,
};

struct ParsedReplCommand {
  ReplCommand kind{ReplCommand::kNone};
  std::string text{};
};

auto trim_copy(const std::string& text) -> std::string {
  const auto begin_it =
      std::ranges::find_if_not(text, [](const unsigned char ch) -> bool { return std::isspace(ch) != 0; });
  const auto end_it = std::ranges::find_if_not(std::views::reverse(text), [](const unsigned char ch) -> bool {
                        return std::isspace(ch) != 0;
                      }).base();
  if (begin_it >= end_it) {
    return {};
  }
  return {begin_it, end_it};
}

auto buffer_has_complete_statement(const std::string& buffer) -> bool {
  int paren_depth = 0;
  bool in_string = false;
  bool escaped = false;
  for (const char ch : buffer) {
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        in_string = false;
      }
      continue;
    }

    if (ch == '"') {
      in_string = true;
      continue;
    }
    if (ch == '(') {
      ++paren_depth;
      continue;
    }
    if (ch == ')') {
      --paren_depth;
    }
  }

  if (paren_depth != 0 || in_string) {
    return false;
  }
  return trim_copy(buffer).ends_with(';');
}

[[nodiscard]] auto repl_color_enabled(const bool requested) -> bool {
  if (!requested) {
    return false;
  }
  const char* no_color = std::getenv("NO_COLOR");
  return no_color == nullptr || *no_color == '\0';
}

auto parse_repl_command(const std::string& line) -> ParsedReplCommand {
  ParsedReplCommand parsed{.text = trim_copy(line)};
  if (parsed.text.empty()) {
    return parsed;
  }
  if (parsed.text == ":quit" || parsed.text == ":q") {
    parsed.kind = ReplCommand::kQuit;
  } else if (parsed.text == ":help" || parsed.text == ":?") {
    parsed.kind = ReplCommand::kHelp;
  } else if (parsed.text == ":clear" || parsed.text == ":c") {
    parsed.kind = ReplCommand::kClear;
  } else if (parsed.text.starts_with(':')) {
    parsed.kind = ReplCommand::kUnknown;
  }
  return parsed;
}

auto compute_repl_style_spans(const std::string_view text) -> std::vector<StyleSpan> {
  const auto tokens = fleaux::frontend::parse::lex_program_best_effort(std::string(text), "<repl>");
  std::vector<StyleSpan> spans;
  spans.reserve(tokens.size());

  for (const auto& token : tokens) {
    TokenClass token_class = TokenClass::kPlain;
    switch (token.kind) {
      case fleaux::frontend::parse::TokenKind::kString:
        token_class = TokenClass::kString;
        break;
      case fleaux::frontend::parse::TokenKind::kNumeric:
      case fleaux::frontend::parse::TokenKind::kBool:
      case fleaux::frontend::parse::TokenKind::kNull:
        token_class = TokenClass::kNumber;
        break;
      case fleaux::frontend::parse::TokenKind::kIdent:
        token_class = fleaux::frontend::parse::is_keyword(token.value) ? TokenClass::kKeyword : TokenClass::kIdentifier;
        break;
      case fleaux::frontend::parse::TokenKind::kSymbol:
        token_class = TokenClass::kOperator;
        break;
      case fleaux::frontend::parse::TokenKind::kError:
        token_class = TokenClass::kError;
        break;
      case fleaux::frontend::parse::TokenKind::kEof:
        continue;
    }

    spans.push_back(StyleSpan{.start = token.offset, .length = token.text.size(), .token_class = token_class});
  }

  return spans;
}

auto make_repl_style_provider() -> auto {
  return [](const std::string_view text) -> std::vector<StyleSpan> { return compute_repl_style_spans(text); };
}

auto seed_completion_symbols(CompletionHandler& completion) -> void {
  for (const auto keyword : fleaux::frontend::parse::keyword_spellings()) {
    completion.load_symbols({keyword});
  }

  std::vector<std::string> builtins;
  builtins.reserve(vm::all_builtin_specs().size());
  for (const auto& spec : vm::all_builtin_specs()) {
    builtins.emplace_back(spec.name);
  }
  completion.load_symbols(builtins);
}

auto extract_declared_functions(const std::string_view snippet) -> std::vector<std::string> {
  auto is_ident_start = [](const char ch) -> bool {
    const auto uch = static_cast<unsigned char>(ch);
    return std::isalpha(uch) != 0 || ch == '_';
  };
  auto is_ident_char = [](const char ch) -> bool {
    const auto uch = static_cast<unsigned char>(ch);
    return std::isalnum(uch) != 0 || ch == '_';
  };
  auto skip_whitespace = [&](std::size_t idx) -> std::size_t {
    while (idx < snippet.size() && std::isspace(static_cast<unsigned char>(snippet[idx])) != 0) {
      ++idx;
    }
    return idx;
  };
  auto is_boundary = [&](const std::size_t idx) -> bool {
    if (idx >= snippet.size()) {
      return true;
    }
    return !is_ident_char(snippet[idx]);
  };

  std::vector<std::string> names;
  for (std::size_t idx = 0; idx + 3 <= snippet.size(); ++idx) {
    if (snippet.substr(idx, 3) != "let") {
      continue;
    }
    if ((idx > 0 && !is_boundary(idx - 1)) || !is_boundary(idx + 3)) {
      continue;
    }

    std::size_t cursor = skip_whitespace(idx + 3);
    if (cursor >= snippet.size() || !is_ident_start(snippet[cursor])) {
      continue;
    }

    const std::size_t name_start = cursor;
    while (cursor < snippet.size()) {
      if (is_ident_char(snippet[cursor])) {
        ++cursor;
        continue;
      }
      if (snippet[cursor] == '.' && cursor + 1 < snippet.size() && is_ident_start(snippet[cursor + 1])) {
        ++cursor;
        continue;
      }
      break;
    }

    if (const std::size_t after_name = skip_whitespace(cursor);
        after_name >= snippet.size() || snippet[after_name] != '(') {
      continue;
    }

    if (const auto symbol = std::string(snippet.substr(name_start, cursor - name_start));
        std::ranges::find(names, symbol) == names.end()) {
      names.push_back(symbol);
    }
  }

  return names;
}

}  // namespace

auto ReplDriver::run(const std::vector<std::string>& process_args, const bool color_enabled,
                     const vm::RuntimeCompileOptions& compile_options) const -> int {
  const bool interactive_stdin = stdin_is_interactive();
  const bool use_color = repl_color_enabled(color_enabled);

  LineEditor line_editor(LineEditorConfig{
      .style_span_provider = use_color ? make_repl_style_provider() : StyleSpanProvider{},
      .completion_handler = CompletionHandler{},
  });

  seed_completion_symbols(line_editor.completion_handler().value());

  constexpr vm::Runtime runtime;
  const auto session = runtime.create_session(process_args, compile_options);
  const auto run_snippet = [session](const std::string& snippet) -> std::optional<vm::RuntimeError> {
    if (const auto result = session.run_snippet(snippet, std::cout); !result) {
      return result.error();
    }
    return std::nullopt;
  };

  const auto print_help = []() -> void {
    std::cout << "Commands:\n"
              << "  :help, :?         Show this help\n"
              << "  :quit, :q         Exit REPL\n"
              << "  :clear, :c        Clear current multiline buffer\n"
              << "\n"
              << "Tips:\n"
              << "  - End statements with ';'\n"
              << "  - Multiline input is supported until a complete statement is formed\n"
              << "  - Normal imports resolve relative to the current working directory\n"
              << "  - Std remains a symbolic import\n";
  };

  std::cout << "Fleaux REPL\n"
            << "Type :help (or :?) for commands, :quit to exit.\n";

  std::string buffer;
  while (true) {
    std::string line;

    if (interactive_stdin) {
      const auto [action, line_opt] = read_interactive_line(line_editor, buffer.empty() ? ">>> " : "... ");
      if (action == LineEditorAction::kEndOfInput) {
        break;
      }
      if (action == LineEditorAction::kClearBuffer) {
        buffer.clear();
        continue;
      }
      if (!line_opt.has_value()) {
        continue;
      }
      line = *line_opt;
    } else {
      std::cout << (buffer.empty() ? ">>> " : "... ");
      if (!std::getline(std::cin, line)) {
        std::cout << '\n';
        break;
      }
    }

    if (buffer.empty()) {
      const auto command = parse_repl_command(line);
      if (command.kind == ReplCommand::kQuit) {
        break;
      }
      if (command.kind == ReplCommand::kHelp) {
        print_help();
        continue;
      }
      if (command.kind == ReplCommand::kClear) {
        buffer.clear();
        continue;
      }
      if (command.kind == ReplCommand::kUnknown) {
        std::cout << "Unknown REPL command: " << command.text << "\n"
                  << "Type :help to list supported commands.\n";
        continue;
      }
      if (command.text.empty()) {
        continue;
      }
    }

    if (!buffer.empty()) {
      buffer.push_back('\n');
    }
    buffer += line;

    if (!buffer_has_complete_statement(buffer)) {
      continue;
    }

    if (const auto error = run_snippet(buffer); error.has_value()) {
      std::cerr << fleaux::frontend::diag::format_diagnostic("vm-repl", error->message, error->span, error->hint)
                << '\n';
    } else {
      for (const auto& symbol : extract_declared_functions(buffer)) {
        line_editor.completion_handler()->load_symbols({std::string_view{symbol}});
      }
    }
    buffer.clear();
  }

  return 0;
}

}  // namespace fleaux::cli
