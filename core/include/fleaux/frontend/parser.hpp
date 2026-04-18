#pragma once

#include <exception>
#include <string>

#include <tl/expected.hpp>

#include "fleaux/frontend/ast.hpp"

namespace fleaux::frontend::parse {

struct ParseError : std::exception {
  std::string message;
  std::optional<std::string> hint;
  std::optional<diag::SourceSpan> span;

  [[nodiscard]] auto what() const noexcept -> const char* override { return message.c_str(); }
};

using ParseResult = tl::expected<model::Program, ParseError>;

class Parser {
public:
  [[nodiscard]] auto parse_program(const std::string& source, const std::string& source_name) const -> ParseResult;
};

}  // namespace fleaux::frontend::parse
