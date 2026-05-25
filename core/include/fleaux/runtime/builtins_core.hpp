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

[[nodiscard]] inline auto try_get_pair_tuple(const Value& value) -> mguid::RefExpected<const Array, mguid::Error> {
  if (const auto arr = value.TryGetArray(); arr.has_value()) {
    if (arr->Size() == 2) {
      return arr;
    }
    if (arr->Size() == 1) {
      if (const auto nested = arr->TryGet(0)->TryGetArray(); nested.has_value() && nested->Size() == 2) {
        return nested;
      }
    }
  }

  return mguid::make_unexpected(mguid::Error{.category = mguid::Error::Category::BadAccess});
}

inline auto require_pair_tuple(const Value& value, const char* op_name) -> const Array& {
  if (const auto pair = try_get_pair_tuple(value); pair.has_value()) {
    return pair.value();
  }
  throw std::invalid_argument{std::string(op_name) + ": expected a 2-tuple"};
}

inline auto normalize_runtime_error_message(std::string message) -> std::string {
  // Unwrap nested runtime wrappers like:
  // "native 'branch_call' threw: native builtin 'Std.X' threw: actual message"
  // or "builtin 'Std.X' threw: actual message" from the fallback builtin map.
  constexpr std::string_view k_threw = "threw: ";
  while (message.starts_with("native ") || message.starts_with("builtin ")) {
    const std::size_t split = message.find(k_threw);
    if (split == std::string::npos) {
      break;
    }
    message = message.substr(split + k_threw.size());
  }
  return message;
}

inline void append_sequence_range(Array& out, const Array& seq, const std::size_t start, const std::size_t stop,
                                  const std::size_t step = 1) {
  for (std::size_t index = start; index < stop; index += step) {
    out.PushBack(*seq.TryGet(index));
  }
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

[[nodiscard]] inline auto First(Value arg) -> Value {
  const auto& values = require_pair_tuple(arg, "First");
  return *values.TryGet(0);
}

[[nodiscard]] inline auto Second(Value arg) -> Value {
  const auto& values = require_pair_tuple(arg, "Second");
  return *values.TryGet(1);
}

[[nodiscard]] inline auto ElementAt(Value arg) -> Value {
  const auto& seq = as_array(array_at(arg, 0));
  const std::size_t idx = as_index_strict(array_at(arg, 1), "ElementAt index");
  auto result = seq.TryGet(idx);
  if (!result)
    throw std::out_of_range{"ElementAt: index out of range"};
  return *result;
}

[[nodiscard]] inline auto Length(Value arg) -> Value { return make_int(static_cast<Int>(as_array(arg).Size())); }

[[nodiscard]] inline auto Take(Value arg) -> Value {
  const auto& seq = as_array(array_at(arg, 0));
  const std::size_t take_count = std::min(as_index_strict(array_at(arg, 1), "Take count"), seq.Size());
  Array out;
  out.Reserve(take_count);
  append_sequence_range(out, seq, 0, take_count);
  return Value{std::move(out)};
}

[[nodiscard]] inline auto Drop(Value arg) -> Value {
  const auto& seq = as_array(array_at(arg, 0));
  const std::size_t start = as_index_strict(array_at(arg, 1), "Drop count");
  const std::size_t clamped_start = std::min(start, seq.Size());
  Array out;
  out.Reserve(seq.Size() - clamped_start);
  append_sequence_range(out, seq, clamped_start, seq.Size());
  return Value{std::move(out)};
}

[[nodiscard]] inline auto Slice(Value arg) -> Value {
  const auto& arr = as_array(arg);
  if (arr.Size() < 2 || arr.Size() > 4) {
    throw std::invalid_argument{"Slice: expected 2, 3, or 4 arguments"};
  }
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
    if (real_step == 0)
      throw std::invalid_argument{"Slice: step cannot be 0"};
  }

  Array out;
  const std::size_t end = std::min(real_stop, seq.Size());
  append_sequence_range(out, seq, real_start, end, real_step);
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
  if (const bool has_integer = is_int_number(lhs) || is_int_number(rhs) || is_uint_number(lhs) || is_uint_number(rhs);
      has_float && has_integer) {
    throw std::invalid_argument{std::string(op_name) +
                                ": cannot mix Float64 with Int64 or UInt64 operands without explicit cast"};
  }
}

[[nodiscard]] inline auto prefer_float_numeric_result(const Value& lhs, const Value& rhs) -> bool {
  return is_float_number(lhs) || is_float_number(rhs);
}

[[nodiscard]] inline auto AddBinary(const Value& lhs, const Value& rhs) -> Value {
  require_no_implicit_float_promotion(lhs, rhs, "Add");
  require_same_integer_kind(lhs, rhs, "Add");
  return num_result(to_double(lhs) + to_double(rhs), is_uint_number(lhs) && is_uint_number(rhs),
                    prefer_float_numeric_result(lhs, rhs));
}

