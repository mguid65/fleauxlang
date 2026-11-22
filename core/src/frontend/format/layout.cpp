#include "fleaux/frontend/format/layout.hpp"

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fleaux::frontend::format {
namespace {

auto make_container(const Doc::Kind kind, std::vector<Doc> parts) -> Doc {
  const bool breaks = std::ranges::any_of(parts, [](const Doc& part) -> bool { return part.breaks; });
  return Doc{.kind = kind, .text = {}, .parts = std::move(parts), .breaks = breaks};
}

// Columns taken by UTF-8 text: one per code point.
auto display_width(const std::string_view text) -> std::size_t {
  return static_cast<std::size_t>(
      std::ranges::count_if(text, [](const char ch) -> bool { return (static_cast<unsigned char>(ch) & 0xC0U) != 0x80U; }));
}

// A line of only indentation becomes empty. Other lines keep their trailing spaces: in a raw string they are content.
auto trim_end(std::string text) -> std::string {
  if (std::ranges::all_of(text, [](const char ch) -> bool { return ch == ' '; })) {
    text.clear();
  }
  return text;
}

class Renderer {
public:
  explicit Renderer(const RenderOptions& options) : options_(options) {}

  auto run(const Doc& root) -> std::string {
    stack_.push_back(Frame{.indent = 0, .flat = false, .doc = &root});
    while (!stack_.empty()) {
      const Frame frame = stack_.back();
      stack_.pop_back();
      step(frame);
    }
    lines_.push_back(OutputLine{.code = trim_end(std::move(code_)), .comment = std::move(comment_)});
    return assemble();
  }

private:
  struct Frame {
    std::size_t indent = 0;
    bool flat = false;
    const Doc* doc = nullptr;
  };

  struct OutputLine {
    std::string code;
    std::string comment;
  };

  void push_parts(const Doc& doc, const std::size_t indent, const bool flat) {
    for (const Doc& part : std::ranges::reverse_view(doc.parts)) {
      stack_.push_back(Frame{.indent = indent, .flat = flat, .doc = &part});
    }
  }

  void step(const Frame& frame) {
    const Doc& doc = *frame.doc;
    switch (doc.kind) {
      case Doc::Kind::kText:
        code_ += doc.text;
        column_ += display_width(doc.text);
        break;
      case Doc::Kind::kLine:
        if (frame.flat) {
          code_ += ' ';
          ++column_;
        } else {
          new_line(frame.indent);
        }
        break;
      case Doc::Kind::kSoftLine:
        if (!frame.flat) {
          new_line(frame.indent);
        }
        break;
      case Doc::Kind::kHardLine:
        new_line(frame.indent);
        break;
      case Doc::Kind::kLineSuffix:
        if (!comment_.empty()) {
          comment_ += ' ';
        }
        comment_ += doc.text;
        break;
      case Doc::Kind::kConcat:
        push_parts(doc, frame.indent, frame.flat);
        break;
      case Doc::Kind::kIndent:
        push_parts(doc, frame.indent + options_.indent, frame.flat);
        break;
      case Doc::Kind::kGroup: {
        const bool flat = frame.flat || (!doc.breaks && fits(doc));
        push_parts(doc, frame.indent, flat);
        break;
      }
    }
  }

  // Whether the group fits on the rest of the current line, laid out flat, together with what follows it up to the
  // next line break. What follows keeps its own layout; a group in it that is not yet decided counts as broken.
  [[nodiscard]] auto fits(const Doc& group_doc) const -> bool {
    auto room = static_cast<std::ptrdiff_t>(options_.width) - static_cast<std::ptrdiff_t>(column_);
    std::vector<Frame> pending{Frame{.indent = 0, .flat = true, .doc = &group_doc}};
    std::size_t rest = stack_.size();
    while (room >= 0) {
      if (pending.empty()) {
        if (rest == 0) {
          return true;
        }
        --rest;
        pending.push_back(stack_[rest]);
      }
      const Frame frame = pending.back();
      pending.pop_back();
      const Doc& doc = *frame.doc;
      switch (doc.kind) {
        case Doc::Kind::kText:
          room -= static_cast<std::ptrdiff_t>(display_width(doc.text));
          break;
        case Doc::Kind::kLine:
          if (!frame.flat) {
            return true;
          }
          --room;
          break;
        case Doc::Kind::kSoftLine:
          if (!frame.flat) {
            return true;
          }
          break;
        case Doc::Kind::kHardLine:
          return true;
        case Doc::Kind::kLineSuffix:
          break;
        case Doc::Kind::kConcat:
        case Doc::Kind::kIndent:
        case Doc::Kind::kGroup: {
          const bool flat = frame.flat && !doc.breaks;
          for (const Doc& part : std::ranges::reverse_view(doc.parts)) {
            pending.push_back(Frame{.indent = 0, .flat = flat, .doc = &part});
          }
          break;
        }
      }
    }
    return false;
  }

  void new_line(const std::size_t indent) {
    lines_.push_back(OutputLine{.code = trim_end(std::move(code_)), .comment = std::move(comment_)});
    code_.assign(indent, ' ');
    comment_.clear();
    column_ = indent;
  }

  [[nodiscard]] auto assemble() -> std::string {
    while (!lines_.empty() && lines_.back().code.empty() && lines_.back().comment.empty()) {
      lines_.pop_back();
    }

    std::string out;
    std::size_t index = 0;
    while (index < lines_.size()) {
      if (lines_[index].comment.empty()) {
        out += lines_[index].code;
        out += options_.newline;
        ++index;
        continue;
      }

      // A run of consecutive lines with trailing comments: the comments start in one column.
      std::size_t run_end = index;
      std::size_t code_width = 0;
      while (run_end < lines_.size() && !lines_[run_end].comment.empty()) {
        code_width = std::max(code_width, display_width(lines_[run_end].code));
        ++run_end;
      }
      for (; index < run_end; ++index) {
        const OutputLine& output_line = lines_[index];
        if (!output_line.code.empty()) {
          out += output_line.code;
          out.append(code_width + 2 - display_width(output_line.code), ' ');
        }
        out += output_line.comment;
        out += options_.newline;
      }
    }
    return out;
  }

  const RenderOptions& options_;
  std::vector<Frame> stack_;
  std::vector<OutputLine> lines_;
  std::string code_;
  std::string comment_;
  std::size_t column_ = 0;
};

}  // namespace

auto text(std::string value) -> Doc { return Doc{.kind = Doc::Kind::kText, .text = std::move(value), .parts = {}, .breaks = false}; }

auto line() -> Doc { return Doc{.kind = Doc::Kind::kLine, .text = {}, .parts = {}, .breaks = false}; }

auto soft_line() -> Doc { return Doc{.kind = Doc::Kind::kSoftLine, .text = {}, .parts = {}, .breaks = false}; }

auto hard_line() -> Doc { return Doc{.kind = Doc::Kind::kHardLine, .text = {}, .parts = {}, .breaks = true}; }

auto line_suffix(std::string comment) -> Doc {
  return Doc{.kind = Doc::Kind::kLineSuffix, .text = std::move(comment), .parts = {}, .breaks = true};
}

auto concat(std::vector<Doc> parts) -> Doc { return make_container(Doc::Kind::kConcat, std::move(parts)); }

auto indent(std::vector<Doc> parts) -> Doc { return make_container(Doc::Kind::kIndent, std::move(parts)); }

auto group(std::vector<Doc> parts) -> Doc { return make_container(Doc::Kind::kGroup, std::move(parts)); }

auto render(const Doc& doc, const RenderOptions& options) -> std::string { return Renderer(options).run(doc); }

}  // namespace fleaux::frontend::format
