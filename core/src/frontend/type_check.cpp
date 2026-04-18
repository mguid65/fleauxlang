#include "fleaux/frontend/type_check.hpp"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "fleaux/common/overloaded.hpp"
#include "fleaux/frontend/ast.hpp"

namespace fleaux::frontend::type_check {
namespace {

struct FunctionSig {
  std::vector<ir::IRParam> params;
  ir::IRSimpleType return_type;
};

enum class TypeKind {
  kUnknown,
  kAny,
  kInt64,
  kUInt64,
  kFloat64,
  kString,
  kBool,
  kNull,
  kTuple,
  kFunction,
};

struct InferredType {
  TypeKind kind = TypeKind::kUnknown;
  std::vector<InferredType> tuple_items;
  std::optional<FunctionSig> function_sig;
};

using LocalTypes = std::unordered_map<std::string, InferredType>;

struct TypedExprRef {
  const ir::IRExpr* expr = nullptr;
  InferredType type;
};

auto make_error(const std::string& message, const std::optional<std::string>& hint,
                const std::optional<diag::SourceSpan>& span) -> TypeCheckError {
  return TypeCheckError{
      .message = message,
      .hint = hint,
      .span = span,
  };
}

auto symbol_key(const std::optional<std::string>& qualifier, const std::string& name) -> std::string {
  return qualifier.has_value() ? (*qualifier + "." + name) : name;
}

auto type_name(const TypeKind kind) -> std::string {
  switch (kind) {
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
    case TypeKind::kTuple:
      return "Tuple";
    case TypeKind::kFunction:
      return "Function";
    case TypeKind::kUnknown:
    default:
      return "Unknown";
  }
}

auto to_type_kind(const ir::IRSimpleType& type) -> TypeKind {
  if (type.name == "Any") return TypeKind::kAny;
  if (type.name == "Int64") return TypeKind::kInt64;
  if (type.name == "UInt64") return TypeKind::kUInt64;
  if (type.name == "Float64") return TypeKind::kFloat64;
  if (type.name == "String") return TypeKind::kString;
  if (type.name == "Bool") return TypeKind::kBool;
  if (type.name == "Null") return TypeKind::kNull;
  if (type.name == "Tuple") return TypeKind::kTuple;
  return TypeKind::kUnknown;
}

auto is_numeric_kind(const TypeKind kind) -> bool {
  return kind == TypeKind::kInt64 || kind == TypeKind::kUInt64 || kind == TypeKind::kFloat64;
}

auto numeric_compatible(const TypeKind expected, const TypeKind actual) -> bool {
  if (!is_numeric_kind(expected) || !is_numeric_kind(actual)) return false;
  if (expected == TypeKind::kFloat64) {
    return actual == TypeKind::kFloat64 || actual == TypeKind::kInt64 || actual == TypeKind::kUInt64;
  }
  return expected == actual;
}

auto is_integer_like_kind(const TypeKind kind) -> bool {
  return kind == TypeKind::kInt64 || kind == TypeKind::kUInt64 || kind == TypeKind::kAny || kind == TypeKind::kUnknown;
}

auto make_type(const TypeKind kind) -> InferredType {
  return InferredType{.kind = kind, .tuple_items = {}, .function_sig = std::nullopt};
}

auto make_type(const ir::IRSimpleType& type) -> InferredType { return make_type(to_type_kind(type)); }

auto make_function_type(const FunctionSig& sig, const bool treat_zero_arity_as_value = false) -> InferredType {
  return InferredType{
      .kind = treat_zero_arity_as_value && sig.params.empty() ? to_type_kind(sig.return_type) : TypeKind::kFunction,
      .tuple_items = {},
      .function_sig = sig,
  };
}

auto is_compatible(const ir::IRSimpleType& expected, const InferredType& actual) -> bool {
  // Union type: compatible if actual matches any alternative.
  if (!expected.alternatives.empty()) {
    if (actual.kind == TypeKind::kAny || actual.kind == TypeKind::kUnknown) return true;
    return std::ranges::any_of(expected.alternatives, [&](const auto& alt_name) -> bool {
      const ir::IRSimpleType alt{.name = alt_name};
      const auto alt_kind = to_type_kind(alt);
      return alt_kind == TypeKind::kAny || numeric_compatible(alt_kind, actual.kind) || alt_kind == actual.kind;
    });
  }
  const auto expected_kind = to_type_kind(expected);
  if (expected_kind == TypeKind::kAny || actual.kind == TypeKind::kAny) return true;
  if (expected_kind == TypeKind::kUnknown || actual.kind == TypeKind::kUnknown) return true;
  if (numeric_compatible(expected_kind, actual.kind)) return true;
  if (expected_kind == TypeKind::kTuple && actual.kind == TypeKind::kTuple) return true;
  return expected_kind == actual.kind;
}

auto is_compatible(const InferredType& expected, const InferredType& actual) -> bool {
  if (expected.kind == TypeKind::kAny || actual.kind == TypeKind::kAny) return true;
  if (expected.kind == TypeKind::kUnknown || actual.kind == TypeKind::kUnknown) return true;
  if (numeric_compatible(expected.kind, actual.kind)) return true;
  if (expected.kind == TypeKind::kTuple && actual.kind == TypeKind::kTuple) return true;
  return expected.kind == actual.kind;
}

auto call_args_from_lhs(const ir::IRExpr& lhs, const InferredType& lhs_type) -> std::vector<std::vector<InferredType>> {
  std::vector<std::vector<InferredType>> candidates;

  const auto* tuple_expr = std::get_if<ir::IRTupleExpr>(&lhs.node);
  if (tuple_expr != nullptr && tuple_expr->items.empty()) { candidates.emplace_back(); }

  std::vector<InferredType> packed;
  packed.push_back(lhs_type);
  candidates.push_back(packed);

  if (tuple_expr != nullptr && tuple_expr->items.size() > 1U && lhs_type.kind == TypeKind::kTuple &&
      lhs_type.tuple_items.size() == tuple_expr->items.size()) {
    candidates.push_back(lhs_type.tuple_items);
  }

  return candidates;
}

auto call_args_from_value_type(const InferredType& value_type) -> std::vector<std::vector<InferredType>> {
  std::vector<std::vector<InferredType>> candidates;
  candidates.push_back({value_type});

  if (value_type.kind == TypeKind::kTuple && value_type.tuple_items.size() > 1U) {
    candidates.push_back(value_type.tuple_items);
  }

  return candidates;
}

auto direct_call_args(const ir::IRExpr& lhs, const InferredType& lhs_type) -> std::vector<TypedExprRef> {
  if (const auto* tuple_expr = std::get_if<ir::IRTupleExpr>(&lhs.node); tuple_expr != nullptr) {
    if (tuple_expr->items.empty()) { return {}; }
    if (tuple_expr->items.size() > 1U && lhs_type.kind == TypeKind::kTuple &&
        lhs_type.tuple_items.size() == tuple_expr->items.size()) {
      std::vector<TypedExprRef> args;
      args.reserve(tuple_expr->items.size());
      for (std::size_t item_index = 0; item_index < tuple_expr->items.size(); ++item_index) {
        args.push_back(TypedExprRef{.expr = &*tuple_expr->items[item_index],
                                    .type = lhs_type.tuple_items[item_index]});
      }
      return args;
    }
  }

  return {TypedExprRef{.expr = &lhs, .type = lhs_type}};
}

auto format_signature(const FunctionSig& sig) -> std::string {
  if (sig.params.empty()) return "()";

  std::string expected{"("};
  for (std::size_t param_index = 0; param_index < sig.params.size(); ++param_index) {
    if (param_index > 0U) expected += ", ";
    if (!sig.params[param_index].type.alternatives.empty()) {
      for (std::size_t alt_index = 0; alt_index < sig.params[param_index].type.alternatives.size(); ++alt_index) {
        if (alt_index > 0U) expected += " | ";
        expected += sig.params[param_index].type.alternatives[alt_index];
      }
    } else {
      expected += sig.params[param_index].type.name;
    }
    if (sig.params[param_index].type.variadic) expected += "...";
  }
  expected.push_back(')');
  return expected;
}

auto format_first_candidate(const std::vector<std::vector<InferredType>>& candidates) -> std::string {
  if (candidates.empty()) return "()";

  const auto& first = candidates.front();
  std::string got{"("};
  for (std::size_t arg_index = 0; arg_index < first.size(); ++arg_index) {
    if (arg_index > 0U) got += ", ";
    got += type_name(first[arg_index].kind);
  }
  got.push_back(')');
  return got;
}

auto candidate_matches(const FunctionSig& sig, const std::vector<InferredType>& args) -> bool {
  if (sig.params.empty()) { return args.empty(); }

  if (const bool variadic = sig.params.back().type.variadic; !variadic) {
    if (args.size() != sig.params.size()) return false;
    for (std::size_t arg_index = 0; arg_index < args.size(); ++arg_index) {
      if (!is_compatible(sig.params[arg_index].type, args[arg_index])) return false;
    }
    return true;
  }

  const std::size_t fixed_count = sig.params.size() - 1U;
  if (args.size() < fixed_count) return false;

  for (std::size_t arg_index = 0; arg_index < fixed_count; ++arg_index) {
    if (!is_compatible(sig.params[arg_index].type, args[arg_index])) return false;
  }
  for (std::size_t arg_index = fixed_count; arg_index < args.size(); ++arg_index) {
    if (!is_compatible(sig.params.back().type, args[arg_index])) return false;
  }
  return true;
}

auto validate_invocation(const std::string& message, const std::optional<diag::SourceSpan>& span,
                         const FunctionSig& sig, const std::vector<std::vector<InferredType>>& candidates)
    -> tl::expected<InferredType, TypeCheckError> {
  for (const auto& candidate : candidates) {
    if (candidate_matches(sig, candidate)) { return make_type(sig.return_type); }
  }

  return tl::unexpected(make_error(
      message, std::format("Expected {} but got {}.", format_signature(sig), format_first_candidate(candidates)),
      span));
}

auto validate_callable_value(const std::string& builtin_name, const std::string& role, const InferredType& callable,
                             const std::optional<diag::SourceSpan>& span)
    -> tl::expected<const FunctionSig*, TypeCheckError> {
  if (callable.function_sig.has_value()) { return &*callable.function_sig; }
  if (callable.kind == TypeKind::kAny || callable.kind == TypeKind::kUnknown) { return nullptr; }
  return tl::unexpected(make_error("Type mismatch in call target arguments.",
                                   std::format("{} expects {} to be callable", builtin_name, role), span));
}

auto validate_callable_invocation(const std::string& builtin_name, const std::string& role,
                                  const InferredType& callable, const ir::IRExpr& arg_expr,
                                  const InferredType& arg_type, const std::optional<diag::SourceSpan>& span)
    -> tl::expected<InferredType, TypeCheckError> {
  auto sig = validate_callable_value(builtin_name, role, callable, span);
  if (!sig) return tl::unexpected(sig.error());
  if (*sig == nullptr) return make_type(TypeKind::kAny);
  return validate_invocation("Type mismatch in call target arguments.", span, **sig,
                             call_args_from_lhs(arg_expr, arg_type));
}

auto validate_callable_invocation(const std::string& builtin_name, const std::string& role,
                                  const InferredType& callable, const InferredType& arg_type,
                                  const std::optional<diag::SourceSpan>& span)
    -> tl::expected<InferredType, TypeCheckError> {
  auto sig = validate_callable_value(builtin_name, role, callable, span);
  if (!sig) return tl::unexpected(sig.error());
  if (*sig == nullptr) return make_type(TypeKind::kAny);
  return validate_invocation("Type mismatch in call target arguments.", span, **sig,
                             call_args_from_value_type(arg_type));
}

auto validate_match_handler_invocation(const InferredType& handler, const ir::IRExpr& subject_expr,
                                       const InferredType& subject_type, const std::optional<diag::SourceSpan>& span)
    -> tl::expected<InferredType, TypeCheckError> {
  auto sig = validate_callable_value("Std.Match", "handler", handler, span);
  if (!sig) return tl::unexpected(sig.error());
  if (*sig == nullptr) return make_type(TypeKind::kAny);
  if ((*sig)->params.empty()) { return make_type((*sig)->return_type); }
  return validate_invocation("Type mismatch in call target arguments.", span, **sig,
                             call_args_from_lhs(subject_expr, subject_type));
}

auto require_bool_result(const std::string& builtin_name, const std::string& role, const InferredType& result_type,
                         const std::optional<diag::SourceSpan>& span) -> tl::expected<void, TypeCheckError> {
  if (result_type.kind == TypeKind::kBool || result_type.kind == TypeKind::kAny ||
      result_type.kind == TypeKind::kUnknown) {
    return {};
  }
  return tl::unexpected(make_error("Type mismatch in call target arguments.",
                                   std::format("{} expects {} to return Bool.", builtin_name, role), span));
}

auto operator_builtin_name(const std::string& op) -> std::optional<std::string> {
  static const std::unordered_map<std::string, std::string> op_to_builtin = {
      {"+", "Std.Add"},
      {"-", "Std.Subtract"},
      {"*", "Std.Multiply"},
      {"/", "Std.Divide"},
      {"%", "Std.Mod"},
      {"^", "Std.Pow"},
      {"==", "Std.Equal"},
      {"!=", "Std.NotEqual"},
      {"<", "Std.LessThan"},
      {">", "Std.GreaterThan"},
      {">=", "Std.GreaterOrEqual"},
      {"<=", "Std.LessOrEqual"},
      {"!", "Std.Not"},
      {"&&", "Std.And"},
      {"||", "Std.Or"},
  };
  if (const auto it = op_to_builtin.find(op); it != op_to_builtin.end()) { return it->second; }
  return std::nullopt;
}

auto resolve_target_key(const ir::IRCallTarget& target) -> std::optional<std::string> {
  if (const auto* op = std::get_if<ir::IROperatorRef>(&target); op != nullptr) { return operator_builtin_name(op->op); }
  if (const auto* name_ref = std::get_if<ir::IRNameRef>(&target); name_ref != nullptr) {
    return symbol_key(name_ref->qualifier, name_ref->name);
  }
  return std::nullopt;
}

auto is_mixed_signed_unsigned_pair(const InferredType& lhs, const InferredType& rhs) -> bool {
  const bool lhs_int = lhs.kind == TypeKind::kInt64;
  const bool lhs_uint = lhs.kind == TypeKind::kUInt64;
  const bool rhs_int = rhs.kind == TypeKind::kInt64;
  const bool rhs_uint = rhs.kind == TypeKind::kUInt64;
  return (lhs_int && rhs_uint) || (lhs_uint && rhs_int);
}

auto check_special_builtin_contract(const std::string& target_key, const ir::IRFlowExpr& flow,
                                    const InferredType& lhs_type)
    -> tl::expected<std::optional<InferredType>, TypeCheckError> {
  const auto args = direct_call_args(*flow.lhs, lhs_type);

  auto tuple_literal_items = [](const TypedExprRef& arg) -> std::vector<TypedExprRef> {
    if (const auto* tuple_expr = std::get_if<ir::IRTupleExpr>(&arg.expr->node);
        tuple_expr != nullptr && arg.type.kind == TypeKind::kTuple &&
        arg.type.tuple_items.size() == tuple_expr->items.size()) {
      std::vector<TypedExprRef> items;
      items.reserve(tuple_expr->items.size());
      for (std::size_t item_index = 0; item_index < tuple_expr->items.size(); ++item_index) {
        items.push_back(TypedExprRef{.expr = &*tuple_expr->items[item_index],
                                     .type = arg.type.tuple_items[item_index]});
      }
      return items;
    }
    return {};
  };

  auto require_integer_arg = [&](const std::size_t index,
                                 const std::string& param_name) -> tl::expected<void, TypeCheckError> {
    if (index >= args.size()) return {};
    if (is_integer_like_kind(args[index].type.kind)) return {};
    return tl::unexpected(make_error("Type mismatch in call target arguments.",
                                     std::format("{} expects '{}' to be Int64/UInt64.", target_key, param_name),
                                     flow.span));
  };

  auto require_integer_args = [&](const std::initializer_list<std::pair<std::size_t, std::string>> checks)
      -> tl::expected<void, TypeCheckError> {
    for (const auto& [idx, name] : checks) {
      if (auto ok = require_integer_arg(idx, name); !ok) return tl::unexpected(ok.error());
    }
    return {};
  };

  auto require_integer_tuple_elements = [&](const std::size_t index,
                                            const std::string& param_name) -> tl::expected<void, TypeCheckError> {
    if (index >= args.size()) return {};
    for (const auto& [expr, type] : tuple_literal_items(args[index])) {
      if (is_integer_like_kind(type.kind)) continue;
      return tl::unexpected(make_error(
          "Type mismatch in call target arguments.",
          std::format("{} expects every element in '{}' to be Int64/UInt64.", target_key, param_name), flow.span));
    }
    return {};
  };

  if (target_key == "Std.Exit") {
    if (auto ok = require_integer_args({{0U, "code"}}); !ok) return tl::unexpected(ok.error());
    return std::nullopt;
  }

  if (target_key == "Std.Bit.And" || target_key == "Std.Bit.Or" || target_key == "Std.Bit.Xor") {
    if (auto ok = require_integer_args({{0U, "lhs"}, {1U, "rhs"}}); !ok) return tl::unexpected(ok.error());
    return std::nullopt;
  }

  if (target_key == "Std.Bit.Not") {
    if (auto ok = require_integer_args({{0U, "value"}}); !ok) return tl::unexpected(ok.error());
    return std::nullopt;
  }

  if (target_key == "Std.Bit.ShiftLeft" || target_key == "Std.Bit.ShiftRight") {
    if (auto ok = require_integer_args({{0U, "value"}, {1U, "bits"}}); !ok) return tl::unexpected(ok.error());
    return std::nullopt;
  }

  if (target_key == "Std.ElementAt") {
    if (auto ok = require_integer_args({{1U, "count"}}); !ok) return tl::unexpected(ok.error());
    return std::nullopt;
  }

  if (target_key == "Std.Take" || target_key == "Std.Drop") {
    if (auto ok = require_integer_args({{1U, "count"}}); !ok) return tl::unexpected(ok.error());
    return std::nullopt;
  }

  if (target_key == "Std.Slice") {
    if (args.size() == 2U) {
      if (auto ok = require_integer_args({{1U, "stop"}}); !ok) return tl::unexpected(ok.error());
    } else if (args.size() == 3U) {
      if (auto ok = require_integer_args({{1U, "start"}, {2U, "stop"}}); !ok) return tl::unexpected(ok.error());
    } else if (args.size() >= 4U) {
      if (auto ok = require_integer_args({{1U, "start"}, {2U, "stop"}, {3U, "step"}}); !ok) {
        return tl::unexpected(ok.error());
      }
    }
    return std::nullopt;
  }

  if (target_key == "Std.String.CharAt") {
    if (auto ok = require_integer_args({{1U, "index"}}); !ok) return tl::unexpected(ok.error());
    return std::nullopt;
  }

  if (target_key == "Std.String.Slice") {
    if (args.size() == 2U) {
      if (auto ok = require_integer_args({{1U, "stop"}}); !ok) return tl::unexpected(ok.error());
    } else if (args.size() >= 3U) {
      if (auto ok = require_integer_args({{1U, "start"}, {2U, "stop"}}); !ok) return tl::unexpected(ok.error());
    }
    return std::nullopt;
  }

  if (target_key == "Std.String.Find") {
    if (args.size() >= 3U) {
      if (auto ok = require_integer_args({{2U, "start"}}); !ok) return tl::unexpected(ok.error());
    }
    return std::nullopt;
  }

  if (target_key == "Std.Tuple.Range") {
    if (args.size() == 1U) {
      if (auto ok = require_integer_args({{0U, "stop"}}); !ok) return tl::unexpected(ok.error());
    } else if (args.size() == 2U) {
      if (auto ok = require_integer_args({{0U, "start"}, {1U, "stop"}}); !ok) return tl::unexpected(ok.error());
    } else if (args.size() >= 3U) {
      if (auto ok = require_integer_args({{0U, "start"}, {1U, "stop"}, {2U, "step"}}); !ok) {
        return tl::unexpected(ok.error());
      }
    }
    return std::nullopt;
  }

  if (target_key == "Std.LoopN") {
    if (auto ok = require_integer_args({{3U, "max_iters"}}); !ok) return tl::unexpected(ok.error());
  }

  if (target_key == "Std.Array.GetAt") {
    if (auto ok = require_integer_args({{1U, "index"}}); !ok) return tl::unexpected(ok.error());
    return std::nullopt;
  }

  if (target_key == "Std.Array.SetAt" || target_key == "Std.Array.InsertAt" || target_key == "Std.Array.RemoveAt") {
    if (auto ok = require_integer_args({{1U, "index"}}); !ok) return tl::unexpected(ok.error());
    return std::nullopt;
  }

  if (target_key == "Std.Array.Slice") {
    if (auto ok = require_integer_args({{1U, "start"}, {2U, "stop"}}); !ok) return tl::unexpected(ok.error());
    return std::nullopt;
  }

  if (target_key == "Std.Array.SetAt2D") {
    if (auto ok = require_integer_args({{1U, "row"}, {2U, "col"}}); !ok) return tl::unexpected(ok.error());
    return std::nullopt;
  }

  if (target_key == "Std.Array.Fill") {
    if (auto ok = require_integer_args({{1U, "start_index"}, {2U, "length"}}); !ok) return tl::unexpected(ok.error());
    return std::nullopt;
  }

  if (target_key == "Std.Array.Slice2D") {
    if (auto ok = require_integer_args({{1U, "row_start"}, {2U, "row_end"}, {3U, "col_start"}, {4U, "col_end"}}); !ok) {
      return tl::unexpected(ok.error());
    }
    return std::nullopt;
  }

  if (target_key == "Std.Array.Reshape") {
    if (auto ok = require_integer_args({{1U, "rows"}, {2U, "cols"}}); !ok) return tl::unexpected(ok.error());
    return std::nullopt;
  }

  if (target_key == "Std.File.ReadChunk") {
    if (auto ok = require_integer_args({{1U, "nbytes"}}); !ok) return tl::unexpected(ok.error());
    return std::nullopt;
  }

  if (target_key == "Std.Array.GetAtND" || target_key == "Std.Array.SetAtND") {
    if (auto ok = require_integer_tuple_elements(1U, "indices"); !ok) return tl::unexpected(ok.error());
    return std::nullopt;
  }

  if (target_key == "Std.Array.ReshapeND") {
    if (auto ok = require_integer_tuple_elements(1U, "shape"); !ok) return tl::unexpected(ok.error());
    return std::nullopt;
  }

  if (target_key == "Std.Apply") {
    if (args.size() != 2U) return std::nullopt;
    auto result =
        validate_callable_invocation(target_key, "callable", args[1].type, *args[0].expr, args[0].type, flow.span);
    if (!result) return tl::unexpected(result.error());
    return *result;
  }

  if (target_key == "Std.Try") {
    if (args.size() != 2U) return std::nullopt;
    auto result =
        validate_callable_invocation(target_key, "callable", args[1].type, *args[0].expr, args[0].type, flow.span);
    if (!result) return tl::unexpected(result.error());
    return std::nullopt;
  }

  if (target_key == "Std.Exp.Parallel") {
    if (args.size() != 2U) return std::nullopt;
    for (const auto& [expr, type] : tuple_literal_items(args[0])) {
      if (auto result = validate_callable_invocation(target_key, "callable", args[1].type, *expr, type, flow.span);
          !result) return tl::unexpected(result.error());
    }
    return std::nullopt;
  }

  if (target_key == "Std.Branch") {
    if (args.size() != 4U) return std::nullopt;
    auto true_result =
        validate_callable_invocation(target_key, "true_func", args[2].type, *args[1].expr, args[1].type, flow.span);
    if (!true_result) return tl::unexpected(true_result.error());
    auto false_result =
        validate_callable_invocation(target_key, "false_func", args[3].type, *args[1].expr, args[1].type, flow.span);
    if (!false_result) return tl::unexpected(false_result.error());
    if (is_compatible(*true_result, *false_result)) { return *true_result; }
    return std::nullopt;
  }

  if (target_key == "Std.Loop" || target_key == "Std.LoopN") {
    if (const std::size_t expected_count = target_key == "Std.Loop" ? 3U : 4U; args.size() != expected_count)
      return std::nullopt;

    auto continue_result =
        validate_callable_invocation(target_key, "continue_func", args[1].type, *args[0].expr, args[0].type, flow.span);
    if (!continue_result) return tl::unexpected(continue_result.error());
    if (auto bool_check = require_bool_result(target_key, "continue_func", *continue_result, flow.span); !bool_check) {
      return tl::unexpected(bool_check.error());
    }

    auto step_result =
        validate_callable_invocation(target_key, "step_func", args[2].type, *args[0].expr, args[0].type, flow.span);
    if (!step_result) return tl::unexpected(step_result.error());
    if (!is_compatible(args[0].type, *step_result)) {
      return tl::unexpected(make_error("Type mismatch in call target arguments.",
                                       std::format("{} expects step_func to return the loop state type.", target_key),
                                       flow.span));
    }

    return args[0].type;
  }

  if (target_key == "Std.Tuple.Map") {
    if (args.size() != 2U) return std::nullopt;
    for (const auto& [expr, type] : tuple_literal_items(args[0])) {
      if (auto result = validate_callable_invocation(target_key, "func", args[1].type, *expr, type, flow.span); !result)
        return tl::unexpected(result.error());
    }
    return std::nullopt;
  }

  if (target_key == "Std.Tuple.Filter" || target_key == "Std.Tuple.FindIndex" || target_key == "Std.Tuple.Any" ||
      target_key == "Std.Tuple.All") {
    if (args.size() != 2U) return std::nullopt;
    for (const auto& [expr, type] : tuple_literal_items(args[0])) {
      auto result = validate_callable_invocation(target_key, "pred", args[1].type, *expr, type, flow.span);
      if (!result) return tl::unexpected(result.error());
      if (auto bool_check = require_bool_result(target_key, "pred", *result, flow.span); !bool_check) {
        return tl::unexpected(bool_check.error());
      }
    }
    return std::nullopt;
  }

  if (target_key == "Std.Tuple.Reduce") {
    if (args.size() != 3U) return std::nullopt;
    InferredType accumulator_type = args[1].type;
    for (const auto& [expr, type] : tuple_literal_items(args[0])) {
      InferredType pair_type;
      pair_type.kind = TypeKind::kTuple;
      pair_type.tuple_items = {accumulator_type, type};
      auto result = validate_callable_invocation(target_key, "func", args[2].type, pair_type, flow.span);
      if (!result) return tl::unexpected(result.error());
      accumulator_type = *result;
    }
    return accumulator_type;
  }

  if (target_key == "Std.File.WithOpen") {
    if (args.size() != 3U) return std::nullopt;
    auto result = validate_callable_invocation(target_key, "func", args[2].type, make_type(TypeKind::kAny), flow.span);
    if (!result) return tl::unexpected(result.error());
    return *result;
  }

  if (target_key == "Std.Match") {
    if (args.size() < 2U) return std::nullopt;
    for (std::size_t case_index = 1; case_index < args.size(); ++case_index) {
      const auto case_items = tuple_literal_items(args[case_index]);
      if (case_items.size() != 2U) continue;

      const auto& [pattern_expr, pattern_type] = case_items[0];
      const auto& [handler_expr, handler_type] = case_items[1];

      if (pattern_type.function_sig.has_value()) {
        auto pattern_result = validate_callable_invocation(target_key, "predicate pattern", pattern_type, *args[0].expr,
                                                           args[0].type, flow.span);
        if (!pattern_result) return tl::unexpected(pattern_result.error());
        if (auto bool_check = require_bool_result(target_key, "predicate pattern", *pattern_result, flow.span);
            !bool_check) {
          return tl::unexpected(bool_check.error());
        }
      }

      if (auto handler_result = validate_match_handler_invocation(handler_type, *args[0].expr, args[0].type, flow.span);
          !handler_result)
        return tl::unexpected(handler_result.error());
    }
    return std::nullopt;
  }

  return std::nullopt;
}

struct CheckContext {
  std::unordered_map<std::string, FunctionSig> functions;

