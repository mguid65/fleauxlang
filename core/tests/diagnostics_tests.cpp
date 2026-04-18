#include <optional>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/frontend/diagnostics.hpp"

TEST_CASE("SourceSpan caret width follows span bounds", "[diagnostics]") {
  fleaux::frontend::diag::SourceSpan span;
  span.line = 3;
  span.col = 5;
  span.end_line = 3;
  span.end_col = 9;

  REQUIRE(span.caret_width() == 4);
}

TEST_CASE("format_diagnostic includes location and hint", "[diagnostics]") {
  fleaux::frontend::diag::SourceSpan span;
  span.source_name = "sample.fleaux";
  span.source_text = "let x: Float64 = 1;\n";
  span.line = 1;
  span.col = 5;
  span.end_line = 1;
  span.end_col = 6;

  const std::string rendered = fleaux::frontend::diag::format_diagnostic(
      "parse",
      "Expected ';'",
      span,
      std::optional<std::string>{"Terminate the current statement with ';'."},
      std::nullopt);

  REQUIRE(rendered.find("error[parse]") != std::string::npos);
  REQUIRE(rendered.find("Expected ';'") != std::string::npos);
  REQUIRE(rendered.find("sample.fleaux") != std::string::npos);
  REQUIRE(rendered.find("note:") != std::string::npos);
  REQUIRE(rendered.find("Terminate the current statement") != std::string::npos);
  REQUIRE(rendered.find("^") != std::string::npos);
}

TEST_CASE("format_diagnostic shows context lines around the error", "[diagnostics]") {
  fleaux::frontend::diag::SourceSpan span;
  span.source_name = "ctx.fleaux";
  span.source_text = "import Std;\nlet Bad = 1;\n(1) -> Std.Println;\n";
  span.line = 2;
  span.col = 9;
  span.end_line = 2;
  span.end_col = 10;

  const std::string rendered = fleaux::frontend::diag::format_diagnostic(
      "parse", "Unexpected '='", span, std::optional<std::string>{"Use '::' or '=' inside a let body."});

  REQUIRE(rendered.find("import Std;") != std::string::npos);
  REQUIRE(rendered.find("let Bad = 1;") != std::string::npos);
  REQUIRE(rendered.find("(1) -> Std.Println;") != std::string::npos);
  REQUIRE(rendered.find("^") != std::string::npos);
  REQUIRE(rendered.find("ctx.fleaux:2:9") != std::string::npos);
}

TEST_CASE("format_diagnostic without span omits location section", "[diagnostics]") {
  const std::string rendered = fleaux::frontend::diag::format_diagnostic(
      "runtime", "Stack underflow", std::nullopt, std::optional<std::string>{"Check your call chain."});

  REQUIRE(rendered.find("error[runtime]") != std::string::npos);
  REQUIRE(rendered.find("Stack underflow") != std::string::npos);
  REQUIRE(rendered.find("note:") != std::string::npos);
  REQUIRE(rendered.find("-->") == std::string::npos);
}

TEST_CASE("format_diagnostic caret spans multi-character tokens", "[diagnostics]") {
  fleaux::frontend::diag::SourceSpan span;
  span.source_name = "width.fleaux";
  span.source_text = "let Foo(): Any = bar;\n";
  span.line = 1;
  span.col = 18;
  span.end_line = 1;
  span.end_col = 21;

  const std::string rendered = fleaux::frontend::diag::format_diagnostic("parse", "Undefined name", span);

  REQUIRE(rendered.find("^~~") != std::string::npos);
}
