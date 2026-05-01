#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/cli/line_editor.hpp"

namespace {

using fleaux::cli::InputEvent;
using fleaux::cli::InputKey;
using fleaux::cli::LineEditor;
using fleaux::cli::LineEditorAction;
using fleaux::cli::LineEditorConfig;
using fleaux::cli::TokenClass;

void submit_line(LineEditor& editor, const std::string& line) {
  for (const char ch : line) {
    const auto result = editor.handle_event(InputEvent::character(ch));
    REQUIRE(result.action == LineEditorAction::kContinue);
  }

  const auto submit_result = editor.handle_event({.key = InputKey::kEnter});
  REQUIRE(submit_result.action == LineEditorAction::kSubmit);
  REQUIRE(submit_result.submitted_line == line);
}

}  // namespace

TEST_CASE("LineEditor inserts characters and edits around the cursor", "[repl][line-editor]") {
  LineEditor editor;

  REQUIRE(editor.handle_event(InputEvent::character('a')).needs_redraw);
  REQUIRE(editor.handle_event(InputEvent::character('c')).needs_redraw);
  REQUIRE(editor.handle_event({.key = InputKey::kArrowLeft}).needs_redraw);
  REQUIRE(editor.handle_event(InputEvent::character('b')).needs_redraw);

  REQUIRE(editor.buffer() == "abc");
  REQUIRE(editor.cursor() == 2);

  REQUIRE(editor.handle_event({.key = InputKey::kBackspace}).needs_redraw);
  REQUIRE(editor.buffer() == "ac");
  REQUIRE(editor.cursor() == 1);
}

TEST_CASE("LineEditor deletes the character under the cursor", "[repl][line-editor]") {
  LineEditor editor;

  editor.handle_event(InputEvent::character('a'));
  editor.handle_event(InputEvent::character('b'));
  editor.handle_event(InputEvent::character('c'));
  editor.handle_event({.key = InputKey::kArrowLeft});

  const auto result = editor.handle_event({.key = InputKey::kDelete});
  REQUIRE(result.action == LineEditorAction::kContinue);
  REQUIRE(result.needs_redraw);
  REQUIRE(editor.buffer() == "ab");
  REQUIRE(editor.cursor() == 2);
}

TEST_CASE("LineEditor recalls history and restores the in-progress edit", "[repl][line-editor]") {
  LineEditor editor;
  submit_line(editor, "first");
  submit_line(editor, "second");

  editor.handle_event(InputEvent::character('d'));
  editor.handle_event(InputEvent::character('r'));
  editor.handle_event(InputEvent::character('a'));
  editor.handle_event(InputEvent::character('f'));
  editor.handle_event(InputEvent::character('t'));

  auto result = editor.handle_event({.key = InputKey::kArrowUp});
  REQUIRE(result.needs_redraw);
  REQUIRE(editor.buffer() == "second");

  result = editor.handle_event({.key = InputKey::kArrowUp});
  REQUIRE(result.needs_redraw);
  REQUIRE(editor.buffer() == "first");

  result = editor.handle_event({.key = InputKey::kArrowDown});
  REQUIRE(result.needs_redraw);
  REQUIRE(editor.buffer() == "second");

  result = editor.handle_event({.key = InputKey::kArrowDown});
  REQUIRE(result.needs_redraw);
  REQUIRE(editor.buffer() == "draft");
}

TEST_CASE("LineEditor ctrl-c clears the current edit buffer", "[repl][line-editor]") {
  LineEditor editor;
  editor.handle_event(InputEvent::character('x'));
  editor.handle_event(InputEvent::character('y'));

  const auto result = editor.handle_event({.key = InputKey::kCtrlC});
  REQUIRE(result.action == LineEditorAction::kClearBuffer);
  REQUIRE(editor.buffer().empty());
  REQUIRE(editor.cursor() == 0);
}

TEST_CASE("LineEditor ctrl-d ends input only on an empty line", "[repl][line-editor]") {
  LineEditor editor;

  auto result = editor.handle_event({.key = InputKey::kCtrlD});
  REQUIRE(result.action == LineEditorAction::kEndOfInput);

  editor.handle_event(InputEvent::character('x'));
  result = editor.handle_event({.key = InputKey::kCtrlD});
  REQUIRE(result.action == LineEditorAction::kContinue);
  REQUIRE(editor.buffer() == "x");
}

