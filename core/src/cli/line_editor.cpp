#include "fleaux/cli/line_editor.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <iostream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

namespace fleaux::cli {
namespace {

class ScopedRawMode {
public:
  ScopedRawMode() {
    if (::tcgetattr(STDIN_FILENO, &original_) != 0) { return; }

    termios raw = original_;
    raw.c_iflag &= static_cast<tcflag_t>(~(BRKINT | ICRNL | INPCK | ISTRIP | IXON));
    raw.c_oflag &= static_cast<tcflag_t>(~OPOST);
    raw.c_cflag |= CS8;
    raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | IEXTEN | ISIG));
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) { enabled_ = true; }
  }

  ScopedRawMode(const ScopedRawMode&) = delete;
  auto operator=(const ScopedRawMode&) -> ScopedRawMode& = delete;

  ~ScopedRawMode() {
    if (enabled_) { static_cast<void>(::tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_)); }
  }

  [[nodiscard]] auto enabled() const -> bool { return enabled_; }

private:
  termios original_{};
  bool enabled_ = false;
};

auto read_byte() -> std::optional<unsigned char> {
  unsigned char value = 0;
  while (true) {
    const auto bytes_read = ::read(STDIN_FILENO, &value, 1);
    if (bytes_read == 1) { return value; }
    if (bytes_read == 0) { return std::nullopt; }
    if (errno == EINTR) { continue; }
    return std::nullopt;
  }
}

auto read_byte_with_timeout(const int timeout_ms) -> std::optional<unsigned char> {
  while (true) {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(STDIN_FILENO, &read_fds);

    timeval timeout{};
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    const auto ready = ::select(STDIN_FILENO + 1, &read_fds, nullptr, nullptr, &timeout);
    if (ready > 0) { return read_byte(); }
    if (ready == 0) { return std::nullopt; }
    if (errno == EINTR) { continue; }
    return std::nullopt;
  }
}

auto decode_escape_sequence() -> InputEvent {
  const auto first = read_byte_with_timeout(20);
  if (!first.has_value()) { return {}; }

  if (*first == '[') {
    const auto second = read_byte_with_timeout(20);
    if (!second.has_value()) { return {}; }

    switch (*second) {
      case 'A':
        return {.key = InputKey::kArrowUp};
      case 'B':
        return {.key = InputKey::kArrowDown};
      case 'C':
        return {.key = InputKey::kArrowRight};
      case 'D':
        return {.key = InputKey::kArrowLeft};
      case 'H':
        return {.key = InputKey::kHome};
      case 'F':
        return {.key = InputKey::kEnd};
      default:
        break;
    }

    if (*second >= '0' && *second <= '9') {
      std::string digits(1, static_cast<char>(*second));
      while (true) {
        const auto next = read_byte_with_timeout(20);
        if (!next.has_value()) { return {}; }
        if (*next >= '0' && *next <= '9') {
          digits.push_back(static_cast<char>(*next));
          continue;
        }
        if (*next != '~') { return {}; }
        if (digits == "1" || digits == "7") { return {.key = InputKey::kHome}; }
        if (digits == "3") { return {.key = InputKey::kDelete}; }
        if (digits == "4" || digits == "8") { return {.key = InputKey::kEnd}; }
        return {};
      }
    }

    return {};
  }

  if (*first == 'O') {
    const auto second = read_byte_with_timeout(20);
    if (!second.has_value()) { return {}; }
    if (*second == 'H') { return {.key = InputKey::kHome}; }
    if (*second == 'F') { return {.key = InputKey::kEnd}; }
  }

  return {};
}

auto read_input_event() -> std::optional<InputEvent> {
  const auto byte = read_byte();
  if (!byte.has_value()) { return std::nullopt; }

  switch (*byte) {
    case '\r':
    case '\n':
      return InputEvent{.key = InputKey::kEnter};
    case 3:
      return InputEvent{.key = InputKey::kCtrlC};
    case 4:
      return InputEvent{.key = InputKey::kCtrlD};
    case 8:
    case 127:
      return InputEvent{.key = InputKey::kBackspace};
    case 1:
      return InputEvent{.key = InputKey::kHome};
    case 5:
      return InputEvent{.key = InputKey::kEnd};
    case 27:
      return decode_escape_sequence();
    default:
      break;
  }

  if (std::isprint(*byte) != 0) { return InputEvent::character(static_cast<char>(*byte)); }
  return InputEvent{};
}

