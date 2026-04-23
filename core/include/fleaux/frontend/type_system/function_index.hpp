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

using FunctionOverloadSet = std::vector<FunctionSig>;

class FunctionIndex {
public:
  explicit FunctionIndex(const ir::IRProgram& program, const std::unordered_set<std::string>& imported_symbols,
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

}  // namespace fleaux::frontend::type_system