TEST_CASE("LineEditor supports token-wise navigation, deletion, and home/end", "[repl][line-editor]") {
  LineEditor editor;
  for (const char ch : std::string{"alpha + beta"}) {
    REQUIRE(editor.handle_event(InputEvent::character(ch)).needs_redraw);
  }

  auto result = editor.handle_event({.key = InputKey::kTokenLeft});
  REQUIRE(result.needs_redraw);
  REQUIRE(editor.cursor() == 8);

  result = editor.handle_event({.key = InputKey::kTokenLeft});
  REQUIRE(result.needs_redraw);
  REQUIRE(editor.cursor() == 6);

  result = editor.handle_event({.key = InputKey::kTokenLeft});
  REQUIRE(result.needs_redraw);
  REQUIRE(editor.cursor() == 0);

  result = editor.handle_event({.key = InputKey::kArrowRight});
  REQUIRE(result.needs_redraw);
  REQUIRE(editor.cursor() == 1);

  result = editor.handle_event({.key = InputKey::kHome});
  REQUIRE(result.needs_redraw);
  REQUIRE(editor.cursor() == 0);

  result = editor.handle_event({.key = InputKey::kTokenRight});
  REQUIRE(result.needs_redraw);
  REQUIRE(editor.cursor() == 5);

  result = editor.handle_event({.key = InputKey::kTokenRight});
  REQUIRE(result.needs_redraw);
  REQUIRE(editor.cursor() == 7);

  result = editor.handle_event({.key = InputKey::kEnd});
  REQUIRE(result.needs_redraw);
  REQUIRE(editor.cursor() == editor.buffer().size());

  result = editor.handle_event({.key = InputKey::kTokenBackspace});
  REQUIRE(result.needs_redraw);
  REQUIRE(editor.buffer() == "alpha + ");
  REQUIRE(editor.cursor() == 8);
}

TEST_CASE("LineEditor returns no redraw for boundary and unknown events", "[repl][line-editor]") {
  LineEditor editor;

  REQUIRE_FALSE(editor.handle_event({.key = InputKey::kBackspace}).needs_redraw);
  REQUIRE_FALSE(editor.handle_event({.key = InputKey::kDelete}).needs_redraw);
  REQUIRE_FALSE(editor.handle_event({.key = InputKey::kArrowLeft}).needs_redraw);
  REQUIRE_FALSE(editor.handle_event({.key = InputKey::kArrowRight}).needs_redraw);
  REQUIRE_FALSE(editor.handle_event({.key = InputKey::kArrowUp}).needs_redraw);
  REQUIRE_FALSE(editor.handle_event({.key = InputKey::kArrowDown}).needs_redraw);
  REQUIRE_FALSE(editor.handle_event({.key = InputKey::kTokenLeft}).needs_redraw);
  REQUIRE_FALSE(editor.handle_event({.key = InputKey::kTokenRight}).needs_redraw);
  REQUIRE_FALSE(editor.handle_event({.key = InputKey::kTokenBackspace}).needs_redraw);
  REQUIRE_FALSE(editor.handle_event({.key = InputKey::kHome}).needs_redraw);
  REQUIRE_FALSE(editor.handle_event({.key = InputKey::kEnd}).needs_redraw);
  REQUIRE_FALSE(editor.handle_event({.key = InputKey::kUnknown}).needs_redraw);

  editor.handle_event(InputEvent::character('x'));
  REQUIRE_FALSE(editor.handle_event({.key = InputKey::kDelete}).needs_redraw);
  REQUIRE_FALSE(editor.handle_event({.key = InputKey::kEnd}).needs_redraw);
}

