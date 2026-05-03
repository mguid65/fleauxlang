#include "fleaux/cli/line_editor.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <conio.h>
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace fleaux::cli {
namespace {

#ifdef _WIN32
[[nodiscard]] auto windows_stdin_is_interactive_impl(const bool has_valid_handle, const bool get_console_mode_succeeded)
    -> bool {
  return has_valid_handle && get_console_mode_succeeded;
}
#endif

#ifdef _WIN32

class ScopedRawMode {
public:
  ScopedRawMode() {
    const auto stdin_handle = ::GetStdHandle(STD_INPUT_HANDLE);
    if (stdin_handle == INVALID_HANDLE_VALUE) {
      return;
    }

    if (!::GetConsoleMode(stdin_handle, &original_mode_)) {
      return;
    }

    // Enable processed input and mouse input, but disable line input and echo
    DWORD new_mode = original_mode_;
    new_mode |= ENABLE_PROCESSED_INPUT;
    new_mode |= ENABLE_MOUSE_INPUT;
    new_mode &= ~ENABLE_LINE_INPUT;
    new_mode &= ~ENABLE_ECHO_INPUT;

    if (::SetConsoleMode(stdin_handle, new_mode)) {
      stdin_handle_ = stdin_handle;
      enabled_ = true;
    }
  }

  ScopedRawMode(const ScopedRawMode&) = delete;
  auto operator=(const ScopedRawMode&) -> ScopedRawMode& = delete;

  ~ScopedRawMode() {
    if (enabled_ && stdin_handle_ != INVALID_HANDLE_VALUE) {
      static_cast<void>(::SetConsoleMode(stdin_handle_, original_mode_));
    }
  }

  [[nodiscard]] auto enabled() const -> bool { return enabled_; }

private:
  HANDLE stdin_handle_ = INVALID_HANDLE_VALUE;
  DWORD original_mode_ = 0;
  bool enabled_ = false;
};

#else

class ScopedRawMode {
public:
  ScopedRawMode() {
    if (::tcgetattr(STDIN_FILENO, &original_) != 0) {
      return;
    }

    termios raw = original_;
    raw.c_iflag &= static_cast<tcflag_t>(~(BRKINT | ICRNL | INPCK | ISTRIP | IXON));
    raw.c_oflag &= static_cast<tcflag_t>(~OPOST);
    raw.c_cflag |= CS8;
    raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | IEXTEN | ISIG));
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == 0) {
      enabled_ = true;
    }
  }

  ScopedRawMode(const ScopedRawMode&) = delete;
  auto operator=(const ScopedRawMode&) -> ScopedRawMode& = delete;

  ~ScopedRawMode() {
    if (enabled_) {
      static_cast<void>(::tcsetattr(STDIN_FILENO, TCSAFLUSH, &original_));
    }
  }

  [[nodiscard]] auto enabled() const -> bool { return enabled_; }

private:
  termios original_{};
  bool enabled_ = false;
};

#endif

// Platform-independent helper functions
auto is_token_word_char(const char ch) -> bool {
  const auto uch = static_cast<unsigned char>(ch);
  return std::isalnum(uch) != 0 || ch == '_';
}

auto is_token_space_char(const char ch) -> bool { return std::isspace(static_cast<unsigned char>(ch)) != 0; }

auto token_class_for_char(const char ch) -> int {
  if (is_token_space_char(ch)) {
    return 0;
  }
  return is_token_word_char(ch) ? 1 : 2;
}

auto is_completion_symbol_char(const char ch) -> bool {
  const auto uch = static_cast<unsigned char>(ch);
  return std::isalnum(uch) != 0 || ch == '_' || ch == '.';
}

