#include "fleaux/frontend/type_system/detail/checker_internal.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "fleaux/frontend/type_system/builtin_contracts.hpp"
#include "fleaux/frontend/type_system/detail/type_format.hpp"

namespace fleaux::frontend::type_system::detail {

auto collect_type_vars(const Type& type, std::unordered_set<std::string>& out) -> void {
  if (type.kind == TypeKind::kTypeVar) {
    out.insert(type.nominal_name);
    return;
  }

  for (const auto& item : type.items) {
    collect_type_vars(item, out);
  }
  for (const auto& member : type.union_members) {
    collect_type_vars(member, out);
  }
  for (const auto& arg : type.applied_args) {
    collect_type_vars(arg, out);
  }
  for (const auto& param : type.function_params) {
    collect_type_vars(param, out);
  }
  if (type.function_return.has_value()) {
    collect_type_vars(*type.function_return, out);
  }
}

auto is_type_var_resolved(const std::string& type_var, const TypeBindings& bindings,
                          const GenericScope& allowed_unbound,
                          std::unordered_set<std::string>& visiting,
                          std::unordered_map<std::string, bool>& resolved_cache) -> bool {
  if (allowed_unbound.contains(type_var)) {
    return true;
  }

  if (const auto cached = resolved_cache.find(type_var); cached != resolved_cache.end()) {
    return cached->second;
  }

  if (visiting.contains(type_var)) {
    resolved_cache.insert_or_assign(type_var, false);
    return false;
  }

  const auto binding_it = bindings.find(type_var);
  if (binding_it == bindings.end()) {
    resolved_cache.insert_or_assign(type_var, false);
    return false;
  }

  visiting.insert(type_var);
  const bool resolved = is_type_resolved(binding_it->second, bindings, allowed_unbound, visiting, resolved_cache);
  visiting.erase(type_var);
  resolved_cache.insert_or_assign(type_var, resolved);
  return resolved;
}

auto is_type_resolved(const Type& type, const TypeBindings& bindings,
                      const GenericScope& allowed_unbound, std::unordered_set<std::string>& visiting,
                      std::unordered_map<std::string, bool>& resolved_cache) -> bool {
  if (type.kind == TypeKind::kTypeVar) {
    return is_type_var_resolved(type.nominal_name, bindings, allowed_unbound, visiting, resolved_cache);
  }

  for (const auto& item : type.items) {
    if (!is_type_resolved(item, bindings, allowed_unbound, visiting, resolved_cache)) {
      return false;
    }
  }
  for (const auto& member : type.union_members) {
    if (!is_type_resolved(member, bindings, allowed_unbound, visiting, resolved_cache)) {
      return false;
    }
  }
  for (const auto& arg : type.applied_args) {
    if (!is_type_resolved(arg, bindings, allowed_unbound, visiting, resolved_cache)) {
      return false;
    }
  }
  for (const auto& param : type.function_params) {
    if (!is_type_resolved(param, bindings, allowed_unbound, visiting, resolved_cache)) {
      return false;
    }
  }
  if (type.function_return.has_value() &&
      !is_type_resolved(*type.function_return, bindings, allowed_unbound, visiting, resolved_cache)) {
    return false;
  }
  return true;
}

auto collect_unresolved_return_type_vars(const Type& return_type, const TypeBindings& bindings,
                                         const GenericScope& allowed_unbound,
                                         std::unordered_set<std::string>& out) -> void {
  std::unordered_set<std::string> return_type_vars;
  collect_type_vars(return_type, return_type_vars);

  std::unordered_set<std::string> visiting;
  std::unordered_map<std::string, bool> resolved_cache;
  for (const auto& type_var : return_type_vars) {
    if (!is_type_var_resolved(type_var, bindings, allowed_unbound, visiting, resolved_cache)) {
      out.insert(type_var);
    }
  }
}

auto collect_unbound_type_vars(const Type& type, const TypeBindings& bindings, std::unordered_set<std::string>& out)
    -> void {
  if (type.kind == TypeKind::kTypeVar) {
    const auto binding_it = bindings.find(type.nominal_name);
    if (binding_it == bindings.end()) {
      out.insert(type.nominal_name);
      return;
    }

    if (binding_it->second.kind == TypeKind::kTypeVar && binding_it->second.nominal_name == type.nominal_name) {
      out.insert(type.nominal_name);
    }
    return;
  }

  for (const auto& item : type.items) {
    collect_unbound_type_vars(item, bindings, out);
  }
  for (const auto& member : type.union_members) {
    collect_unbound_type_vars(member, bindings, out);
  }
  for (const auto& arg : type.applied_args) {
    collect_unbound_type_vars(arg, bindings, out);
  }
  for (const auto& param : type.function_params) {
    collect_unbound_type_vars(param, bindings, out);
  }
  if (type.function_return.has_value()) {
    collect_unbound_type_vars(*type.function_return, bindings, out);
  }
}

auto join_sorted_type_var_names(const std::unordered_set<std::string>& names) -> std::string {
  std::vector<std::string> ordered(names.begin(), names.end());
  std::ranges::sort(ordered);

  std::string joined;
  append_joined_fragments(joined, ordered, ", ", [](const std::string& name) -> const std::string& { return name; });
  return joined;
}

auto generic_var_suffix_for_type(const Type& type) -> std::string {
  const TypeBindings empty_bindings;
  std::unordered_set<std::string> vars;
  collect_unbound_type_vars(type, empty_bindings, vars);
  if (vars.empty()) {
    return {};
  }
  return std::format(" for type variable(s): {}.", join_sorted_type_var_names(vars));
}

auto type_debug_name(const Type& type) -> std::string {
  switch (type.kind) {
    case TypeKind::kUnknown:
      return "Unknown";
    case TypeKind::kNever:
      return "Never";
    case TypeKind::kAny:
      return "Any";
    case TypeKind::kInt64:
      return "Int64";
    case TypeKind::kUInt64:
      return "UInt64";
    case TypeKind::kFloat64:
      return "Float64";
    case TypeKind::kString:
      return "String";
    case TypeKind::kBool:
      return "Bool";
    case TypeKind::kNull:
      return "Null";
    case TypeKind::kNominal:
    case TypeKind::kTypeVar:
      return type.nominal_name;
    case TypeKind::kTuple: {
      std::string out = "Tuple<";
      append_joined_type_fragments(
          out, type.items, ", ", [](const Type& item) { return type_debug_name(item); },
          [](const Type& item) { return item.variadic; });
      out += ">";
      return out;
    }
    case TypeKind::kUnion: {
      std::string out;
      append_joined_fragments(out, type.union_members, " | ",
                              [](const Type& member) { return type_debug_name(member); });
      return out;
    }
    case TypeKind::kApplied: {
      std::string out = type.applied_name + "<";
      append_joined_fragments(out, type.applied_args, ", ", [](const Type& arg) { return type_debug_name(arg); });
      out += ">";
      return out;
    }
    case TypeKind::kFunction: {
      std::string out = "(";
      append_joined_type_fragments(
          out, type.function_params, ", ", [](const Type& param) { return type_debug_name(param); },
          [](const Type& param) { return param.variadic; });
      out += ") => ";
      out += type.function_return.has_value() ? type_debug_name(*type.function_return) : "Any";
      return out;
    }
    default:
      return "Unknown";
  }
}

auto generic_binding_detail_for_type(const Type& expected_type, const TypeBindings& bindings, const Type& actual)
    -> std::string {
  const TypeBindings empty_bindings;
  std::unordered_set<std::string> vars;
  collect_unbound_type_vars(expected_type, empty_bindings, vars);
  if (vars.empty()) {
    return {};
  }

  std::vector<std::string> ordered(vars.begin(), vars.end());
  std::ranges::sort(ordered);

  std::vector<std::string> details;
  details.reserve(ordered.size());
  for (const auto& var : ordered) {
    if (const auto it = bindings.find(var); it != bindings.end()) {
      details.push_back(std::format("{} bound as {}", var, type_debug_name(it->second)));
    }
  }
  if (details.empty()) {
    return {};
  }

  std::string joined;
  append_joined_fragments(joined, details, "; ",
                          [](const std::string& detail) -> const std::string& { return detail; });
  return std::format("Binding detail: {}. got {}.", joined, type_debug_name(actual));
}

auto is_numeric_scalar_type(const Type& type) -> bool {
  return type.kind == TypeKind::kInt64 || type.kind == TypeKind::kUInt64 || type.kind == TypeKind::kFloat64;
}

// Explains a failed binding where one type parameter received two different numeric types, such as (1, 2.0).
auto numeric_conversion_note(const Type& expected_type, const TypeBindings& bindings, const Type& actual)
    -> std::string {
  if (expected_type.kind != TypeKind::kTypeVar || !is_numeric_scalar_type(actual)) {
    return {};
  }
  const auto binding = bindings.find(expected_type.nominal_name);
  if (binding == bindings.end() || !is_numeric_scalar_type(binding->second) || binding->second.kind == actual.kind) {
    return {};
  }
  return " Numbers are not converted implicitly; use Std.ToInt64, Std.ToUInt64, or Std.ToFloat64.";
}

auto generic_arg_mismatch_hint(const std::string& full_name, const std::size_t arg_index, const Type& expected_type,
                               const bool variadic, const TypeBindings& bindings, const Type& actual) -> std::string {
  const auto var_suffix = generic_var_suffix_for_type(expected_type);
  if (variadic) {
    std::string hint =
        std::format("{} expects variadic argument {} to match declared type{}", full_name, arg_index, var_suffix);
    if (const auto detail = generic_binding_detail_for_type(expected_type, bindings, actual); !detail.empty()) {
      hint += " " + detail;
    }
    return hint + numeric_conversion_note(expected_type, bindings, actual);
  }
  std::string hint = std::format("{} expects argument {} to match declared type{}", full_name, arg_index, var_suffix);
  if (const auto detail = generic_binding_detail_for_type(expected_type, bindings, actual); !detail.empty()) {
    hint += " " + detail;
  }
  return hint + numeric_conversion_note(expected_type, bindings, actual);
}

[[nodiscard]] auto make_error(const std::string& message, const std::optional<std::string>& hint,
                              const std::optional<diag::SourceSpan>& span) -> type_check::AnalysisError {
  return type_check::AnalysisError{
      .message = message,
      .hint = hint,
      .span = span,
  };
}

[[nodiscard]] auto make_unresolved_symbol_error(const std::string& symbol, const std::optional<diag::SourceSpan>& span)
    -> type_check::AnalysisError {
  return make_error("Unresolved symbol.", std::format("Could not resolve '{}'.", symbol), span);
}

auto expand_aliases_in_type_impl(const Type& type, const KnownTypes& known_strong_type_names,
                                 const AliasIndex& alias_index, const GenericScope& generic_params,
                                 const std::optional<diag::SourceSpan>& span, std::unordered_set<std::string>& visiting,
                                 std::unordered_map<std::string, Type>& resolved_cache)
    -> tl::expected<Type, type_check::AnalysisError>;

auto expand_alias_name_impl(const std::string& name, const KnownTypes& known_strong_type_names,
                            const AliasIndex& alias_index, const GenericScope& generic_params,
                            const std::optional<diag::SourceSpan>& span, std::unordered_set<std::string>& visiting,
                            std::unordered_map<std::string, Type>& resolved_cache)
    -> tl::expected<Type, type_check::AnalysisError> {
  if (generic_params.contains(name) || known_strong_type_names.contains(name)) {
    return make_nominal_type(name);
  }

  const auto alias_decl = alias_index.resolve_name(name);
  if (!alias_decl.has_value()) {
    return make_nominal_type(name);
  }

  if (const auto cached = resolved_cache.find(name); cached != resolved_cache.end()) {
    return cached->second;
  }

  if (visiting.contains(name)) {
    auto cycle_names = visiting;
    cycle_names.insert(name);
    return tl::unexpected(make_error(
        "Alias cycle detected.",
        std::format("Transparent aliases form a declaration cycle: {}.", join_sorted_type_var_names(cycle_names)),
        alias_decl->get().span.has_value() ? alias_decl->get().span : span));
  }

  visiting.insert(name);
  auto expanded = expand_aliases_in_type_impl(alias_decl->get().target_type, known_strong_type_names, alias_index,
                                              generic_params,
                                              alias_decl->get().span.has_value() ? alias_decl->get().span : span,
                                              visiting, resolved_cache);
  visiting.erase(name);
  if (!expanded) {
    return tl::unexpected(expanded.error());
  }

  resolved_cache.insert_or_assign(name, *expanded);
  return *expanded;
}

auto expand_aliases_in_type_impl(const Type& type, const KnownTypes& known_strong_type_names,
                                 const AliasIndex& alias_index, const GenericScope& generic_params,
                                 const std::optional<diag::SourceSpan>& span, std::unordered_set<std::string>& visiting,
                                 std::unordered_map<std::string, Type>& resolved_cache)
    -> tl::expected<Type, type_check::AnalysisError> {
  if (type.kind == TypeKind::kNominal) {
    return expand_alias_name_impl(type.nominal_name, known_strong_type_names, alias_index, generic_params, span,
                                  visiting, resolved_cache);
  }

  Type expanded = type;
  for (auto& item : expanded.items) {
    auto resolved_item = expand_aliases_in_type_impl(item, known_strong_type_names, alias_index, generic_params, span,
                                                     visiting, resolved_cache);
    if (!resolved_item) {
      return tl::unexpected(resolved_item.error());
    }
    const bool variadic = item.variadic;
    item = *resolved_item;
    item.variadic = variadic;
  }
  for (auto& member : expanded.union_members) {
    auto resolved_member = expand_aliases_in_type_impl(member, known_strong_type_names, alias_index, generic_params,
                                                       span, visiting, resolved_cache);
    if (!resolved_member) {
      return tl::unexpected(resolved_member.error());
    }
    member = *resolved_member;
  }
  for (auto& arg : expanded.applied_args) {
    auto resolved_arg = expand_aliases_in_type_impl(arg, known_strong_type_names, alias_index, generic_params, span,
                                                    visiting, resolved_cache);
    if (!resolved_arg) {
      return tl::unexpected(resolved_arg.error());
    }
    arg = *resolved_arg;
  }
  for (auto& param : expanded.function_params) {
    auto resolved_param = expand_aliases_in_type_impl(param, known_strong_type_names, alias_index, generic_params, span,
                                                      visiting, resolved_cache);
    if (!resolved_param) {
      return tl::unexpected(resolved_param.error());
    }
    const bool variadic = param.variadic;
    param = *resolved_param;
    param.variadic = variadic;
  }
  if (expanded.function_return.has_value()) {
    auto resolved_return = expand_aliases_in_type_impl(*expanded.function_return, known_strong_type_names, alias_index,
                                                       generic_params, span, visiting, resolved_cache);
    if (!resolved_return) {
      return tl::unexpected(resolved_return.error());
    }
    expanded.function_return = common::make_indirect_optional<Type>(*resolved_return);
  }

  return normalize_type(std::move(expanded));
}

auto validate_declared_type_against_names(const Type& type, const KnownTypes& known_strong_type_names,
                                          const AliasIndex& alias_index,
                                          const GenericScope& generic_params,
                                          const std::optional<diag::SourceSpan>& span)
    -> tl::expected<void, type_check::AnalysisError> {
  auto expanded = expand_aliases_in_type(type, known_strong_type_names, alias_index, generic_params, span);
  if (!expanded) {
    return tl::unexpected(expanded.error());
  }

  switch (expanded->kind) {
    case TypeKind::kNominal: {
      if (generic_params.contains(expanded->nominal_name)) {
        return {};
      }
      if (const auto known = known_strong_type_names.find(expanded->nominal_name);
          known != known_strong_type_names.end()) {
        if (known->second == 0U) {
          return {};
        }
        return tl::unexpected(make_error(
            "Missing type arguments.",
            std::format("Generic type '{}' takes {} type argument{}. Write '{}<...>'.", expanded->nominal_name,
                        known->second, known->second == 1U ? "" : "s", expanded->nominal_name),
            span));
      }
      return tl::unexpected(
          make_error("Unknown type.", std::format("Could not resolve type '{}'.", expanded->nominal_name), span));
    }
    case TypeKind::kTuple:
      for (const auto& item : expanded->items) {
        if (auto validated =
                validate_declared_type_against_names(item, known_strong_type_names, alias_index, generic_params, span);
            !validated) {
          return tl::unexpected(validated.error());
        }
      }
      return {};
    case TypeKind::kUnion:
      for (const auto& member : expanded->union_members) {
        if (auto validated = validate_declared_type_against_names(member, known_strong_type_names, alias_index,
                                                                  generic_params, span);
            !validated) {
          return tl::unexpected(validated.error());
        }
      }
      return {};
    case TypeKind::kApplied: {
      const auto known = known_strong_type_names.find(expanded->applied_name);
      if (known == known_strong_type_names.end()) {
        return tl::unexpected(
            make_error("Unknown type.", std::format("Could not resolve type '{}'.", expanded->applied_name), span));
      }
      if (known->second == 0U) {
        return tl::unexpected(make_error(
            "Unexpected type arguments.",
            std::format("Type '{}' is not generic and does not take type arguments.", expanded->applied_name), span));
      }
      if (known->second != expanded->applied_args.size()) {
        return tl::unexpected(make_error(
            "Wrong number of type arguments.",
            std::format("Generic type '{}' takes {} type argument{} but got {}.", expanded->applied_name, known->second,
                        known->second == 1U ? "" : "s", expanded->applied_args.size()),
            span));
      }
      for (const auto& arg : expanded->applied_args) {
        if (auto validated =
                validate_declared_type_against_names(arg, known_strong_type_names, alias_index, generic_params, span);
            !validated) {
          return tl::unexpected(validated.error());
        }
      }
      return {};
    }
    case TypeKind::kFunction:
      for (const auto& param : expanded->function_params) {
        if (auto validated =
                validate_declared_type_against_names(param, known_strong_type_names, alias_index, generic_params, span);
            !validated) {
          return tl::unexpected(validated.error());
        }
      }
      if (expanded->function_return.has_value()) {
        return validate_declared_type_against_names(*expanded->function_return, known_strong_type_names, alias_index,
                                                    generic_params, span);
      }
      return {};
    default:
      return {};
  }
}

[[nodiscard]] auto qualified_symbol_name(const std::optional<std::string>& qualifier, const std::string& name)
    -> std::string {
  return qualifier.has_value() ? (*qualifier + "." + name) : name;
}

[[nodiscard]] auto is_removed_symbolic_alias(const std::optional<std::string>& qualifier, const std::string& name)
    -> bool {
  return qualifier.has_value() && *qualifier == "Std" && name == "TypeOf";
}

[[nodiscard]] auto resolve_name_or_symbolic_builtin(const FunctionIndex& index,
                                                    const std::optional<std::string>& qualifier,
                                                    const std::string& name)
    -> std::optional<std::reference_wrapper<const FunctionOverloadSet>> {
  return index.resolve_name(qualifier, name);
}

[[nodiscard]] auto target_name(const ir::IRCallTarget& target) -> std::optional<std::string> {
  if (const auto* name_ref = std::get_if<ir::IRNameRef>(&target); name_ref != nullptr) {
    return name_ref->qualifier.has_value() ? (*name_ref->qualifier + "." + name_ref->name) : name_ref->name;
  }
  return std::nullopt;
}

[[nodiscard]] auto resolve_signature(const FunctionIndex& index, const ir::IRCallTarget& target)
    -> std::optional<std::reference_wrapper<const FunctionOverloadSet>> {
  if (const auto* name_ref = std::get_if<ir::IRNameRef>(&target); name_ref != nullptr) {
    return resolve_name_or_symbolic_builtin(index, name_ref->qualifier, name_ref->name);
  }
  return std::nullopt;
}

[[nodiscard]] auto function_type_from_sig(const FunctionSig& sig) -> Type {
  Type out;
  out.kind = TypeKind::kFunction;
  out.function_params.reserve(sig.params.size());
  for (const auto& param : sig.params) {
    Type param_type = param.type;
    param_type.variadic = param.variadic;
    out.function_params.push_back(std::move(param_type));
  }
  out.function_return = common::make_indirect_optional<Type>(sig.return_type);
  return out;
}


auto collect_known_strong_type_names(const ir::IRProgram& program,
                                     const std::vector<ir::IRTypeDecl>& imported_type_decls) -> KnownTypes {
  KnownTypes names;
  names.reserve(imported_type_decls.size() + program.type_decls.size());
  for (const auto& imported_type_decl : imported_type_decls) {
    names.insert_or_assign(imported_type_decl.name, imported_type_decl.generic_params.size());
  }
  for (const auto& type_decl : program.type_decls) {
    names.insert_or_assign(type_decl.name, type_decl.generic_params.size());
  }
  return names;
}

auto resolve_explicit_type_args(const std::vector<ir::IRSimpleType>& explicit_type_args,
                                const StrongTypeIndex& type_index, const AliasIndex& alias_index,
                                const GenericScope& generic_params,
                                const std::optional<diag::SourceSpan>& span)
    -> tl::expected<std::vector<Type>, type_check::AnalysisError> {
  std::vector<Type> resolved;
  resolved.reserve(explicit_type_args.size());
  for (const auto& explicit_type_arg : explicit_type_args) {
    const Type type_arg = rewrite_generic_type(from_ir_type(explicit_type_arg), generic_params);
    if (auto validated = validate_declared_type(type_arg, type_index, alias_index, generic_params, span); !validated) {
      return tl::unexpected(validated.error());
    }
    auto expanded = expand_aliases_in_type(type_arg, type_index, alias_index, generic_params, span);
    if (!expanded) {
      return tl::unexpected(expanded.error());
    }
    resolved.push_back(*expanded);
  }
  return resolved;
}

auto explicit_type_arg_bindings_for_sig(const std::string& full_name, const std::optional<diag::SourceSpan>& span,
                                        const FunctionSig& sig, const std::vector<Type>& explicit_type_args)
    -> tl::expected<TypeBindings, type_check::AnalysisError> {
  if (explicit_type_args.empty()) {
    return TypeBindings{};
  }

  if (sig.generic_params.empty()) {
    return tl::unexpected(
        make_error("Invalid explicit type argument application.",
                   std::format("{} is not generic and does not accept explicit type arguments.", full_name), span));
  }

  if (sig.generic_params.size() != explicit_type_args.size()) {
    return tl::unexpected(make_error("Invalid explicit type argument application.",
                                     std::format("{} expects {} explicit type argument(s) but got {}.", full_name,
                                                 sig.generic_params.size(), explicit_type_args.size()),
                                     span));
  }

  TypeBindings bindings;
  bindings.reserve(sig.generic_params.size());
  for (std::size_t index = 0; index < sig.generic_params.size(); ++index) {
    bindings.insert_or_assign(sig.generic_params[index], explicit_type_args[index]);
  }
  return bindings;
}

auto explicit_type_arg_arities(const FunctionOverloadSet& overloads) -> std::vector<std::size_t> {
  std::vector<std::size_t> arities;
  arities.reserve(overloads.size());
  for (const auto& overload : overloads) {
    arities.push_back(overload.generic_params.size());
  }

  std::ranges::sort(arities);
  arities.erase(std::ranges::unique(arities).begin(), arities.end());

  return arities;
}

auto explicit_type_arg_arity_summary(const FunctionOverloadSet& overloads) -> std::string {
  auto arities = explicit_type_arg_arities(overloads);

  if (arities.size() == 1U) {
    return std::format("expects {} explicit type argument(s)", arities.front());
  }

  std::string joined;
  append_joined_fragments(joined, arities, ", ", [](const std::size_t arity) { return std::to_string(arity); });
  return std::format("available explicit type argument arities are {}", joined);
}

auto explicit_type_arg_zero_and_generic_arity(const FunctionOverloadSet& overloads) -> std::optional<std::size_t> {
  if (const auto arities = explicit_type_arg_arities(overloads); arities.size() == 2U && arities.front() == 0U) {
    return arities.back();
  }
  return std::nullopt;
}

auto filter_overloads_for_explicit_type_args(const std::string& full_name, const std::optional<diag::SourceSpan>& span,
                                             const FunctionOverloadSet& overloads,
                                             const std::vector<Type>& explicit_type_args)
    -> tl::expected<std::vector<const FunctionSig*>, type_check::AnalysisError> {
  std::vector<const FunctionSig*> filtered;
  filtered.reserve(overloads.size());

  if (explicit_type_args.empty()) {
    for (const auto& overload : overloads) {
      filtered.push_back(&overload);
    }
    return filtered;
  }

  for (const auto& overload : overloads) {
    if (overload.generic_params.size() == explicit_type_args.size()) {
      filtered.push_back(&overload);
    }
  }
  if (!filtered.empty()) {
    return filtered;
  }

  const bool has_generic_overload = std::ranges::any_of(
      overloads, [](const FunctionSig& overload) -> bool { return !overload.generic_params.empty(); });
  if (!has_generic_overload) {
    return tl::unexpected(
        make_error("Invalid explicit type argument application.",
                   std::format("{} is not generic and does not accept explicit type arguments.", full_name), span));
  }

  if (overloads.size() == 1U) {
    return tl::unexpected(make_error("Invalid explicit type argument application.",
                                     std::format("{} expects {} explicit type argument(s) but got {}.", full_name,
                                                 overloads.front().generic_params.size(), explicit_type_args.size()),
                                     span));
  }

  if (const auto arities = explicit_type_arg_arities(overloads); arities.size() == 1U) {
    return tl::unexpected(make_error("Invalid explicit type argument application.",
                                     std::format("{} expects {} explicit type argument(s) but got {}.", full_name,
                                                 arities.front(), explicit_type_args.size()),
                                     span));
  }

  if (const auto generic_arity = explicit_type_arg_zero_and_generic_arity(overloads); generic_arity.has_value()) {
    return tl::unexpected(make_error(
        "Invalid explicit type argument application.",
        std::format("No overload of {} accepts {} explicit type argument(s); generic overloads expect {} explicit type "
                    "argument(s), and non-generic overloads accept none.",
                    full_name, explicit_type_args.size(), *generic_arity),
        span));
  }

  return tl::unexpected(make_error("Invalid explicit type argument application.",
                                   std::format("No overload of {} accepts {} explicit type argument(s); {}.", full_name,
                                               explicit_type_args.size(), explicit_type_arg_arity_summary(overloads)),
                                   span));
}

auto declaration_symbol_key(const ir::IRLet& let) -> std::string {
  return let.qualifier.has_value() ? (*let.qualifier + "." + let.name) : let.name;
}

auto format_signature_debug_name(const std::string& full_name, const FunctionSig& sig) -> std::string {
  std::string out = full_name + "(";
  append_joined_type_fragments(
      out, sig.params, ", ", [](const ParamSig& param) { return type_debug_name(param.type); },
      [](const ParamSig& param) { return param.variadic; });
  out += "): ";
  out += type_debug_name(sig.return_type);
  return out;
}

auto overload_candidate_list(const std::string& full_name, const FunctionOverloadSet& overloads) -> std::string {
  std::string out;
  append_joined_fragments(out, overloads, "; ", [&](const FunctionSig& overload) {
    return format_signature_debug_name(full_name, overload);
  });
  return out;
}

auto overload_candidate_list(const std::string& full_name, const std::vector<const FunctionSig*>& overloads)
    -> std::string {
  std::string out;
  append_joined_fragments(out, overloads, "; ", [&](const FunctionSig* overload) {
    return format_signature_debug_name(full_name, *overload);
  });
  return out;
}

auto call_shape_matches(const FunctionSig& sig, const std::size_t arg_count) -> bool {
  if (!sig.params.empty() && sig.params.back().variadic) {
    return arg_count + 1U >= sig.params.size();
  }
  return sig.params.size() == arg_count;
}

auto overload_shapes_overlap(const FunctionSig& lhs, const FunctionSig& rhs) -> bool {
  const bool lhs_variadic = !lhs.params.empty() && lhs.params.back().variadic;
  const bool rhs_variadic = !rhs.params.empty() && rhs.params.back().variadic;

  if (!lhs_variadic && !rhs_variadic) {
    return lhs.params.size() == rhs.params.size();
  }
  if (lhs_variadic && rhs_variadic) {
    return true;
  }

  const auto& variadic_sig = lhs_variadic ? lhs : rhs;
  const auto& fixed_sig = lhs_variadic ? rhs : lhs;
  const std::size_t variadic_fixed_count = variadic_sig.params.size() - 1U;
  return fixed_sig.params.size() >= variadic_fixed_count;
}

auto function_sig_from_let(const ir::IRLet& let) -> FunctionSig {
  FunctionSig sig;
  sig.resolved_symbol_key = let.symbol_key.empty() ? declaration_symbol_key(let) : let.symbol_key;
  sig.generic_params = let.generic_params;
  sig.is_builtin = let.is_builtin;
  sig.return_type = from_ir_type(let.return_type);
  sig.params.reserve(let.params.size());
  for (const auto& param : let.params) {
    sig.params.push_back(ParamSig{
        .name = param.name,
        .type = from_ir_type(param.type),
        .variadic = param.type.variadic,
    });
  }
  return sig;
}

auto validate_alias_declarations(const ir::IRProgram& program, const std::vector<ir::IRTypeDecl>& imported_type_decls,
                                 const std::vector<ir::IRAliasDecl>& imported_alias_decls)
    -> tl::expected<AliasIndex, type_check::AnalysisError> {
  const auto known_strong_type_names = collect_known_strong_type_names(program, imported_type_decls);

  std::unordered_map<std::string, std::optional<diag::SourceSpan>> seen_aliases;
  seen_aliases.reserve(imported_alias_decls.size() + program.alias_decls.size());
  const auto validate_alias_name = [&](const ir::IRAliasDecl& alias_decl) -> std::optional<type_check::AnalysisError> {
    if (known_strong_type_names.contains(alias_decl.name)) {
      return make_error("Duplicate alias declaration.",
                        std::format("Alias '{}' conflicts with an existing strong type declaration.", alias_decl.name),
                        alias_decl.span);
    }

    if (const auto it = seen_aliases.find(alias_decl.name); it != seen_aliases.end()) {
      std::string hint = std::format("Alias '{}' is already declared", alias_decl.name);
      if (it->second.has_value() && !it->second->source_name.empty()) {
        hint += std::format(" in '{}'.", it->second->source_name);
      } else {
        hint += ".";
      }
      return make_error("Duplicate alias declaration.", hint, alias_decl.span);
    }

    seen_aliases.insert_or_assign(alias_decl.name, alias_decl.span);
    return std::nullopt;
  };

  for (const auto& imported_alias_decl : imported_alias_decls) {
    if (auto error = validate_alias_name(imported_alias_decl); error.has_value()) {
      return tl::unexpected(std::move(*error));
    }
  }
  for (const auto& alias_decl : program.alias_decls) {
    if (auto error = validate_alias_name(alias_decl); error.has_value()) {
      return tl::unexpected(std::move(*error));
    }
  }

  AliasIndex alias_index(program, imported_alias_decls);
  const GenericScope no_generic_params;
  for (const auto& imported_alias_decl : imported_alias_decls) {
    if (auto validated = validate_declared_type_against_names(
            alias_index.resolve_name(imported_alias_decl.name)->get().target_type, known_strong_type_names, alias_index,
            no_generic_params, imported_alias_decl.span);
        !validated) {
      return tl::unexpected(validated.error());
    }
  }
  for (const auto& alias_decl : program.alias_decls) {
    if (auto validated = validate_declared_type_against_names(alias_index.resolve_name(alias_decl.name)->get().target_type,
                                                              known_strong_type_names, alias_index,
                                                              no_generic_params, alias_decl.span);
        !validated) {
      return tl::unexpected(validated.error());
    }
  }

  return alias_index;
}

auto validate_supported_overload_sets(const ir::IRProgram& program, const std::vector<ir::IRLet>& imported_typed_lets)
    -> tl::expected<void, type_check::AnalysisError> {
  std::unordered_map<std::string, std::vector<const ir::IRLet*>> builtin_overload_sets;
  const auto collect_builtin_overloads = [&](const auto& lets) -> void {
    for (const auto& let : lets) {
      if (!let.is_builtin) {
        continue;
      }
      builtin_overload_sets[declaration_symbol_key(let)].push_back(&let);
    }
  };

  collect_builtin_overloads(imported_typed_lets);
  collect_builtin_overloads(program.lets);

  for (const auto& [full_name, lets] : builtin_overload_sets) {
    if (lets.size() <= 1U) {
      continue;
    }

    FunctionOverloadSet overloads;
    overloads.reserve(lets.size());
    for (const auto* let : lets) {
      overloads.push_back(function_sig_from_let(*let));
    }

    for (std::size_t lhs_index = 0; lhs_index < overloads.size(); ++lhs_index) {
      for (std::size_t rhs_index = lhs_index + 1U; rhs_index < overloads.size(); ++rhs_index) {
        if (!overload_shapes_overlap(overloads[lhs_index], overloads[rhs_index])) {
          continue;
        }
        return tl::unexpected(make_error(
            "Unsupported builtin overload set.",
            std::format("{} builtin overloads must differ by call shape because VM builtin dispatch does not "
                        "resolve by argument types. Candidates: {}.",
                        full_name, overload_candidate_list(full_name, overloads)),
            lets[lhs_index]->span));
      }
    }
  }

  return {};
}

auto validate_strong_type_declarations(const ir::IRProgram& program,
                                       const std::vector<ir::IRTypeDecl>& imported_type_decls,
                                       const AliasIndex& alias_index) -> tl::expected<void, type_check::AnalysisError> {
  std::unordered_map<std::string, std::optional<diag::SourceSpan>> seen;
  seen.reserve(imported_type_decls.size() + program.type_decls.size());

  const auto validate_decl_name = [&](const ir::IRTypeDecl& type_decl) -> std::optional<type_check::AnalysisError> {
    if (const auto it = seen.find(type_decl.name); it != seen.end()) {
      std::string hint = std::format("Strong type '{}' is already declared", type_decl.name);
      if (it->second.has_value() && !it->second->source_name.empty()) {
        hint += std::format(" in '{}'.", it->second->source_name);
      } else {
        hint += ".";
      }
      return make_error("Duplicate strong type declaration.", hint, type_decl.span);
    }

    seen.insert_or_assign(type_decl.name, type_decl.span);
    return std::nullopt;
  };

  for (const auto& imported_type_decl : imported_type_decls) {
    if (auto error = validate_decl_name(imported_type_decl); error.has_value()) {
      return tl::unexpected(std::move(*error));
    }
  }
  for (const auto& type_decl : program.type_decls) {
    if (auto error = validate_decl_name(type_decl); error.has_value()) {
      return tl::unexpected(std::move(*error));
    }
  }

  const auto known_strong_type_names = collect_known_strong_type_names(program, imported_type_decls);
  for (const auto& type_decl : program.type_decls) {
    std::unordered_set<std::string> seen_generic_params;
    GenericScope generic_param_set;
    for (const auto& generic_param : type_decl.generic_params) {
      if (known_strong_type_names.contains(generic_param) || alias_index.has_name(generic_param)) {
        return tl::unexpected(make_error(
            "Invalid generic parameter.",
            std::format("Generic parameter '{}' of type '{}' shadows a declared type.", generic_param, type_decl.name),
            type_decl.span));
      }
      generic_param_set.add(generic_param);
      if (!seen_generic_params.insert(generic_param).second) {
        return tl::unexpected(make_error(
            "Duplicate generic parameter.",
            std::format("Type '{}' declares generic parameter '{}' more than once.", type_decl.name, generic_param),
            type_decl.span));
      }
    }

    if (type_decl.is_builtin) {
      continue;
    }

    if (auto validated = validate_declared_type_against_names(from_ir_type(type_decl.target), known_strong_type_names,
                                                              alias_index, generic_param_set, type_decl.span);
        !validated) {
      return validated;
    }
  }

  return {};
}

auto expand_aliases_in_type(const Type& type, const KnownTypes& known_strong_type_names, const AliasIndex& alias_index,
                            const GenericScope& generic_params,
                            const std::optional<diag::SourceSpan>& span)
    -> tl::expected<Type, type_check::AnalysisError> {
  std::unordered_set<std::string> visiting;
  std::unordered_map<std::string, Type> resolved_cache;
  return expand_aliases_in_type_impl(type, known_strong_type_names, alias_index, generic_params, span, visiting,
                                     resolved_cache);
}

auto expand_aliases_in_type(const Type& type, const StrongTypeIndex& type_index, const AliasIndex& alias_index,
                            const GenericScope& generic_params,
                            const std::optional<diag::SourceSpan>& span)
    -> tl::expected<Type, type_check::AnalysisError> {
  return expand_aliases_in_type(type, type_index.known_names(), alias_index, generic_params, span);
}

auto validate_declared_type(const Type& type, const StrongTypeIndex& type_index, const AliasIndex& alias_index,
                            const GenericScope& generic_params,
                            const std::optional<diag::SourceSpan>& span)
    -> tl::expected<void, type_check::AnalysisError> {
  return validate_declared_type_against_names(type, type_index.known_names(), alias_index, generic_params, span);
}

auto make_generic_scope(const std::vector<std::string>& names, const std::vector<ir::IRSimpleTypeBox>& bounds,
                        const KnownTypes& known_types, const AliasIndex& alias_index,
                        const std::optional<diag::SourceSpan>& span, const GenericScope& enclosing)
    -> tl::expected<GenericScope, type_check::AnalysisError> {
  GenericScope scope = enclosing;
  const GenericScope no_generic_params;
  for (std::size_t index = 0; index < names.size(); ++index) {
    if (index >= bounds.size() || !bounds[index].has_value()) {
      scope.add(names[index]);
      continue;
    }

    // Bounds are closed types: they may name declared types and aliases but not other generic parameters.
    const Type declared_bound = from_ir_type(*bounds[index]);
    if (auto validated =
            validate_declared_type_against_names(declared_bound, known_types, alias_index, no_generic_params, span);
        !validated) {
      auto error = validated.error();
      error.hint = std::format("In the bound of generic parameter '{}': {}", names[index], error.hint.value_or(""));
      return tl::unexpected(std::move(error));
    }
    auto expanded_bound = expand_aliases_in_type(declared_bound, known_types, alias_index, no_generic_params, span);
    if (!expanded_bound) {
      return tl::unexpected(expanded_bound.error());
    }
    scope.add(names[index], normalize_type(*expanded_bound));
  }
  return scope;
}

auto make_generic_scope(const std::vector<std::string>& names, const std::vector<ir::IRSimpleTypeBox>& bounds,
                        const StrongTypeIndex& type_index, const AliasIndex& alias_index,
                        const std::optional<diag::SourceSpan>& span, const GenericScope& enclosing)
    -> tl::expected<GenericScope, type_check::AnalysisError> {
  return make_generic_scope(names, bounds, type_index.known_names(), alias_index, span, enclosing);
}

auto rewrite_generic_type(const Type& type, const GenericScope& generic_params) -> Type {
  Type out = type;

  if (out.kind == TypeKind::kNominal && generic_params.contains(out.nominal_name)) {
    out.kind = TypeKind::kTypeVar;
    if (const Type* bound = generic_params.bound_of(out.nominal_name); bound != nullptr) {
      out.type_var_bound = common::make_indirect_optional<Type>(*bound);
    }
    return out;
  }

  for (auto& item : out.items) {
    item = rewrite_generic_type(item, generic_params);
  }
  for (auto& member : out.union_members) {
    member = rewrite_generic_type(member, generic_params);
  }
  for (auto& arg : out.applied_args) {
    arg = rewrite_generic_type(arg, generic_params);
  }
  for (auto& param : out.function_params) {
    param = rewrite_generic_type(param, generic_params);
  }
  if (out.function_return.has_value()) {
    out.function_return =
        common::make_indirect_optional<Type>(rewrite_generic_type(*out.function_return, generic_params));
  }

  return out;
}

auto type_complexity(const Type& type) -> std::size_t {
  std::size_t total = 1U;
  for (const auto& item : type.items) {
    total += type_complexity(item);
  }
  for (const auto& member : type.union_members) {
    total += type_complexity(member);
  }
  for (const auto& arg : type.applied_args) {
    total += type_complexity(arg);
  }
  for (const auto& param : type.function_params) {
    total += type_complexity(param);
  }
  if (type.function_return.has_value()) {
    total += type_complexity(*type.function_return);
  }
  return total;
}

auto binding_quality(const TypeBindings& bindings) -> BindingQuality {
  BindingQuality quality;
  quality.total_bindings = bindings.size();

  std::unordered_set<std::string> visiting;
  std::unordered_map<std::string, bool> resolved_cache;

  for (const auto& [type_var, bound_type] : bindings) {
    (void)type_var;
    if (const GenericScope no_unbound; is_type_resolved(bound_type, bindings, no_unbound, visiting, resolved_cache)) {
      ++quality.resolved_bindings;
    }
    quality.complexity += type_complexity(bound_type);
  }

  return quality;
}

auto is_better_binding_quality(const TypeBindings& candidate, const TypeBindings& incumbent) -> bool {
  const auto [candidate_resolved_bindings, candidate_total_bindings, candidate_complexity] = binding_quality(candidate);
  const auto [incumbent_resolved_bindings, incumbent_total_bindings, incumbent_complexity] = binding_quality(incumbent);

  if (candidate_resolved_bindings != incumbent_resolved_bindings) {
    return candidate_resolved_bindings > incumbent_resolved_bindings;
  }
  if (candidate_total_bindings != incumbent_total_bindings) {
    return candidate_total_bindings > incumbent_total_bindings;
  }
  if (candidate_complexity != incumbent_complexity) {
    return candidate_complexity < incumbent_complexity;
  }
  return false;
}

auto merge_binding_types(const Type& lhs, const Type& rhs) -> Type {
  if (is_consistent(lhs, rhs) && is_consistent(rhs, lhs)) {
    return normalize_type(lhs);
  }

  Type merged;
  merged.kind = TypeKind::kUnion;
  merged.union_members = {lhs, rhs};
  return normalize_type(std::move(merged));
}

auto union_member_requires_coverage(const Type& member) -> bool { return member.kind != TypeKind::kTypeVar; }

auto fixed_union_members_are_covered(const std::vector<Type>& expected_members, const std::vector<Type>& actual_members,
                                     const TypeBindings& bindings) -> bool {
  for (const auto& expected_member : expected_members) {
    if (!union_member_requires_coverage(expected_member)) {
      continue;
    }

    const auto instantiated_expected = instantiate_generic_type(expected_member, bindings);
    const bool covered = std::ranges::any_of(actual_members, [&](const Type& actual_member) -> bool {
      return is_consistent(instantiated_expected, actual_member);
    });
    if (!covered) {
      return false;
    }
  }
  return true;
}

auto bind_union_members(const std::vector<Type>& expected_members, const std::vector<Type>& actual_members,
                        const std::size_t actual_index, const TypeBindings& base_bindings, TypeBindings& bindings)
    -> bool {
  if (actual_index >= actual_members.size()) {
    return true;
  }

  std::optional<TypeBindings> best_bindings;
  for (const auto& expected_option : expected_members) {
    auto trial_bindings = bindings;
    if (!try_bind_union_option(expected_option, actual_members[actual_index], base_bindings, trial_bindings)) {
      continue;
    }
    if (!bind_union_members(expected_members, actual_members, actual_index + 1U, base_bindings, trial_bindings)) {
      continue;
    }
    if (!best_bindings.has_value() || is_better_binding_quality(trial_bindings, *best_bindings)) {
      best_bindings = std::move(trial_bindings);
    }
  }

  if (!best_bindings.has_value()) {
    return false;
  }
  bindings = std::move(*best_bindings);
  return true;
}

auto instantiate_generic_type(const Type& type, const TypeBindings& bindings) -> Type {
  if (type.kind == TypeKind::kTypeVar) {
    if (const auto it = bindings.find(type.nominal_name); it != bindings.end()) {
      return normalize_type(it->second);
    }
    return make_type(TypeKind::kAny);
  }

  Type out = type;
  for (auto& item : out.items) {
    const bool variadic = item.variadic;
    item = instantiate_generic_type(item, bindings);
    item.variadic = variadic;
  }
  for (auto& member : out.union_members) {
    member = instantiate_generic_type(member, bindings);
  }
  for (auto& arg : out.applied_args) {
    arg = instantiate_generic_type(arg, bindings);
  }
  for (auto& param : out.function_params) {
    const bool variadic = param.variadic;
    param = instantiate_generic_type(param, bindings);
    param.variadic = variadic;
  }
  if (out.function_return.has_value()) {
    out.function_return =
        common::make_indirect_optional<Type>(instantiate_generic_type(*out.function_return, bindings));
  }
  return normalize_type(std::move(out));
}

// Binds one parameter of a function-typed argument. Parameters are contravariant, so once the expected parameter
// is fully determined by earlier bindings, a function whose parameter accepts a wider type still fits: a
// (Int64 | String) => String callback can be passed where (T) => U is expected and T is already Int64.
auto bind_function_param(const Type& expected, const Type& actual, TypeBindings& bindings) -> bool {
  if (TypeBindings attempt = bindings; bind_generic_type(expected, actual, attempt)) {
    bindings = std::move(attempt);
    return true;
  }

  std::unordered_set<std::string> type_vars;
  collect_type_vars(expected, type_vars);
  if (!std::ranges::all_of(type_vars, [&](const std::string& name) { return bindings.contains(name); })) {
    return false;
  }
  return is_consistent(actual, instantiate_generic_type(expected, bindings));
}

auto bind_function_type(const Type& expected, const Type& actual, TypeBindings& bindings) -> bool {
  if (actual.kind != TypeKind::kFunction) {
    return false;
  }
  if (expected.function_params.size() != actual.function_params.size()) {
    return false;
  }

  for (std::size_t i = 0; i < expected.function_params.size(); ++i) {
    if (expected.function_params[i].variadic != actual.function_params[i].variadic) {
      return false;
    }
    if (!bind_function_param(expected.function_params[i], actual.function_params[i], bindings)) {
      return false;
    }
  }

  if (expected.function_return.has_value() != actual.function_return.has_value()) {
    return false;
  }
  if (!expected.function_return.has_value()) {
    return true;
  }
  return bind_generic_type(*expected.function_return, *actual.function_return, bindings);
}

// Joins two bindings of one type parameter that differ only where one side is Never. Never has no values, so
// Result<Int64, Never> and Result<Never, String> join to Result<Int64, String>. Any other difference does not join.
auto join_never_bindings(const Type& lhs, const Type& rhs) -> std::optional<Type> {
  if (lhs.kind == TypeKind::kNever) {
    return rhs;
  }
  if (rhs.kind == TypeKind::kNever) {
    return lhs;
  }
  if (is_consistent(lhs, rhs) && is_consistent(rhs, lhs)) {
    return lhs;
  }
  if (lhs.kind != rhs.kind) {
    return std::nullopt;
  }

  const auto join_each = [](const std::vector<Type>& lhs_items,
                            const std::vector<Type>& rhs_items) -> std::optional<std::vector<Type>> {
    if (lhs_items.size() != rhs_items.size()) {
      return std::nullopt;
    }
    std::vector<Type> joined;
    joined.reserve(lhs_items.size());
    for (std::size_t index = 0; index < lhs_items.size(); ++index) {
      if (lhs_items[index].variadic != rhs_items[index].variadic) {
        return std::nullopt;
      }
      auto item = join_never_bindings(lhs_items[index], rhs_items[index]);
      if (!item.has_value()) {
        return std::nullopt;
      }
      item->variadic = lhs_items[index].variadic;
      joined.push_back(std::move(*item));
    }
    return joined;
  };

  Type joined = lhs;
  if (lhs.kind == TypeKind::kApplied && lhs.applied_name == rhs.applied_name) {
    auto args = join_each(lhs.applied_args, rhs.applied_args);
    if (!args.has_value()) {
      return std::nullopt;
    }
    joined.applied_args = std::move(*args);
    return normalize_type(std::move(joined));
  }
  if (lhs.kind == TypeKind::kTuple) {
    auto items = join_each(lhs.items, rhs.items);
    if (!items.has_value()) {
      return std::nullopt;
    }
    joined.items = std::move(*items);
    return normalize_type(std::move(joined));
  }
  return std::nullopt;
}

auto bind_generic_type(const Type& expected, const Type& actual, TypeBindings& bindings) -> bool {
  if (expected.kind == TypeKind::kTypeVar) {
    if (const auto it = bindings.find(expected.nominal_name); it != bindings.end()) {
      if (is_consistent(it->second, actual) && is_consistent(actual, it->second)) {
        return true;
      }
      if (auto joined = join_never_bindings(it->second, actual); joined.has_value()) {
        it->second = normalize_type(std::move(*joined));
        return true;
      }
      return false;
    }
    bindings.insert_or_assign(expected.nominal_name, normalize_type(actual));
    return true;
  }

  if (expected.kind == TypeKind::kTuple) {
    if (actual.kind != TypeKind::kTuple) {
      return false;
    }
    const auto variadic_it =
        std::ranges::find_if(expected.items, [](const Type& item) -> bool { return item.variadic; });
    if (variadic_it == expected.items.end()) {
      if (expected.items.size() != actual.items.size()) {
        return false;
      }
      for (std::size_t i = 0; i < expected.items.size(); ++i) {
        if (!bind_generic_type(expected.items[i], actual.items[i], bindings)) {
          return false;
        }
      }
      return true;
    }

    const auto variadic_index = static_cast<std::size_t>(std::distance(expected.items.begin(), variadic_it));
    if (variadic_index + 1U != expected.items.size() || actual.items.size() < variadic_index) {
      return false;
    }
    for (std::size_t i = 0; i < variadic_index; ++i) {
      if (!bind_generic_type(expected.items[i], actual.items[i], bindings)) {
        return false;
      }
    }
    Type repeated_expected = expected.items.back();
    repeated_expected.variadic = false;
    for (auto i = variadic_index; i < actual.items.size(); ++i) {
      if (!bind_generic_type(repeated_expected, actual.items[i], bindings)) {
        return false;
      }
    }
    return true;
  }

  if (expected.kind == TypeKind::kApplied) {
    if (actual.kind != TypeKind::kApplied || expected.applied_name != actual.applied_name ||
        expected.applied_args.size() != actual.applied_args.size()) {
      return false;
    }
    for (std::size_t i = 0; i < expected.applied_args.size(); ++i) {
      if (!bind_generic_type(expected.applied_args[i], actual.applied_args[i], bindings)) {
        return false;
      }
    }
    return true;
  }

  if (expected.kind == TypeKind::kFunction) {
    return bind_function_type(expected, actual, bindings);
  }

  if (expected.kind == TypeKind::kUnion) {
    if (actual.kind == TypeKind::kUnion) {
      if (expected.union_members.empty() || actual.union_members.empty()) {
        return false;
      }
      if (const auto base_bindings = bindings;
          !bind_union_members(expected.union_members, actual.union_members, 0U, base_bindings, bindings)) {
        return false;
      }
      return fixed_union_members_are_covered(expected.union_members, actual.union_members, bindings);
    }

    std::optional<TypeBindings> best_bindings;
    for (const auto& option : expected.union_members) {
      if (auto trial_bindings = bindings; bind_generic_type(option, actual, trial_bindings)) {
        if (!best_bindings.has_value() || is_better_binding_quality(trial_bindings, *best_bindings)) {
          best_bindings = std::move(trial_bindings);
        }
      }
    }
    if (!best_bindings.has_value()) {
      return false;
    }
    bindings = std::move(*best_bindings);
    return true;
  }

  return is_consistent(expected, actual);
}

auto try_bind_union_option(const Type& expected_option, const Type& actual_member, const TypeBindings& base_bindings,
                           TypeBindings& trial_bindings) -> bool {
  if (expected_option.kind != TypeKind::kTypeVar || base_bindings.contains(expected_option.nominal_name)) {
    return bind_generic_type(expected_option, actual_member, trial_bindings);
  }

  if (const auto it = trial_bindings.find(expected_option.nominal_name); it != trial_bindings.end()) {
    it->second = merge_binding_types(it->second, actual_member);
    return true;
  }

  trial_bindings.insert_or_assign(expected_option.nominal_name, normalize_type(actual_member));
  return true;
}

namespace {

auto types_are_identical(const Type& lhs, const Type& rhs) -> bool {
  return is_consistent(lhs, rhs) && is_consistent(rhs, lhs);
}

// Returns the underlying type of a declared strong type, instantiated with the type arguments when the
// strong type is generic. Builtin types and non-strong types have no underlying type.
auto strong_underlying_type(const Type& type, const StrongTypeIndex& type_index) -> std::optional<Type> {
  const bool is_nominal = type.kind == TypeKind::kNominal;
  if (!is_nominal && type.kind != TypeKind::kApplied) {
    return std::nullopt;
  }
  const auto decl = type_index.resolve_name(is_nominal ? type.nominal_name : type.applied_name);
  if (!decl.has_value() || decl->get().is_builtin) {
    return std::nullopt;
  }
  const auto& generic_params = decl->get().generic_params;
  const std::size_t arg_count = is_nominal ? 0U : type.applied_args.size();
  if (generic_params.size() != arg_count) {
    return std::nullopt;
  }
  if (generic_params.empty()) {
    return decl->get().target_type;
  }
  TypeBindings bindings;
  for (std::size_t i = 0; i < generic_params.size(); ++i) {
    bindings.insert_or_assign(generic_params[i], type.applied_args[i]);
  }
  return instantiate_generic_type(decl->get().target_type, bindings);
}

auto is_exact_strong_underlying_cast_pair(const Type& source, const Type& target, const StrongTypeIndex& type_index)
    -> bool {
  if (const auto source_underlying = strong_underlying_type(source, type_index);
      source_underlying.has_value() && types_are_identical(*source_underlying, target)) {
    return true;
  }

  if (const auto target_underlying = strong_underlying_type(target, type_index);
      target_underlying.has_value() && types_are_identical(source, *target_underlying)) {
    return true;
  }

  return false;
}

auto build_runtime_type_check(const std::string& full_name, const Type& target, const StrongTypeIndex& type_index,
                              const std::optional<diag::SourceSpan>& span, std::unordered_set<std::string>& expanding)
    -> tl::expected<common::RuntimeTypeCheck, type_check::AnalysisError> {
  using common::RuntimeTypeCheck;
  using common::RuntimeTypeCheckKind;

  const auto unsupported = [&](const std::string& reason) -> tl::unexpected<type_check::AnalysisError> {
    return tl::unexpected(make_error(
        std::format("Invalid {} invocation.", full_name),
        std::format("{} from Any cannot check {} at runtime: {}. Give the value a precise type instead of Any.",
                    full_name, type_debug_name(target), reason),
        span));
  };
  const auto leaf = [](const RuntimeTypeCheckKind kind) -> RuntimeTypeCheck { return RuntimeTypeCheck{.kind = kind}; };
  const auto build_items =
      [&](const std::vector<Type>& types) -> tl::expected<std::vector<RuntimeTypeCheck>, type_check::AnalysisError> {
    std::vector<RuntimeTypeCheck> items;
    items.reserve(types.size());
    for (const auto& type : types) {
      auto item = build_runtime_type_check(full_name, type, type_index, span, expanding);
      if (!item) {
        return tl::unexpected(item.error());
      }
      items.push_back(std::move(*item));
    }
    return items;
  };

  switch (target.kind) {
    case TypeKind::kAny:
      return leaf(RuntimeTypeCheckKind::kAny);
    case TypeKind::kNever:
      return leaf(RuntimeTypeCheckKind::kNever);
    case TypeKind::kInt64:
      return leaf(RuntimeTypeCheckKind::kInt64);
    case TypeKind::kUInt64:
      return leaf(RuntimeTypeCheckKind::kUInt64);
    case TypeKind::kFloat64:
      return leaf(RuntimeTypeCheckKind::kFloat64);
    case TypeKind::kString:
      return leaf(RuntimeTypeCheckKind::kString);
    case TypeKind::kBool:
      return leaf(RuntimeTypeCheckKind::kBool);
    case TypeKind::kNull:
      return leaf(RuntimeTypeCheckKind::kNull);
    case TypeKind::kTuple: {
      auto items = build_items(target.items);
      if (!items) {
        return tl::unexpected(items.error());
      }
      return RuntimeTypeCheck{
          .kind = RuntimeTypeCheckKind::kTuple,
          .items = std::move(*items),
          .variadic_tail = !target.items.empty() && target.items.back().variadic,
      };
    }
    case TypeKind::kUnion: {
      auto items = build_items(target.union_members);
      if (!items) {
        return tl::unexpected(items.error());
      }
      return RuntimeTypeCheck{.kind = RuntimeTypeCheckKind::kUnion, .items = std::move(*items)};
    }
    case TypeKind::kFunction:
      return unsupported("function parameter and return types are not stored with callable values");
    case TypeKind::kTypeVar:
      return unsupported("generic parameters are not known at runtime");
    case TypeKind::kNominal:
    case TypeKind::kApplied: {
      const std::string& name = target.kind == TypeKind::kNominal ? target.nominal_name : target.applied_name;
      if (type_index.is_builtin_type(target)) {
        if (name == "FileHandle") {
          return leaf(RuntimeTypeCheckKind::kFileHandle);
        }
        if (name == "RandomGenerator") {
          return leaf(RuntimeTypeCheckKind::kRandomGenerator);
        }
        if (name == "TaskHandle") {
          if (!target.applied_args.empty() && target.applied_args.front().kind != TypeKind::kAny) {
            return unsupported("a task's result type is not stored with its handle; cast to TaskHandle<Any>");
          }
          return leaf(RuntimeTypeCheckKind::kTaskHandle);
        }
        if ((name == "Dict" || name == "Result") && target.applied_args.size() == 2U) {
          auto items = build_items(target.applied_args);
          if (!items) {
            return tl::unexpected(items.error());
          }
          return RuntimeTypeCheck{
              .kind = name == "Dict" ? RuntimeTypeCheckKind::kDict : RuntimeTypeCheckKind::kResult,
              .items = std::move(*items),
          };
        }
        return unsupported("it has no runtime representation that can be checked");
      }

      const auto underlying = strong_underlying_type(target, type_index);
      if (!underlying.has_value()) {
        return unsupported("the type is unknown");
      }
      const std::string key = type_debug_name(target);
      if (!expanding.insert(key).second) {
        return unsupported("the type refers to itself");
      }
      auto expanded = build_runtime_type_check(full_name, *underlying, type_index, span, expanding);
      expanding.erase(key);
      return expanded;
    }
    default:
      return unsupported("the type is unknown");
  }
}

auto validate_std_cast_invocation(const Type& source_type, const Type& target_type, const StrongTypeIndex& type_index,
                                  const std::optional<diag::SourceSpan>& span)
    -> tl::expected<void, type_check::AnalysisError> {
  if (source_type.kind == TypeKind::kAny) {
    // The value is checked against the target at runtime; runtime_type_check_for_cast rejects targets that cannot
    // be checked.
    return {};
  }

  if (type_index.is_builtin_type(source_type) || type_index.is_builtin_type(target_type)) {
    // Builtin types have no underlying type, so only an exact identity cast is allowed. Gradual consistency is
    // not enough here because Any is consistent with every type.
    if (type_debug_name(normalize_type(source_type)) == type_debug_name(normalize_type(target_type))) {
      return {};
    }

    return tl::unexpected(make_error(
        "Invalid Std.Cast invocation.",
        std::format("Std.Cast does not permit casts to or from builtin types. Got {} -> {}.",
                    type_debug_name(source_type), type_debug_name(target_type)),
        span));
  }

  if (types_are_identical(source_type, target_type)) {
    return {};
  }

  if (is_exact_strong_underlying_cast_pair(source_type, target_type, type_index)) {
    return {};
  }

  return tl::unexpected(make_error(
      "Invalid Std.Cast invocation.",
      std::format("Std.Cast only permits identity casts or exact strong-type and underlying-type pairs. Got {} -> {}.",
                  type_debug_name(source_type), type_debug_name(target_type)),
      span));
}

}  // namespace

auto runtime_type_check_for_cast(const std::string& full_name, const Type& target, const StrongTypeIndex& type_index,
                                 const std::optional<diag::SourceSpan>& span)
    -> tl::expected<common::RuntimeTypeCheck, type_check::AnalysisError> {
  std::unordered_set<std::string> expanding;
  return build_runtime_type_check(full_name, normalize_type(target), type_index, span, expanding);
}

auto check_generic_bounds(const std::string& full_name, const std::optional<diag::SourceSpan>& span,
                          const FunctionSig& sig, const TypeBindings& bindings)
    -> tl::expected<void, type_check::AnalysisError> {
  for (const auto& generic_param : sig.generic_params) {
    const auto bound_it = sig.generic_bounds.find(generic_param);
    if (bound_it == sig.generic_bounds.end()) {
      continue;
    }
    const auto binding_it = bindings.find(generic_param);
    if (binding_it == bindings.end() || satisfies_generic_bound(binding_it->second, bound_it->second)) {
      continue;
    }

    const Type& binding = binding_it->second;
    std::string binding_text = type_debug_name(binding);
    if (binding.kind == TypeKind::kTypeVar) {
      binding_text = binding.type_var_bound.has_value()
                         ? std::format("type parameter '{}' bounded by {}", binding.nominal_name,
                                       type_debug_name(*binding.type_var_bound))
                         : std::format("type parameter '{}'", binding.nominal_name);
    }
    std::string hint = std::format("{} requires {}: {}, but {} is {}.", full_name, generic_param,
                                   type_debug_name(bound_it->second), generic_param, binding_text);
    if (binding.kind == TypeKind::kTypeVar && !binding.type_var_bound.has_value()) {
      hint += std::format(" Type parameter '{}' has no bound; declare it as '<{}: ...>'.", binding.nominal_name,
                          binding.nominal_name);
    } else if (binding.kind == TypeKind::kUnion) {
      hint += " A union cannot satisfy a bound; it must be exactly one of the allowed types.";
    }
    return tl::unexpected(make_error("Type argument does not satisfy its bound.", hint, span));
  }
  return {};
}

auto check_invocation(const std::string& full_name, const std::optional<diag::SourceSpan>& span, const FunctionSig& sig,
                      const std::vector<Type>& args, const StrongTypeIndex& type_index,
                      const GenericScope& allowed_unbound,
                      const std::vector<Type>& explicit_type_args) -> tl::expected<Type, type_check::AnalysisError> {
  std::size_t fixed_count = sig.params.size();
  bool has_variadic = false;
  if (!sig.params.empty() && sig.params.back().variadic) {
    has_variadic = true;
    fixed_count = sig.params.size() - 1U;
  }

  if (has_variadic) {
    if (args.size() < fixed_count) {
      return tl::unexpected(make_error("Type mismatch in call target arguments.",
                                       std::format("{} requires at least {} argument(s).", full_name, fixed_count),
                                       span));
    }
  } else if (args.size() != sig.params.size()) {
    return tl::unexpected(make_error(
        "Type mismatch in call target arguments.",
        std::format("{} expects {} argument(s) but got {}.", full_name, sig.params.size(), args.size()), span));
  }

  auto explicit_bindings = explicit_type_arg_bindings_for_sig(full_name, span, sig, explicit_type_args);
  if (!explicit_bindings) {
    return tl::unexpected(explicit_bindings.error());
  }

  TypeBindings generic_bindings = std::move(*explicit_bindings);
  const bool is_generic_call = !sig.generic_params.empty();

  for (std::size_t i = 0; i < fixed_count; ++i) {
    if (is_generic_call) {
      if (!bind_generic_type(sig.params[i].type, args[i], generic_bindings)) {
        return tl::unexpected(make_error(
            "Type mismatch in call target arguments.",
            generic_arg_mismatch_hint(full_name, i, sig.params[i].type, false, generic_bindings, args[i]), span));
      }

      if (const Type instantiated = instantiate_generic_type(sig.params[i].type, generic_bindings);
          !is_consistent(instantiated, args[i])) {
        return tl::unexpected(make_error(
            "Type mismatch in call target arguments.",
            generic_arg_mismatch_hint(full_name, i, sig.params[i].type, false, generic_bindings, args[i]), span));
      }
      continue;
    }

    if (!is_consistent(sig.params[i].type, args[i])) {
      return tl::unexpected(make_error("Type mismatch in call target arguments.",
                                       std::format("{} expects argument {} to match declared type.", full_name, i),
                                       span));
    }
  }

  if (has_variadic) {
    const Type& variadic_type = sig.params.back().type;
    for (std::size_t i = fixed_count; i < args.size(); ++i) {
      if (is_generic_call) {
        if (!bind_generic_type(variadic_type, args[i], generic_bindings)) {
          return tl::unexpected(make_error(
              "Type mismatch in call target arguments.",
              generic_arg_mismatch_hint(full_name, i, variadic_type, true, generic_bindings, args[i]), span));
        }
        if (const Type instantiated = instantiate_generic_type(variadic_type, generic_bindings);
            !is_consistent(instantiated, args[i])) {
          return tl::unexpected(make_error(
              "Type mismatch in call target arguments.",
              generic_arg_mismatch_hint(full_name, i, variadic_type, true, generic_bindings, args[i]), span));
        }
        continue;
      }

      if (!is_consistent(variadic_type, args[i])) {
        return tl::unexpected(
            make_error("Type mismatch in call target arguments.",
                       std::format("{} expects variadic argument {} to match declared type.", full_name, i), span));
      }
    }
  }

  if (auto bounds_ok = check_generic_bounds(full_name, span, sig, generic_bindings); !bounds_ok) {
    return tl::unexpected(bounds_ok.error());
  }

  if (std::string builtin_contract_error;
      !validate_builtin_contract(full_name, args, type_index, builtin_contract_error)) {
    return tl::unexpected(make_error("Type mismatch in call target arguments.", builtin_contract_error, span));
  }

  if (full_name == "Std.Cast" || full_name == "Std.TryCast") {
    // Std.Cast<T> returns T and Std.TryCast<T> returns Result<T, String>; either way the cast target is T.
    const Type& declared_target =
        full_name == "Std.Cast" || sig.return_type.applied_args.empty() ? sig.return_type : sig.return_type.applied_args.front();
    Type target_type = is_generic_call ? instantiate_generic_type(declared_target, generic_bindings) : declared_target;

    std::unordered_map<std::string, bool> resolved_cache;
    if (std::unordered_set<std::string> visiting;
        !is_type_resolved(target_type, generic_bindings, allowed_unbound, visiting, resolved_cache)) {
      std::unordered_set<std::string> unbound;
      collect_unresolved_return_type_vars(sig.return_type, generic_bindings, allowed_unbound, unbound);
      return tl::unexpected(make_error("Type mismatch in call target arguments.",
                                       std::format("{} could not infer generic return type variable(s): {}.", full_name,
                                                   join_sorted_type_var_names(unbound)),
                                       span));
    }

    if (auto validated = validate_std_cast_invocation(args.front(), target_type, type_index, span); !validated) {
      return tl::unexpected(validated.error());
    }
    return is_generic_call ? instantiate_generic_type(sig.return_type, generic_bindings) : sig.return_type;
  }

  if (is_generic_call) {
    std::unordered_set<std::string> unbound;
    collect_unresolved_return_type_vars(sig.return_type, generic_bindings, allowed_unbound, unbound);
    if (!unbound.empty()) {
      return tl::unexpected(make_error("Type mismatch in call target arguments.",
                                       std::format("{} could not infer generic return type variable(s): {}.", full_name,
                                                   join_sorted_type_var_names(unbound)),
                                       span));
    }
    return instantiate_generic_type(sig.return_type, generic_bindings);
  }
  return sig.return_type;
}

auto resolve_overload_invocation(const std::string& full_name, const std::optional<diag::SourceSpan>& span,
                                 const FunctionOverloadSet& overloads, const std::vector<Type>& args,
                                 const StrongTypeIndex& type_index,
                                 const GenericScope& allowed_unbound,
                                 const std::vector<Type>& explicit_type_args)
    -> tl::expected<ResolvedInvocation, type_check::AnalysisError> {
  const auto filtered = filter_overloads_for_explicit_type_args(full_name, span, overloads, explicit_type_args);
  if (!filtered) {
    return tl::unexpected(filtered.error());
  }

  std::vector<const FunctionSig*> shape_matches;
  shape_matches.reserve(filtered->size());
  for (const auto* overload : *filtered) {
    if (call_shape_matches(*overload, args.size())) {
      shape_matches.push_back(overload);
    }
  }

  if (shape_matches.empty()) {
    if (filtered->size() == 1U) {
      if (auto checked = check_invocation(full_name, span, **filtered->begin(), args, type_index, allowed_unbound,
                                          explicit_type_args);
          !checked.has_value()) {
        return tl::unexpected(checked.error());
      }
    }
    return tl::unexpected(make_error("Type mismatch in call target arguments.",
                                     std::format("{} has no overload that accepts {} argument(s). Candidates: {}.",
                                                 full_name, args.size(), overload_candidate_list(full_name, *filtered)),
                                     span));
  }

  std::vector<Type> matched_returns;
  matched_returns.reserve(shape_matches.size());
  std::vector<const FunctionSig*> matched_overloads;
  matched_overloads.reserve(shape_matches.size());
  std::optional<type_check::AnalysisError> first_error;
  for (const auto* overload : shape_matches) {
    auto checked = check_invocation(full_name, span, *overload, args, type_index, allowed_unbound, explicit_type_args);
    if (checked.has_value()) {
      matched_returns.push_back(*checked);
      matched_overloads.push_back(overload);
      continue;
    }
    if (!first_error.has_value()) {
      first_error = checked.error();
    }
  }

  std::optional<std::string> matched_symbol_key;
  for (const auto* overload : shape_matches) {
    if (auto checked =
            check_invocation(full_name, span, *overload, args, type_index, allowed_unbound, explicit_type_args);
        !checked.has_value()) {
      continue;
    }
    if (matched_symbol_key.has_value()) {
      matched_symbol_key = std::nullopt;
      break;
    }
    matched_symbol_key = overload->resolved_symbol_key;
  }

  if (matched_returns.size() == 1U && matched_symbol_key.has_value()) {
    return ResolvedInvocation{
        .return_type = matched_returns.front(),
        .resolved_symbol_key = *matched_symbol_key,
    };
  }

  if (matched_returns.size() > 1U) {
    return tl::unexpected(
        make_error("Ambiguous overloaded call target.",
                   std::format("{} has multiple matching overloads for the provided argument types. Candidates: {}.",
                               full_name, overload_candidate_list(full_name, matched_overloads)),
                   span));
  }

  if (shape_matches.size() == 1U && first_error.has_value()) {
    return tl::unexpected(*first_error);
  }

  return tl::unexpected(make_error("Type mismatch in call target arguments.",
                                   std::format("No overload of {} matches the provided argument types. Candidates: {}.",
                                               full_name, overload_candidate_list(full_name, shape_matches)),
                                   span));
}

}  // namespace fleaux::frontend::type_system::detail
