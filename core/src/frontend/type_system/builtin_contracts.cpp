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

[[nodiscard]] auto is_deferred_numeric_check_type(const Type& type) -> bool {
  return type.kind == TypeKind::kAny || type.kind == TypeKind::kUnknown;
}

[[nodiscard]] auto is_deferred_string_check_type(const Type& type) -> bool {
  return type.kind == TypeKind::kAny || type.kind == TypeKind::kUnknown;
}

[[nodiscard]] auto is_deferred_callable_check_type(const Type& type) -> bool {
  return type.kind == TypeKind::kAny || type.kind == TypeKind::kUnknown || type.kind == TypeKind::kTypeVar;
}

[[nodiscard]] auto is_deferred_expected_type_check_type(const Type& type) -> bool {
  return type.kind == TypeKind::kAny || type.kind == TypeKind::kUnknown || type.kind == TypeKind::kTypeVar;
}

struct CallableSlotContract {
  std::size_t index;
  std::size_t arity;
};

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

auto check_numeric_arg(const std::vector<Type>& args, const std::size_t index, std::string& error_message,
                       const std::string& full_name) -> bool {
  if (index >= args.size()) {
    error_message = "Type mismatch in call target arguments.";
    return false;
  }

  if (is_deferred_numeric_check_type(args[index])) {
    return true;
  }

  if (const unsigned mask = numeric_type_mask(args[index]);
      (mask & kNumericOther) == 0U &&
      (mask & (kNumericInt64 | kNumericUInt64 | kNumericFloat64 | kNumericDeferred)) != 0U) {
    return true;
  }

  error_message = full_name + " expects Int64, UInt64, or Float64 input.";
  return false;
}

// Accepts Int64 or Float64 only. Unsigned values have no negation of the same type.
// Phase 2 of the type system soundness plan replaces this with a generic bound.
auto check_signed_numeric_arg(const std::vector<Type>& args, const std::size_t index, std::string& error_message,
                              const std::string& full_name) -> bool {
  if (index >= args.size()) {
    error_message = "Type mismatch in call target arguments.";
    return false;
  }

  if (is_deferred_numeric_check_type(args[index])) {
    return true;
  }

  if (const unsigned mask = numeric_type_mask(args[index]);
      mask == kNumericInt64 || mask == kNumericFloat64 || mask == kNumericDeferred) {
    return true;
  }

  error_message = full_name + " expects Int64 or Float64 input.";
  return false;
}

auto check_string_arg(const std::vector<Type>& args, const std::size_t index, std::string& error_message,
                      const std::string& full_name) -> bool {
  if (index >= args.size()) {
    error_message = "Type mismatch in call target arguments.";
    return false;
  }
  if (args[index].kind == TypeKind::kString || is_deferred_string_check_type(args[index])) {
    return true;
  }
  error_message = full_name + " expects String arguments.";
  return false;
}

auto check_string_args(const std::vector<Type>& args, const std::initializer_list<std::size_t> indices,
                       std::string& error_message, const std::string& full_name) -> bool {
  for (const std::size_t index : indices) {
    if (!check_string_arg(args, index, error_message, full_name)) {
      return false;
    }
  }
  return true;
}

auto check_callable_arg(const std::vector<Type>& args, const std::size_t index, const std::size_t expected_arity,
                        std::string& error_message, const std::string& full_name) -> bool {
  if (index >= args.size()) {
    error_message = "Type mismatch in call target arguments.";
    return false;
  }

  const Type& arg = args[index];
  if (is_deferred_callable_check_type(arg)) {
    return true;
  }
  if (arg.kind != TypeKind::kFunction) {
    error_message = full_name + " expects callable arguments.";
    return false;
  }
  if (arg.function_params.size() != expected_arity ||
      std::ranges::any_of(arg.function_params, [](const Type& param) { return param.variadic; })) {
    error_message = full_name + " expects callable arguments with " + std::to_string(expected_arity) +
                    (expected_arity == 1U ? " parameter." : " parameters.");
    return false;
  }
  return true;
}

