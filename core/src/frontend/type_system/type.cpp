#include "fleaux/frontend/type_system/type.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <format>
#include <optional>
#include <unordered_map>

#include "fleaux/frontend/type_system/detail/type_format.hpp"

namespace fleaux::frontend::type_system {
namespace {

auto from_name(const std::string& name) -> Type {
  if (name == "Never") {
    return make_type(TypeKind::kNever);
  }
  if (name == "Any") {
    return make_type(TypeKind::kAny);
  }
  if (name == "Int64") {
    return make_type(TypeKind::kInt64);
  }
  if (name == "UInt64") {
    return make_type(TypeKind::kUInt64);
  }
  if (name == "Float64") {
    return make_type(TypeKind::kFloat64);
  }
  if (name == "String") {
    return make_type(TypeKind::kString);
  }
  if (name == "Bool") {
    return make_type(TypeKind::kBool);
  }
  if (name == "Null") {
    return make_type(TypeKind::kNull);
  }
  if (name == "Tuple") {
    return make_type(TypeKind::kTuple);
  }
  if (name == "Function") {
    return make_type(TypeKind::kFunction);
  }
  return make_nominal_type(name);
}

auto type_sort_key(const Type& type) -> std::string {
  switch (type.kind) {
    case TypeKind::kUnknown:
      return "0:Unknown";
    case TypeKind::kNever:
      return "1:Never";
    case TypeKind::kAny:
      return "2:Any";
    case TypeKind::kInt64:
      return "3:Int64";
    case TypeKind::kUInt64:
      return "4:UInt64";
    case TypeKind::kFloat64:
      return "5:Float64";
    case TypeKind::kString:
      return "6:String";
    case TypeKind::kBool:
      return "7:Bool";
    case TypeKind::kNull:
      return "8:Null";
    case TypeKind::kNominal:
      return "9:N:" + type.nominal_name;
    case TypeKind::kTypeVar:
      return "10:V:" + type.nominal_name;
    case TypeKind::kApplied: {
      std::string key = "11:A:" + type.applied_name + "(";
      detail::append_joined_fragments(key, type.applied_args, ",", [](const Type& arg) { return type_sort_key(arg); });
      key += ")";
      return key;
    }
    case TypeKind::kTuple: {
      std::string key = "12:T(";
      detail::append_joined_type_fragments(
          key, type.items, ",", [](const Type& item) { return type_sort_key(item); },
          [](const Type& item) { return item.variadic; });
      key += ")";
      return key;
    }
    case TypeKind::kUnion: {
      std::string key = "13:U(";
      detail::append_joined_fragments(key, type.union_members, "|",
                                      [](const Type& member) { return type_sort_key(member); });
      key += ")";
      return key;
    }
    case TypeKind::kFunction: {
      std::string key = "14:F(";
      detail::append_joined_type_fragments(
          key, type.function_params, ",", [](const Type& param) { return type_sort_key(param); },
          [](const Type& param) { return param.variadic; });
      key += ")->";
      if (type.function_return.has_value()) {
        key += type_sort_key(*type.function_return);
      } else {
        key += "Any";
      }
      return key;
    }
    default:
      return "99:Unknown";
  }
}

auto push_union_member(std::vector<Type>& out, Type member) -> void {
  if (member.kind == TypeKind::kUnion) {
    for (auto& nested : member.union_members) {
      push_union_member(out, std::move(nested));
    }
    return;
  }
  out.push_back(std::move(member));
}

}  // namespace

auto normalize_type(Type type) -> Type {
  for (auto& item : type.items) {
    const bool variadic = item.variadic;
    item = normalize_type(std::move(item));
    item.variadic = variadic;
  }

  for (auto& arg : type.applied_args) {
    arg = normalize_type(std::move(arg));
  }

  for (auto& param : type.function_params) {
    const bool variadic = param.variadic;
    param = normalize_type(std::move(param));
    param.variadic = variadic;
  }

  if (type.function_return.has_value()) {
    type.function_return = common::make_indirect_optional<Type>(normalize_type(std::move(*type.function_return)));
  }

  if (type.kind != TypeKind::kUnion) {
    return type;
  }

  std::vector<Type> normalized_members;
  for (auto& member : type.union_members) {
    push_union_member(normalized_members, normalize_type(std::move(member)));
  }

  // Never has no values, so it contributes nothing to a union: T | Never is T.
  std::erase_if(normalized_members, [](const Type& member) -> bool { return member.kind == TypeKind::kNever; });
  if (normalized_members.empty()) {
    Type never = make_type(TypeKind::kNever);
    never.variadic = type.variadic;
    return never;
  }

  std::ranges::sort(normalized_members,
                    [](const Type& lhs, const Type& rhs) -> bool { return type_sort_key(lhs) < type_sort_key(rhs); });

  std::vector<Type> deduped;
  std::string previous_key;
  for (auto& member : normalized_members) {
    const auto key = type_sort_key(member);
    if (!deduped.empty() && key == previous_key) {
      continue;
    }
    previous_key = key;
    deduped.push_back(std::move(member));
  }

  if (deduped.size() == 1U) {
    Type single = std::move(deduped.front());
    single.variadic = single.variadic || type.variadic;
    return single;
  }
  type.union_members = std::move(deduped);
  return type;
}

auto param_local_type(const Type& declared, const bool variadic) -> Type {
  if (!variadic) {
    return declared;
  }
  Type item = declared;
  item.variadic = true;
  Type tuple = make_type(TypeKind::kTuple);
  tuple.items.push_back(std::move(item));
  return normalize_type(std::move(tuple));
}

namespace {

using TypeVarNames = std::unordered_map<std::string, std::string>;
using TypeVarBindings = std::unordered_map<std::string, Type>;

template <typename Visit>
auto map_type_children(Type out, Visit&& visit) -> Type {
  for (auto& item : out.items) {
    const bool variadic = item.variadic;
    item = visit(item);
    item.variadic = variadic;
  }
  for (auto& member : out.union_members) {
    member = visit(member);
  }
  for (auto& arg : out.applied_args) {
    arg = visit(arg);
  }
  for (auto& param : out.function_params) {
    const bool variadic = param.variadic;
    param = visit(param);
    param.variadic = variadic;
  }
  if (out.function_return.has_value()) {
    out.function_return = common::make_indirect_optional<Type>(visit(*out.function_return));
  }
  return out;
}

auto rename_type_vars(const Type& type, const TypeVarNames& names) -> Type {
  if (type.kind == TypeKind::kTypeVar) {
    Type renamed = type;
    if (const auto it = names.find(type.nominal_name); it != names.end()) {
      renamed.nominal_name = it->second;
    }
    return renamed;
  }
  return map_type_children(type, [&](const Type& child) { return rename_type_vars(child, names); });
}

auto substitute_type_vars(const Type& type, const TypeVarBindings& bindings) -> Type {
  if (type.kind == TypeKind::kTypeVar) {
    if (const auto it = bindings.find(type.nominal_name); it != bindings.end()) {
      Type bound = it->second;
      bound.variadic = type.variadic;
      return bound;
    }
    return type;
  }
  return normalize_type(map_type_children(type, [&](const Type& child) { return substitute_type_vars(child, bindings); }));
}

auto find_type_var_bound(const Type& type, const std::string& name) -> std::optional<Type> {
  if (type.kind == TypeKind::kTypeVar && type.nominal_name == name) {
    return type.type_var_bound.has_value() ? std::optional<Type>{*type.type_var_bound} : std::nullopt;
  }
  const auto search = [&](const std::vector<Type>& children) -> std::optional<Type> {
    for (const auto& child : children) {
      if (auto found = find_type_var_bound(child, name)) {
        return found;
      }
    }
    return std::nullopt;
  };
  if (auto found = search(type.items)) return found;
  if (auto found = search(type.union_members)) return found;
  if (auto found = search(type.applied_args)) return found;
  if (auto found = search(type.function_params)) return found;
  if (type.function_return.has_value()) {
    return find_type_var_bound(*type.function_return, name);
  }
  return std::nullopt;
}

// Collects bindings for the open type variables in `pattern` from the matching parts of `concrete`. Parts that do
// not line up are left alone: the consistency check on the instantiated type decides whether they fit.
auto collect_open_bindings(const Type& pattern, const Type& concrete, const std::vector<std::string>& open,
                           TypeVarBindings& bindings) -> bool {
  if (pattern.kind == TypeKind::kTypeVar && std::ranges::find(open, pattern.nominal_name) != open.end()) {
    Type value = concrete;
    value.variadic = false;
    if (const auto it = bindings.find(pattern.nominal_name); it != bindings.end()) {
      return is_consistent(it->second, value) && is_consistent(value, it->second);
    }
    bindings.insert_or_assign(pattern.nominal_name, std::move(value));
    return true;
  }
  if (pattern.kind != concrete.kind) {
    return true;
  }
  const auto pairwise = [&](const std::vector<Type>& lhs, const std::vector<Type>& rhs) -> bool {
    for (std::size_t index = 0; index < std::min(lhs.size(), rhs.size()); ++index) {
      if (!collect_open_bindings(lhs[index], rhs[index], open, bindings)) {
        return false;
      }
    }
    return true;
  };
  if (!pairwise(pattern.items, concrete.items) || !pairwise(pattern.applied_args, concrete.applied_args) ||
      !pairwise(pattern.function_params, concrete.function_params)) {
    return false;
  }
  if (pattern.union_members.size() == concrete.union_members.size() &&
      !pairwise(pattern.union_members, concrete.union_members)) {
    return false;
  }
  if (pattern.function_return.has_value() && concrete.function_return.has_value()) {
    return collect_open_bindings(*pattern.function_return, *concrete.function_return, open, bindings);
  }
  return true;
}

}  // namespace

auto make_open_function_type(const Type& function_type, const std::vector<std::string>& generic_params,
                             std::string name) -> Type {
  static std::atomic<std::uint64_t> next_open_id{0};
  const auto open_id = next_open_id.fetch_add(1U, std::memory_order_relaxed);
  TypeVarNames names;
  Type open = function_type;
  open.function_generic_params.clear();
  for (const auto& param : generic_params) {
    // The quote cannot appear in a source identifier, so a fresh name never clashes with a declared one.
    const std::string fresh = std::format("{}'{}", param, open_id);
    names.insert_or_assign(param, fresh);
    open.function_generic_params.push_back(fresh);
  }
  auto generic_params_of_open = open.function_generic_params;
  open = rename_type_vars(open, names);
  open.function_generic_params = std::move(generic_params_of_open);
  open.open_function_name = std::move(name);
  return open;
}

auto instantiate_open_function(const Type& open, const Type& expected) -> std::optional<Type> {
  if (expected.kind != TypeKind::kFunction || !expected.function_return.has_value() || !open.function_return.has_value() ||
      expected.function_params.size() != open.function_params.size()) {
    return std::nullopt;
  }

  // Type variables are taken from the parameters: the expected type says what the function will be called with.
  TypeVarBindings bindings;
  for (std::size_t index = 0; index < open.function_params.size(); ++index) {
    if (!collect_open_bindings(open.function_params[index], expected.function_params[index],
                               open.function_generic_params, bindings)) {
      return std::nullopt;
    }
  }
  for (const auto& name : open.function_generic_params) {
    const auto binding = bindings.find(name);
    if (binding == bindings.end()) {
      return std::nullopt;
    }
    if (const auto bound = find_type_var_bound(open, name); bound.has_value() &&
                                                            !satisfies_generic_bound(binding->second, *bound)) {
      return std::nullopt;
    }
  }

  Type instantiated = substitute_type_vars(open, bindings);
  instantiated.function_generic_params.clear();
  instantiated.open_function_name.clear();
  return instantiated;
}

auto open_function_signature(const Type& open, const GenericScope& scope) -> OpenFunctionSignature {
  OpenFunctionSignature signature;
  TypeVarNames names;
  for (const auto& fresh : open.function_generic_params) {
    const std::string declared = fresh.substr(0, fresh.find('\''));
    const std::string& name = scope.contains(declared) ? fresh : declared;
    names.insert_or_assign(fresh, name);
    signature.generic_params.push_back(name);
    if (auto bound = find_type_var_bound(open, fresh); bound.has_value()) {
      signature.generic_bounds.insert_or_assign(name, std::move(*bound));
    }
  }
  signature.type = rename_type_vars(open, names);
  signature.type.function_generic_params.clear();
  signature.type.open_function_name.clear();
  return signature;
}

auto from_ir_type(const ir::IRSimpleType& type) -> Type {
  if (!type.alternative_types.empty()) {
    Type out;
    out.kind = TypeKind::kUnion;
    out.union_members.reserve(type.alternative_types.size());
    for (const auto& alt : type.alternative_types) {
      out.union_members.push_back(from_ir_type(alt));
    }
    return normalize_type(std::move(out));
  }

  if (type.name == "Tuple" || !type.tuple_items.empty()) {
    Type out;
    out.kind = TypeKind::kTuple;
    out.variadic = type.variadic;
    out.items.reserve(type.tuple_items.size());
    for (const auto& item : type.tuple_items) {
      out.items.push_back(from_ir_type(item));
    }
    return normalize_type(std::move(out));
  }

  if (type.function_sig.has_value()) {
    Type out;
    out.kind = TypeKind::kFunction;
    out.function_params.reserve(type.function_sig->param_types.size());
    for (const auto& param_type : type.function_sig->param_types) {
      out.function_params.push_back(from_ir_type(param_type));
    }
    out.function_return = common::make_indirect_optional<Type>(from_ir_type(*type.function_sig->return_type));
    out.variadic = type.variadic;
    return normalize_type(std::move(out));
  }

  if (!type.type_args.empty()) {
    Type out;
    out.kind = TypeKind::kApplied;
    out.applied_name = type.name;
    out.applied_args.reserve(type.type_args.size());
    for (const auto& arg : type.type_args) {
      out.applied_args.push_back(from_ir_type(arg));
    }
    out.variadic = type.variadic;
    return normalize_type(std::move(out));
  }

  Type out = from_name(type.name);
  out.variadic = type.variadic;
  return normalize_type(std::move(out));
}

auto to_ir_type(const Type& type) -> ir::IRSimpleType {
  ir::IRSimpleType out;
  out.variadic = type.variadic;
  const auto convert_all = [](const std::vector<Type>& types) {
    std::vector<ir::IRSimpleType> converted;
    converted.reserve(types.size());
    for (const auto& item : types) {
      converted.push_back(to_ir_type(item));
    }
    return converted;
  };
  switch (type.kind) {
    case TypeKind::kInt64:
      out.name = "Int64";
      break;
    case TypeKind::kUInt64:
      out.name = "UInt64";
      break;
    case TypeKind::kFloat64:
      out.name = "Float64";
      break;
    case TypeKind::kString:
      out.name = "String";
      break;
    case TypeKind::kBool:
      out.name = "Bool";
      break;
    case TypeKind::kNull:
      out.name = "Null";
      break;
    case TypeKind::kNever:
      out.name = "Never";
      break;
    case TypeKind::kNominal:
    case TypeKind::kTypeVar:
      out.name = type.nominal_name;
      break;
    case TypeKind::kTuple:
      out.name = "Tuple";
      out.tuple_items = convert_all(type.items);
      break;
    case TypeKind::kUnion:
      out.alternative_types = convert_all(type.union_members);
      out.name = out.alternative_types.empty() ? std::string{} : out.alternative_types.front().name;
      break;
    case TypeKind::kApplied:
      out.name = type.applied_name;
      out.type_args = convert_all(type.applied_args);
      break;
    case TypeKind::kFunction: {
      ir::IRSimpleType::FunctionSignature signature;
      signature.param_types = convert_all(type.function_params);
      signature.return_type = type.function_return.has_value()
                                  ? ir::IRSimpleTypeBox(std::in_place, to_ir_type(*type.function_return))
                                  : ir::IRSimpleTypeBox(std::in_place, ir::IRSimpleType{.name = "Any"});
      out.function_sig = std::move(signature);
      break;
    }
    default:
      out.name = "Any";
      break;
  }
  return out;
}

auto is_integer_like(const Type& type) -> bool {
  return type.kind == TypeKind::kInt64 || type.kind == TypeKind::kUInt64;
}

namespace {

auto are_union_types_consistent(const Type& expected, const Type& actual) -> bool {
  if (expected.union_members.empty() || actual.union_members.empty()) {
    return false;
  }

  return std::ranges::all_of(actual.union_members, [&](const Type& actual_member) -> bool {
    return std::ranges::any_of(expected.union_members, [&](const Type& expected_member) -> bool {
      return is_consistent(expected_member, actual_member);
    });
  });
}

auto is_expected_union_consistent(const Type& expected, const Type& actual) -> bool {
  if (expected.union_members.empty()) {
    return false;
  }

  return std::ranges::any_of(expected.union_members,
                             [&](const Type& member) -> bool { return is_consistent(member, actual); });
}

auto is_actual_union_consistent(const Type& expected, const Type& actual) -> bool {
  if (actual.union_members.empty()) {
    return false;
  }

  return std::ranges::all_of(actual.union_members,
                             [&](const Type& member) -> bool { return is_consistent(expected, member); });
}

auto are_tuple_types_consistent(const Type& expected, const Type& actual) -> bool {
  if (actual.kind != TypeKind::kTuple) {
    return false;
  }

  if (const auto variadic_it =
          std::ranges::find_if(expected.items, [](const Type& item) -> bool { return item.variadic; });
      variadic_it != expected.items.end()) {
    const auto variadic_index = static_cast<std::size_t>(std::distance(expected.items.begin(), variadic_it));
    if (variadic_index + 1U != expected.items.size()) {
      return false;
    }
    if (actual.items.size() < variadic_index) {
      return false;
    }

    for (std::size_t i = 0; i < variadic_index; ++i) {
      if (!is_consistent(expected.items[i], actual.items[i])) {
        return false;
      }
    }

    Type repeated_expected = expected.items.back();
    repeated_expected.variadic = false;
    for (std::size_t i = variadic_index; i < actual.items.size(); ++i) {
      if (!is_consistent(repeated_expected, actual.items[i])) {
        return false;
      }
    }
    return true;
  }

  if (expected.items.size() != actual.items.size()) {
    return false;
  }

  return std::equal(expected.items.begin(), expected.items.end(), actual.items.begin(),
                    [](const Type& lhs, const Type& rhs) -> bool { return is_consistent(lhs, rhs); });
}

auto are_applied_types_consistent(const Type& expected, const Type& actual) -> bool {
  if (actual.kind != TypeKind::kApplied) {
    return false;
  }
  if (expected.applied_name != actual.applied_name) {
    return false;
  }
  if (expected.applied_args.size() != actual.applied_args.size()) {
    return false;
  }

  for (std::size_t i = 0; i < expected.applied_args.size(); ++i) {
    if (!is_consistent(expected.applied_args[i], actual.applied_args[i])) {
      return false;
    }
  }
  return true;
}

auto are_function_types_consistent(const Type& expected, const Type& actual) -> bool {
  if (actual.kind != TypeKind::kFunction) {
    return false;
  }
  if (is_open_function(actual)) {
    const auto instantiated = instantiate_open_function(actual, expected);
    return instantiated.has_value() && are_function_types_consistent(expected, *instantiated);
  }

  if (!expected.function_return.has_value() || !actual.function_return.has_value()) {
    return true;
  }

  if (expected.function_params.size() != actual.function_params.size()) {
    return false;
  }

  for (std::size_t i = 0; i < expected.function_params.size(); ++i) {
    if (expected.function_params[i].variadic != actual.function_params[i].variadic) {
      return false;
    }
    // Parameters are contravariant: the actual function must accept every argument the expected type allows.
    if (!is_consistent(actual.function_params[i], expected.function_params[i])) {
      return false;
    }
  }

  // Returns are covariant: every result of the actual function must fit the expected return type.
  return is_consistent(*expected.function_return, *actual.function_return);
}

}  // namespace

namespace {

auto bound_alternatives(const Type& bound) -> std::vector<Type> {
  if (bound.kind == TypeKind::kUnion) {
    return bound.union_members;
  }
  return {bound};
}

auto is_bound_alternative(const Type& candidate, const Type& alternative) -> bool {
  return is_consistent(alternative, candidate) && is_consistent(candidate, alternative);
}

}  // namespace

auto satisfies_generic_bound(const Type& actual, const Type& bound) -> bool {
  switch (actual.kind) {
    case TypeKind::kUnknown:
    case TypeKind::kNever:
      return true;
    case TypeKind::kAny:
    case TypeKind::kUnion:
      return false;
    case TypeKind::kTypeVar: {
      if (!actual.type_var_bound.has_value()) {
        return false;
      }
      const auto alternatives = bound_alternatives(bound);
      return std::ranges::all_of(bound_alternatives(*actual.type_var_bound), [&](const Type& own) -> bool {
        return std::ranges::any_of(alternatives,
                                   [&](const Type& alternative) { return is_bound_alternative(own, alternative); });
      });
    }
    default: {
      const auto alternatives = bound_alternatives(bound);
      return std::ranges::any_of(alternatives,
                                 [&](const Type& alternative) { return is_bound_alternative(actual, alternative); });
    }
  }
}

auto is_consistent(const Type& expected, const Type& actual) -> bool {
  if (actual.kind == TypeKind::kNever) {
    return true;
  }
  // An open function has no type until it is instantiated from a function type, so even Any cannot take it.
  if (is_open_function(actual) && expected.kind != TypeKind::kFunction && expected.kind != TypeKind::kUnion) {
    return false;
  }
  // Any is the top type: every value fits an Any slot, but an Any value fits only an Any slot. Using it as
  // anything more specific needs Std.Cast, which is checked at runtime.
  if (expected.kind == TypeKind::kAny) {
    return true;
  }
  if (actual.kind == TypeKind::kAny) {
    return false;
  }

  if (expected.kind == TypeKind::kNever) {
    return actual.kind == TypeKind::kNever;
  }

  if (expected.kind == TypeKind::kUnion && actual.kind == TypeKind::kUnion) {
    return are_union_types_consistent(expected, actual);
  }

  if (expected.kind == TypeKind::kUnion) {
    return is_expected_union_consistent(expected, actual);
  }

  if (actual.kind == TypeKind::kUnion) {
    return is_actual_union_consistent(expected, actual);
  }

  if (expected.kind == TypeKind::kTuple) {
    return are_tuple_types_consistent(expected, actual);
  }

  if (expected.kind == TypeKind::kApplied) {
    return are_applied_types_consistent(expected, actual);
  }

  if (expected.kind == TypeKind::kTypeVar || actual.kind == TypeKind::kTypeVar) {
    return expected.kind == actual.kind && expected.nominal_name == actual.nominal_name;
  }

  if (expected.kind == TypeKind::kNominal || actual.kind == TypeKind::kNominal) {
    return expected.kind == actual.kind && expected.nominal_name == actual.nominal_name;
  }

  if (expected.kind == TypeKind::kFunction) {
    return are_function_types_consistent(expected, actual);
  }

  return expected.kind == actual.kind;
}

}  // namespace fleaux::frontend::type_system
