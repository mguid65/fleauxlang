// libFuzzer harness for the lexer, the parser, and the formatter: any bytes as Fleaux source. Source that parses is
// formatted too, which parses the formatted text again and checks that it is the same program.

#include <cstddef>
#include <cstdint>
#include <string>

#include "fleaux/frontend/format/formatter.hpp"
#include "fleaux/frontend/parser.hpp"

extern "C" auto LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) -> int {
  const std::string source(reinterpret_cast<const char*>(data), size);
  constexpr fleaux::frontend::parse::Parser parser;
  if (const auto parsed = parser.parse_program(source, "fuzz.fleaux"); parsed.has_value()) {
    (void)fleaux::frontend::format::format_source(source, "fuzz.fleaux");
  }
  return 0;
}