auto check_callable_args(const std::vector<Type>& args, const std::initializer_list<CallableSlotContract> contracts,
                         std::string& error_message, const std::string& full_name) -> bool {
  for (const auto [index, arity] : contracts) {
    if (!check_callable_arg(args, index, arity, error_message, full_name)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] auto make_variadic_tuple_of_type(Type item_type) -> Type {
  Type tuple = make_type(TypeKind::kTuple);
  item_type.variadic = true;
  tuple.items.push_back(std::move(item_type));
  return tuple;
}

auto check_expected_type_arg(const std::vector<Type>& args, const std::size_t index, const Type& expected,
                             const std::string& expected_description, std::string& error_message,
                             const std::string& full_name) -> bool {
  if (index >= args.size()) {
    error_message = "Type mismatch in call target arguments.";
    return false;
  }

  const Type& arg = args[index];
  if (is_deferred_expected_type_check_type(arg)) {
    return true;
  }
  if (is_consistent(expected, arg)) {
    return true;
  }

  error_message = full_name + " expects " + expected_description + ".";
  return false;
}

auto check_expected_type_args(const std::vector<Type>& args, const std::initializer_list<std::size_t> indices,
                              const Type& expected, const std::string& expected_description, std::string& error_message,
                              const std::string& full_name) -> bool {
  for (const std::size_t index : indices) {
    if (!check_expected_type_arg(args, index, expected, expected_description, error_message, full_name)) {
      return false;
    }
  }
  return true;
}

auto check_uint64_tuple_arg(const std::vector<Type>& args, const std::size_t index, std::string& error_message,
                            const std::string& full_name) -> bool {
  if (index >= args.size()) {
    error_message = "Type mismatch in call target arguments.";
    return false;
  }

  const Type& arg = args[index];
  if (is_deferred_expected_type_check_type(arg)) {
    return true;
  }
  if (const Type expected = make_variadic_tuple_of_type(make_type(TypeKind::kUInt64)); is_consistent(expected, arg)) {
    return true;
  }
  if (arg.kind != TypeKind::kTuple) {
    error_message = full_name + " expects tuple indices.";
    return false;
  }
  for (const auto& item : arg.items) {
    if (!is_deferred_expected_type_check_type(item) && !is_consistent(make_type(TypeKind::kUInt64), item)) {
      error_message = full_name + " expects all index tuple elements to be UInt64.";
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
  if (arg.kind == TypeKind::kAny || arg.kind == TypeKind::kUnknown || arg.kind == TypeKind::kTypeVar) {
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
  if (arg.kind == TypeKind::kAny || arg.kind == TypeKind::kUnknown || arg.kind == TypeKind::kTypeVar) {
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
  if (is_numeric_cast_sensitive_builtin(full_name) &&
      !check_no_implicit_float_promotion(args, error_message, full_name)) {
    return false;
  }
  if (full_name == "Std.Array.GetAt" || full_name == "Std.Array.SetAt" || full_name == "Std.Array.InsertAt" ||
      full_name == "Std.Array.RemoveAt") {
    return check_expected_type_arg(args, 1U, make_type(TypeKind::kUInt64), "UInt64 arguments", error_message,
                                   full_name);
  }
  if (full_name == "Std.Take" || full_name == "Std.Drop" || full_name == "Std.ElementAt" ||
      full_name == "Std.String.CharAt" || full_name == "Std.Bit.ShiftLeft" || full_name == "Std.Bit.ShiftRight") {
    return check_expected_type_arg(args, 1U, make_type(TypeKind::kUInt64), "UInt64 arguments", error_message,
                                   full_name);
  }
  if (full_name == "Std.Random.Create") {
    return check_expected_type_arg(args, 0U, make_type(TypeKind::kUInt64), "UInt64 arguments", error_message,
                                   full_name);
  }
  if (full_name == "Std.Random.NextUInt64" || full_name == "Std.Random.NextInt64" ||
      full_name == "Std.Random.NextFloat64" || full_name == "Std.Random.NextBool" || full_name == "Std.Random.Split") {
    return check_expected_type_arg(args, 0U, make_nominal_type("RandomGenerator"), "RandomGenerator arguments",
                                   error_message, full_name);
  }
  if (full_name == "Std.UnaryMinus") {
    return check_signed_numeric_arg(args, 0U, error_message, full_name);
  }
  if (full_name == "Std.ToInt64" || full_name == "Std.ToUInt64" || full_name == "Std.ToFloat64") {
    return check_numeric_arg(args, 0U, error_message, full_name);
  }
  if (full_name == "Std.OS.Env" || full_name == "Std.OS.HasEnv" || full_name == "Std.OS.UnsetEnv" ||
      full_name == "Std.OS.Exec" || full_name == "Std.Path.Normalize" || full_name == "Std.Path.Basename" ||
      full_name == "Std.Path.Dirname" || full_name == "Std.Path.Exists" || full_name == "Std.Path.IsFile" ||
      full_name == "Std.Path.IsDir" || full_name == "Std.Path.Absolute" || full_name == "Std.Path.Extension" ||
      full_name == "Std.Path.Stem" || full_name == "Std.File.ReadText" || full_name == "Std.File.ReadLines" ||
      full_name == "Std.File.Delete" || full_name == "Std.File.Size" || full_name == "Std.Dir.Create" ||
      full_name == "Std.Dir.Delete" || full_name == "Std.Dir.List" || full_name == "Std.Dir.ListFull") {
    return check_string_arg(args, 0U, error_message, full_name);
  }
  if (full_name == "Std.Path.Join") {
    for (std::size_t index = 0; index < args.size(); ++index) {
      if (!check_string_arg(args, index, error_message, full_name)) {
        return false;
      }
    }
    return true;
  }
  if (full_name == "Std.OS.SetEnv" || full_name == "Std.Path.WithExtension" || full_name == "Std.Path.WithBasename" ||
      full_name == "Std.File.WriteText" || full_name == "Std.File.AppendText" || full_name == "Std.File.Open") {
    return check_string_args(args, {0U, 1U}, error_message, full_name);
  }
  if (full_name == "Std.File.ReadLine" || full_name == "Std.File.Flush" || full_name == "Std.File.Close") {
    return check_expected_type_arg(args, 0U, make_nominal_type("FileHandle"), "FileHandle arguments", error_message,
                                   full_name);
  }
  if (full_name == "Std.File.WriteChunk") {
    return check_expected_type_arg(args, 0U, make_nominal_type("FileHandle"), "FileHandle arguments", error_message,
                                   full_name) &&
           check_string_arg(args, 1U, error_message, full_name);
  }
  if (full_name == "Std.Try" || full_name == "Std.Parallel.Map" || full_name == "Std.Parallel.ForEach" ||
      full_name == "Std.Apply" || full_name == "Std.Tuple.Map" || full_name == "Std.Tuple.Filter" ||
      full_name == "Std.Tuple.FindIndex" || full_name == "Std.Tuple.Any" || full_name == "Std.Tuple.All") {
    return check_callable_arg(args, 1U, 1U, error_message, full_name);
  }
  if (full_name == "Std.Task.Spawn") {
    return check_callable_arg(args, 0U, 1U, error_message, full_name);
  }
  if (full_name == "Std.Task.WithTimeout") {
    return check_expected_type_arg(args, 1U, make_type(TypeKind::kUInt64), "UInt64 arguments", error_message,
                                   full_name);
  }
  if (full_name == "Std.Parallel.WithOptions") {
    return check_callable_arg(args, 1U, 1U, error_message, full_name) &&
           check_parallel_options_arg(args, 2U, type_index, error_message, full_name);
  }
  if (full_name == "Std.Parallel.Reduce" || full_name == "Std.Tuple.Reduce") {
    return check_callable_arg(args, 2U, 2U, error_message, full_name);
  }
  if (full_name == "Std.Branch") {
    return check_callable_args(args, {{2U, 1U}, {3U, 1U}}, error_message, full_name);
  }
  if (full_name == "Std.Loop") {
    return check_callable_args(args, {{1U, 1U}, {2U, 1U}}, error_message, full_name);
  }
  if (full_name == "Std.LoopN") {
    return check_callable_args(args, {{1U, 1U}, {2U, 1U}}, error_message, full_name) &&
           check_expected_type_arg(args, 3U, make_type(TypeKind::kUInt64), "UInt64 arguments", error_message,
                                   full_name);
  }
  if (full_name == "Std.File.WithOpen") {
    return check_string_args(args, {0U, 1U}, error_message, full_name) &&
           check_callable_arg(args, 2U, 1U, error_message, full_name);
  }
  if (full_name == "Std.Exit" && args.size() == 1U) {
    return check_expected_type_arg(args, 0U, make_type(TypeKind::kInt64), "Int64 arguments", error_message,
                                   full_name);
  }
  if (full_name == "Std.Tuple.Range") {
    switch (args.size()) {
      case 1U:
        return check_expected_type_arg(args, 0U, make_type(TypeKind::kInt64), "Int64 arguments", error_message,
                                       full_name);
      case 2U:
        return check_expected_type_args(args, {0U, 1U}, make_type(TypeKind::kInt64), "Int64 arguments",
                                        error_message, full_name);
      case 3U:
        return check_expected_type_args(args, {0U, 1U, 2U}, make_type(TypeKind::kInt64), "Int64 arguments",
                                        error_message, full_name);
      default:
        return true;
    }
  }
  if (full_name == "Std.Slice") {
    switch (args.size()) {
      case 2U:
        return check_expected_type_arg(args, 1U, make_type(TypeKind::kUInt64), "UInt64 arguments", error_message,
                                       full_name);
      case 3U:
        return check_expected_type_args(args, {1U, 2U}, make_type(TypeKind::kUInt64), "UInt64 arguments", error_message,
                                        full_name);
      case 4U:
        return check_expected_type_args(args, {1U, 2U, 3U}, make_type(TypeKind::kUInt64), "UInt64 arguments",
                                        error_message, full_name);
      default:
        return true;
    }
  }
  if (full_name == "Std.Array.Slice" || full_name == "Std.Array.SetAt2D" || full_name == "Std.Array.Fill" ||
      full_name == "Std.Array.Reshape") {
    return check_expected_type_args(args, {1U, 2U}, make_type(TypeKind::kUInt64), "UInt64 arguments", error_message,
                                    full_name);
  }
  if (full_name == "Std.Array.Slice2D") {
    return check_expected_type_args(args, {1U, 2U, 3U, 4U}, make_type(TypeKind::kUInt64), "UInt64 arguments",
                                    error_message, full_name);
  }
  if (full_name == "Std.String.Slice") {
    switch (args.size()) {
      case 2U:
        return check_expected_type_arg(args, 1U, make_type(TypeKind::kUInt64), "UInt64 arguments", error_message,
                                       full_name);
      case 3U:
        return check_expected_type_args(args, {1U, 2U}, make_type(TypeKind::kUInt64), "UInt64 arguments", error_message,
                                        full_name);
      default:
        return true;
    }
  }
  if (full_name == "Std.String.Find") {
    if (args.size() == 3U) {
      return check_expected_type_arg(args, 2U, make_type(TypeKind::kUInt64), "UInt64 arguments", error_message,
                                     full_name);
    }
    return true;
  }
  if (full_name == "Std.Array.GetAtND" || full_name == "Std.Array.SetAtND" || full_name == "Std.Array.ReshapeND") {
    return check_uint64_tuple_arg(args, 1U, error_message, full_name);
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
      full_name == "Std.Dict.Clear" || full_name == "Std.Dict.Length") {
    return check_dict_key_channel_arg(args, 0U, type_index, error_message, full_name);
  }
  if (full_name == "Std.File.ReadChunk") {
    return check_expected_type_arg(args, 0U, make_nominal_type("FileHandle"), "FileHandle arguments", error_message,
                                   full_name) &&
           check_expected_type_arg(args, 1U, make_type(TypeKind::kUInt64), "UInt64 arguments", error_message,
                                   full_name);
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
