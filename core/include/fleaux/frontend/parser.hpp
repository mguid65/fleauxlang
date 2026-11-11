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
  // Parses a single standalone type such as 'Result<Tuple<Int64...>, String>'.
  [[nodiscard]] auto parse_type(const std::string& source, const std::string& source_name) const
      -> tl::expected<model::TypeNode, ParseError>;
  [[nodiscard]] auto dump_ast(const model::Program& program) const -> std::string;
};

}  // namespace fleaux::frontend::parse