  [[nodiscard]] auto resolve_signature(const ir::IRCallTarget& target) const -> const FunctionSig* {
    if (const auto* op = std::get_if<ir::IROperatorRef>(&target); op != nullptr) {
      if (const auto builtin = operator_builtin_name(op->op); builtin.has_value()) {
        if (const auto sig_it = functions.find(*builtin); sig_it != functions.end()) { return &sig_it->second; }
      }
      return nullptr;
    }

    const auto* name_ref = std::get_if<ir::IRNameRef>(&target);
    if (name_ref == nullptr) return nullptr;
    const auto full_key = symbol_key(name_ref->qualifier, name_ref->name);
    if (const auto it = functions.find(full_key); it != functions.end()) { return &it->second; }
    if (!name_ref->qualifier.has_value()) {
      if (const auto it = functions.find(name_ref->name); it != functions.end()) { return &it->second; }
    }
    return nullptr;
  }
};

auto check_expr(const ir::IRExpr& expr, const CheckContext& ctx, const LocalTypes& locals)
    -> tl::expected<InferredType, TypeCheckError>;

auto check_call_signature(const ir::IRFlowExpr& flow, const FunctionSig& sig,
                          const std::vector<std::vector<InferredType>>& candidates)
    -> tl::expected<InferredType, TypeCheckError> {
  return validate_invocation("Type mismatch in call target arguments.", flow.span, sig, candidates);
}

auto check_closure(const ir::IRClosureExpr& closure, const CheckContext& ctx, const LocalTypes& outer_locals)
    -> tl::expected<InferredType, TypeCheckError> {
  LocalTypes inner_locals = outer_locals;
  for (const auto& param : closure.params) { inner_locals[param.name] = make_type(param.type); }

  auto body_type = check_expr(*closure.body, ctx, inner_locals);
  if (!body_type) return tl::unexpected(body_type.error());

  if (!is_compatible(closure.return_type, *body_type)) {
    return tl::unexpected(make_error(
        "Closure return type does not match declared type.",
        std::format("Declared '{}' but inferred '{}'.", closure.return_type.name, type_name(body_type->kind)),
        closure.span));
  }

  return make_function_type(FunctionSig{.params = closure.params, .return_type = closure.return_type});
}

auto check_expr(const ir::IRExpr& expr, const CheckContext& ctx, const LocalTypes& locals)
    -> tl::expected<InferredType, TypeCheckError> {
  return std::visit(
      common::overloaded{
          [&](const ir::IRConstant& constant) -> tl::expected<InferredType, TypeCheckError> {
            return std::visit(
                common::overloaded{[](std::monostate) -> InferredType { return make_type(TypeKind::kNull); },
                                   [](bool) -> InferredType { return make_type(TypeKind::kBool); },
                                   [](std::int64_t) -> InferredType { return make_type(TypeKind::kInt64); },
                                   [](std::uint64_t) -> InferredType { return make_type(TypeKind::kUInt64); },
                                   [](double) -> InferredType { return make_type(TypeKind::kFloat64); },
                                   [](const std::string&) -> InferredType { return make_type(TypeKind::kString); }},
                constant.val);
          },
          [&](const ir::IRTupleExpr& tuple) -> tl::expected<InferredType, TypeCheckError> {
            if (tuple.items.size() == 1U) { return check_expr(*tuple.items[0], ctx, locals); }
            InferredType result;
            result.kind = TypeKind::kTuple;
            result.tuple_items.reserve(tuple.items.size());
            for (const auto& item : tuple.items) {
              auto item_type = check_expr(*item, ctx, locals);
              if (!item_type) return tl::unexpected(item_type.error());
              result.tuple_items.push_back(*item_type);
            }
            return result;
          },
          [&](const ir::IRNameRef& name_ref) -> tl::expected<InferredType, TypeCheckError> {
            if (!name_ref.qualifier.has_value()) {
              if (const auto local_it = locals.find(name_ref.name); local_it != locals.end()) {
                return local_it->second;
              }
              if (ctx.functions.contains(name_ref.name)) {
                return make_function_type(ctx.functions.at(name_ref.name), true);
              }
              return make_type(TypeKind::kUnknown);
            }

            const std::string key = *name_ref.qualifier + "." + name_ref.name;
            if (const auto fn_it = ctx.functions.find(key); fn_it != ctx.functions.end()) {
              return make_function_type(fn_it->second, true);
            }
            return make_type(TypeKind::kUnknown);
          },
          [&](const ir::IRClosureExprBox& closure_ptr) -> tl::expected<InferredType, TypeCheckError> {
            return check_closure(*closure_ptr, ctx, locals);
          },
          [&](const ir::IRFlowExpr& flow) -> tl::expected<InferredType, TypeCheckError> {
            auto lhs_type = check_expr(*flow.lhs, ctx, locals);
            if (!lhs_type) return tl::unexpected(lhs_type.error());

            const auto* signature = ctx.resolve_signature(flow.rhs);
            if (signature == nullptr) { return make_type(TypeKind::kAny); }

            const auto candidates = call_args_from_lhs(*flow.lhs, *lhs_type);
            auto inferred = check_call_signature(flow, *signature, candidates);
            if (!inferred) return tl::unexpected(inferred.error());

            if (const auto target_key = resolve_target_key(flow.rhs); target_key.has_value()) {
              const auto direct_args = direct_call_args(*flow.lhs, *lhs_type);
              const bool is_mixed_integer_guarded_target =
                  target_key.value() == "Std.Add" || target_key.value() == "Std.Subtract" ||
                  target_key.value() == "Std.Multiply" || target_key.value() == "Std.Divide" ||
                  target_key.value() == "Std.Mod";
              if (direct_args.size() == 2U && is_mixed_integer_guarded_target) {
                if (is_mixed_signed_unsigned_pair(direct_args[0].type, direct_args[1].type)) {
                  return tl::unexpected(make_error("Type mismatch in call target arguments.",
                                                   std::format("{} does not allow mixed Int64/UInt64 operands. Use "
                                                               "Std.ToInt64/Std.ToUInt64/Std.ToFloat64 explicitly.",
                                                               target_key.value()),
                                                   flow.span));
                }
              }

              auto refined = check_special_builtin_contract(*target_key, flow, *lhs_type);
              if (!refined) return tl::unexpected(refined.error());
              if (refined->has_value()) { return **refined; }
            }

            return *inferred;
          },
      },
      expr.node);
}

}  // namespace

auto validate_program(const ir::IRProgram& program) -> tl::expected<void, TypeCheckError> {
  CheckContext ctx;
  ctx.functions.reserve(program.lets.size());

  for (const auto& let : program.lets) {
    ctx.functions[symbol_key(let.qualifier, let.name)] =
        FunctionSig{.params = let.params, .return_type = let.return_type};
    if (!let.qualifier.has_value()) {
      ctx.functions.try_emplace(let.name, FunctionSig{.params = let.params, .return_type = let.return_type});
    }
  }

  for (const auto& let : program.lets) {
    if (!let.body.has_value()) continue;

    LocalTypes locals;
    for (const auto& param : let.params) { locals[param.name] = make_type(param.type); }

    auto inferred_body = check_expr(*let.body, ctx, locals);
    if (!inferred_body) return tl::unexpected(inferred_body.error());

    if (!is_compatible(let.return_type, *inferred_body)) {
      return tl::unexpected(
          make_error("Function return type does not match declared type.",
                     std::format("Function '{}' declares '{}' but inferred '{}'.", symbol_key(let.qualifier, let.name),
                                 let.return_type.name, type_name(inferred_body->kind)),
                     let.span));
    }
  }

  for (const auto& [expr, span] : program.expressions) {
    LocalTypes empty_locals;
    if (auto checked = check_expr(expr, ctx, empty_locals); !checked) return tl::unexpected(checked.error());
  }

  return {};
}

}  // namespace fleaux::frontend::type_check
