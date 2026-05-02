#include "fleaux/frontend/type_system/function_index.hpp"

namespace fleaux::frontend::type_system {
namespace {

auto symbol_key(const std::optional<std::string>& qualifier, const std::string& name) -> std::string {
  return qualifier.has_value() ? (*qualifier + "." + name) : name;
}

auto rewrite_generic_type(const Type& type, const std::unordered_set<std::string>& generic_params) -> Type {
  Type out = type;

  if (out.kind == TypeKind::kNominal && generic_params.contains(out.nominal_name)) {
    out.kind = TypeKind::kTypeVar;
    return out;
  }

  for (auto& item : out.items) { item = rewrite_generic_type(item, generic_params); }
  for (auto& member : out.union_members) { member = rewrite_generic_type(member, generic_params); }
  for (auto& arg : out.applied_args) { arg = rewrite_generic_type(arg, generic_params); }
  for (auto& param : out.function_params) { param = rewrite_generic_type(param, generic_params); }
  if (out.function_return.has_value()) {
    out.function_return = make_box<Type>(rewrite_generic_type(**out.function_return, generic_params));
  }

  return out;
}

}  // namespace

FunctionIndex::FunctionIndex(const ir::IRProgram& program, const std::unordered_set<std::string>& imported_symbols,
                             const std::vector<ir::IRLet>& imported_typed_lets) {
  symbols_.reserve(program.lets.size() * 2U);
  unqualified_symbols_.reserve(program.lets.size() + imported_symbols.size());
  qualified_symbols_.reserve(imported_symbols.size());
  for (const auto& imported_symbol : imported_symbols) {
    if (imported_symbol.empty()) { continue; }
    if (imported_symbol.find('.') != std::string::npos) {
      qualified_symbols_.insert(imported_symbol);
      continue;
    }
    unqualified_symbols_.insert(imported_symbol);
  }

  auto index_signature = [&](const ir::IRLet& let) -> void {
    FunctionSig sig;
    sig.resolved_symbol_key = let.symbol_key.empty() ? symbol_key(let.qualifier, let.name) : let.symbol_key;
    sig.generic_params = let.generic_params;
    sig.is_builtin = let.is_builtin;

    std::unordered_set<std::string> generic_param_set;
    generic_param_set.reserve(let.generic_params.size());
    for (const auto& generic_param : let.generic_params) { generic_param_set.insert(generic_param); }

    sig.return_type = rewrite_generic_type(from_ir_type(let.return_type), generic_param_set);
    sig.params.reserve(let.params.size());
    for (const auto& param : let.params) {
      sig.params.push_back(ParamSig{
          .name = param.name,
          .type = rewrite_generic_type(from_ir_type(param.type), generic_param_set),
          .variadic = param.type.variadic,
      });
    }

    symbols_[symbol_key(let.qualifier, let.name)].push_back(sig);
    if (!let.qualifier.has_value()) { unqualified_symbols_.insert(let.name); }
  };

  for (const auto& imported_typed_let : imported_typed_lets) {
    if (imported_typed_let.name.empty()) { continue; }
    index_signature(imported_typed_let);
  }

  for (const auto& let : program.lets) { index_signature(let); }
}

auto FunctionIndex::resolve_name(const std::optional<std::string>& qualifier, const std::string& name) const
    -> const FunctionOverloadSet* {
  const auto key = symbol_key(qualifier, name);
  if (const auto it = symbols_.find(key); it != symbols_.end()) { return &it->second; }
  return nullptr;
}

auto FunctionIndex::has_unqualified_symbol(const std::string& name) const -> bool {
  return unqualified_symbols_.contains(name);
}

auto FunctionIndex::has_qualified_symbol(const std::optional<std::string>& qualifier, const std::string& name) const
    -> bool {
  if (!qualifier.has_value()) { return false; }
  return qualified_symbols_.contains(symbol_key(qualifier, name));
}

StrongTypeIndex::StrongTypeIndex(const ir::IRProgram& program, const std::vector<ir::IRTypeDecl>& imported_type_decls) {
  decls_.reserve(imported_type_decls.size() + program.type_decls.size());

  const auto index_decl = [&](const ir::IRTypeDecl& type_decl) -> void {
    decls_.insert_or_assign(type_decl.name,
                            StrongTypeDecl{
                                .name = type_decl.name,
                                .target_type = from_ir_type(type_decl.target),
                                .span = type_decl.span,
                            });
  };

  for (const auto& imported_type_decl : imported_type_decls) { index_decl(imported_type_decl); }
  for (const auto& type_decl : program.type_decls) { index_decl(type_decl); }
}

auto StrongTypeIndex::resolve_name(const std::string& name) const -> const StrongTypeDecl* {
  if (const auto it = decls_.find(name); it != decls_.end()) { return &it->second; }
  return nullptr;
}

auto StrongTypeIndex::has_name(const std::string& name) const -> bool { return decls_.contains(name); }

}  // namespace fleaux::frontend::type_system