[[nodiscard]] inline auto Add(Value arg) -> Value {
  return AddBinary(array_at(arg, 0), array_at(arg, 1));
}

[[nodiscard]] inline auto SubtractBinary(const Value& lhs, const Value& rhs) -> Value {
  require_no_implicit_float_promotion(lhs, rhs, "Subtract");
  require_same_integer_kind(lhs, rhs, "Subtract");
  return num_result(to_double(lhs) - to_double(rhs), is_uint_number(lhs) && is_uint_number(rhs),
                    prefer_float_numeric_result(lhs, rhs));
}

[[nodiscard]] inline auto Subtract(Value arg) -> Value {
  return SubtractBinary(array_at(arg, 0), array_at(arg, 1));
}

[[nodiscard]] inline auto MultiplyBinary(const Value& lhs, const Value& rhs) -> Value {
  require_no_implicit_float_promotion(lhs, rhs, "Multiply");
  require_same_integer_kind(lhs, rhs, "Multiply");
  return num_result(to_double(lhs) * to_double(rhs), is_uint_number(lhs) && is_uint_number(rhs),
                    prefer_float_numeric_result(lhs, rhs));
}

[[nodiscard]] inline auto Multiply(Value arg) -> Value {
  return MultiplyBinary(array_at(arg, 0), array_at(arg, 1));
}

[[nodiscard]] inline auto DivideBinary(const Value& lhs, const Value& rhs) -> Value {
  require_no_implicit_float_promotion(lhs, rhs, "Divide");
  require_same_integer_kind(lhs, rhs, "Divide");
  return num_result(to_double(lhs) / to_double(rhs), is_uint_number(lhs) && is_uint_number(rhs),
                    prefer_float_numeric_result(lhs, rhs));
}

[[nodiscard]] inline auto Divide(Value arg) -> Value {
  return DivideBinary(array_at(arg, 0), array_at(arg, 1));
}

[[nodiscard]] inline auto ModBinary(const Value& lhs, const Value& rhs) -> Value {
  require_no_implicit_float_promotion(lhs, rhs, "Mod");
  require_same_integer_kind(lhs, rhs, "Mod");
  return num_result(std::fmod(to_double(lhs), to_double(rhs)), is_uint_number(lhs) && is_uint_number(rhs),
                    prefer_float_numeric_result(lhs, rhs));
}

[[nodiscard]] inline auto Mod(Value arg) -> Value {
  return ModBinary(array_at(arg, 0), array_at(arg, 1));
}

[[nodiscard]] inline auto PowBinary(const Value& lhs, const Value& rhs) -> Value {
  require_no_implicit_float_promotion(lhs, rhs, "Pow");
  return num_result(std::pow(to_double(lhs), to_double(rhs)), false, prefer_float_numeric_result(lhs, rhs));
}

[[nodiscard]] inline auto Pow(Value arg) -> Value {
  return PowBinary(array_at(arg, 0), array_at(arg, 1));
}

[[nodiscard]] inline auto BitAndBinary(const Value& lhs, const Value& rhs) -> Value {
  return make_int(as_int_value_strict(lhs, "BitAnd lhs") & as_int_value_strict(rhs, "BitAnd rhs"));
}

[[nodiscard]] inline auto BitAnd(Value arg) -> Value {
  return BitAndBinary(array_at(arg, 0), array_at(arg, 1));
}

[[nodiscard]] inline auto BitOrBinary(const Value& lhs, const Value& rhs) -> Value {
  return make_int(as_int_value_strict(lhs, "BitOr lhs") | as_int_value_strict(rhs, "BitOr rhs"));
}

[[nodiscard]] inline auto BitOr(Value arg) -> Value {
  return BitOrBinary(array_at(arg, 0), array_at(arg, 1));
}

[[nodiscard]] inline auto BitXorBinary(const Value& lhs, const Value& rhs) -> Value {
  return make_int(as_int_value_strict(lhs, "BitXor lhs") ^ as_int_value_strict(rhs, "BitXor rhs"));
}

[[nodiscard]] inline auto BitXor(Value arg) -> Value {
  return BitXorBinary(array_at(arg, 0), array_at(arg, 1));
}

[[nodiscard]] inline auto BitNot(Value arg) -> Value {
  return make_int(~as_int_value_strict(unwrap_singleton_arg(std::move(arg)), "BitNot value"));
}

[[nodiscard]] inline auto BitShiftLeftBinary(const Value& lhs, const Value& rhs) -> Value {
  const Int value = as_int_value_strict(lhs, "BitShiftLeft value");
  const std::size_t shift = as_index_strict(rhs, "BitShiftLeft shift");
  return make_int(value << shift);
}

