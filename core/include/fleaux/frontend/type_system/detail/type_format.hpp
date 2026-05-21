#pragma once

#include <string>
#include <string_view>
#include <utility>

namespace fleaux::frontend::type_system::detail {

template <typename Range, typename Formatter>
void append_joined_fragments(std::string& out, const Range& values, const std::string_view separator,
                                    Formatter&& formatter) {
  bool first = true;
  for (const auto& value : values) {
    if (!first) {
      out += separator;
    }
    first = false;
    out += std::forward<Formatter>(formatter)(value);
  }
}

template <typename Range, typename Formatter, typename VariadicPredicate>
void append_joined_type_fragments(std::string& out, const Range& values, const std::string_view separator,
                                         Formatter&& formatter, VariadicPredicate&& variadic_predicate) {
  bool first = true;
  for (const auto& value : values) {
    if (!first) {
      out += separator;
    }
    first = false;
    out += std::forward<Formatter>(formatter)(value);
    if (std::forward<VariadicPredicate>(variadic_predicate)(value)) {
      out += "...";
    }
  }
}

}  // namespace fleaux::frontend::type_system::detail

