#pragma once

#include <string>

#include <tl/expected.hpp>

#include "fleaux/frontend/ast.hpp"

namespace fleaux::frontend::parse {

struct ParseError {
  std::string message;
  std::optional<std::string> hint;
  std::optional<diag::SourceSpan> span;
};

using ParseResult = tl::expected<model::Program, ParseError>;

class Parser {
 public:
  ParseResult parse_program(const std::string& source,
                           const std::string& source_name) const;
};

}  // namespace fleaux::frontend::parse

