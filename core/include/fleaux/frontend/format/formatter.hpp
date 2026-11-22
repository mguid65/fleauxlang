#pragma once

#include <cstddef>
#include <string>

#include <tl/expected.hpp>

#include "fleaux/frontend/parser.hpp"

namespace fleaux::frontend::format {

struct FormatOptions {
  std::size_t width = 100;
  std::size_t indent = 2;
};

// Formats Fleaux source. Comments, blank lines between statements (at most one), and the spelling of every literal and
// name are kept; only whitespace changes. Source that does not parse comes back as its parse error, and the formatted
// source always parses to the same program, or the formatter reports an error instead of returning it. The result
// keeps the source's line endings.
[[nodiscard]] auto format_source(const std::string& source, const std::string& source_name,
                                 const FormatOptions& options = {}) -> tl::expected<std::string, parse::ParseError>;

}  // namespace fleaux::frontend::format
