#pragma once

#include <optional>
#include <string>
#include <unordered_map>
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
  // For kTypeVar: the bound declared on the generic parameter ('<T: Bound>'), if any.
  common::IndirectOptional<Type> type_var_bound;
  // For kFunction: the type parameters of a generic function used as a value without type arguments (`Std.Add`, a
  // generic closure). Such an open function is instantiated from the function type it is passed as; until then its
  // own type variables, renamed so they cannot clash with others, appear in its parameter and return types.
  std::vector<std::string> function_generic_params;
  // For an open function: the name it was referenced by (or "closure"), for diagnostics.
  std::string open_function_name;
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

// The type a parameter has inside its function body. A variadic parameter `items: T...` collects the remaining
// arguments, so inside the body it is `Tuple<T...>`; any other parameter keeps its declared type.
[[nodiscard]] auto param_local_type(const Type& declared, bool variadic) -> Type;

[[nodiscard]] auto from_ir_type(const ir::IRSimpleType& type) -> Type;

// The IR form of a type, so that types the checker derives can be recorded in the IR and in bytecode.
[[nodiscard]] auto to_ir_type(const Type& type) -> ir::IRSimpleType;

// The open function value of generic function type `function_type` with type parameters `generic_params`: each type
// parameter is renamed to a fresh type variable (keeping its bound) and listed in function_generic_params. `name` is
// used in diagnostics.
[[nodiscard]] auto make_open_function_type(const Type& function_type, const std::vector<std::string>& generic_params,
                                           std::string name) -> Type;

[[nodiscard]] inline auto is_open_function(const Type& type) -> bool {
  return type.kind == TypeKind::kFunction && !type.function_generic_params.empty();
}

// Instantiates an open function from the function type it is expected to have. Its type variables are taken from the
// expected parameter types and must satisfy their bounds; a type variable that appears only in the return type
// cannot be inferred. Returns nullopt when the open function cannot take the expected type's parameters.
[[nodiscard]] auto instantiate_open_function(const Type& open, const Type& expected) -> std::optional<Type>;

[[nodiscard]] auto is_integer_like(const Type& type) -> bool;

[[nodiscard]] auto is_consistent(const Type& expected, const Type& actual) -> bool;

// True when actual may bind a generic parameter declared with this bound. A bound is a type, usually a union of
// alternatives; the binding must be exactly one alternative, not a union of several. A type variable satisfies the
// bound when its own bound only allows alternatives of this bound. Never satisfies every bound; Any satisfies none.
[[nodiscard]] auto satisfies_generic_bound(const Type& actual, const Type& bound) -> bool;

// The generic parameters visible at a point in a program, each with its optional bound.
class GenericScope {
public:
  GenericScope() = default;

  // Adds a parameter, or replaces one with the same name (an inner scope shadowing an outer one).
  void add(const std::string& name, std::optional<Type> bound = std::nullopt) {
    params_.insert_or_assign(name, std::move(bound));
  }

  [[nodiscard]] auto contains(const std::string& name) const -> bool { return params_.contains(name); }

  // The parameter's bound, or nullptr when it is unbounded or not in scope.
  [[nodiscard]] auto bound_of(const std::string& name) const -> const Type* {
    const auto it = params_.find(name);
    return it != params_.end() && it->second.has_value() ? &*it->second : nullptr;
  }

  [[nodiscard]] auto empty() const -> bool { return params_.empty(); }

private:
  std::unordered_map<std::string, std::optional<Type>> params_;
};

}  // namespace fleaux::frontend::type_system
