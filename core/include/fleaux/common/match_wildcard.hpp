#pragma once

#include <string_view>

namespace fleaux::common {

// Lowering writes the `_` pattern of a Std.Match case as this string constant. The type checker, the bytecode
// compiler, and the Std.Match builtin all know a wildcard case by it.
inline constexpr std::string_view kMatchWildcardSentinel = "__fleaux_match_wildcard__";

}  // namespace fleaux::common
