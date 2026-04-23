#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/cli/line_editor.hpp"

namespace {

using fleaux::cli::InputEvent;
using fleaux::cli::InputKey;
using fleaux::cli::LineEditor;
using fleaux::cli::LineEditorAction;

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
