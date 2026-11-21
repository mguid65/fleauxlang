#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "fleaux/frontend/ast.hpp"
#include "fleaux/frontend/type_system/type.hpp"

namespace fleaux::frontend::type_system {

struct ParamSig {
  std::string name;
  Type type;
  bool variadic = false;
};

// An operator that a generic function applies to a type parameter whose bound contains a strong type, such as `+`
// on `T: Int64 | Money`. The implementation depends on the type the parameter binds to (Std.Add or Money.Add), and
// strong types do not exist at runtime, so the function takes it as a hidden leading argument from its callers.
struct OperatorRequirement {
  std::string op;          // The operator symbol, such as "+".
  std::string type_param;  // The function's own type parameter the operator is applied to.
  Type signature;          // The operator's function type, in terms of the function's own type parameters.
};

struct FunctionSig {
  std::string resolved_symbol_key;
  std::vector<std::string> generic_params;
  // Alias-expanded bounds of the bounded generic parameters, keyed by parameter name.
  std::unordered_map<std::string, Type> generic_bounds;
  std::vector<ParamSig> params;
  Type return_type;
  bool is_builtin = false;
  // A value let (`let X: T = ...`): its name stands for its value, so naming it as a call target calls that value.
  bool is_value_binding = false;
  // Hidden leading arguments, in order; see OperatorRequirement.
  std::vector<OperatorRequirement> operator_requirements;
};

struct StrongTypeDecl {
  std::string name;
  std::vector<std::string> generic_params;
  // Runtime-provided type declared with '__builtin__'. It has no target type.
  bool is_builtin = false;
  Type target_type;
  std::optional<diag::SourceSpan> span;
};

struct AliasDecl {
  std::string name;
  Type target_type;
  std::optional<diag::SourceSpan> span;
};

using FunctionOverloadSet = std::vector<FunctionSig>;

class AliasIndex;
class StrongTypeIndex;

class FunctionIndex {
public:
  explicit FunctionIndex(const ir::IRProgram& program, const std::unordered_set<std::string>& imported_symbols,
                         const StrongTypeIndex& type_index, const AliasIndex& alias_index,
                         const std::vector<ir::IRLet>& imported_typed_lets = {});

  [[nodiscard]] auto resolve_name(const std::optional<std::string>& qualifier, const std::string& name) const
      -> std::optional<std::reference_wrapper<const FunctionOverloadSet>>;

  [[nodiscard]] auto has_unqualified_symbol(const std::string& name) const -> bool;
  [[nodiscard]] auto has_qualified_symbol(const std::optional<std::string>& qualifier, const std::string& name) const
      -> bool;

  // Records the operator requirements of the function with this symbol key, found while checking its body.
  void set_operator_requirements(const std::string& resolved_symbol_key,
                                 const std::vector<OperatorRequirement>& requirements);

private:
  std::unordered_map<std::string, FunctionOverloadSet> symbols_;
  std::unordered_set<std::string> unqualified_symbols_;
  std::unordered_set<std::string> qualified_symbols_;
};

class AliasIndex {
public:
  explicit AliasIndex(const ir::IRProgram& program,
                      const std::vector<ir::IRAliasDecl>& imported_alias_decls = {});

  [[nodiscard]] auto resolve_name(const std::string& name) const -> std::optional<std::reference_wrapper<const AliasDecl>>;
  [[nodiscard]] auto has_name(const std::string& name) const -> bool;

private:
  std::unordered_map<std::string, AliasDecl> decls_;
};

class StrongTypeIndex {
public:
  explicit StrongTypeIndex(const ir::IRProgram& program, const AliasIndex& alias_index,
                           const std::vector<ir::IRTypeDecl>& imported_type_decls = {});

  [[nodiscard]] auto resolve_name(const std::string& name) const
      -> std::optional<std::reference_wrapper<const StrongTypeDecl>>;
  [[nodiscard]] auto has_name(const std::string& name) const -> bool;
  [[nodiscard]] auto known_names() const -> const std::unordered_map<std::string, std::size_t>&;
  // True when the type is a declared builtin type, applied or not.
  [[nodiscard]] auto is_builtin_type(const Type& type) const -> bool;

private:
  std::unordered_map<std::string, std::size_t> known_names_;
  std::unordered_map<std::string, StrongTypeDecl> decls_;
};

}  // namespace fleaux::frontend::type_system