auto ansi_code_for_token_class(const TokenClass token_class) -> std::string_view {
  switch (token_class) {
    case TokenClass::kPlain:
      return "\x1b[0m";
    case TokenClass::kKeyword:
      return "\x1b[1;36m";
    case TokenClass::kString:
      return "\x1b[32m";
    case TokenClass::kNumber:
      return "\x1b[35m";
    case TokenClass::kOperator:
      return "\x1b[33m";
    case TokenClass::kIdentifier:
      return "\x1b[0m";
    case TokenClass::kError:
      return "\x1b[1;31m";
  }
  return "\x1b[0m";
}

void render_with_styles(const std::string_view buffer, const StyleSpanProvider& style_provider) {
  if (!style_provider) {
    std::cout << buffer;
    return;
  }

  const auto spans = normalize_style_spans(buffer.size(), style_provider(buffer));
  if (spans.empty()) {
    std::cout << buffer;
    return;
  }

  std::size_t cursor = 0;
  for (const auto& span : spans) {
    if (cursor < span.start) {
      std::cout << ansi_code_for_token_class(TokenClass::kPlain) << buffer.substr(cursor, span.start - cursor);
    }

    std::cout << ansi_code_for_token_class(span.token_class) << buffer.substr(span.start, span.length);
    cursor = span.start + span.length;
  }

  if (cursor < buffer.size()) { std::cout << ansi_code_for_token_class(TokenClass::kPlain) << buffer.substr(cursor); }

  std::cout << "\x1b[0m";
}

void render_line(std::string_view prompt, const LineEditor& editor) {
  std::cout << '\r' << prompt;
  render_with_styles(editor.buffer(), editor.config().style_span_provider);
  std::cout << "\x1b[K";
  const auto tail_size = editor.buffer().size() - editor.cursor();
  if (tail_size > 0) { std::cout << "\x1b[" << tail_size << 'D'; }
  std::cout.flush();
}

auto read_fallback_line(std::string_view prompt) -> InteractiveReadResult {
  std::cout << prompt;
  std::string line;
  if (!std::getline(std::cin, line)) {
    std::cout << '\n';
    return {.action = LineEditorAction::kEndOfInput};
  }
  return {.action = LineEditorAction::kSubmit, .line = std::move(line)};
}

}  // namespace

auto normalize_style_spans(const std::size_t buffer_size, const std::vector<StyleSpan>& spans)
    -> std::vector<StyleSpan> {
  std::vector<StyleSpan> cleaned;
  cleaned.reserve(spans.size());

  for (const auto& [start, length, token_class] : spans) {
    if (length == 0 || start >= buffer_size) { continue; }
    const auto clamped_length = std::min(length, buffer_size - start);
    cleaned.push_back(StyleSpan{.start = start, .length = clamped_length, .token_class = token_class});
  }

  std::ranges::sort(cleaned, [](const StyleSpan& lhs, const StyleSpan& rhs) -> bool {
    if (lhs.start != rhs.start) { return lhs.start < rhs.start; }
    return lhs.length < rhs.length;
  });

  std::vector<StyleSpan> merged;
  merged.reserve(cleaned.size());
  std::size_t next_free = 0;
  for (const auto& span : cleaned) {
    if (span.start < next_free) { continue; }
    merged.push_back(span);
    next_free = span.start + span.length;
  }
  return merged;
}

LineEditor::LineEditor(LineEditorConfig config) : config_(std::move(config)) {}

