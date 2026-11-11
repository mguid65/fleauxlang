#include "fleaux/frontend/type_system/type.hpp"

#include <algorithm>

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

auto from_ir_type(const ir::IRSimpleType& type) -> Type {
  if (!type.alternative_types.empty() || !type.alternatives.empty()) {
    Type out;
    out.kind = TypeKind::kUnion;

    if (!type.alternative_types.empty()) {
      out.union_members.reserve(type.alternative_types.size());
      for (const auto& alt : type.alternative_types) {
        out.union_members.push_back(from_ir_type(alt));
      }
      return normalize_type(std::move(out));
    }

    out.union_members.reserve(type.alternatives.size());
    for (const auto& alt_name : type.alternatives) {
      out.union_members.push_back(from_name(alt_name));
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
    case TypeKind::kAny:
    case TypeKind::kUnknown:
    case TypeKind::kNever:
      return true;
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
  if (expected.kind == TypeKind::kAny || actual.kind == TypeKind::kAny) {
    return true;
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
