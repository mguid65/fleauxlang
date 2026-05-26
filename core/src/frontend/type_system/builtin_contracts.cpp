#include "fleaux/frontend/type_system/builtin_contracts.hpp"

#include <algorithm>
#include <initializer_list>
#include <unordered_set>

#include "fleaux/frontend/type_system/function_index.hpp"

namespace fleaux::frontend::type_system {
namespace {

enum NumericTypeMask : unsigned {
  kNumericNone = 0U,
  kNumericInt64 = 1U << 0U,
  kNumericUInt64 = 1U << 1U,
  kNumericFloat64 = 1U << 2U,
  kNumericDeferred = 1U << 3U,
  kNumericOther = 1U << 4U,
};

[[nodiscard]] auto is_deferred_integer_check_type(const Type& type) -> bool {
  return type.kind == TypeKind::kAny || type.kind == TypeKind::kUnknown;
}

[[nodiscard]] auto is_deferred_dict_key_check_type(const Type& type) -> bool {
  return type.kind == TypeKind::kAny || type.kind == TypeKind::kUnknown || type.kind == TypeKind::kTypeVar;
}

[[nodiscard]] auto numeric_type_mask(const Type& type) -> unsigned {
  switch (type.kind) {
    case TypeKind::kInt64:
      return kNumericInt64;
    case TypeKind::kUInt64:
      return kNumericUInt64;
    case TypeKind::kFloat64:
      return kNumericFloat64;
    case TypeKind::kAny:
    case TypeKind::kUnknown:
      return kNumericDeferred;
    case TypeKind::kUnion: {
      unsigned mask = kNumericNone;
      for (const auto& member : type.union_members) {
        mask |= numeric_type_mask(member);
      }
      return mask == kNumericNone ? kNumericOther : mask;
    }
    default:
      return kNumericOther;
  }
}

[[nodiscard]] auto is_numeric_cast_sensitive_builtin(const std::string& full_name) -> bool {
  return full_name == "Std.Add" || full_name == "Std.Subtract" || full_name == "Std.Multiply" ||
         full_name == "Std.Divide" || full_name == "Std.Mod" || full_name == "Std.Pow" ||
         full_name == "Std.GreaterThan" || full_name == "Std.LessThan" || full_name == "Std.GreaterOrEqual" ||
         full_name == "Std.LessOrEqual";
}

[[nodiscard]] auto is_numeric_arithmetic_builtin(const std::string& full_name) -> bool {
  return full_name == "Std.Add" || full_name == "Std.Subtract" || full_name == "Std.Multiply" ||
         full_name == "Std.Divide" || full_name == "Std.Mod" || full_name == "Std.Pow";
}

auto check_no_implicit_float_promotion(const std::vector<Type>& args, std::string& error_message,
                                       const std::string& full_name) -> bool {
  unsigned combined_mask = kNumericNone;
  for (const auto& arg : args) {
    combined_mask |= numeric_type_mask(arg);
  }

  if ((combined_mask & kNumericDeferred) != 0U) {
    return true;
  }
  if ((combined_mask & kNumericOther) != 0U) {
    return true;
  }

  const bool has_float = (combined_mask & kNumericFloat64) != 0U;
  if (const bool has_integer = (combined_mask & (kNumericInt64 | kNumericUInt64)) != 0U; !has_float || !has_integer) {
    return true;
  }

  error_message = full_name + " does not implicitly cast Int64 or UInt64 to Float64. Use Std.ToFloat64 explicitly.";
  return false;
}

auto check_integer_index_arg(const std::vector<Type>& args, const std::size_t index, std::string& error_message,
                             const std::string& full_name) -> bool {
  if (index >= args.size()) {
    error_message = "Type mismatch in call target arguments.";
    return false;
  }
  if (is_integer_like(args[index]) || is_deferred_integer_check_type(args[index])) {
    return true;
  }
  error_message = full_name + " expects Int64 or UInt64 for integer arguments.";
  return false;
}

auto check_integer_index_args(const std::vector<Type>& args, const std::initializer_list<std::size_t> indices,
                              std::string& error_message, const std::string& full_name) -> bool {
  for (const std::size_t index : indices) {
    if (!check_integer_index_arg(args, index, error_message, full_name)) {
      return false;
    }
  }
  return true;
}

auto check_integer_tuple_arg(const std::vector<Type>& args, const std::size_t index, std::string& error_message,
                             const std::string& full_name) -> bool {
  if (index >= args.size()) {
    error_message = "Type mismatch in call target arguments.";
    return false;
  }
  if (is_deferred_integer_check_type(args[index])) {
    return true;
  }
  if (args[index].kind != TypeKind::kTuple) {
    error_message = full_name + " expects tuple indices.";
    return false;
  }
  for (const auto& item : args[index].items) {
    if (!is_integer_like(item) && !is_deferred_integer_check_type(item)) {
      error_message = full_name + " expects all index tuple elements to be Int64 or UInt64.";
      return false;
    }
  }
  return true;
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
      return true;
    case TypeKind::kUnion:
      return std::ranges::all_of(type.union_members, [&](const Type& member) {
        return is_scalar_dict_key_type_impl(member, type_index, visiting);
      });
    case TypeKind::kNominal:
      if (is_builtin_opaque_nominal_type_name(type.nominal_name)) {
        return false;
      }
      if (visiting.contains(type.nominal_name)) {
        return false;
      }
      if (const auto decl = type_index.resolve_name(type.nominal_name); decl.has_value()) {
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

auto check_dict_key_channel_arg(const std::vector<Type>& args, const std::size_t index, const StrongTypeIndex& type_index,
                                std::string& error_message, const std::string& full_name) -> bool {
  if (index >= args.size()) {
    error_message = "Type mismatch in call target arguments.";
    return false;
  }
  const Type& arg = args[index];
  if (arg.kind == TypeKind::kAny || arg.kind == TypeKind::kUnknown || arg.kind == TypeKind::kTypeVar) {
    return true;
  }
  if (arg.kind != TypeKind::kApplied || arg.applied_name != "Dict" || arg.applied_args.size() != 2U) {
    error_message = full_name + " expects dictionary arguments.";
    return false;
  }
  return check_scalar_dict_key_type(arg.applied_args[0], type_index, error_message, full_name);
}

auto check_direct_dict_key_arg(const std::vector<Type>& args, const std::size_t index, const StrongTypeIndex& type_index,
                               std::string& error_message, const std::string& full_name) -> bool {
  if (index >= args.size()) {
    error_message = "Type mismatch in call target arguments.";
    return false;
  }
  return check_scalar_dict_key_type(args[index], type_index, error_message, full_name);
}

auto check_parallel_options_arg(const std::vector<Type>& args, const std::size_t index, const StrongTypeIndex& type_index,
                                std::string& error_message, const std::string& full_name) -> bool {
  (void)type_index;
  if (index >= args.size()) {
    error_message = "Type mismatch in call target arguments.";
    return false;
  }

  const Type& arg = args[index];
  if (arg.kind == TypeKind::kAny || arg.kind == TypeKind::kUnknown || arg.kind == TypeKind::kTypeVar) {
    return true;
  }
  if (arg.kind != TypeKind::kApplied || arg.applied_name != "Dict" || arg.applied_args.size() != 2U) {
    error_message = full_name + " expects options dictionaries.";
    return false;
  }
  if (arg.applied_args[0].kind != TypeKind::kString || arg.applied_args[1].kind != TypeKind::kAny) {
    error_message = full_name + " requires options of type Dict(String, Any).";
    return false;
  }
  return true;
}

}  // namespace

auto validate_builtin_contract(const std::string& full_name, const std::vector<Type>& args, const StrongTypeIndex& type_index,
                              std::string& error_message) -> bool {
  if (is_numeric_cast_sensitive_builtin(full_name) &&
      !check_no_implicit_float_promotion(args, error_message, full_name)) {
    return false;
  }
  if (full_name == "Std.Array.GetAt" || full_name == "Std.Array.SetAt" || full_name == "Std.Array.InsertAt" ||
      full_name == "Std.Array.RemoveAt" || full_name == "Std.Take" || full_name == "Std.Drop" ||
      full_name == "Std.ElementAt" || full_name == "Std.String.CharAt" ||
      full_name == "Std.File.ReadChunk" || full_name == "Std.Bit.ShiftLeft" || full_name == "Std.Bit.ShiftRight") {
    return check_integer_index_arg(args, 1U, error_message, full_name);
  }
  if (full_name == "Std.Random.Create") {
    return check_integer_index_arg(args, 0U, error_message, full_name);
  }
  if (full_name == "Std.Exit" && args.size() == 1U) {
    return check_integer_index_arg(args, 0U, error_message, full_name);
  }
  if (full_name == "Std.Tuple.Range") {
    switch (args.size()) {
      case 1U:
        return check_integer_index_arg(args, 0U, error_message, full_name);
      case 2U:
        return check_integer_index_args(args, {0U, 1U}, error_message, full_name);
      case 3U:
        return check_integer_index_args(args, {0U, 1U, 2U}, error_message, full_name);
      default:
        return true;
    }
  }
  if (full_name == "Std.Slice") {
    switch (args.size()) {
      case 2U:
        return check_integer_index_arg(args, 1U, error_message, full_name);
      case 3U:
        return check_integer_index_args(args, {1U, 2U}, error_message, full_name);
      case 4U:
        return check_integer_index_args(args, {1U, 2U, 3U}, error_message, full_name);
      default:
        return true;
    }
  }
  if (full_name == "Std.Array.Slice" || full_name == "Std.Array.SetAt2D" || full_name == "Std.Array.Fill" ||
      full_name == "Std.Array.Reshape") {
    return check_integer_index_args(args, {1U, 2U}, error_message, full_name);
  }
  if (full_name == "Std.Array.Slice2D") {
    return check_integer_index_args(args, {1U, 2U, 3U, 4U}, error_message, full_name);
  }
  if (full_name == "Std.String.Slice") {
    switch (args.size()) {
      case 2U:
        return check_integer_index_arg(args, 1U, error_message, full_name);
      case 3U:
        return check_integer_index_args(args, {1U, 2U}, error_message, full_name);
      default:
        return true;
    }
  }
  if (full_name == "Std.String.Find") {
    if (args.size() == 3U) {
      return check_integer_index_arg(args, 2U, error_message, full_name);
    }
    return true;
  }
  if (full_name == "Std.Array.GetAtND" || full_name == "Std.Array.SetAtND" || full_name == "Std.Array.ReshapeND") {
    return check_integer_tuple_arg(args, 1U, error_message, full_name);
  }
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
      full_name == "Std.Dict.Clear") {
    return check_dict_key_channel_arg(args, 0U, type_index, error_message, full_name);
  }
  if (full_name == "Std.Parallel.WithOptions") {
    return check_parallel_options_arg(args, 2U, type_index, error_message, full_name);
  }

  return true;
}

auto refine_builtin_return_type(const std::string& full_name, const std::vector<Type>& args,
                                const Type& declared_return_type) -> Type {
  if (!is_numeric_arithmetic_builtin(full_name)) {
    return declared_return_type;
  }

  unsigned combined_mask = kNumericNone;
  for (const auto& arg : args) {
    combined_mask |= numeric_type_mask(arg);
  }

  if ((combined_mask & (kNumericDeferred | kNumericOther)) != 0U) {
    return declared_return_type;
  }
  if (combined_mask == kNumericInt64) {
    return make_type(TypeKind::kInt64);
  }
  if (combined_mask == kNumericUInt64) {
    return make_type(TypeKind::kUInt64);
  }
  if (combined_mask == kNumericFloat64) {
    return make_type(TypeKind::kFloat64);
  }
  return declared_return_type;
}

}  // namespace fleaux::frontend::type_system