TEST_CASE("LineEditor reset clears transient state and preserves history", "[repl][line-editor]") {
  LineEditor editor(LineEditorConfig{.style_span_provider = [](const std::string_view text) -> std::vector<fleaux::cli::StyleSpan> {
    return {{.start = 0, .length = text.empty() ? 0U : 1U, .token_class = TokenClass::kKeyword}};
  }});

  const auto empty_submit = editor.handle_event({.key = InputKey::kEnter});
  REQUIRE(empty_submit.action == LineEditorAction::kSubmit);
  REQUIRE(empty_submit.submitted_line == std::string{});
  REQUIRE(editor.history().empty());

  submit_line(editor, "saved");
  REQUIRE(editor.history().size() == 1);
  REQUIRE(editor.config().style_span_provider("alpha").size() == 1);

  editor.handle_event(InputEvent::character('d'));
  editor.handle_event(InputEvent::character('r'));
  editor.handle_event(InputEvent::character('a'));
  editor.handle_event(InputEvent::character('f'));
  editor.handle_event(InputEvent::character('t'));
  REQUIRE(editor.handle_event({.key = InputKey::kArrowUp}).needs_redraw);
  REQUIRE(editor.buffer() == "saved");

  editor.reset();
  REQUIRE(editor.buffer().empty());
  REQUIRE(editor.cursor() == 0);
  REQUIRE(editor.history().size() == 1);
  REQUIRE_FALSE(editor.handle_event({.key = InputKey::kArrowDown}).needs_redraw);
}

TEST_CASE("LineEditor tab completes symbols from the configured completion handler", "[repl][line-editor]") {
  fleaux::cli::CompletionHandler completion;
  completion.load_symbols({"Std.Print", "Std.Println", "Square"});

  LineEditor editor(LineEditorConfig{.completion_handler = &completion});
  for (const char ch : std::string{"Std.Pr"}) {
    REQUIRE(editor.handle_event(InputEvent::character(ch)).needs_redraw);
  }

  auto result = editor.handle_event({.key = InputKey::kTab});
  REQUIRE(result.needs_redraw);
  REQUIRE(result.completion_suggestions == std::vector<std::string>{"Std.Print", "Std.Println"});
  REQUIRE(editor.buffer() == "Std.Print");

  REQUIRE(editor.handle_event(InputEvent::character('l')).needs_redraw);

  result = editor.handle_event({.key = InputKey::kTab});
  REQUIRE(result.needs_redraw);
  REQUIRE(result.completion_suggestions.empty());
  REQUIRE(editor.buffer() == "Std.Println");
}

TEST_CASE("LineEditor tab completion is a no-op without a symbol prefix", "[repl][line-editor]") {
  fleaux::cli::CompletionHandler completion;
  completion.load_symbols({"Std.Println"});
  LineEditor editor(LineEditorConfig{.completion_handler = &completion});

  REQUIRE_FALSE(editor.handle_event({.key = InputKey::kTab}).needs_redraw);
  REQUIRE(editor.handle_event({.key = InputKey::kTab}).completion_suggestions.empty());
  REQUIRE(editor.buffer().empty());
}

#ifdef _WIN32
TEST_CASE("Windows stdin interactivity requires a console-capable handle", "[repl][line-editor][windows]") {
  REQUIRE_FALSE(fleaux::cli::detail::windows_stdin_is_interactive_for_testing(false, false));
  REQUIRE_FALSE(fleaux::cli::detail::windows_stdin_is_interactive_for_testing(true, false));
  REQUIRE(fleaux::cli::detail::windows_stdin_is_interactive_for_testing(true, true));
}
#endif

TEST_CASE("LineEditor stores non-empty submissions in deduplicated adjacent history", "[repl][line-editor]") {
  LineEditor editor;

  submit_line(editor, "repeat");
  submit_line(editor, "repeat");

  REQUIRE(editor.history().size() == 1);
  REQUIRE(editor.history().front() == "repeat");
}


TEST_CASE("normalize_style_spans clamps out-of-bounds ranges and drops zero-length spans", "[repl][line-editor]") {
  const std::vector<fleaux::cli::StyleSpan> spans = {
      {.start = 0, .length = 0, .token_class = fleaux::cli::TokenClass::kKeyword},
      {.start = 2, .length = 10, .token_class = fleaux::cli::TokenClass::kString},
      {.start = 100, .length = 3, .token_class = fleaux::cli::TokenClass::kNumber},
  };

  const auto normalized = fleaux::cli::normalize_style_spans(5, spans);
  REQUIRE(normalized.size() == 1);
  REQUIRE(normalized[0].start == 2);
  REQUIRE(normalized[0].length == 3);
  REQUIRE(normalized[0].token_class == fleaux::cli::TokenClass::kString);
}