auto decode_csi_with_params(const std::string& params, const char final_char) -> InputEvent {
  if (final_char == '~') {
    if (params == "1" || params == "7") {
      return {.key = InputKey::kHome};
    }
    if (params == "3") {
      return {.key = InputKey::kDelete};
    }
    if (params == "4" || params == "8") {
      return {.key = InputKey::kEnd};
    }
    return {};
  }

  if (final_char == 'D') {
    if (params == "1;5" || params == "1;3" || params == "5" || params == "3") {
      return {.key = InputKey::kTokenLeft};
    }
    return {.key = InputKey::kArrowLeft};
  }
  if (final_char == 'C') {
    if (params == "1;5" || params == "1;3" || params == "5" || params == "3") {
      return {.key = InputKey::kTokenRight};
    }
    return {.key = InputKey::kArrowRight};
  }
  if (final_char == 'A') {
    return {.key = InputKey::kArrowUp};
  }
  if (final_char == 'B') {
    return {.key = InputKey::kArrowDown};
  }
  if (final_char == 'H') {
    return {.key = InputKey::kHome};
  }
  if (final_char == 'F') {
    return {.key = InputKey::kEnd};
  }

  return {};
}

auto completion_lines_for_width(const std::span<const std::string> suggestions, std::size_t terminal_width)
    -> std::vector<std::string> {
  if (suggestions.empty()) {
    return {};
  }

  constexpr std::size_t kColumnSpacing = 2;
  if (terminal_width == 0) {
    terminal_width = 80;
  }

  const std::size_t count = suggestions.size();
  std::size_t best_columns = 1;
  std::size_t best_rows = count;
  std::vector<std::size_t> best_widths(1, 0);

  for (std::size_t columns = count; columns >= 1; --columns) {
    const std::size_t rows = (count + columns - 1) / columns;
    std::vector<std::size_t> widths(columns, 0);

    for (std::size_t row = 0; row < rows; ++row) {
      for (std::size_t col = 0; col < columns; ++col) {
        const std::size_t idx = row * columns + col;
        if (idx >= count) {
          break;
        }
        widths[col] = std::max(widths[col], suggestions[idx].size());
      }
    }

    std::size_t total_width = 0;
    for (std::size_t col = 0; col < columns; ++col) {
      total_width += widths[col];
      if (col + 1 < columns) {
        total_width += kColumnSpacing;
      }
    }

    if (total_width <= terminal_width || columns == 1) {
      best_columns = columns;
      best_rows = rows;
      best_widths = std::move(widths);
      break;
    }
  }

  std::vector<std::string> lines;
  lines.reserve(best_rows);
  for (std::size_t row = 0; row < best_rows; ++row) {
    std::string line;
    for (std::size_t col = 0; col < best_columns; ++col) {
      const std::size_t idx = row * best_columns + col;
      if (idx >= count) {
        break;
      }

      const std::string& text = suggestions[idx];
      line += text;

      if (const std::size_t next_idx = row * best_columns + (col + 1); (col + 1) < best_columns && next_idx < count) {
        const std::size_t pad = (best_widths[col] - text.size()) + kColumnSpacing;
        line.append(pad, ' ');
      }
    }
    lines.push_back(std::move(line));
  }

  return lines;
}

auto terminal_width_chars() -> std::size_t {
#ifdef _WIN32
  const auto stdout_handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
  if (stdout_handle == INVALID_HANDLE_VALUE) {
    return 80;
  }

  CONSOLE_SCREEN_BUFFER_INFO buffer_info;
  if (!::GetConsoleScreenBufferInfo(stdout_handle, &buffer_info)) {
    return 80;
  }
  return static_cast<std::size_t>(buffer_info.srWindow.Right - buffer_info.srWindow.Left + 1);
#else
  winsize ws{};
  if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0 || ws.ws_col == 0) {
    return 80;
  }
  return static_cast<std::size_t>(ws.ws_col);
#endif
}

#ifdef _WIN32

