#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
  kHome,
  kEnd,
  kCtrlC,
  kCtrlD,
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

[[nodiscard]] auto normalize_style_spans(std::size_t buffer_size, const std::vector<StyleSpan>& spans)
    -> std::vector<StyleSpan>;

using StyleSpanProvider = std::function<std::vector<StyleSpan>(std::string_view)>;

struct LineEditorConfig {
  StyleSpanProvider style_span_provider;
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

}  // namespace fleaux::cli