[[nodiscard]] inline auto BitShiftLeft(Value arg) -> Value {
  return BitShiftLeftBinary(array_at(arg, 0), array_at(arg, 1));
}

[[nodiscard]] inline auto BitShiftRightBinary(const Value& lhs, const Value& rhs) -> Value {
  const Int value = as_int_value_strict(lhs, "BitShiftRight value");
  const std::size_t shift = as_index_strict(rhs, "BitShiftRight shift");
  return make_int(value >> shift);
}

[[nodiscard]] inline auto BitShiftRight(Value arg) -> Value {
  return BitShiftRightBinary(array_at(arg, 0), array_at(arg, 1));
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

[[nodiscard]] inline auto GreaterThanBinary(const Value& lhs, const Value& rhs) -> Value {
  require_no_implicit_float_promotion(lhs, rhs, "GreaterThan");
  return make_bool(compare_numbers(lhs, rhs) > 0);
}

[[nodiscard]] inline auto GreaterThan(Value arg) -> Value {
  return GreaterThanBinary(array_at(arg, 0), array_at(arg, 1));
}

[[nodiscard]] inline auto LessThanBinary(const Value& lhs, const Value& rhs) -> Value {
  require_no_implicit_float_promotion(lhs, rhs, "LessThan");
  return make_bool(compare_numbers(lhs, rhs) < 0);
}

[[nodiscard]] inline auto LessThan(Value arg) -> Value {
  return LessThanBinary(array_at(arg, 0), array_at(arg, 1));
}

[[nodiscard]] inline auto GreaterOrEqualBinary(const Value& lhs, const Value& rhs) -> Value {
  require_no_implicit_float_promotion(lhs, rhs, "GreaterOrEqual");
  return make_bool(compare_numbers(lhs, rhs) >= 0);
}

[[nodiscard]] inline auto GreaterOrEqual(Value arg) -> Value {
  return GreaterOrEqualBinary(array_at(arg, 0), array_at(arg, 1));
}

[[nodiscard]] inline auto LessOrEqualBinary(const Value& lhs, const Value& rhs) -> Value {
  require_no_implicit_float_promotion(lhs, rhs, "LessOrEqual");
  return make_bool(compare_numbers(lhs, rhs) <= 0);
}

[[nodiscard]] inline auto LessOrEqual(Value arg) -> Value {
  return LessOrEqualBinary(array_at(arg, 0), array_at(arg, 1));
}

[[nodiscard]] inline auto EqualBinary(const Value& lhs, const Value& rhs) -> Value { return make_bool(lhs == rhs); }

[[nodiscard]] inline auto Equal(Value arg) -> Value { return EqualBinary(array_at(arg, 0), array_at(arg, 1)); }

[[nodiscard]] inline auto NotEqualBinary(const Value& lhs, const Value& rhs) -> Value { return make_bool(lhs != rhs); }

[[nodiscard]] inline auto NotEqual(Value arg) -> Value { return NotEqualBinary(array_at(arg, 0), array_at(arg, 1)); }

[[nodiscard]] inline auto Not(Value arg) -> Value { return make_bool(!as_bool(unwrap_singleton_arg(std::move(arg)))); }

[[nodiscard]] inline auto AndBinary(const Value& lhs, const Value& rhs) -> Value {
  return make_bool(as_bool(lhs) && as_bool(rhs));
}

[[nodiscard]] inline auto And(Value arg) -> Value {
  return AndBinary(array_at(arg, 0), array_at(arg, 1));
}

[[nodiscard]] inline auto OrBinary(const Value& lhs, const Value& rhs) -> Value { return make_bool(as_bool(lhs) || as_bool(rhs)); }

[[nodiscard]] inline auto Or(Value arg) -> Value { return OrBinary(array_at(arg, 0), array_at(arg, 1)); }

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
  if (!arg.HasArray()) {
    const std::string fmt = to_string(arg);
    runtime_output_stream() << format_values(fmt, std::vector<Value>{});
    return make_tuple(std::move(arg));
  }

  const auto& args = as_array(arg);
  if (args.Size() == 0U) {
    throw std::logic_error{"internal error: Printf called with unexpected validated arity"};
  }
  const std::string fmt = to_string(array_at(arg, 0));
  std::vector<Value> values;
  values.reserve(args.Size() - 1U);
  for (std::size_t arg_index = 1; arg_index < args.Size(); ++arg_index) {
    values.push_back(array_at(arg, arg_index));
  }

  runtime_output_stream() << format_values(fmt, values);
  return arg;
}

[[nodiscard]] inline auto read_input_line() -> Value {
  std::string line;
  if (!std::getline(runtime_input_stream(), line)) {
    return make_string("");
  }
  return make_string(line);
}

[[nodiscard]] inline auto Input_Void(Value arg) -> Value {
  (void)arg;
  return read_input_line();
}

