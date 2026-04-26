#include "fleaux/frontend/type_system/detail/checker_internal.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "fleaux/frontend/type_system/builtin_contracts.hpp"

namespace fleaux::frontend::type_system::detail {

auto collect_type_vars(const Type& type, std::unordered_set<std::string>& out) -> void {
  if (type.kind == TypeKind::kTypeVar) {
    out.insert(type.nominal_name);
    return;
  }

  for (const auto& item : type.items) { collect_type_vars(item, out); }
  for (const auto& member : type.union_members) { collect_type_vars(member, out); }
  for (const auto& arg : type.applied_args) { collect_type_vars(arg, out); }
  for (const auto& param : type.function_params) { collect_type_vars(param, out); }
  if (type.function_return.has_value()) { collect_type_vars(**type.function_return, out); }
}

auto is_type_var_resolved(const std::string& type_var, const TypeBindings& bindings,
                          const std::unordered_set<std::string>& allowed_unbound,
                          std::unordered_set<std::string>& visiting,
                          std::unordered_map<std::string, bool>& resolved_cache) -> bool {
  if (allowed_unbound.contains(type_var)) { return true; }

  if (const auto cached = resolved_cache.find(type_var); cached != resolved_cache.end()) { return cached->second; }

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
                      const std::unordered_set<std::string>& allowed_unbound, std::unordered_set<std::string>& visiting,
                      std::unordered_map<std::string, bool>& resolved_cache) -> bool {
  if (type.kind == TypeKind::kTypeVar) {
    return is_type_var_resolved(type.nominal_name, bindings, allowed_unbound, visiting, resolved_cache);
  }

  for (const auto& item : type.items) {
    if (!is_type_resolved(item, bindings, allowed_unbound, visiting, resolved_cache)) { return false; }
  }
  for (const auto& member : type.union_members) {
    if (!is_type_resolved(member, bindings, allowed_unbound, visiting, resolved_cache)) { return false; }
  }
  for (const auto& arg : type.applied_args) {
    if (!is_type_resolved(arg, bindings, allowed_unbound, visiting, resolved_cache)) { return false; }
  }
  for (const auto& param : type.function_params) {
    if (!is_type_resolved(param, bindings, allowed_unbound, visiting, resolved_cache)) { return false; }
  }
  if (type.function_return.has_value() &&
      !is_type_resolved(**type.function_return, bindings, allowed_unbound, visiting, resolved_cache)) {
    return false;
  }
  return true;
}

auto collect_unresolved_return_type_vars(const Type& return_type, const TypeBindings& bindings,
                                         const std::unordered_set<std::string>& allowed_unbound,
                                         std::unordered_set<std::string>& out) -> void {
  std::unordered_set<std::string> return_type_vars;
  collect_type_vars(return_type, return_type_vars);

  std::unordered_set<std::string> visiting;
  std::unordered_map<std::string, bool> resolved_cache;
  for (const auto& type_var : return_type_vars) {
    if (!is_type_var_resolved(type_var, bindings, allowed_unbound, visiting, resolved_cache)) { out.insert(type_var); }
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

  for (const auto& item : type.items) { collect_unbound_type_vars(item, bindings, out); }
  for (const auto& member : type.union_members) { collect_unbound_type_vars(member, bindings, out); }
  for (const auto& arg : type.applied_args) { collect_unbound_type_vars(arg, bindings, out); }
  for (const auto& param : type.function_params) { collect_unbound_type_vars(param, bindings, out); }
  if (type.function_return.has_value()) { collect_unbound_type_vars(**type.function_return, bindings, out); }
}

auto join_sorted_type_var_names(const std::unordered_set<std::string>& names) -> std::string {
  std::vector<std::string> ordered(names.begin(), names.end());
  std::ranges::sort(ordered);

  std::string joined;
  for (std::size_t i = 0; i < ordered.size(); ++i) {
    if (i > 0U) { joined += ", "; }
    joined += ordered[i];
  }
  return joined;
}

auto generic_var_suffix_for_type(const Type& type) -> std::string {
  const TypeBindings empty_bindings;
  std::unordered_set<std::string> vars;
  collect_unbound_type_vars(type, empty_bindings, vars);
  if (vars.empty()) { return {}; }
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
      std::string out = "Tuple(";
      for (std::size_t i = 0; i < type.items.size(); ++i) {
        if (i > 0U) { out += ", "; }
        out += type_debug_name(type.items[i]);
        if (type.items[i].variadic) { out += "..."; }
      }
      out += ")";
      return out;
    }
    case TypeKind::kUnion: {
      std::string out;
      for (std::size_t i = 0; i < type.union_members.size(); ++i) {
        if (i > 0U) { out += " | "; }
        out += type_debug_name(type.union_members[i]);
      }
      return out;
    }
    case TypeKind::kApplied: {
      std::string out = type.applied_name + "(";
      for (std::size_t i = 0; i < type.applied_args.size(); ++i) {
        if (i > 0U) { out += ", "; }
        out += type_debug_name(type.applied_args[i]);
      }
      out += ")";
      return out;
    }
    case TypeKind::kFunction: {
      std::string out = "(";
      for (std::size_t i = 0; i < type.function_params.size(); ++i) {
        if (i > 0U) { out += ", "; }
        out += type_debug_name(type.function_params[i]);
        if (type.function_params[i].variadic) { out += "..."; }
      }
      out += ") => ";
      out += type.function_return.has_value() ? type_debug_name(**type.function_return) : "Any";
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
  if (vars.empty()) { return {}; }

  std::vector<std::string> ordered(vars.begin(), vars.end());
  std::ranges::sort(ordered);

  std::vector<std::string> details;
  details.reserve(ordered.size());
  for (const auto& var : ordered) {
    if (const auto it = bindings.find(var); it != bindings.end()) {
      details.push_back(std::format("{} bound as {}", var, type_debug_name(it->second)));
    }
  }
  if (details.empty()) { return {}; }

  std::string joined;
  for (std::size_t i = 0; i < details.size(); ++i) {
    if (i > 0U) { joined += "; "; }
    joined += details[i];
  }
  return std::format("Binding detail: {}. got {}.", joined, type_debug_name(actual));
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
    return hint;
  }
  std::string hint = std::format("{} expects argument {} to match declared type{}", full_name, arg_index, var_suffix);
  if (const auto detail = generic_binding_detail_for_type(expected_type, bindings, actual); !detail.empty()) {
    hint += " " + detail;
  }
  return hint;
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

[[nodiscard]] auto qualified_symbol_name(const std::optional<std::string>& qualifier, const std::string& name)
    -> std::string {
  return qualifier.has_value() ? (*qualifier + "." + name) : name;
}

[[nodiscard]] auto is_symbolic_qualifier(const std::optional<std::string>& qualifier) -> bool {
  if (!qualifier.has_value()) { return false; }
  return *qualifier == "Std" || qualifier->starts_with("Std.") || *qualifier == "StdBuiltins" ||
         qualifier->starts_with("StdBuiltins.");
}

[[nodiscard]] auto is_removed_symbolic_alias(const std::optional<std::string>& qualifier, const std::string& name)
    -> bool {
  return qualifier.has_value() && *qualifier == "Std" && name == "TypeOf";
}

[[nodiscard]] auto make_type_var(const std::string& name) -> Type {
  return Type{
      .kind = TypeKind::kTypeVar,
      .nominal_name = name,
  };
}

[[nodiscard]] auto make_applied_type(const std::string& name, std::vector<Type> args) -> Type {
  return Type{
      .kind = TypeKind::kApplied,
      .applied_name = name,
      .applied_args = std::move(args),
  };
}

[[nodiscard]] auto symbolic_builtin_overloads(const std::optional<std::string>& qualifier, const std::string& name)
    -> const FunctionOverloadSet* {
  if (!is_symbolic_qualifier(qualifier)) { return nullptr; }

  static const FunctionOverloadSet kStdDictCreateOverloads = [] {
    FunctionOverloadSet overloads;
    overloads.push_back(FunctionSig{
        .resolved_symbol_key = "Std.Dict.Create#0",
        .generic_params = {},
        .params = {},
        .return_type = make_applied_type("Dict", {Type{.kind = TypeKind::kAny}, Type{.kind = TypeKind::kAny}}),
        .is_builtin = true,
    });
    overloads.push_back(FunctionSig{
        .resolved_symbol_key = "Std.Dict.Create#1",
        .generic_params = {"K", "V"},
        .params = {{.name = "dict", .type = make_applied_type("Dict", {make_type_var("K"), make_type_var("V")}), .variadic = false}},
        .return_type = make_applied_type("Dict", {make_type_var("K"), make_type_var("V")}),
        .is_builtin = true,
    });
    return overloads;
  }();

  static const FunctionOverloadSet kStdExitOverloads = [] {
    FunctionOverloadSet overloads;
    overloads.push_back(FunctionSig{
        .resolved_symbol_key = "Std.Exit#0",
        .generic_params = {},
        .params = {},
        .return_type = Type{.kind = TypeKind::kNever},
        .is_builtin = true,
    });
    overloads.push_back(FunctionSig{
        .resolved_symbol_key = "Std.Exit#1",
        .generic_params = {},
        .params = {{.name = "code", .type = Type{.kind = TypeKind::kInt64}, .variadic = false}},
        .return_type = Type{.kind = TypeKind::kNever},
        .is_builtin = true,
    });
    return overloads;
  }();

  const auto full_name = qualified_symbol_name(qualifier, name);
  if (full_name == "Std.Dict.Create") { return &kStdDictCreateOverloads; }
  if (full_name == "Std.Exit") { return &kStdExitOverloads; }
  return nullptr;
}

[[nodiscard]] auto resolve_name_or_symbolic_builtin(const FunctionIndex& index, const std::optional<std::string>& qualifier,
                                                    const std::string& name) -> const FunctionOverloadSet* {
  if (const auto* overloads = index.resolve_name(qualifier, name); overloads != nullptr) { return overloads; }
  return symbolic_builtin_overloads(qualifier, name);
}

[[nodiscard]] auto target_name(const ir::IRCallTarget& target) -> std::optional<std::string> {
  if (const auto* name_ref = std::get_if<ir::IRNameRef>(&target); name_ref != nullptr) {
    return name_ref->qualifier.has_value() ? (*name_ref->qualifier + "." + name_ref->name) : name_ref->name;
  }
  return std::nullopt;
}

[[nodiscard]] auto resolve_signature(const FunctionIndex& index, const ir::IRCallTarget& target)
    -> const FunctionOverloadSet* {
  if (const auto* name_ref = std::get_if<ir::IRNameRef>(&target); name_ref != nullptr) {
    return resolve_name_or_symbolic_builtin(index, name_ref->qualifier, name_ref->name);
  }
  return nullptr;
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
  out.function_return = make_box<Type>(sig.return_type);
  return out;
}

auto declaration_symbol_key(const ir::IRLet& let) -> std::string {
  return let.qualifier.has_value() ? (*let.qualifier + "." + let.name) : let.name;
}

auto format_signature_debug_name(const std::string& full_name, const FunctionSig& sig) -> std::string {
  std::string out = full_name + "(";
  for (std::size_t i = 0; i < sig.params.size(); ++i) {
    if (i > 0U) { out += ", "; }
    out += type_debug_name(sig.params[i].type);
    if (sig.params[i].variadic) { out += "..."; }
  }
  out += "): ";
  out += type_debug_name(sig.return_type);
  return out;
}

auto overload_candidate_list(const std::string& full_name, const FunctionOverloadSet& overloads) -> std::string {
  std::string out;
  for (std::size_t i = 0; i < overloads.size(); ++i) {
    if (i > 0U) { out += "; "; }
    out += format_signature_debug_name(full_name, overloads[i]);
  }
  return out;
}

auto call_shape_matches(const FunctionSig& sig, const std::size_t arg_count) -> bool {
  if (!sig.params.empty() && sig.params.back().variadic) { return arg_count + 1U >= sig.params.size(); }
  return sig.params.size() == arg_count;
}

auto overload_shapes_overlap(const FunctionSig& lhs, const FunctionSig& rhs) -> bool {
  const bool lhs_variadic = !lhs.params.empty() && lhs.params.back().variadic;
  const bool rhs_variadic = !rhs.params.empty() && rhs.params.back().variadic;

  if (!lhs_variadic && !rhs_variadic) { return lhs.params.size() == rhs.params.size(); }
  if (lhs_variadic && rhs_variadic) { return true; }

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

auto validate_supported_overload_sets(const ir::IRProgram& program, const std::vector<ir::IRLet>& imported_typed_lets)
    -> tl::expected<void, type_check::AnalysisError> {
  std::unordered_map<std::string, std::vector<const ir::IRLet*>> builtin_overload_sets;
  const auto collect_builtin_overloads = [&](const auto& lets) -> void {
    for (const auto& let : lets) {
      if (!let.is_builtin) { continue; }
      builtin_overload_sets[declaration_symbol_key(let)].push_back(&let);
    }
  };

  collect_builtin_overloads(imported_typed_lets);
  collect_builtin_overloads(program.lets);

  for (const auto& [full_name, lets] : builtin_overload_sets) {
    if (lets.size() <= 1U) { continue; }

    FunctionOverloadSet overloads;
    overloads.reserve(lets.size());
    for (const auto* let : lets) { overloads.push_back(function_sig_from_let(*let)); }

    for (std::size_t lhs_index = 0; lhs_index < overloads.size(); ++lhs_index) {
      for (std::size_t rhs_index = lhs_index + 1U; rhs_index < overloads.size(); ++rhs_index) {
        if (!overload_shapes_overlap(overloads[lhs_index], overloads[rhs_index])) { continue; }
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

auto type_complexity(const Type& type) -> std::size_t {
  std::size_t total = 1U;
  for (const auto& item : type.items) { total += type_complexity(item); }
  for (const auto& member : type.union_members) { total += type_complexity(member); }
  for (const auto& arg : type.applied_args) { total += type_complexity(arg); }
  for (const auto& param : type.function_params) { total += type_complexity(param); }
  if (type.function_return.has_value()) { total += type_complexity(**type.function_return); }
  return total;
}

auto binding_quality(const TypeBindings& bindings) -> BindingQuality {
  BindingQuality quality;
  quality.total_bindings = bindings.size();

  std::unordered_set<std::string> visiting;
  std::unordered_map<std::string, bool> resolved_cache;
  const std::unordered_set<std::string> no_unbound;

  for (const auto& [type_var, bound_type] : bindings) {
    (void)type_var;
    if (is_type_resolved(bound_type, bindings, no_unbound, visiting, resolved_cache)) { ++quality.resolved_bindings; }
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
  if (candidate_complexity != incumbent_complexity) { return candidate_complexity < incumbent_complexity; }
  return false;
}

auto merge_binding_types(const Type& lhs, const Type& rhs) -> Type {
  if (is_consistent(lhs, rhs) && is_consistent(rhs, lhs)) { return normalize_type(lhs); }

  Type merged;
  merged.kind = TypeKind::kUnion;
  merged.union_members = {lhs, rhs};
  return normalize_type(std::move(merged));
}

auto union_member_requires_coverage(const Type& member) -> bool { return member.kind != TypeKind::kTypeVar; }

auto fixed_union_members_are_covered(const std::vector<Type>& expected_members, const std::vector<Type>& actual_members,
                                     const TypeBindings& bindings) -> bool {
  for (const auto& expected_member : expected_members) {
    if (!union_member_requires_coverage(expected_member)) { continue; }

    const auto instantiated_expected = instantiate_generic_type(expected_member, bindings);
    const bool covered = std::ranges::any_of(actual_members, [&](const Type& actual_member) -> bool {
      return is_consistent(instantiated_expected, actual_member);
    });
    if (!covered) { return false; }
  }
  return true;
}

auto bind_union_members(const std::vector<Type>& expected_members, const std::vector<Type>& actual_members,
                        const std::size_t actual_index, const TypeBindings& base_bindings, TypeBindings& bindings)
    -> bool {
  if (actual_index >= actual_members.size()) { return true; }

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

  if (!best_bindings.has_value()) { return false; }
  bindings = std::move(*best_bindings);
  return true;
}

auto instantiate_generic_type(const Type& type, const TypeBindings& bindings) -> Type {
  if (type.kind == TypeKind::kTypeVar) {
    if (const auto it = bindings.find(type.nominal_name); it != bindings.end()) { return normalize_type(it->second); }
    return Type{.kind = TypeKind::kAny};
  }

  Type out = type;
  for (auto& item : out.items) {
    const bool variadic = item.variadic;
    item = instantiate_generic_type(item, bindings);
    item.variadic = variadic;
  }
  for (auto& member : out.union_members) { member = instantiate_generic_type(member, bindings); }
  for (auto& arg : out.applied_args) { arg = instantiate_generic_type(arg, bindings); }
  for (auto& param : out.function_params) {
    const bool variadic = param.variadic;
    param = instantiate_generic_type(param, bindings);
    param.variadic = variadic;
  }
  if (out.function_return.has_value()) {
    out.function_return = make_box<Type>(instantiate_generic_type(**out.function_return, bindings));
  }
  return normalize_type(std::move(out));
}

auto bind_function_type(const Type& expected, const Type& actual, TypeBindings& bindings) -> bool {
  if (actual.kind != TypeKind::kFunction) { return false; }
  if (expected.function_params.size() != actual.function_params.size()) { return false; }

  for (std::size_t i = 0; i < expected.function_params.size(); ++i) {
    if (expected.function_params[i].variadic != actual.function_params[i].variadic) { return false; }
    if (!bind_generic_type(expected.function_params[i], actual.function_params[i], bindings)) { return false; }
  }

  if (expected.function_return.has_value() != actual.function_return.has_value()) { return false; }
  if (!expected.function_return.has_value()) { return true; }
  return bind_generic_type(**expected.function_return, **actual.function_return, bindings);
}

auto bind_generic_type(const Type& expected, const Type& actual, TypeBindings& bindings) -> bool {
  if (expected.kind == TypeKind::kTypeVar) {
    if (const auto it = bindings.find(expected.nominal_name); it != bindings.end()) {
      return is_consistent(it->second, actual) && is_consistent(actual, it->second);
    }
    bindings.insert_or_assign(expected.nominal_name, normalize_type(actual));
    return true;
  }

  if (expected.kind == TypeKind::kTuple) {
    if (actual.kind != TypeKind::kTuple) { return false; }
    const auto variadic_it =
        std::ranges::find_if(expected.items, [](const Type& item) -> bool { return item.variadic; });
    if (variadic_it == expected.items.end()) {
      if (expected.items.size() != actual.items.size()) { return false; }
      for (std::size_t i = 0; i < expected.items.size(); ++i) {
        if (!bind_generic_type(expected.items[i], actual.items[i], bindings)) { return false; }
      }
      return true;
    }

    const auto variadic_index = static_cast<std::size_t>(std::distance(expected.items.begin(), variadic_it));
    if (variadic_index + 1U != expected.items.size() || actual.items.size() < variadic_index) { return false; }
    for (std::size_t i = 0; i < variadic_index; ++i) {
      if (!bind_generic_type(expected.items[i], actual.items[i], bindings)) { return false; }
    }
    Type repeated_expected = expected.items.back();
    repeated_expected.variadic = false;
    for (auto i = variadic_index; i < actual.items.size(); ++i) {
      if (!bind_generic_type(repeated_expected, actual.items[i], bindings)) { return false; }
    }
    return true;
  }

  if (expected.kind == TypeKind::kApplied) {
    if (actual.kind != TypeKind::kApplied || expected.applied_name != actual.applied_name ||
        expected.applied_args.size() != actual.applied_args.size()) {
      return false;
    }
    for (std::size_t i = 0; i < expected.applied_args.size(); ++i) {
      if (!bind_generic_type(expected.applied_args[i], actual.applied_args[i], bindings)) { return false; }
    }
    return true;
  }

  if (expected.kind == TypeKind::kFunction) { return bind_function_type(expected, actual, bindings); }

  if (expected.kind == TypeKind::kUnion) {
    if (actual.kind == TypeKind::kUnion) {
      if (expected.union_members.empty() || actual.union_members.empty()) { return false; }
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
    if (!best_bindings.has_value()) { return false; }
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

auto check_invocation(const std::string& full_name, const std::optional<diag::SourceSpan>& span, const FunctionSig& sig,
                      const std::vector<Type>& args, const std::unordered_set<std::string>& allowed_unbound)
    -> tl::expected<Type, type_check::AnalysisError> {
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

  TypeBindings generic_bindings;
  const bool is_generic_call = !sig.generic_params.empty();

  for (std::size_t i = 0; i < fixed_count; ++i) {
    if (is_generic_call) {
      if (!bind_generic_type(sig.params[i].type, args[i], generic_bindings)) {
        return tl::unexpected(make_error(
            "Type mismatch in call target arguments.",
            generic_arg_mismatch_hint(full_name, i, sig.params[i].type, false, generic_bindings, args[i]), span));
      }

      const Type instantiated = instantiate_generic_type(sig.params[i].type, generic_bindings);
      if (!is_consistent(instantiated, args[i])) {
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

  if (std::string builtin_contract_error; !validate_builtin_contract(full_name, args, builtin_contract_error)) {
    return tl::unexpected(make_error("Type mismatch in call target arguments.", builtin_contract_error, span));
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
                                 const std::unordered_set<std::string>& allowed_unbound)
    -> tl::expected<ResolvedInvocation, type_check::AnalysisError> {
  std::vector<const FunctionSig*> shape_matches;
  shape_matches.reserve(overloads.size());
  for (const auto& overload : overloads) {
    if (call_shape_matches(overload, args.size())) { shape_matches.push_back(&overload); }
  }

  if (shape_matches.empty()) {
    if (overloads.size() == 1U) {
      if (auto checked = check_invocation(full_name, span, overloads.front(), args, allowed_unbound);
          !checked.has_value()) {
        return tl::unexpected(checked.error());
      }
    }
    return tl::unexpected(make_error("Type mismatch in call target arguments.",
                                     std::format("{} has no overload that accepts {} argument(s). Candidates: {}.",
                                                 full_name, args.size(), overload_candidate_list(full_name, overloads)),
                                     span));
  }

  std::vector<Type> matched_returns;
  matched_returns.reserve(shape_matches.size());
  std::optional<type_check::AnalysisError> first_error;
  for (const auto* overload : shape_matches) {
    auto checked = check_invocation(full_name, span, *overload, args, allowed_unbound);
    if (checked.has_value()) {
      matched_returns.push_back(*checked);
      continue;
    }
    if (!first_error.has_value()) { first_error = checked.error(); }
  }

  std::optional<std::string> matched_symbol_key;
  for (const auto* overload : shape_matches) {
    if (auto checked = check_invocation(full_name, span, *overload, args, allowed_unbound); !checked.has_value()) {
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
                               full_name, overload_candidate_list(full_name, overloads)),
                   span));
  }

  if (shape_matches.size() == 1U && first_error.has_value()) { return tl::unexpected(*first_error); }

  return tl::unexpected(make_error("Type mismatch in call target arguments.",
                                   std::format("No overload of {} matches the provided argument types. Candidates: {}.",
                                               full_name, overload_candidate_list(full_name, overloads)),
                                   span));
}

}  // namespace fleaux::frontend::type_system::detail