auto LineEditor::handle_event(const InputEvent& event) -> LineEditorResult {
  switch (event.key) {
    case InputKey::kCharacter:
      buffer_.insert(buffer_.begin() + static_cast<std::ptrdiff_t>(cursor_), event.ch);
      ++cursor_;
      return {.needs_redraw = true};

    case InputKey::kBackspace:
      if (cursor_ == 0) { return {}; }
      buffer_.erase(buffer_.begin() + static_cast<std::ptrdiff_t>(cursor_ - 1));
      --cursor_;
      return {.needs_redraw = true};

    case InputKey::kDelete:
      if (cursor_ >= buffer_.size()) { return {}; }
      buffer_.erase(buffer_.begin() + static_cast<std::ptrdiff_t>(cursor_));
      return {.needs_redraw = true};

    case InputKey::kArrowLeft:
      if (cursor_ == 0) { return {}; }
      --cursor_;
      return {.needs_redraw = true};

    case InputKey::kArrowRight:
      if (cursor_ >= buffer_.size()) { return {}; }
      ++cursor_;
      return {.needs_redraw = true};

    case InputKey::kArrowUp:
      if (history_.empty()) { return {}; }
      if (!history_index_.has_value()) {
        history_edit_buffer_ = buffer_;
        history_index_ = history_.size() - 1;
      } else if (*history_index_ > 0) {
        --(*history_index_);
      }
      return restore_history_entry(*history_index_);

    case InputKey::kArrowDown:
      if (!history_index_.has_value()) { return {}; }
      if (*history_index_ + 1 < history_.size()) {
        ++(*history_index_);
        return restore_history_entry(*history_index_);
      }
      history_index_.reset();
      buffer_ = history_edit_buffer_;
      cursor_ = buffer_.size();
      return {.needs_redraw = true};

    case InputKey::kHome:
      if (cursor_ == 0) { return {}; }
      cursor_ = 0;
      return {.needs_redraw = true};

    case InputKey::kEnd:
      if (cursor_ == buffer_.size()) { return {}; }
      cursor_ = buffer_.size();
      return {.needs_redraw = true};

    case InputKey::kEnter: {
      auto submitted_line = buffer_;
      push_history_entry(submitted_line);
      buffer_.clear();
      cursor_ = 0;
      history_index_.reset();
      history_edit_buffer_.clear();
      return {
          .action = LineEditorAction::kSubmit,
          .submitted_line = std::move(submitted_line),
      };
    }

    case InputKey::kCtrlC:
      buffer_.clear();
      cursor_ = 0;
      history_index_.reset();
      history_edit_buffer_.clear();
      return {.action = LineEditorAction::kClearBuffer};

    case InputKey::kCtrlD:
      if (buffer_.empty()) { return {.action = LineEditorAction::kEndOfInput}; }
      return {};

    case InputKey::kUnknown:
      return {};
  }

  return {};
}

auto LineEditor::buffer() const -> const std::string& { return buffer_; }

auto LineEditor::cursor() const -> std::size_t { return cursor_; }

auto LineEditor::history() const -> const std::vector<std::string>& { return history_; }

auto LineEditor::config() const -> const LineEditorConfig& { return config_; }

void LineEditor::reset() {
  buffer_.clear();
  cursor_ = 0;
  history_index_.reset();
  history_edit_buffer_.clear();
}

void LineEditor::push_history_entry(const std::string& entry) {
  if (entry.empty()) { return; }
  if (!history_.empty() && history_.back() == entry) { return; }
  history_.push_back(entry);
}

auto LineEditor::restore_history_entry(const std::size_t index) -> LineEditorResult {
  buffer_ = history_.at(index);
  cursor_ = buffer_.size();
  return {.needs_redraw = true};
}

auto stdin_is_interactive() -> bool { return ::isatty(STDIN_FILENO) != 0; }

auto read_interactive_line(LineEditor& editor, std::string_view prompt) -> InteractiveReadResult {
  editor.reset();

  if (!stdin_is_interactive()) { return read_fallback_line(prompt); }

  const ScopedRawMode raw_mode;
  if (!raw_mode.enabled()) { return read_fallback_line(prompt); }

  render_line(prompt, editor);
  while (true) {
    const auto event = read_input_event();
    if (!event.has_value()) {
      std::cout << "\r\n";
      return {.action = LineEditorAction::kEndOfInput};
    }

    const auto [action, needs_redraw, submitted_line_opt] = editor.handle_event(*event);
    if (action == LineEditorAction::kContinue) {
      if (needs_redraw) { render_line(prompt, editor); }
      continue;
    }

    if (action == LineEditorAction::kSubmit) {
      const auto submitted_line = submitted_line_opt.value_or(std::string{});
      std::cout << '\r' << prompt;
      render_with_styles(submitted_line, editor.config().style_span_provider);
      std::cout << "\x1b[K\r\n";
      std::cout.flush();
      return {.action = action, .line = submitted_line};
    }

    if (action == LineEditorAction::kClearBuffer) {
      std::cout << "\r\x1b[K^C\r\n";
      std::cout.flush();
      return {.action = action};
    }

    if (action == LineEditorAction::kEndOfInput) {
      std::cout << "\r\n";
      std::cout.flush();
      return {.action = action};
    }
  }
}

}  // namespace fleaux::cli
