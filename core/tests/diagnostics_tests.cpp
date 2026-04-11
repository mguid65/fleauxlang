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
  span.source_text = "let x: Number = 1;\n";
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

  REQUIRE(rendered.find("[parse] Expected ';'") != std::string::npos);
  REQUIRE(rendered.find("sample.fleaux") != std::string::npos);
  REQUIRE(rendered.find("Terminate the current statement") != std::string::npos);
}

