#pragma once
// Core builtins: sequence access, arithmetic, comparison, logical, output, control flow.
// Part of the split runtime support layer; included by fleaux/runtime/runtime_support.hpp.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <thread>
#include <vector>

#include "fleaux/runtime/value.hpp"

namespace fleaux::runtime {

inline constexpr std::string_view k_match_wildcard_sentinel = "__fleaux_match_wildcard__";
inline constexpr std::string_view k_task_handle_tag = "__fleaux_task__";
inline constexpr std::string_view k_task_cancelled_message = "Task cancelled";
inline constexpr std::string_view k_task_timeout_message = "Task.WithTimeout: timeout exceeded";

inline auto require_result_tuple(const Value& value, const char* op_name) -> const Array& {
  const auto& result = as_array(value);
  if (result.Size() != 2) {
    throw std::invalid_argument{std::string(op_name) + ": expected Result tuple (tag, payload)"};
  }

  if (const Value& tag = *result.TryGet(0); !tag.HasBool()) {
    throw std::invalid_argument{std::string(op_name) + ": Result tag must be a Bool (true for Ok, false for Err)"};
  }
  return result;
}

inline auto normalize_runtime_error_message(std::string message) -> std::string {
  // Unwrap nested runtime wrappers like:
  // "native 'branch_call' threw: native builtin 'Std.X' threw: actual message"
  // or "builtin 'Std.X' threw: actual message" from the fallback builtin map.
  constexpr std::string_view k_threw = "threw: ";
  while (message.starts_with("native ") || message.starts_with("builtin ")) {
    const std::size_t split = message.find(k_threw);
    if (split == std::string::npos) { break; }
    message = message.substr(split + k_threw.size());
  }
  return message;
}

class RuntimePayloadError final : public std::exception {
public:
  RuntimePayloadError(Value payload, std::string message)
      : payload_(std::move(payload)), message_(std::move(message)) {}

  [[nodiscard]] auto payload() const -> const Value& { return payload_; }