auto read_input_event() -> std::optional<InputEvent> {
  const auto stdin_handle = ::GetStdHandle(STD_INPUT_HANDLE);
  if (stdin_handle == INVALID_HANDLE_VALUE) {
    return std::nullopt;
  }

  INPUT_RECORD input_record;
  DWORD num_events_read = 0;

  if (!::ReadConsoleInput(stdin_handle, &input_record, 1, &num_events_read)) {
    return std::nullopt;
  }

  if (num_events_read == 0) {
    return std::nullopt;
  }

  if (input_record.EventType == KEY_EVENT) {
    const auto& key_event = input_record.Event.KeyEvent;
    if (!key_event.bKeyDown) {
      return InputEvent{};
    }

    const auto vk = key_event.wVirtualKeyCode;
    const auto ch = key_event.uChar.AsciiChar;

    // Handle Ctrl+C
    if ((vk == 'C' || ch == 3) && (key_event.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED))) {
      return InputEvent{.key = InputKey::kCtrlC};
    }

    // Handle Ctrl+D
    if ((vk == 'D' || ch == 4) && (key_event.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED))) {
      return InputEvent{.key = InputKey::kCtrlD};
    }

    // Handle Ctrl+W (token backspace)
    if ((vk == 'W' || ch == 23) && (key_event.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED))) {
      return InputEvent{.key = InputKey::kTokenBackspace};
    }

    // Handle regular character input
    if (ch != 0 && std::isprint(static_cast<unsigned char>(ch)) != 0) {
      return InputEvent::character(ch);
    }

    // Handle function keys
    switch (vk) {
      case VK_TAB:
        return InputEvent{.key = InputKey::kTab};
      case VK_RETURN:
        return InputEvent{.key = InputKey::kEnter};
      case VK_BACK:
        return InputEvent{.key = InputKey::kBackspace};
      case VK_DELETE:
        return InputEvent{.key = InputKey::kDelete};
      case VK_LEFT:
        if (key_event.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
          return InputEvent{.key = InputKey::kTokenLeft};
        }
        return InputEvent{.key = InputKey::kArrowLeft};
      case VK_RIGHT:
        if (key_event.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
          return InputEvent{.key = InputKey::kTokenRight};
        }
        return InputEvent{.key = InputKey::kArrowRight};
      case VK_UP:
        return InputEvent{.key = InputKey::kArrowUp};
      case VK_DOWN:
        return InputEvent{.key = InputKey::kArrowDown};
      case VK_HOME:
        return InputEvent{.key = InputKey::kHome};
      case VK_END:
        return InputEvent{.key = InputKey::kEnd};
      default:
        break;
    }
  }

  return InputEvent{};
}

#else

constexpr int kEscapeSequenceTimeoutMs = 40;

auto read_byte() -> std::optional<unsigned char> {
  unsigned char value = 0;
  while (true) {
    const auto bytes_read = ::read(STDIN_FILENO, &value, 1);
    if (bytes_read == 1) {
      return value;
    }
    if (bytes_read == 0) {
      return std::nullopt;
    }
    if (errno == EINTR) {
      continue;
    }
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
    if (ready > 0) {
      return read_byte();
    }
    if (ready == 0) {
      return std::nullopt;
    }
    if (errno == EINTR) {
      continue;
    }
    return std::nullopt;
  }
}