[[nodiscard]] inline auto Input_String(Value arg) -> Value {
  auto& output = runtime_output_stream();
  output << to_string(unwrap_singleton_arg(std::move(arg)));
  output.flush();
  return read_input_line();
}

[[nodiscard]] inline auto GetArgs(Value arg) -> Value {
  (void)arg;
  Array out;
  const auto args = get_process_args();
  out.Reserve(args.size());
  for (const auto& process_arg : args) {
    out.PushBack(make_string(process_arg));
  }
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
        if (signed_value < 0) {
          throw std::invalid_argument{"ToUInt64: cannot cast negative Int64 to UInt64"};
        }
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
  (void)arg;
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

  std::exit(to_exit_code(unwrap_singleton_arg(std::move(arg))));
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
  if (args.Size() < 2) {
    throw std::invalid_argument{"Match expects a value and at least one case"};
  }

  const Value subject = *args.TryGet(0);
  for (std::size_t case_index = 1; case_index < args.Size(); ++case_index) {
    const auto& case_tuple = as_array(*args.TryGet(case_index));
    if (case_tuple.Size() != 2) {
      throw std::invalid_argument{"Match case must be a (pattern, handler) tuple"};
    }

    const Value pattern = *case_tuple.TryGet(0);
    const Value handler = *case_tuple.TryGet(1);

    const bool wildcard_match = pattern.HasString() && as_string(pattern) == k_match_wildcard_sentinel;
    bool predicate_match = false;
    if (!wildcard_match && callable_id_from_value(pattern).has_value()) {
      predicate_match = as_bool(invoke_callable_ref(pattern, subject));
    }

    if (wildcard_match || predicate_match || pattern == subject) {
      return invoke_callable_ref(handler, subject);
    }
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
  if (!as_bool(*tuple.TryGet(0))) {
    throw std::runtime_error{"Result.Unwrap expected Ok (true), got Err (false)"};
  }
  return *tuple.TryGet(1);
}

[[nodiscard]] inline auto ResultUnwrapErr(Value arg) -> Value {
  const Value result = unwrap_singleton_arg(std::move(arg));
  const auto& tuple = require_result_tuple(result, "Result.UnwrapErr");
  if (as_bool(*tuple.TryGet(0))) {
    throw std::runtime_error{"Result.UnwrapErr expected Err (false), got Ok (true)"};
  }
  return *tuple.TryGet(1);
}

[[nodiscard]] inline auto Try(Value arg) -> Value {
  const Value& value = array_at(arg, 0);
  const Value& function_ref = array_at(arg, 1);
  try {
    return ResultOk(make_tuple(invoke_callable_ref(function_ref, value)));
  } catch (const std::exception& ex) {
    return ResultErr(make_tuple(make_string(normalize_runtime_error_message(ex.what()))));
  }
}

[[nodiscard]] inline auto Apply(Value arg) -> Value {
  // arg = [value, func_ref]
  return invoke_callable_ref(array_at(arg, 1), array_at(arg, 0));
}

[[nodiscard]] inline auto Branch(Value arg) -> Value {
  // arg = [condition, value, true_func_ref, false_func_ref]
  const Value& condition = array_at(arg, 0);
  const Value& value = array_at(arg, 1);
  const Value& true_func = array_at(arg, 2);
  const Value& false_func = array_at(arg, 3);
  return as_bool(condition) ? invoke_callable_ref(true_func, value) : invoke_callable_ref(false_func, value);
}

[[nodiscard]] inline auto Loop(Value arg) -> Value {
  // arg = [state, continue_func_ref, step_func_ref]
  Value state = std::move(array_at(arg, 0));
  const Value& continue_func = array_at(arg, 1);
  const Value& step_func = array_at(arg, 2);
  const RuntimeCallable continue_callable = resolve_callable_ref(continue_func);
  const RuntimeCallable step_callable = resolve_callable_ref(step_func);
  while (as_bool(continue_callable(state))) {
    state = step_callable(std::move(state));
  }
  return state;
}

[[nodiscard]] inline auto LoopN(Value arg) -> Value {
  // arg = [state, continue_func_ref, step_func_ref, max_iters]
  Value state = std::move(array_at(arg, 0));
  const Value& continue_func = array_at(arg, 1);
  const Value& step_func = array_at(arg, 2);
  const std::size_t max_iters = as_index_strict(array_at(arg, 3), "LoopN max_iters");
  const RuntimeCallable continue_callable = resolve_callable_ref(continue_func);
  const RuntimeCallable step_callable = resolve_callable_ref(step_func);

  std::size_t steps = 0;
  while (as_bool(continue_callable(state))) {
    if (steps >= max_iters) {
      throw std::runtime_error{"LoopN: exceeded max_iters"};
    }
    state = step_callable(std::move(state));
    ++steps;
  }
  return state;
}

}  // namespace fleaux::runtime
