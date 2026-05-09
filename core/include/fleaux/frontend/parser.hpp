#pragma once

#include <optional>
#include <string>

#include <tl/expected.hpp>

#include "fleaux/frontend/ast.hpp"

namespace fleaux::frontend::parse {

struct ParseError {
  std::string message{};
  std::optional<std::string> hint{std::nullopt};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

using ParseResult = tl::expected<model::Program, ParseError>;

class Parser {
public:
  [[nodiscard]] auto parse_program(const std::string& source, const std::string& source_name) const -> ParseResult;
  [[nodiscard]] auto dump_ast(const model::Program& program) const -> std::string;
};

}  // namespace fleaux::frontend::parse
