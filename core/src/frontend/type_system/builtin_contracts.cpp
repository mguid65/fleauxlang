#include "fleaux/frontend/type_system/builtin_contracts.hpp"

namespace fleaux::frontend::type_system {
namespace {

[[nodiscard]] auto is_deferred_integer_check_type(const Type& type) -> bool {
  return type.kind == TypeKind::kAny || type.kind == TypeKind::kUnknown;
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
  if (full_name == "Std.Array.GetAt") { return check_integer_index_arg(args, 1U, error_message, full_name); }
  if (full_name == "Std.Array.GetAtND") { return check_integer_tuple_arg(args, 1U, error_message, full_name); }
  if (full_name == "Std.Array.ReshapeND") { return check_integer_tuple_arg(args, 1U, error_message, full_name); }

  return true;
}

}  // namespace fleaux::frontend::type_system
