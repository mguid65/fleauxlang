#pragma once

#include <optional>
#include <string>
#include <vector>

#include "fleaux/frontend/ast.hpp"
#include "fleaux/frontend/box.hpp"

namespace fleaux::frontend::type_system {

enum class TypeKind {
  kUnknown,
  kNever,
  kAny,
  kInt64,
  kUInt64,
  kFloat64,
  kString,
  kBool,
  kNull,
  kTuple,
  kUnion,
  kApplied,
  kFunction,
  kTypeVar,
  kNominal,
};

struct Type {
  TypeKind kind = TypeKind::kUnknown;
  std::string nominal_name;
  bool variadic = false;
  std::vector<Type> items;
  std::vector<Type> union_members;
  std::string applied_name;
  std::vector<Type> applied_args;
  std::vector<Type> function_params;
  std::optional<Box<Type>> function_return;
};

[[nodiscard]] auto normalize_type(Type type) -> Type;

[[nodiscard]] auto from_ir_type(const ir::IRSimpleType& type) -> Type;

[[nodiscard]] auto is_builtin_opaque_nominal_type_name(const std::string& name) -> bool;

[[nodiscard]] auto is_integer_like(const Type& type) -> bool;

[[nodiscard]] auto is_consistent(const Type& expected, const Type& actual) -> bool;

}  // namespace fleaux::frontend::type_system
