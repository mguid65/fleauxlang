// The display of a failed run's calls: the notes diagnostics show after the error, and the span of the failure.

#include <filesystem>
#include <format>
#include <string>
#include <vector>

#include "fleaux/common/fnv1a.hpp"
#include "fleaux/frontend/source_loader.hpp"
#include "fleaux/vm/runtime.hpp"

namespace fleaux::vm {
namespace {

auto describe_position(const RuntimeCall& call) -> std::string {
  if (call.file.empty()) {
    return "no location";
  }
  return std::format("{}:{}:{}", std::filesystem::path(call.file).filename().string(), call.line, call.column);
}

auto describe_caller(const RuntimeCall& call) -> std::string {
  const std::string function = call.function.empty() ? std::string{"the top level"} : call.function;
  if (!call.through.empty()) {
    return std::format("called by {} from {} ({})", call.through, function, describe_position(call));
  }
  return std::format("called from {} ({})", function, describe_position(call));
}

// The lines kept of a chain longer than the limit: its first and its last.
constexpr std::size_t kRuntimeCallNotesKeptFirst = 15U;
constexpr std::size_t kRuntimeCallNotesKeptLast = 5U;
static_assert(kRuntimeCallNotesKeptFirst + kRuntimeCallNotesKeptLast == kRuntimeCallNoteLimit);

}  // namespace

auto describe_runtime_calls(const std::vector<RuntimeCall>& calls) -> std::vector<std::string> {
  std::vector<std::string> notes;
  if (calls.empty()) {
    return notes;
  }
  if (!calls.front().function.empty()) {
    notes.push_back("in " + calls.front().function);
  }

  // Consecutive calls from one place, as in a recursion, are one line.
  for (std::size_t index = 1U; index < calls.size();) {
    const std::string line = describe_caller(calls[index]);
    std::size_t repeats = 1U;
    while (index + repeats < calls.size() && describe_caller(calls[index + repeats]) == line) {
      ++repeats;
    }
    notes.push_back(repeats == 1U ? line : std::format("{}, {} times", line, repeats));
    index += repeats;
  }

  if (notes.size() > kRuntimeCallNoteLimit) {
    const std::size_t hidden = notes.size() - kRuntimeCallNotesKeptFirst - kRuntimeCallNotesKeptLast;
    std::vector<std::string> kept(notes.begin(), notes.begin() + kRuntimeCallNotesKeptFirst);
    kept.push_back(std::format("... {} more line{} of calls", hidden, hidden == 1U ? "" : "s"));
    kept.insert(kept.end(), notes.end() - kRuntimeCallNotesKeptLast, notes.end());
    notes = std::move(kept);
  }
  return notes;
}

auto runtime_failure_span(const RuntimeError& error, const SourceTextReader& read_text) -> std::optional<frontend::diag::SourceSpan> {
  if (error.calls.empty()) {
    return error.span;
  }
  const RuntimeCall& failure = error.calls.front();
  if (failure.file.empty()) {
    return std::nullopt;
  }
  std::optional<std::string> text;
  if (read_text) {
    text = read_text(failure.file);
  } else if (auto read = frontend::source_loader::read_text_file(failure.file); read) {
    text = std::move(*read);
  }
  // A file that changed since it was compiled would show the wrong line, so only the position is shown.
  if (text.has_value() && common::fnv1a(*text) != failure.file_hash) {
    text.reset();
  }
  return frontend::diag::SourceSpan{
      .file = frontend::diag::SourceFile(failure.file, text.value_or(std::string{})),
      .line = static_cast<int>(failure.line),
      .col = static_cast<int>(failure.column),
      .end_line = static_cast<int>(failure.end_line),
      .end_col = static_cast<int>(failure.end_column),
  };
}

}  // namespace fleaux::vm
