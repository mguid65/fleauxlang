#pragma once

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

struct FunctionSig {
  std::string resolved_symbol_key;
  std::vector<std::string> generic_params;
  std::vector<ParamSig> params;
  Type return_type;
  bool is_builtin = false;
};

struct StrongTypeDecl {
  std::string name;
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
      -> const FunctionOverloadSet*;

  [[nodiscard]] auto has_unqualified_symbol(const std::string& name) const -> bool;
  [[nodiscard]] auto has_qualified_symbol(const std::optional<std::string>& qualifier, const std::string& name) const
      -> bool;

private:
  std::unordered_map<std::string, FunctionOverloadSet> symbols_;
  std::unordered_set<std::string> unqualified_symbols_;
  std::unordered_set<std::string> qualified_symbols_;
};

class AliasIndex {
public:
  explicit AliasIndex(const ir::IRProgram& program,
                      const std::vector<ir::IRAliasDecl>& imported_alias_decls = {});

  [[nodiscard]] auto resolve_name(const std::string& name) const -> const AliasDecl*;
  [[nodiscard]] auto has_name(const std::string& name) const -> bool;

private:
  std::unordered_map<std::string, AliasDecl> decls_;
};

class StrongTypeIndex {
public:
  explicit StrongTypeIndex(const ir::IRProgram& program, const AliasIndex& alias_index,
                           const std::vector<ir::IRTypeDecl>& imported_type_decls = {});

  [[nodiscard]] auto resolve_name(const std::string& name) const -> const StrongTypeDecl*;
  [[nodiscard]] auto has_name(const std::string& name) const -> bool;
  [[nodiscard]] auto known_names() const -> const std::unordered_set<std::string>&;

private:
  std::unordered_set<std::string> known_names_;
  std::unordered_map<std::string, StrongTypeDecl> decls_;
};

}  // namespace fleaux::frontend::type_system
