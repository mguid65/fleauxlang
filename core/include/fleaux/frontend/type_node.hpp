#pragma once

#include <optional>
#include <string>
#include <vector>

#include "fleaux/frontend/diagnostics.hpp"

namespace fleaux::frontend::types {

enum class TypeNodeKind {
  kNamed,
  kTuple,
  kUnion,
  kApplied,   // Named type with type arguments, e.g. Dict(String, Any)
  kFunction,  // Function type, e.g. (Any, String) => Bool
};

struct TypeNode {
  TypeNodeKind kind = TypeNodeKind::kNamed;
  std::string name;
  bool variadic = false;
  std::vector<TypeNode> items;
  std::optional<diag::SourceSpan> span;
};

}  // namespace fleaux::frontend::types