auto decode_escape_sequence() -> InputEvent {
  const auto first = read_byte_with_timeout(kEscapeSequenceTimeoutMs);
  if (!first.has_value()) {
    return {};
  }

  // Alt-modified keys are commonly encoded as an extra ESC prefix.
  if (*first == 27) {
    return decode_escape_sequence();
  }

  if (*first == '[') {
    const auto second = read_byte_with_timeout(kEscapeSequenceTimeoutMs);
    if (!second.has_value()) {
      return {};
    }

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
      std::string params(1, static_cast<char>(*second));
      while (true) {
        const auto next = read_byte_with_timeout(kEscapeSequenceTimeoutMs);
        if (!next.has_value()) {
          return {};
        }
        if ((*next >= '0' && *next <= '9') || *next == ';') {
          params.push_back(static_cast<char>(*next));
          continue;
        }
        return decode_csi_with_params(params, static_cast<char>(*next));
      }
    }

    return {};
  }

  if (*first == 'O') {
    const auto second = read_byte_with_timeout(kEscapeSequenceTimeoutMs);
    if (!second.has_value()) {
      return {};
    }
    if (*second == 'A') {
      return {.key = InputKey::kArrowUp};
    }
    if (*second == 'B') {
      return {.key = InputKey::kArrowDown};
    }
    if (*second == 'C') {
      return {.key = InputKey::kArrowRight};
    }
    if (*second == 'D') {
      return {.key = InputKey::kArrowLeft};
    }
    if (*second == 'H') {
      return {.key = InputKey::kHome};
    }
    if (*second == 'F') {
      return {.key = InputKey::kEnd};
    }
  }

  if (*first == 'b' || *first == 'B') {
    return {.key = InputKey::kTokenLeft};
  }
  if (*first == 'f' || *first == 'F') {
    return {.key = InputKey::kTokenRight};
  }
  if (*first == 127 || *first == 8) {
    return {.key = InputKey::kTokenBackspace};
  }

  return {};
}