  [[nodiscard]] auto what() const noexcept -> const char* override { return message_.c_str(); }

private:
  Value payload_;
  std::string message_;
};

[[nodiscard]] inline auto Wrap(Value value) -> Value { return make_tuple(std::move(value)); }

[[nodiscard]] inline auto Unwrap(Value value) -> Value {
  const auto& args = require_args(value, 1, "Unwrap");
  return *args.TryGet(0);
}

[[nodiscard]] inline auto ElementAt(Value arg) -> Value {
  const auto& seq = as_array(array_at(arg, 0));
  const std::size_t idx = as_index_strict(array_at(arg, 1), "ElementAt index");
  auto result = seq.TryGet(idx);
  if (!result) throw std::out_of_range{"ElementAt: index out of range"};
  return *result;
}

[[nodiscard]] inline auto Length(Value arg) -> Value { return make_int(static_cast<Int>(as_array(arg).Size())); }

[[nodiscard]] inline auto Take(Value arg) -> Value {
  const auto& seq = as_array(array_at(arg, 0));
  const std::size_t take_count = std::min(as_index_strict(array_at(arg, 1), "Take count"), seq.Size());
  Array out;
  out.Reserve(take_count);
  for (std::size_t index = 0; index < take_count; ++index) { out.PushBack(*seq.TryGet(index)); }
  return Value{std::move(out)};
}

[[nodiscard]] inline auto Drop(Value arg) -> Value {
  const auto& seq = as_array(array_at(arg, 0));
  const std::size_t start = as_index_strict(array_at(arg, 1), "Drop count");
  Array out;
  for (std::size_t index = start; index < seq.Size(); ++index) { out.PushBack(*seq.TryGet(index)); }
  return Value{std::move(out)};
}

[[nodiscard]] inline auto Slice(Value arg) -> Value {
  const auto& arr = as_array(arg);
  if (arr.Size() < 2 || arr.Size() > 4) { throw std::invalid_argument{"Slice: expected 2, 3, or 4 arguments"}; }
  const auto& seq = as_array(*arr.TryGet(0));

  std::size_t real_start{0};
  std::size_t real_stop{0};
  std::size_t real_step{1};
  if (arr.Size() == 2) {
    real_stop = as_index_strict(*arr.TryGet(1), "Slice stop");
  } else if (arr.Size() == 3) {
    real_start = as_index_strict(*arr.TryGet(1), "Slice start");
    real_stop = as_index_strict(*arr.TryGet(2), "Slice stop");
  } else {
    real_start = as_index_strict(*arr.TryGet(1), "Slice start");
    real_stop = as_index_strict(*arr.TryGet(2), "Slice stop");
    real_step = as_index_strict(*arr.TryGet(3), "Slice step");
    if (real_step == 0) throw std::invalid_argument{"Slice: step cannot be 0"};
  }

  Array out;
  const std::size_t end = std::min(real_stop, seq.Size());
  for (std::size_t index = real_start; index < end; index += real_step) { out.PushBack(*seq.TryGet(index)); }
  return Value{std::move(out)};
}

// Arithmetic

inline auto require_same_integer_kind(const Value& lhs, const Value& rhs, const char* op_name) -> void {
  if (is_mixed_signed_unsigned_integer_pair(lhs, rhs)) {
    throw std::invalid_argument{std::string(op_name) + ": cannot mix Int64 and UInt64 operands without explicit cast"};
  }
}

inline auto require_no_implicit_float_promotion(const Value& lhs, const Value& rhs, const char* op_name) -> void {
  const bool has_float = is_float_number(lhs) || is_float_number(rhs);
  const bool has_integer = is_int_number(lhs) || is_int_number(rhs) || is_uint_number(lhs) || is_uint_number(rhs);
  if (has_float && has_integer) {
    throw std::invalid_argument{std::string(op_name) +
                                ": cannot mix Float64 with Int64 or UInt64 operands without explicit cast"};
  }
}

[[nodiscard]] inline auto prefer_float_numeric_result(const Value& lhs, const Value& rhs) -> bool {
  return is_float_number(lhs) || is_float_number(rhs);
}

[[nodiscard]] inline auto Add(Value arg) -> Value {
  const Value& lhs = array_at(arg, 0);
  const Value& rhs = array_at(arg, 1);
  if (lhs.HasString() && rhs.HasString()) { return make_string(as_string(lhs) + as_string(rhs)); }
  require_no_implicit_float_promotion(lhs, rhs, "Add");
  require_same_integer_kind(lhs, rhs, "Add");
  return num_result(to_double(lhs) + to_double(rhs), is_uint_number(lhs) && is_uint_number(rhs),
                    prefer_float_numeric_result(lhs, rhs));
}

[[nodiscard]] inline auto Subtract(Value arg) -> Value {
  const Value& lhs = array_at(arg, 0);
  const Value& rhs = array_at(arg, 1);
  require_no_implicit_float_promotion(lhs, rhs, "Subtract");
  require_same_integer_kind(lhs, rhs, "Subtract");
  return num_result(to_double(lhs) - to_double(rhs), is_uint_number(lhs) && is_uint_number(rhs),
                    prefer_float_numeric_result(lhs, rhs));
}

[[nodiscard]] inline auto Multiply(Value arg) -> Value {
  const Value& lhs = array_at(arg, 0);
  const Value& rhs = array_at(arg, 1);
  require_no_implicit_float_promotion(lhs, rhs, "Multiply");
  require_same_integer_kind(lhs, rhs, "Multiply");
  return num_result(to_double(lhs) * to_double(rhs), is_uint_number(lhs) && is_uint_number(rhs),
                    prefer_float_numeric_result(lhs, rhs));
}

[[nodiscard]] inline auto Divide(Value arg) -> Value {
  const Value& lhs = array_at(arg, 0);
  const Value& rhs = array_at(arg, 1);
  require_no_implicit_float_promotion(lhs, rhs, "Divide");
  require_same_integer_kind(lhs, rhs, "Divide");
  return num_result(to_double(lhs) / to_double(rhs), is_uint_number(lhs) && is_uint_number(rhs),
                    prefer_float_numeric_result(lhs, rhs));
}

[[nodiscard]] inline auto Mod(Value arg) -> Value {
  const Value& lhs = array_at(arg, 0);
  const Value& rhs = array_at(arg, 1);
  require_no_implicit_float_promotion(lhs, rhs, "Mod");
  require_same_integer_kind(lhs, rhs, "Mod");
  return num_result(std::fmod(to_double(lhs), to_double(rhs)), is_uint_number(lhs) && is_uint_number(rhs),
                    prefer_float_numeric_result(lhs, rhs));
}

[[nodiscard]] inline auto Pow(Value arg) -> Value {
  const Value& lhs = array_at(arg, 0);
  const Value& rhs = array_at(arg, 1);
  require_no_implicit_float_promotion(lhs, rhs, "Pow");
  return num_result(std::pow(to_double(lhs), to_double(rhs)), false, prefer_float_numeric_result(lhs, rhs));
}

[[nodiscard]] inline auto BitAnd(Value arg) -> Value {
  return make_int(as_int_value_strict(array_at(arg, 0), "BitAnd lhs") &
                  as_int_value_strict(array_at(arg, 1), "BitAnd rhs"));
}

[[nodiscard]] inline auto BitOr(Value arg) -> Value {
  return make_int(as_int_value_strict(array_at(arg, 0), "BitOr lhs") |
                  as_int_value_strict(array_at(arg, 1), "BitOr rhs"));
}

[[nodiscard]] inline auto BitXor(Value arg) -> Value {
  return make_int(as_int_value_strict(array_at(arg, 0), "BitXor lhs") ^
                  as_int_value_strict(array_at(arg, 1), "BitXor rhs"));
}

[[nodiscard]] inline auto BitNot(Value arg) -> Value {
  return make_int(~as_int_value_strict(unwrap_singleton_arg(std::move(arg)), "BitNot value"));
}

[[nodiscard]] inline auto BitShiftLeft(Value arg) -> Value {
  const Int value = as_int_value_strict(array_at(arg, 0), "BitShiftLeft value");
  const Int shift = as_int_value_strict(array_at(arg, 1), "BitShiftLeft shift");
  if (shift < 0) { throw std::invalid_argument{"BitShiftLeft: shift must be non-negative"}; }
  return make_int(value << shift);
}

[[nodiscard]] inline auto BitShiftRight(Value arg) -> Value {
  const Int value = as_int_value_strict(array_at(arg, 0), "BitShiftRight value");
  const Int shift = as_int_value_strict(array_at(arg, 1), "BitShiftRight shift");
  if (shift < 0) { throw std::invalid_argument{"BitShiftRight: shift must be non-negative"}; }
  return make_int(value >> shift);
}

[[nodiscard]] inline auto UnaryMinus(Value arg) -> Value {
  const Value value = unwrap_singleton_arg(std::move(arg));
  return num_result(-to_double(value), false, is_float_number(value));
}

[[nodiscard]] inline auto UnaryPlus(Value arg) -> Value {
  const Value value = unwrap_singleton_arg(std::move(arg));
  return num_result(+to_double(value), false, is_float_number(value));
}

// Comparison & logical

[[nodiscard]] inline auto GreaterThan(Value arg) -> Value {
  require_no_implicit_float_promotion(array_at(arg, 0), array_at(arg, 1), "GreaterThan");
  return make_bool(compare_numbers(array_at(arg, 0), array_at(arg, 1)) > 0);
}

[[nodiscard]] inline auto LessThan(Value arg) -> Value {
  require_no_implicit_float_promotion(array_at(arg, 0), array_at(arg, 1), "LessThan");
  return make_bool(compare_numbers(array_at(arg, 0), array_at(arg, 1)) < 0);
}

[[nodiscard]] inline auto GreaterOrEqual(Value arg) -> Value {
  require_no_implicit_float_promotion(array_at(arg, 0), array_at(arg, 1), "GreaterOrEqual");
  return make_bool(compare_numbers(array_at(arg, 0), array_at(arg, 1)) >= 0);
}

[[nodiscard]] inline auto LessOrEqual(Value arg) -> Value {
  require_no_implicit_float_promotion(array_at(arg, 0), array_at(arg, 1), "LessOrEqual");
  return make_bool(compare_numbers(array_at(arg, 0), array_at(arg, 1)) <= 0);
}

[[nodiscard]] inline auto Equal(Value arg) -> Value { return make_bool(array_at(arg, 0) == array_at(arg, 1)); }

[[nodiscard]] inline auto NotEqual(Value arg) -> Value { return make_bool(array_at(arg, 0) != array_at(arg, 1)); }

[[nodiscard]] inline auto Not(Value arg) -> Value { return make_bool(!as_bool(unwrap_singleton_arg(std::move(arg)))); }

[[nodiscard]] inline auto And(Value arg) -> Value {
  return make_bool(as_bool(array_at(arg, 0)) && as_bool(array_at(arg, 1)));
}

[[nodiscard]] inline auto Or(Value arg) -> Value {
  return make_bool(as_bool(array_at(arg, 0)) || as_bool(array_at(arg, 1)));
}

// Output

[[nodiscard]] inline auto Println(Value arg) -> Value {
  // Prints the value, returns it unchanged.
  auto& output = runtime_output_stream();
  print_value_varargs(output, arg);
  output << std::endl;
  return arg;
}

[[nodiscard]] inline auto Printf(Value arg) -> Value {
  // arg = [format, arg0, arg1, ...]
  // Prints formatted text and returns the original argument tuple unchanged.
  const auto& args = as_array(arg);
  if (args.Size() < 1) { throw std::invalid_argument{"Printf expects at least 1 argument"}; }
  const std::string fmt = to_string(*args.TryGet(0));
  std::vector<Value> values;
  values.reserve(args.Size() > 0 ? args.Size() - 1 : 0);
  for (std::size_t arg_index = 1; arg_index < args.Size(); ++arg_index) { values.push_back(*args.TryGet(arg_index)); }

  runtime_output_stream() << format_values(fmt, values);
  return arg;
}

[[nodiscard]] inline auto Input(Value arg) -> Value {
  // arg = [] | [prompt] | prompt
  auto read_line = []() -> Value {
    std::string line;
    if (!std::getline(runtime_input_stream(), line)) { return make_string(""); }
    return make_string(line);
  };

  auto& output = runtime_output_stream();
  if (!arg.HasArray()) {
    output << to_string(arg);
    output.flush();
    return read_line();
  }

  const auto& args = as_array(arg);
  if (args.Size() == 0) { return read_line(); }
  if (args.Size() == 1) {
    output << to_string(*args.TryGet(0));
    output.flush();
    return read_line();
  }
  throw std::invalid_argument{"Input expects 0 or 1 argument"};
}

[[nodiscard]] inline auto GetArgs(Value arg) -> Value {
  (void)require_args(arg, 0, "GetArgs");
  Array out;
  const auto args = get_process_args();
  out.Reserve(args.size());
  for (const auto& process_arg : args) { out.PushBack(make_string(process_arg)); }
  return Value{std::move(out)};
}

[[nodiscard]] inline auto Type(Value arg) -> Value {
  // arg = [value] | value -> String runtime type name
  return make_string(type_name(unwrap_singleton_arg(std::move(arg))));
}

[[nodiscard]] inline auto Cast(Value arg) -> Value { return unwrap_singleton_arg(std::move(arg)); }

[[nodiscard]] inline auto ToInt64(Value arg) -> Value {
  return make_int(as_int_value_strict(unwrap_singleton_arg(std::move(arg)), "ToInt64"));
}

[[nodiscard]] inline auto ToUInt64(Value arg) -> Value {
  const Value value = unwrap_singleton_arg(std::move(arg));
  return as_number(value).Visit(
      [](const Int signed_value) -> Value {
        if (signed_value < 0) { throw std::invalid_argument{"ToUInt64: cannot cast negative Int64 to UInt64"}; }
        return make_uint(static_cast<UInt>(signed_value));
      },
      [](const UInt unsigned_value) -> Value { return make_uint(unsigned_value); },
      [](const Float float_value) -> Value {
        if (!std::isfinite(float_value) || std::floor(float_value) != float_value || float_value < 0.0) {
          throw std::invalid_argument{"ToUInt64: cannot cast Float64 value to UInt64"};
        }
        if (float_value > static_cast<double>(std::numeric_limits<UInt>::max())) {
          throw std::out_of_range{"ToUInt64: Float64 value out of UInt64 range"};
        }
        return make_uint(static_cast<UInt>(float_value));
      });
}

[[nodiscard]] inline auto ToFloat64(Value arg) -> Value {
  return make_float(to_double(unwrap_singleton_arg(std::move(arg))));
}

[[nodiscard]] inline auto Exit_Void(Value arg) -> Value {
  const auto& arr = arg.TryGetArray();
  if (!arr || arr->Size() != 0) { throw std::invalid_argument{"Exit_Void expects 0 arguments"}; }
  std::exit(0);
}

[[nodiscard]] inline auto Exit_Int64(Value arg) -> Value {
  auto to_exit_code = [](const Value& value) -> int {
    const Int code = as_int_value_strict(value, "Exit code");
    if (code < static_cast<Int>(std::numeric_limits<int>::min()) ||
        code > static_cast<Int>(std::numeric_limits<int>::max())) {
      throw std::invalid_argument{"Exit code out of range for host int"};
    }
    return static_cast<int>(code);
  };

  const auto& arr = arg.TryGetArray();
  if (arr) {
    if (arr->Size() != 1) { throw std::invalid_argument{"Exit_Int64 expects 1 argument"}; }
    std::exit(to_exit_code(*arr->TryGet(0)));
  }

  std::exit(to_exit_code(arg));
}

// Control flow (templated: functions remain concrete C++ callables)

[[nodiscard]] inline auto Select(Value arg) -> Value {
  // arg = [condition, true_val, false_val] — all Values
  return as_bool(array_at(arg, 0)) ? array_at(arg, 1) : array_at(arg, 2);
}

[[nodiscard]] inline auto Match(Value arg) -> Value {
  // arg = [value, [pattern, handler], [pattern, handler], ...]
  // Pattern wildcard is encoded as the lowering sentinel string.
  // Callable patterns are predicates: pattern(subject) -> Bool.
  const auto& args = as_array(arg);
  if (args.Size() < 2) { throw std::invalid_argument{"Match expects a value and at least one case"}; }

  const Value subject = *args.TryGet(0);
  for (std::size_t case_index = 1; case_index < args.Size(); ++case_index) {
    const auto& case_tuple = as_array(*args.TryGet(case_index));
    if (case_tuple.Size() != 2) { throw std::invalid_argument{"Match case must be a (pattern, handler) tuple"}; }

    const Value pattern = *case_tuple.TryGet(0);
    const Value handler = *case_tuple.TryGet(1);

    const bool wildcard_match = pattern.HasString() && as_string(pattern) == k_match_wildcard_sentinel;
    bool predicate_match = false;
    if (!wildcard_match && callable_id_from_value(pattern).has_value()) {
      predicate_match = as_bool(invoke_callable_ref(pattern, subject));
    }

    if (wildcard_match || predicate_match || pattern == subject) { return invoke_callable_ref(handler, subject); }
  }

  throw std::runtime_error{"Match: no case matched and no wildcard case provided"};
}

[[nodiscard]] inline auto ResultOk(Value arg) -> Value {
  return make_tuple(make_bool(true), unwrap_singleton_arg(std::move(arg)));
}

[[nodiscard]] inline auto ResultErr(Value arg) -> Value {
  return make_tuple(make_bool(false), unwrap_singleton_arg(std::move(arg)));
}

[[nodiscard]] inline auto ResultTag(Value arg) -> Value {
  const Value result = unwrap_singleton_arg(std::move(arg));
  const auto& tuple = require_result_tuple(result, "Result.Tag");
  return *tuple.TryGet(0);
}

[[nodiscard]] inline auto ResultPayload(Value arg) -> Value {
  const Value result = unwrap_singleton_arg(std::move(arg));
  const auto& tuple = require_result_tuple(result, "Result.Payload");
  return *tuple.TryGet(1);
}

[[nodiscard]] inline auto ResultIsOk(Value arg) -> Value {
  const Value result = unwrap_singleton_arg(std::move(arg));
  const auto& tuple = require_result_tuple(result, "Result.IsOk");
  return *tuple.TryGet(0);
}

[[nodiscard]] inline auto ResultIsErr(Value arg) -> Value {
  const Value result = unwrap_singleton_arg(std::move(arg));
  const auto& tuple = require_result_tuple(result, "Result.IsErr");
  return make_bool(!as_bool(*tuple.TryGet(0)));
}

[[nodiscard]] inline auto ResultUnwrap(Value arg) -> Value {
  const Value result = unwrap_singleton_arg(std::move(arg));
  const auto& tuple = require_result_tuple(result, "Result.Unwrap");
  if (!as_bool(*tuple.TryGet(0))) { throw std::runtime_error{"Result.Unwrap expected Ok (true), got Err (false)"}; }
  return *tuple.TryGet(1);
}

[[nodiscard]] inline auto ResultUnwrapErr(Value arg) -> Value {
  const Value result = unwrap_singleton_arg(std::move(arg));
  const auto& tuple = require_result_tuple(result, "Result.UnwrapErr");
  if (as_bool(*tuple.TryGet(0))) { throw std::runtime_error{"Result.UnwrapErr expected Err (false), got Ok (true)"}; }
  return *tuple.TryGet(1);
}

[[nodiscard]] inline auto Try(Value arg) -> Value {
  const auto& args = require_args(arg, 2, "Try");
  const Value& value = *args.TryGet(0);
  const Value& function_ref = *args.TryGet(1);
  try {
    return ResultOk(make_tuple(invoke_callable_ref(function_ref, value)));
  } catch (const std::exception& ex) {
    return ResultErr(make_tuple(make_string(normalize_runtime_error_message(ex.what()))));
  }
}

[[nodiscard]] inline auto Apply(Value arg) -> Value {
  // arg = [value, func_ref]
  const auto& args = require_args(arg, 2, "Apply");
  return invoke_callable_ref(*args.TryGet(1), *args.TryGet(0));
}

[[nodiscard]] inline auto Branch(Value arg) -> Value {
  // arg = [condition, value, true_func_ref, false_func_ref]
  const auto& args = require_args(arg, 4, "Branch");
  const Value& condition = *args.TryGet(0);
  const Value& value = *args.TryGet(1);
  const Value& true_func = *args.TryGet(2);
  const Value& false_func = *args.TryGet(3);
  return as_bool(condition) ? invoke_callable_ref(true_func, value) : invoke_callable_ref(false_func, value);
}

[[nodiscard]] inline auto Loop(Value arg) -> Value {
  // arg = [state, continue_func_ref, step_func_ref]
  const auto& args = require_args(arg, 3, "Loop");
  Value state = *args.TryGet(0);
  const Value& continue_func = *args.TryGet(1);
  const Value& step_func = *args.TryGet(2);
  while (as_bool(invoke_callable_ref(continue_func, state))) {
    state = invoke_callable_ref(step_func, std::move(state));
  }
  return state;
}

[[nodiscard]] inline auto LoopN(Value arg) -> Value {
  // arg = [state, continue_func_ref, step_func_ref, max_iters]
  const auto& args = require_args(arg, 4, "LoopN");
  Value state = *args.TryGet(0);
  const Value& continue_func = *args.TryGet(1);
  const Value& step_func = *args.TryGet(2);
  const std::size_t max_iters = as_index_strict(*args.TryGet(3), "LoopN max_iters");

  std::size_t steps = 0;
  while (as_bool(invoke_callable_ref(continue_func, state))) {
    if (steps >= max_iters) { throw std::runtime_error{"LoopN: exceeded max_iters"}; }
    state = invoke_callable_ref(step_func, std::move(state));
    ++steps;
  }
  return state;
}

}  // namespace fleaux::runtime
