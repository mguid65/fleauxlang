#include "fleaux/frontend/type_system/builtin_contracts.hpp"

#include <algorithm>
#include <unordered_set>

#include "fleaux/frontend/type_system/function_index.hpp"

namespace fleaux::frontend::type_system {
namespace {

// Inside a generic body a key type may still be a type parameter; the call that instantiates it is checked instead.
// Any is not deferred: a dictionary cannot have Any keys, because the runtime only stores scalar keys.
[[nodiscard]] auto is_deferred_dict_key_check_type(const Type& type) -> bool {
  return type.kind == TypeKind::kUnknown || type.kind == TypeKind::kTypeVar;
}

[[nodiscard]] auto is_scalar_dict_key_type_impl(const Type& type, const StrongTypeIndex& type_index,
                                                std::unordered_set<std::string>& visiting) -> bool {
  if (is_deferred_dict_key_check_type(type)) {
    return true;
  }

  switch (type.kind) {
    case TypeKind::kInt64:
    case TypeKind::kUInt64:
    case TypeKind::kFloat64:
    case TypeKind::kString:
    case TypeKind::kBool:
    case TypeKind::kNull:
    case TypeKind::kNever:
      return true;
    case TypeKind::kUnion:
      return std::ranges::all_of(type.union_members, [&](const Type& member) {
        return is_scalar_dict_key_type_impl(member, type_index, visiting);
      });
    case TypeKind::kNominal:
      if (visiting.contains(type.nominal_name)) {
        return false;
      }
      if (const auto decl = type_index.resolve_name(type.nominal_name); decl.has_value() && !decl->get().is_builtin) {
        visiting.insert(type.nominal_name);
        const bool result = is_scalar_dict_key_type_impl(decl->get().target_type, type_index, visiting);
        visiting.erase(type.nominal_name);
        return result;
      }
      return false;
    default:
      return false;
  }
}

[[nodiscard]] auto is_scalar_dict_key_type(const Type& type, const StrongTypeIndex& type_index) -> bool {
  std::unordered_set<std::string> visiting;
  return is_scalar_dict_key_type_impl(type, type_index, visiting);
}

auto check_scalar_dict_key_type(const Type& key_type, const StrongTypeIndex& type_index, std::string& error_message,
                                const std::string& full_name) -> bool {
  if (is_scalar_dict_key_type(key_type, type_index)) {
    return true;
  }

  error_message =
      full_name + " requires dictionary key types to be scalar values (Null, Bool, Int64, UInt64, Float64, or String).";
  return false;
}

auto check_dict_key_channel_arg(const std::vector<Type>& args, const std::size_t index,
                                const StrongTypeIndex& type_index, std::string& error_message,
                                const std::string& full_name) -> bool {
  if (index >= args.size()) {
    error_message = "Type mismatch in call target arguments.";
    return false;
  }
  const Type& arg = args[index];
  if (arg.kind == TypeKind::kUnknown || arg.kind == TypeKind::kTypeVar) {
    return true;
  }
  if (arg.kind != TypeKind::kApplied || arg.applied_name != "Dict" || arg.applied_args.size() != 2U) {
    error_message = full_name + " expects dictionary arguments.";
    return false;
  }
  return check_scalar_dict_key_type(arg.applied_args[0], type_index, error_message, full_name);
}

auto check_direct_dict_key_arg(const std::vector<Type>& args, const std::size_t index,
                               const StrongTypeIndex& type_index, std::string& error_message,
                               const std::string& full_name) -> bool {
  if (index >= args.size()) {
    error_message = "Type mismatch in call target arguments.";
    return false;
  }
  return check_scalar_dict_key_type(args[index], type_index, error_message, full_name);
}

auto check_parallel_options_arg(const std::vector<Type>& args, const std::size_t index,
                                const StrongTypeIndex& type_index, std::string& error_message,
                                const std::string& full_name) -> bool {
  (void)type_index;
  if (index >= args.size()) {
    error_message = "Type mismatch in call target arguments.";
    return false;
  }

  const Type& arg = args[index];
  if (arg.kind == TypeKind::kUnknown || arg.kind == TypeKind::kTypeVar) {
    return true;
  }
  if (arg.kind != TypeKind::kApplied || arg.applied_name != "Dict" || arg.applied_args.size() != 2U) {
    error_message = full_name + " expects options dictionaries.";
    return false;
  }
  if (arg.applied_args[0].kind != TypeKind::kString || arg.applied_args[1].kind != TypeKind::kAny) {
    error_message = full_name + " requires options of type Dict<String, Any>.";
    return false;
  }
  return true;
}

}  // namespace

auto validate_builtin_contract(const std::string& full_name, const std::vector<Type>& args,
                               const StrongTypeIndex& type_index, std::string& error_message) -> bool {
  // Parallel options are read as Dict<String, Any>; a narrower value type is rejected so options stay extensible.
  if (full_name == "Std.Parallel.WithOptions") {
    return check_parallel_options_arg(args, 2U, type_index, error_message, full_name);
  }
  // Dictionary keys must be scalar. Key types may be unions of scalars or strong types over scalars, which a generic
  // bound cannot express.
  if (full_name == "Std.Dict.Create" && args.size() == 1U) {
    return check_dict_key_channel_arg(args, 0U, type_index, error_message, full_name);
  }
  if (full_name == "Std.Dict.Set" || full_name == "Std.Dict.Get" || full_name == "Std.Dict.GetDefault" ||
      full_name == "Std.Dict.Contains" || full_name == "Std.Dict.Delete") {
    return check_dict_key_channel_arg(args, 0U, type_index, error_message, full_name) &&
           check_direct_dict_key_arg(args, 1U, type_index, error_message, full_name);
  }
  if (full_name == "Std.Dict.Merge") {
    return check_dict_key_channel_arg(args, 0U, type_index, error_message, full_name) &&
           check_dict_key_channel_arg(args, 1U, type_index, error_message, full_name);
  }
  if (full_name == "Std.Dict.Keys" || full_name == "Std.Dict.Values" || full_name == "Std.Dict.Entries" ||
      full_name == "Std.Dict.Clear" || full_name == "Std.Dict.Length") {
    return check_dict_key_channel_arg(args, 0U, type_index, error_message, full_name);
  }

  return true;
}

}  // namespace fleaux::frontend::type_system