auto read_input_event_unix() -> std::optional<InputEvent> {
  const auto byte = read_byte();
  if (!byte.has_value()) {
    return std::nullopt;
  }

  switch (*byte) {
    case '\t':
      return InputEvent{.key = InputKey::kTab};
    case '\r':
    case '\n':
      return InputEvent{.key = InputKey::kEnter};
    case 3:
      return InputEvent{.key = InputKey::kCtrlC};
    case 4:
      return InputEvent{.key = InputKey::kCtrlD};
    case 23:
      return InputEvent{.key = InputKey::kTokenBackspace};
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

  if (std::isprint(*byte) != 0) {
    return InputEvent::character(static_cast<char>(*byte));
  }
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

#endif

#ifdef _WIN32

auto get_console_color_for_token_class(const TokenClass token_class) -> WORD {
  switch (token_class) {
    case TokenClass::kKeyword:
      return FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY;  // Cyan
    case TokenClass::kString:
      return FOREGROUND_GREEN;
    case TokenClass::kNumber:
      return FOREGROUND_RED | FOREGROUND_BLUE | FOREGROUND_INTENSITY;  // Magenta
    case TokenClass::kOperator:
      return FOREGROUND_RED | FOREGROUND_GREEN;  // Yellow (without intensity)
    case TokenClass::kError:
      return FOREGROUND_RED | FOREGROUND_INTENSITY;
    case TokenClass::kPlain:
    case TokenClass::kIdentifier:
    default:
      return FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
  }
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

  const auto stdout_handle = ::GetStdHandle(STD_OUTPUT_HANDLE);
  if (stdout_handle == INVALID_HANDLE_VALUE) {
    std::cout << buffer;
    return;
  }

  CONSOLE_SCREEN_BUFFER_INFO buffer_info;
  if (!::GetConsoleScreenBufferInfo(stdout_handle, &buffer_info)) {
    std::cout << buffer;
    return;
  }

  const auto default_color = buffer_info.wAttributes;
  std::size_t cursor = 0;

  for (const auto& [start, length, token_class] : spans) {
    if (cursor < start) {
      const auto text = buffer.substr(cursor, start - cursor);
      ::SetConsoleTextAttribute(stdout_handle, default_color);
      std::cout << text;
    }

    const auto color = get_console_color_for_token_class(token_class);
    ::SetConsoleTextAttribute(stdout_handle, color);
    std::cout << buffer.substr(start, length);
    cursor = start + length;
  }

  if (cursor < buffer.size()) {
    ::SetConsoleTextAttribute(stdout_handle, default_color);
    std::cout << buffer.substr(cursor);
  }

  ::SetConsoleTextAttribute(stdout_handle, default_color);
}

void render_line(const std::string_view prompt, const LineEditor& editor) {
  const auto stdout_handle = ::GetStdHandle(STD_OUTPUT_HANDLE);

  if (stdout_handle != INVALID_HANDLE_VALUE) {
    CONSOLE_SCREEN_BUFFER_INFO buffer_info;
    if (::GetConsoleScreenBufferInfo(stdout_handle, &buffer_info)) {
      COORD home{0, buffer_info.dwCursorPosition.Y};
      ::SetConsoleCursorPosition(stdout_handle, home);

      DWORD chars_written = 0;
      ::FillConsoleOutputCharacter(stdout_handle, ' ', buffer_info.dwSize.X, home, &chars_written);
      ::SetConsoleCursorPosition(stdout_handle, home);
    }
  }

  std::cout << prompt;
  render_with_styles(editor.buffer(), editor.config().style_span_provider);

  if (const auto tail_size = editor.buffer().size() - editor.cursor(); tail_size > 0) {
    if (stdout_handle != INVALID_HANDLE_VALUE) {
      CONSOLE_SCREEN_BUFFER_INFO buffer_info;
      if (::GetConsoleScreenBufferInfo(stdout_handle, &buffer_info)) {
        SHORT col = static_cast<SHORT>(
            std::max(0L, static_cast<LONG>(buffer_info.dwCursorPosition.X) - static_cast<LONG>(tail_size)));
        COORD new_pos{col, buffer_info.dwCursorPosition.Y};
        ::SetConsoleCursorPosition(stdout_handle, new_pos);
      }
    }
  }

  std::cout.flush();
}

#else

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
  for (const auto& [start, length, token_class] : spans) {
    if (cursor < start) {
      std::cout << ansi_code_for_token_class(TokenClass::kPlain) << buffer.substr(cursor, start - cursor);
    }

    std::cout << ansi_code_for_token_class(token_class) << buffer.substr(start, length);
    cursor = start + length;
  }

  if (cursor < buffer.size()) {
    std::cout << ansi_code_for_token_class(TokenClass::kPlain) << buffer.substr(cursor);
  }

  std::cout << "\x1b[0m";
}

void render_line(const std::string_view prompt, const LineEditor& editor) {
  std::cout << '\r' << prompt;
  render_with_styles(editor.buffer(), editor.config().style_span_provider);
  std::cout << "\x1b[K";
  if (const auto tail_size = editor.buffer().size() - editor.cursor(); tail_size > 0) {
    std::cout << "\x1b[" << tail_size << 'D';
  }
  std::cout.flush();
}

#endif

auto read_fallback_line(const std::string_view prompt) -> InteractiveReadResult {
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
    if (length == 0 || start >= buffer_size) {
      continue;
    }
    const auto clamped_length = std::min(length, buffer_size - start);
    cleaned.push_back(StyleSpan{.start = start, .length = clamped_length, .token_class = token_class});
  }

  std::ranges::sort(cleaned, [](const StyleSpan& lhs, const StyleSpan& rhs) -> bool {
    if (lhs.start != rhs.start) {
      return lhs.start < rhs.start;
    }
    return lhs.length < rhs.length;
  });

  std::vector<StyleSpan> merged;
  merged.reserve(cleaned.size());
  std::size_t next_free = 0;
  for (const auto& span : cleaned) {
    if (span.start < next_free) {
      continue;
    }
    merged.push_back(span);
    next_free = span.start + span.length;
  }
  return merged;
}

LineEditor::LineEditor(LineEditorConfig config) : config_(std::move(config)) {}

