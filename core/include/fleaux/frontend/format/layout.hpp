#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fleaux::frontend::format {

// A document for the pretty printer, after Wadler's "A prettier printer": text joined by line breaks that are either
// all kept on one line, when their group fits the width, or all broken.
struct Doc {
  enum class Kind : std::uint8_t {
    // Text that is never broken.
    kText,
    // A space when its group stays on one line, a line break when the group breaks.
    kLine,
    // Nothing when its group stays on one line, a line break when the group breaks.
    kSoftLine,
    // Always a line break. The groups around it break.
    kHardLine,
    // A trailing comment, moved to the end of the output line it is on. The groups around it break, so that no code
    // follows the comment on its line.
    kLineSuffix,
    kConcat,
    // Its parts, with the line breaks inside them indented one more level.
    kIndent,
    // Its parts on one line if they fit the width, otherwise every line break directly inside the group breaks.
    kGroup,
  };

  Kind kind = Kind::kText;
  std::string text{};
  std::vector<Doc> parts{};
  // Contains a hard line or a line suffix, so every group around it must break.
  bool breaks = false;
};

[[nodiscard]] auto text(std::string value) -> Doc;
[[nodiscard]] auto line() -> Doc;
[[nodiscard]] auto soft_line() -> Doc;
[[nodiscard]] auto hard_line() -> Doc;
[[nodiscard]] auto line_suffix(std::string comment) -> Doc;
[[nodiscard]] auto concat(std::vector<Doc> parts) -> Doc;
[[nodiscard]] auto indent(std::vector<Doc> parts) -> Doc;
[[nodiscard]] auto group(std::vector<Doc> parts) -> Doc;

struct RenderOptions {
  std::size_t width = 100;
  std::size_t indent = 2;
  std::string newline = "\n";
};

// Lays the document out within the width. Trailing comments on consecutive lines line up in one column, two spaces
// after the longest code among those lines. The result ends with one newline, or is empty for an empty document.
[[nodiscard]] auto render(const Doc& doc, const RenderOptions& options) -> std::string;

}  // namespace fleaux::frontend::format