TEST_CASE("normalize_style_spans sorts by start and removes overlaps", "[repl][line-editor]") {
  const std::vector<fleaux::cli::StyleSpan> spans = {
      {.start = 4, .length = 2, .token_class = fleaux::cli::TokenClass::kNumber},
      {.start = 0, .length = 3, .token_class = fleaux::cli::TokenClass::kKeyword},
      {.start = 2, .length = 2, .token_class = fleaux::cli::TokenClass::kString},
      {.start = 6, .length = 1, .token_class = fleaux::cli::TokenClass::kOperator},
  };

  const auto normalized = fleaux::cli::normalize_style_spans(8, spans);
  REQUIRE(normalized.size() == 3);
  REQUIRE(normalized[0].start == 0);
  REQUIRE(normalized[0].length == 3);
  REQUIRE(normalized[1].start == 4);
  REQUIRE(normalized[1].length == 2);
  REQUIRE(normalized[2].start == 6);
  REQUIRE(normalized[2].length == 1);
}

TEST_CASE("decode_escape_bytes_for_testing recognizes common escape sequences", "[repl][line-editor]") {
  using fleaux::cli::detail::decode_escape_bytes_for_testing;

  SECTION("arrow and navigation keys") {
    REQUIRE(decode_escape_bytes_for_testing("\x1b[A").key == InputKey::kArrowUp);
    REQUIRE(decode_escape_bytes_for_testing("\x1b[B").key == InputKey::kArrowDown);
    REQUIRE(decode_escape_bytes_for_testing("\x1b[C").key == InputKey::kArrowRight);
    REQUIRE(decode_escape_bytes_for_testing("\x1b[D").key == InputKey::kArrowLeft);
    REQUIRE(decode_escape_bytes_for_testing("\x1bOH").key == InputKey::kHome);
    REQUIRE(decode_escape_bytes_for_testing("\x1bOF").key == InputKey::kEnd);
  }

  SECTION("csi parameter variants") {
    REQUIRE(decode_escape_bytes_for_testing("\x1b[3~").key == InputKey::kDelete);
    REQUIRE(decode_escape_bytes_for_testing("\x1b[1~").key == InputKey::kHome);
    REQUIRE(decode_escape_bytes_for_testing("\x1b[4~").key == InputKey::kEnd);
    REQUIRE(decode_escape_bytes_for_testing("\x1b[1;5D").key == InputKey::kTokenLeft);
    REQUIRE(decode_escape_bytes_for_testing("\x1b[1;5C").key == InputKey::kTokenRight);
  }

  SECTION("alt-style token movement and backspace") {
    REQUIRE(decode_escape_bytes_for_testing("\x1b" "b").key == InputKey::kTokenLeft);
    REQUIRE(decode_escape_bytes_for_testing("\x1b" "f").key == InputKey::kTokenRight);
    REQUIRE(decode_escape_bytes_for_testing("\x1b\x7f").key == InputKey::kTokenBackspace);
  }

  SECTION("unknown and truncated sequences stay unknown") {
    REQUIRE(decode_escape_bytes_for_testing("").key == InputKey::kUnknown);
    REQUIRE(decode_escape_bytes_for_testing("\x1b[").key == InputKey::kUnknown);
    REQUIRE(decode_escape_bytes_for_testing("\x1b[99").key == InputKey::kUnknown);
  }
}

TEST_CASE("format_completion_suggestions_for_testing keeps one line when width allows", "[repl][line-editor]") {
  const std::vector<std::string> suggestions = {"alpha", "beta", "gamma"};
  const auto lines = fleaux::cli::detail::format_completion_suggestions_for_testing(suggestions, 40);

  REQUIRE(lines == std::vector<std::string>{"alpha  beta  gamma"});
}

TEST_CASE("format_completion_suggestions_for_testing uses minimal rows with aligned columns", "[repl][line-editor]") {
  const std::vector<std::string> suggestions = {"aaa", "bbbb", "cc", "dddd"};
  const auto lines = fleaux::cli::detail::format_completion_suggestions_for_testing(suggestions, 12);

  REQUIRE(lines == std::vector<std::string>{"aaa  bbbb", "cc   dddd"});
}

TEST_CASE("format_completion_suggestions_for_testing falls back to one-per-line when narrow", "[repl][line-editor]") {
  const std::vector<std::string> suggestions = {"longer", "x"};
  const auto lines = fleaux::cli::detail::format_completion_suggestions_for_testing(suggestions, 4);

  REQUIRE(lines == std::vector<std::string>{"longer", "x"});
}

