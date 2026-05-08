#pragma once

#include <string>
#include <vector>

#include "fleaux/common/indirect_optional.hpp"
#include "fleaux/frontend/ast.hpp"

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
  common::IndirectOptional<Type> function_return;
};

[[nodiscard]] inline auto make_type(const TypeKind kind) -> Type {
  Type type{};
  type.kind = kind;
  return type;
}

[[nodiscard]] inline auto make_nominal_type(std::string name) -> Type {
  Type type = make_type(TypeKind::kNominal);
  type.nominal_name = std::move(name);
  return type;
}

[[nodiscard]] auto normalize_type(Type type) -> Type;

[[nodiscard]] auto from_ir_type(const ir::IRSimpleType& type) -> Type;

[[nodiscard]] auto is_builtin_opaque_nominal_type_name(const std::string& name) -> bool;

[[nodiscard]] auto is_integer_like(const Type& type) -> bool;

[[nodiscard]] auto is_consistent(const Type& expected, const Type& actual) -> bool;

}  // namespace fleaux::frontend::type_system