auto LineEditor::handle_event(const InputEvent& event) -> LineEditorResult {
  const auto move_token_left = [this]() -> bool {
    if (cursor_ == 0) {
      return false;
    }

    std::size_t idx = cursor_;
    while (idx > 0 && is_token_space_char(buffer_[idx - 1])) {
      --idx;
    }
    if (idx == 0) {
      cursor_ = 0;
      return true;
    }

    const auto cls = token_class_for_char(buffer_[idx - 1]);
    while (idx > 0 && token_class_for_char(buffer_[idx - 1]) == cls) {
      --idx;
    }
    if (idx == cursor_) {
      return false;
    }
    cursor_ = idx;
    return true;
  };

  const auto move_token_right = [this]() -> bool {
    if (cursor_ >= buffer_.size()) {
      return false;
    }

    std::size_t idx = cursor_;
    while (idx < buffer_.size() && is_token_space_char(buffer_[idx])) {
      ++idx;
    }
    if (idx >= buffer_.size()) {
      cursor_ = buffer_.size();
      return true;
    }

    const auto cls = token_class_for_char(buffer_[idx]);
    while (idx < buffer_.size() && token_class_for_char(buffer_[idx]) == cls) {
      ++idx;
    }
    if (idx == cursor_) {
      return false;
    }
    cursor_ = idx;
    return true;
  };

  const auto delete_token_left = [this]() -> bool {
    if (cursor_ == 0) {
      return false;
    }

    std::size_t start = cursor_;
    while (start > 0 && is_token_space_char(buffer_[start - 1])) {
      --start;
    }
    if (start > 0) {
      const auto cls = token_class_for_char(buffer_[start - 1]);
      while (start > 0 && token_class_for_char(buffer_[start - 1]) == cls) {
        --start;
      }
    }

    if (start == cursor_) {
      return false;
    }
    buffer_.erase(buffer_.begin() + static_cast<std::ptrdiff_t>(start),
                  buffer_.begin() + static_cast<std::ptrdiff_t>(cursor_));
    cursor_ = start;
    return true;
  };

  const auto apply_completion = [this]() -> LineEditorResult {
    if (config_.completion_handler == nullptr || config_.completion_handler->empty()) {
      return {};
    }

    std::size_t start = cursor_;
    while (start > 0 && is_completion_symbol_char(buffer_[start - 1])) {
      --start;
    }
    std::size_t end = cursor_;
    while (end < buffer_.size() && is_completion_symbol_char(buffer_[end])) {
      ++end;
    }
    if (start == cursor_) {
      return {};
    }

    const std::string_view partial(buffer_.data() + static_cast<std::ptrdiff_t>(start), cursor_ - start);
    auto completions = config_.completion_handler->get_completions(partial);
    if (completions.empty()) {
      return {};
    }

    std::ranges::sort(completions);
    if (completions.size() > 1) {
      // if all completion options share a common prefix,
      // then set the cursor to the common prefix before returning suggestions
      if (const auto partial_replacement = [&]() -> std::optional<std::string> {
            const auto& shortest = completions.front();
            const auto& longest = completions.back();

            auto idx{0};
            for (; idx < longest.length(); ++idx) {
              if (shortest[idx] != longest[idx]) {
                break;
              }
            }
            if (idx != 0) {
              return {longest.substr(0, idx)};
            }
            return std::nullopt;
          }();
          partial_replacement.has_value()) {
        buffer_.replace(start, end - start, *partial_replacement);
        cursor_ = start + partial_replacement->size();
      }

      return {.needs_redraw = true, .completion_suggestions = std::move(completions)};
    }

    const std::string& replacement = completions.front();

    if (buffer_.compare(start, end - start, replacement) == 0) {
      cursor_ = start + replacement.size();
      return {};
    }

    buffer_.replace(start, end - start, replacement);
    cursor_ = start + replacement.size();
    return {.needs_redraw = true};
  };

  switch (event.key) {
    case InputKey::kCharacter:
      buffer_.insert(buffer_.begin() + static_cast<std::ptrdiff_t>(cursor_), event.ch);
      ++cursor_;
      return {.needs_redraw = true};

    case InputKey::kBackspace:
      if (cursor_ == 0) {
        return {};
      }
      buffer_.erase(buffer_.begin() + static_cast<std::ptrdiff_t>(cursor_ - 1));
      --cursor_;
      return {.needs_redraw = true};

    case InputKey::kDelete:
      if (cursor_ >= buffer_.size()) {
        return {};
      }
      buffer_.erase(buffer_.begin() + static_cast<std::ptrdiff_t>(cursor_));
      return {.needs_redraw = true};

    case InputKey::kArrowLeft:
      if (cursor_ == 0) {
        return {};
      }
      --cursor_;
      return {.needs_redraw = true};

    case InputKey::kArrowRight:
      if (cursor_ >= buffer_.size()) {
        return {};
      }
      ++cursor_;
      return {.needs_redraw = true};

    case InputKey::kArrowUp:
      if (history_.empty()) {
        return {};
      }
      if (!history_index_.has_value()) {
        history_edit_buffer_ = buffer_;
        history_index_ = history_.size() - 1;
      } else if (*history_index_ > 0) {
        --(*history_index_);
      }
      return restore_history_entry(*history_index_);

    case InputKey::kArrowDown:
      if (!history_index_.has_value()) {
        return {};
      }
      if (*history_index_ + 1 < history_.size()) {
        ++(*history_index_);
        return restore_history_entry(*history_index_);
      }
      history_index_.reset();
      buffer_ = history_edit_buffer_;
      cursor_ = buffer_.size();
      return {.needs_redraw = true};

    case InputKey::kTokenLeft:
      return {.needs_redraw = move_token_left()};

    case InputKey::kTokenRight:
      return {.needs_redraw = move_token_right()};

    case InputKey::kTokenBackspace:
      return {.needs_redraw = delete_token_left()};

    case InputKey::kHome:
      if (cursor_ == 0) {
        return {};
      }
      cursor_ = 0;
      return {.needs_redraw = true};

    case InputKey::kEnd:
      if (cursor_ == buffer_.size()) {
        return {};
      }
      cursor_ = buffer_.size();
      return {.needs_redraw = true};

    case InputKey::kTab:
      return apply_completion();

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
      if (buffer_.empty()) {
        return {.action = LineEditorAction::kEndOfInput};
      }
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
  if (entry.empty()) {
    return;
  }
  if (!history_.empty() && history_.back() == entry) {
    return;
  }
  history_.push_back(entry);
}

auto LineEditor::restore_history_entry(const std::size_t index) -> LineEditorResult {
  buffer_ = history_.at(index);
  cursor_ = buffer_.size();
  return {.needs_redraw = true};
}

auto stdin_is_interactive() -> bool {
#ifdef _WIN32
  const auto stdin_handle = ::GetStdHandle(STD_INPUT_HANDLE);
  if (stdin_handle == INVALID_HANDLE_VALUE || stdin_handle == nullptr) {
    return false;
  }

  DWORD console_mode = 0;
  return windows_stdin_is_interactive_impl(true, ::GetConsoleMode(stdin_handle, &console_mode) != 0);
#else
  return ::isatty(STDIN_FILENO) != 0;
#endif
}

auto read_interactive_line(LineEditor& editor, const std::string_view prompt) -> InteractiveReadResult {
  editor.reset();

  if (!stdin_is_interactive()) {
    return read_fallback_line(prompt);
  }

  // This cannot be put into the if-init. It sets terminal states and is necessary for handling control characters
  const ScopedRawMode raw_mode;
  if (!raw_mode.enabled()) {
    return read_fallback_line(prompt);
  }

  render_line(prompt, editor);
  while (true) {
#ifdef _WIN32
    const auto event = read_input_event();
#else
    const auto event = read_input_event_unix();
#endif
    if (!event.has_value()) {
      std::cout << "\r\n";
      return {.action = LineEditorAction::kEndOfInput};
    }

    const auto [action, needs_redraw, submitted_line_opt, completion_suggestions] = editor.handle_event(*event);
    if (!completion_suggestions.empty()) {
      const auto lines = completion_lines_for_width(completion_suggestions, terminal_width_chars());
      std::cout << "\r\n";
      for (const auto& line : lines) {
        std::cout << line << "\r\n";
      }
      render_line(prompt, editor);
      continue;
    }

    if (action == LineEditorAction::kContinue) {
      if (needs_redraw) {
        render_line(prompt, editor);
      }
      continue;
    }

    if (action == LineEditorAction::kSubmit) {
      const auto submitted_line = submitted_line_opt.value_or(std::string{});
      (void)submitted_line;
      std::cout << "\r\n";
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
  (void)raw_mode;  // This is here so the IDE will shutup about putting it in the if-init
}

auto decode_escape_bytes_for_testing_impl(const std::string_view bytes) -> InputEvent {
  std::size_t cursor = 0;
  const auto parse = [&](const auto& self) -> InputEvent {
    if (cursor >= bytes.size()) {
      return {};
    }

    const auto first = static_cast<unsigned char>(bytes[cursor++]);

    if (first == 27U) {
      return self(self);
    }

    if (first == static_cast<unsigned char>('[')) {
      if (cursor >= bytes.size()) {
        return {};
      }
      const auto second = static_cast<unsigned char>(bytes[cursor++]);

      switch (second) {
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

      if (second >= '0' && second <= '9') {
        std::string params(1, static_cast<char>(second));
        while (cursor < bytes.size()) {
          const auto next = static_cast<unsigned char>(bytes[cursor]);
          if ((next >= '0' && next <= '9') || next == ';') {
            params.push_back(static_cast<char>(next));
            ++cursor;
            continue;
          }
          ++cursor;
          return decode_csi_with_params(params, static_cast<char>(next));
        }
      }

      return {};
    }

    if (first == static_cast<unsigned char>('O')) {
      if (cursor >= bytes.size()) {
        return {};
      }
      const auto second = static_cast<unsigned char>(bytes[cursor++]);
      if (second == 'A') {
        return {.key = InputKey::kArrowUp};
      }
      if (second == 'B') {
        return {.key = InputKey::kArrowDown};
      }
      if (second == 'C') {
        return {.key = InputKey::kArrowRight};
      }
      if (second == 'D') {
        return {.key = InputKey::kArrowLeft};
      }
      if (second == 'H') {
        return {.key = InputKey::kHome};
      }
      if (second == 'F') {
        return {.key = InputKey::kEnd};
      }
      return {};
    }

    if (first == static_cast<unsigned char>('b') || first == static_cast<unsigned char>('B')) {
      return {.key = InputKey::kTokenLeft};
    }
    if (first == static_cast<unsigned char>('f') || first == static_cast<unsigned char>('F')) {
      return {.key = InputKey::kTokenRight};
    }
    if (first == 127U || first == 8U) {
      return {.key = InputKey::kTokenBackspace};
    }

    return {};
  };

  return parse(parse);
}

}  // namespace fleaux::cli

#ifdef _WIN32
auto fleaux::cli::detail::windows_stdin_is_interactive_for_testing(const bool has_valid_handle,
                                                                   const bool get_console_mode_succeeded) -> bool {
  return windows_stdin_is_interactive_impl(has_valid_handle, get_console_mode_succeeded);
}
#endif

auto fleaux::cli::detail::decode_escape_bytes_for_testing(std::string_view bytes) -> InputEvent {
  return decode_escape_bytes_for_testing_impl(bytes);
}

auto fleaux::cli::detail::format_completion_suggestions_for_testing(std::span<const std::string> suggestions,
                                                                    const std::size_t terminal_width)
    -> std::vector<std::string> {
  return completion_lines_for_width(suggestions, terminal_width);
}
