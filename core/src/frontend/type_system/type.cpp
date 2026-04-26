#include "fleaux/frontend/type_system/type.hpp"

#include <algorithm>

namespace fleaux::frontend::type_system {
namespace {

auto from_name(const std::string& name) -> Type {
  if (name == "Never") { return Type{.kind = TypeKind::kNever}; }
  if (name == "Any") { return Type{.kind = TypeKind::kAny}; }
  if (name == "Int64") { return Type{.kind = TypeKind::kInt64}; }
  if (name == "UInt64") { return Type{.kind = TypeKind::kUInt64}; }
  if (name == "Float64") { return Type{.kind = TypeKind::kFloat64}; }
  if (name == "String") { return Type{.kind = TypeKind::kString}; }
  if (name == "Bool") { return Type{.kind = TypeKind::kBool}; }
  if (name == "Null") { return Type{.kind = TypeKind::kNull}; }
  if (name == "Tuple") { return Type{.kind = TypeKind::kTuple}; }
  if (name == "Function") { return Type{.kind = TypeKind::kFunction}; }
  return Type{.kind = TypeKind::kNominal, .nominal_name = name};
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
      for (std::size_t i = 0; i < type.applied_args.size(); ++i) {
        if (i > 0U) { key += ","; }
        key += type_sort_key(type.applied_args[i]);
      }
      key += ")";
      return key;
    }
    case TypeKind::kTuple: {
      std::string key = "12:T(";
      for (std::size_t i = 0; i < type.items.size(); ++i) {
        if (i > 0U) { key += ","; }
        key += type_sort_key(type.items[i]);
        if (type.items[i].variadic) { key += "..."; }
      }
      key += ")";
      return key;
    }
    case TypeKind::kUnion: {
      std::string key = "13:U(";
      for (std::size_t i = 0; i < type.union_members.size(); ++i) {
        if (i > 0U) { key += "|"; }
        key += type_sort_key(type.union_members[i]);
      }
      key += ")";
      return key;
    }
    case TypeKind::kFunction: {
      std::string key = "14:F(";
      for (std::size_t i = 0; i < type.function_params.size(); ++i) {
        if (i > 0U) { key += ","; }
        key += type_sort_key(type.function_params[i]);
        if (type.function_params[i].variadic) { key += "..."; }
      }
      key += ")->";
      if (type.function_return.has_value()) {
        key += type_sort_key(**type.function_return);
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
    for (auto& nested : member.union_members) { push_union_member(out, std::move(nested)); }
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

  for (auto& arg : type.applied_args) { arg = normalize_type(std::move(arg)); }

  for (auto& param : type.function_params) {
    const bool variadic = param.variadic;
    param = normalize_type(std::move(param));
    param.variadic = variadic;
  }

  if (type.function_return.has_value()) {
    type.function_return = make_box<Type>(normalize_type(std::move(**type.function_return)));
  }

  if (type.kind != TypeKind::kUnion) { return type; }

  std::vector<Type> normalized_members;
  for (auto& member : type.union_members) { push_union_member(normalized_members, normalize_type(std::move(member))); }

  std::ranges::sort(normalized_members,
                    [](const Type& lhs, const Type& rhs) -> bool { return type_sort_key(lhs) < type_sort_key(rhs); });

  std::vector<Type> deduped;
  std::string previous_key;
  for (auto& member : normalized_members) {
    const auto key = type_sort_key(member);
    if (!deduped.empty() && key == previous_key) { continue; }
    previous_key = key;
    deduped.push_back(std::move(member));
  }

  if (deduped.size() == 1U) { return std::move(deduped.front()); }
  type.union_members = std::move(deduped);
  return type;
}

auto from_ir_type(const ir::IRSimpleType& type) -> Type {
  if (!type.alternative_types.empty() || !type.alternatives.empty()) {
    Type out;
    out.kind = TypeKind::kUnion;

    if (!type.alternative_types.empty()) {
      out.union_members.reserve(type.alternative_types.size());
      for (const auto& alt : type.alternative_types) { out.union_members.push_back(from_ir_type(alt)); }
      return normalize_type(std::move(out));
    }

    out.union_members.reserve(type.alternatives.size());
    for (const auto& alt_name : type.alternatives) { out.union_members.push_back(from_name(alt_name)); }
    return normalize_type(std::move(out));
  }

  if (type.name == "Tuple" || !type.tuple_items.empty()) {
    Type out;
    out.kind = TypeKind::kTuple;
    out.variadic = type.variadic;
    out.items.reserve(type.tuple_items.size());
    for (const auto& item : type.tuple_items) { out.items.push_back(from_ir_type(item)); }
    return normalize_type(std::move(out));
  }

  if (type.function_sig.has_value()) {
    Type out;
    out.kind = TypeKind::kFunction;
    out.function_params.reserve(type.function_sig->param_types.size());
    for (const auto& param_type : type.function_sig->param_types) {
      out.function_params.push_back(from_ir_type(param_type));
    }
    out.function_return = make_box<Type>(from_ir_type(*type.function_sig->return_type));
    return normalize_type(std::move(out));
  }

  if (!type.type_args.empty()) {
    Type out;
    out.kind = TypeKind::kApplied;
    out.applied_name = type.name;
    out.applied_args.reserve(type.type_args.size());
    for (const auto& arg : type.type_args) { out.applied_args.push_back(from_ir_type(arg)); }
    return normalize_type(std::move(out));
  }

  Type out = from_name(type.name);
  out.variadic = type.variadic;
  return normalize_type(std::move(out));
}

auto is_integer_like(const Type& type) -> bool {
  return type.kind == TypeKind::kInt64 || type.kind == TypeKind::kUInt64;
}

auto is_consistent(const Type& expected, const Type& actual) -> bool {
  if (actual.kind == TypeKind::kNever) { return true; }
  if (expected.kind == TypeKind::kAny || actual.kind == TypeKind::kAny) { return true; }

  if (expected.kind == TypeKind::kNever) { return actual.kind == TypeKind::kNever; }


  if (expected.kind == TypeKind::kUnion && actual.kind == TypeKind::kUnion) {
    if (expected.union_members.empty() || actual.union_members.empty()) { return false; }
    return std::ranges::all_of(actual.union_members, [&](const Type& actual_member) -> bool {
      return std::ranges::any_of(expected.union_members, [&](const Type& expected_member) -> bool {
        return is_consistent(expected_member, actual_member);
      });
    });
  }

  if (expected.kind == TypeKind::kUnion) {
    if (expected.union_members.empty()) { return false; }
    return std::ranges::any_of(expected.union_members,
                               [&](const Type& member) -> bool { return is_consistent(member, actual); });
  }

  if (actual.kind == TypeKind::kUnion) {
    if (actual.union_members.empty()) { return false; }
    return std::ranges::all_of(actual.union_members,
                               [&](const Type& member) -> bool { return is_consistent(expected, member); });
  }

  if (expected.kind == TypeKind::kTuple) {
    if (actual.kind != TypeKind::kTuple) { return false; }

    const auto variadic_it =
        std::ranges::find_if(expected.items, [](const Type& item) -> bool { return item.variadic; });
    if (variadic_it != expected.items.end()) {
      const auto variadic_index = static_cast<std::size_t>(std::distance(expected.items.begin(), variadic_it));
      if (variadic_index + 1U != expected.items.size()) { return false; }
      if (actual.items.size() < variadic_index) { return false; }

      for (std::size_t i = 0; i < variadic_index; ++i) {
        if (!is_consistent(expected.items[i], actual.items[i])) { return false; }
      }

      Type repeated_expected = expected.items.back();
      repeated_expected.variadic = false;
      for (std::size_t i = variadic_index; i < actual.items.size(); ++i) {
        if (!is_consistent(repeated_expected, actual.items[i])) { return false; }
      }
      return true;
    }

    if (expected.items.size() != actual.items.size()) { return false; }
    return std::equal(expected.items.begin(), expected.items.end(), actual.items.begin(),
                      [](const Type& lhs, const Type& rhs) -> bool { return is_consistent(lhs, rhs); });
  }

  if (expected.kind == TypeKind::kApplied) {
    if (actual.kind != TypeKind::kApplied) { return false; }
    if (expected.applied_name != actual.applied_name) { return false; }
    if (expected.applied_args.size() != actual.applied_args.size()) { return false; }

    for (std::size_t i = 0; i < expected.applied_args.size(); ++i) {
      if (!is_consistent(expected.applied_args[i], actual.applied_args[i])) { return false; }
    }
    return true;
  }

  if (expected.kind == TypeKind::kTypeVar || actual.kind == TypeKind::kTypeVar) {
    return expected.kind == actual.kind && expected.nominal_name == actual.nominal_name;
  }

  if (expected.kind == TypeKind::kNominal || actual.kind == TypeKind::kNominal) {
    return expected.kind == actual.kind && expected.nominal_name == actual.nominal_name;
  }

  if (expected.kind == TypeKind::kFunction) {
    if (actual.kind != TypeKind::kFunction) { return false; }

    if (!expected.function_return.has_value() || !actual.function_return.has_value()) { return true; }

    if (expected.function_params.size() != actual.function_params.size()) { return false; }

    for (std::size_t i = 0; i < expected.function_params.size(); ++i) {
      if (expected.function_params[i].variadic != actual.function_params[i].variadic) { return false; }
      if (!is_consistent(expected.function_params[i], actual.function_params[i]) ||
          !is_consistent(actual.function_params[i], expected.function_params[i])) {
        return false;
      }
    }

    return is_consistent(**expected.function_return, **actual.function_return) &&
           is_consistent(**actual.function_return, **expected.function_return);
  }

  return expected.kind == actual.kind;
}

}  // namespace fleaux::frontend::type_system
