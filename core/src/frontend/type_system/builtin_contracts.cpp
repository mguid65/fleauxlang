#include "fleaux/frontend/type_system/builtin_contracts.hpp"

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
      for (const auto& member : type.union_members) { mask |= numeric_type_mask(member); }
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
  for (const auto& arg : args) { combined_mask |= numeric_type_mask(arg); }

  if ((combined_mask & kNumericDeferred) != 0U) { return true; }
  if ((combined_mask & kNumericOther) != 0U) { return true; }

  const bool has_float = (combined_mask & kNumericFloat64) != 0U;
  const bool has_integer = (combined_mask & (kNumericInt64 | kNumericUInt64)) != 0U;
  if (!has_float || !has_integer) { return true; }

  error_message = full_name + " does not implicitly cast Int64 or UInt64 to Float64. Use Std.ToFloat64 explicitly.";
  return false;
}

auto check_integer_index_arg(const std::vector<Type>& args, const std::size_t index, std::string& error_message,
                             const std::string& full_name) -> bool {
  if (index >= args.size()) {
    error_message = "Type mismatch in call target arguments.";
    return false;
  }
  if (is_integer_like(args[index]) || is_deferred_integer_check_type(args[index])) { return true; }
  error_message = full_name + " expects Int64 or UInt64 for integer arguments.";
  return false;
}

auto check_integer_tuple_arg(const std::vector<Type>& args, const std::size_t index, std::string& error_message,
                             const std::string& full_name) -> bool {
  if (index >= args.size()) {
    error_message = "Type mismatch in call target arguments.";
    return false;
  }
  if (is_deferred_integer_check_type(args[index])) { return true; }
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

}  // namespace

auto validate_builtin_contract(const std::string& full_name, const std::vector<Type>& args, std::string& error_message)
    -> bool {
  if (is_numeric_cast_sensitive_builtin(full_name) && !check_no_implicit_float_promotion(args, error_message, full_name)) {
    return false;
  }
  if (full_name == "Std.Array.GetAt") { return check_integer_index_arg(args, 1U, error_message, full_name); }
  if (full_name == "Std.Array.GetAtND") { return check_integer_tuple_arg(args, 1U, error_message, full_name); }
  if (full_name == "Std.Array.ReshapeND") { return check_integer_tuple_arg(args, 1U, error_message, full_name); }

  return true;
}

auto refine_builtin_return_type(const std::string& full_name, const std::vector<Type>& args, const Type& declared_return_type)
    -> Type {
  if (!is_numeric_arithmetic_builtin(full_name)) { return declared_return_type; }

  unsigned combined_mask = kNumericNone;
  for (const auto& arg : args) { combined_mask |= numeric_type_mask(arg); }

  if ((combined_mask & (kNumericDeferred | kNumericOther)) != 0U) { return declared_return_type; }
  if (combined_mask == kNumericFloat64) { return Type{.kind = TypeKind::kFloat64}; }
  return declared_return_type;
}

}  // namespace fleaux::frontend::type_system
