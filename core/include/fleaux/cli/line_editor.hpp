#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "fleaux/common/trie.hpp"

namespace fleaux::cli {

enum class InputKey {
  kCharacter,
  kEnter,
  kBackspace,
  kDelete,
  kArrowLeft,
  kArrowRight,
  kArrowUp,
  kArrowDown,
  kTokenLeft,
  kTokenRight,
  kTokenBackspace,
  kHome,
  kEnd,
  kCtrlC,
  kCtrlD,
  kTab,
  kUnknown,
};

struct InputEvent {
  InputKey key = InputKey::kUnknown;
  char ch = '\0';

  [[nodiscard]] static auto character(const char value) -> InputEvent {
    return InputEvent{.key = InputKey::kCharacter, .ch = value};
  }
};

enum class TokenClass {
  kPlain,
  kKeyword,
  kString,
  kNumber,
  kOperator,
  kIdentifier,
  kError,
};

struct StyleSpan {
  std::size_t start = 0;
  std::size_t length = 0;
  TokenClass token_class = TokenClass::kPlain;
};

struct CompletionHandler {
  // Loads a brace-enclosed list of symbol name literals.
  // Example: handler.load_symbols({"let", "import", "print"});
  void load_symbols(std::initializer_list<std::string_view> symbols) {
    for (const auto sym : symbols) { m_trie.insert(sym); }
  }

  // Loads symbols from any contiguous range of std::string (e.g. std::vector<std::string>).
  void load_symbols(std::span<const std::string> symbols) {
    for (const auto& sym : symbols) { m_trie.insert(sym); }
  }

  // Returns all previously loaded symbols that begin with partial_symbol.
  // An empty partial_symbol returns every symbol in the handler.
  [[nodiscard]] auto get_completions(const std::string_view partial_symbol) const -> std::vector<std::string> {
    return m_trie.completions(partial_symbol);
  }

  // Removes all loaded symbols.
  void clear() { m_trie.clear(); }

  // Returns true when no symbols have been loaded.
  [[nodiscard]] auto empty() const -> bool { return m_trie.empty(); }

private:
  common::Trie m_trie;
};

[[nodiscard]] auto normalize_style_spans(std::size_t buffer_size, const std::vector<StyleSpan>& spans)
    -> std::vector<StyleSpan>;

using StyleSpanProvider = std::function<std::vector<StyleSpan>(std::string_view)>;

struct LineEditorConfig {
  StyleSpanProvider style_span_provider;
  CompletionHandler* completion_handler = nullptr;
};

enum class LineEditorAction {
  kContinue,
  kSubmit,
  kClearBuffer,
  kEndOfInput,
};

struct LineEditorResult {
  LineEditorAction action = LineEditorAction::kContinue;
  bool needs_redraw = false;
  std::optional<std::string> submitted_line;
  std::vector<std::string> completion_suggestions;
};

class LineEditor {
public:
  explicit LineEditor(LineEditorConfig config = {});

  auto handle_event(const InputEvent& event) -> LineEditorResult;
  [[nodiscard]] auto buffer() const -> const std::string&;
  [[nodiscard]] auto cursor() const -> std::size_t;
  [[nodiscard]] auto history() const -> const std::vector<std::string>&;
  [[nodiscard]] auto config() const -> const LineEditorConfig&;

  void reset();

private:
  void push_history_entry(const std::string& entry);
  auto restore_history_entry(std::size_t index) -> LineEditorResult;

  LineEditorConfig config_;
  std::string buffer_;
  std::size_t cursor_ = 0;
  std::vector<std::string> history_;
  std::optional<std::size_t> history_index_;
  std::string history_edit_buffer_;
};

struct InteractiveReadResult {
  LineEditorAction action = LineEditorAction::kContinue;
  std::optional<std::string> line;
};

[[nodiscard]] auto stdin_is_interactive() -> bool;
auto read_interactive_line(LineEditor& editor, std::string_view prompt) -> InteractiveReadResult;

namespace detail {
#ifdef _WIN32
[[nodiscard]] auto windows_stdin_is_interactive_for_testing(bool has_valid_handle, bool get_console_mode_succeeded)
    -> bool;
#endif
[[nodiscard]] auto decode_escape_bytes_for_testing(std::string_view bytes) -> InputEvent;
[[nodiscard]] auto format_completion_suggestions_for_testing(std::span<const std::string> suggestions,
                                                             std::size_t terminal_width) -> std::vector<std::string>;
}  // namespace detail

}  // namespace fleaux::cli
