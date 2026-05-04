#include "fleaux/cli/repl_driver.hpp"

#include <array>
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
#include "fleaux/vm/builtin_catalog.hpp"
#include "fleaux/vm/runtime.hpp"
#include <algorithm>

namespace fleaux::cli {
namespace {

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

auto make_repl_style_provider() -> StyleSpanProvider {
  return [](const std::string_view text) -> std::vector<StyleSpan> {
    constexpr std::array kKeywords = {
        std::string_view{"import"}, std::string_view{"let"},    std::string_view{"__builtin__"},
        std::string_view{"Int64"},  std::string_view{"UInt64"}, std::string_view{"Float64"},
        std::string_view{"String"}, std::string_view{"Bool"},   std::string_view{"Null"},
        std::string_view{"Any"},    std::string_view{"Tuple"},
    };
    constexpr std::array kLiterals = {
        std::string_view{"null"},
        std::string_view{"True"},
        std::string_view{"False"},
    };
    constexpr std::array<char, 8> kOtherSymbols = {'(', ')', ',', ';', ':', '=', '|', '.'};

    auto is_ident_start = [](const char ch) -> bool {
      const auto uch = static_cast<unsigned char>(ch);
      return std::isalpha(uch) != 0 || ch == '_';
    };
    auto is_ident_char = [&](const char ch) -> bool {
      const auto uch = static_cast<unsigned char>(ch);
      return std::isalnum(uch) != 0 || ch == '_';
    };
    auto is_keyword = [&](const std::string_view token) -> bool {
      return std::ranges::any_of(kKeywords, [&](const auto& keyword) -> bool { return keyword == token; });
    };
    auto is_literal = [&](const std::string_view token) -> bool {
      return std::ranges::any_of(kLiterals, [&](const auto& literal) -> bool { return literal == token; });
    };
    auto is_other_symbol = [&](const char ch) -> bool {
      return std::ranges::any_of(kOtherSymbols, [&](const auto& symbol) -> bool { return symbol == ch; });
    };

    std::vector<StyleSpan> spans;
    spans.reserve(text.size() / 4);

    std::size_t cursor = 0;
    while (cursor < text.size()) {
      const char ch = text[cursor];

      if (ch == '"') {
        const std::size_t start = cursor++;
        bool escaped = false;
        while (cursor < text.size()) {
          const char current = text[cursor++];
          if (escaped) {
            escaped = false;
            continue;
          }
          if (current == '\\') {
            escaped = true;
            continue;
          }
          if (current == '"') {
            break;
          }
        }
        spans.push_back(StyleSpan{.start = start, .length = cursor - start, .token_class = TokenClass::kString});
        continue;
      }

      if (std::isdigit(static_cast<unsigned char>(ch)) != 0) {
        const std::size_t start = cursor++;
        bool seen_dot = false;
        while (cursor < text.size()) {
          const char current = text[cursor];
          if (std::isdigit(static_cast<unsigned char>(current)) != 0) {
            ++cursor;
            continue;
          }
          if (!seen_dot && current == '.') {
            seen_dot = true;
            ++cursor;
            continue;
          }
          break;
        }
        spans.push_back(StyleSpan{.start = start, .length = cursor - start, .token_class = TokenClass::kNumber});
        continue;
      }

      if (is_ident_start(ch)) {
        const std::size_t start = cursor++;
        while (cursor < text.size() && is_ident_char(text[cursor])) {
          ++cursor;
        }
        const auto token = text.substr(start, cursor - start);
        const auto token_class = is_keyword(token)   ? TokenClass::kKeyword
                                 : is_literal(token) ? TokenClass::kNumber
                                                     : TokenClass::kIdentifier;
        spans.push_back(StyleSpan{.start = start, .length = cursor - start, .token_class = token_class});
        continue;
      }

      if ((ch == '-' && (cursor + 1) < text.size() && text[cursor + 1] == '>') || is_other_symbol(ch)) {
        const std::size_t length = (ch == '-') ? 2U : 1U;
        spans.push_back(StyleSpan{.start = cursor, .length = length, .token_class = TokenClass::kOperator});
        cursor += length;
        continue;
      }

      ++cursor;
    }

    return spans;
  };
}

constexpr std::array<std::string_view, 11> kReplKeywords = {
    "import", "let", "__builtin__", "Int64", "UInt64", "Float64", "String", "Bool", "Null", "Any", "Tuple",
};

auto seed_completion_symbols(CompletionHandler& completion) -> void {
  for (const auto keyword : kReplKeywords) {
    completion.load_symbols({keyword});
  }

  std::vector<std::string> builtins;
  builtins.reserve(fleaux::vm::all_builtin_specs().size());
  for (const auto& spec : fleaux::vm::all_builtin_specs()) {
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
                     const fleaux::vm::RuntimeCompileOptions& compile_options) const -> int {
  const bool interactive_stdin = stdin_is_interactive();
  const bool use_color = repl_color_enabled(color_enabled);
  CompletionHandler completion_handler;
  seed_completion_symbols(completion_handler);
  LineEditor line_editor(LineEditorConfig{
      .style_span_provider = use_color ? make_repl_style_provider() : StyleSpanProvider{},
      .completion_handler = &completion_handler,
  });
  constexpr fleaux::vm::Runtime runtime;
  const auto session = runtime.create_session(process_args, compile_options);
  const auto run_snippet = [session](const std::string& snippet) -> std::optional<fleaux::vm::RuntimeError> {
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
      const auto command = trim_copy(line);
      if (command == ":quit" || command == ":q") {
        break;
      }
      if (command == ":help" || command == ":?") {
        print_help();
        continue;
      }
      if (command == ":clear" || command == ":c") {
        buffer.clear();
        continue;
      }
      if (!command.empty() && command.starts_with(':')) {
        std::cout << "Unknown REPL command: " << command << "\n"
                  << "Type :help to list supported commands.\n";
        continue;
      }
      if (command.empty()) {
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
        completion_handler.load_symbols({std::string_view{symbol}});
      }
    }
    buffer.clear();
  }

  return 0;
}

}  // namespace fleaux::cli
