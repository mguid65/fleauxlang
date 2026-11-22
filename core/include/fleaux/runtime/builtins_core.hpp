#pragma once
// Core builtins: sequence access, arithmetic, comparison, logical, output, control flow.
// Part of the split runtime support layer; included by fleaux/runtime/runtime_support.hpp.
#include <algorithm>
#include <cmath>
#include <type_traits>
#include <string>
#include <stdexcept>
#include <optional>
#include <concepts>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <thread>
#include <vector>

#include "fleaux/runtime/integer_arithmetic.hpp"
#include "fleaux/runtime/value.hpp"

namespace fleaux::runtime {

inline constexpr std::string_view k_match_wildcard_sentinel = "__fleaux_match_wildcard__";
inline constexpr std::string_view k_task_handle_tag = "__fleaux_task__";
inline constexpr std::string_view k_task_cancelled_message = "Task cancelled";
inline constexpr std::string_view k_task_timeout_message = "Task.WithTimeout: timeout exceeded";

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
  const auto& args = as_array(value);
  if (args.Size() != 1U) {
    throw std::logic_error{"internal error: Unwrap called with unexpected validated arity"};
  }
  return array_at(value, 0);
}

[[nodiscard]] inline auto First(Value arg) -> Value {
  const auto& values = as_array(arg);
  return *values.TryGet(0);
}

[[nodiscard]] inline auto Second(Value arg) -> Value {
  const auto& values = as_array(arg);
  return *values.TryGet(1);
}

[[nodiscard]] inline auto ElementAt(Value arg) -> Value {
  const auto& seq = as_array(array_at(arg, 0));
  const std::size_t idx = uint64_index(array_at(arg, 1));
  auto result = seq.TryGet(idx);
  if (!result)
    throw std::out_of_range{"ElementAt: index out of range"};
  return *result;
}

[[nodiscard]] inline auto Length(Value arg) -> Value { return make_int(static_cast<Int>(as_array(arg).Size())); }

[[nodiscard]] inline auto Take(Value arg) -> Value {
  const auto& seq = as_array(array_at(arg, 0));
  const std::size_t take_count = std::min(uint64_index(array_at(arg, 1)), seq.Size());
  Array out;
  out.Reserve(take_count);
  append_sequence_range(out, seq, 0, take_count);
  return Value{std::move(out)};
}

[[nodiscard]] inline auto Drop(Value arg) -> Value {
  const auto& seq = as_array(array_at(arg, 0));
  const std::size_t start = uint64_index(array_at(arg, 1));
  const std::size_t clamped_start = std::min(start, seq.Size());
  Array out;
  out.Reserve(seq.Size() - clamped_start);
  append_sequence_range(out, seq, clamped_start, seq.Size());
  return Value{std::move(out)};
}

[[nodiscard]] inline auto slice_impl(const Array& seq, const std::size_t real_start, const std::size_t real_stop,
                                     const std::size_t real_step) -> Value {
  Array out;
  const std::size_t end = std::min(real_stop, seq.Size());
  append_sequence_range(out, seq, real_start, end, real_step);
  return Value{std::move(out)};
}

[[nodiscard]] inline auto SliceTupleUInt64(Value arg) -> Value {
  const auto& seq = as_array(array_at(arg, 0));
  const std::size_t stop = uint64_index(array_at(arg, 1));
  return slice_impl(seq, 0, stop, 1);
}

[[nodiscard]] inline auto SliceTupleUInt64UInt64(Value arg) -> Value {
  const auto& seq = as_array(array_at(arg, 0));
  const std::size_t start = uint64_index(array_at(arg, 1));
  const std::size_t stop = uint64_index(array_at(arg, 2));
  return slice_impl(seq, start, stop, 1);
}

[[nodiscard]] inline auto SliceTupleUInt64UInt64UInt64(Value arg) -> Value {
  const auto& seq = as_array(array_at(arg, 0));
  const std::size_t start = uint64_index(array_at(arg, 1));
  const std::size_t stop = uint64_index(array_at(arg, 2));
  const std::size_t step = uint64_index(array_at(arg, 3));
  if (step == 0) {
    throw std::invalid_argument{"Slice: step cannot be 0"};
  }
  return slice_impl(seq, start, stop, step);
}

// Arithmetic

// Exact numeric arithmetic. Int64 and UInt64 operations are computed in their own type and fail on overflow;
// Float64 operations follow IEEE-754. The type checker guarantees both operands have the same numeric type.

template <std::integral T>
[[nodiscard]] constexpr auto integer_type_name() -> const char* {
  return std::is_signed_v<T> ? "Int64" : "UInt64";
}

template <std::integral T>
[[nodiscard]] inline auto integer_operand(const Value& value) -> T {
  return as_number(value).Visit([](const Int signed_value) -> T { return static_cast<T>(signed_value); },
                                [](const UInt unsigned_value) -> T { return static_cast<T>(unsigned_value); },
                                [](const Float float_value) -> T { return static_cast<T>(float_value); });
}

template <std::integral T>
[[nodiscard]] inline auto make_integer(const T value) -> Value {
  if constexpr (std::is_signed_v<T>) {
    return make_int(value);
  } else {
    return make_uint(value);
  }
}

template <std::integral T>
[[nodiscard]] inline auto require_integer_result(const std::optional<T>& result, const char* op_name) -> T {
  if (!result.has_value()) {
    throw std::overflow_error{std::string(op_name) + ": " + integer_type_name<T>() + " overflow"};
  }
  return *result;
}

template <std::integral T>
inline auto require_nonzero_divisor(const T divisor, const char* op_name) -> void {
  if (divisor == 0) {
    throw std::domain_error{std::string(op_name) + ": " + integer_type_name<T>() + " division by zero"};
  }
}

// Applies an operation to two numbers of the same type. Int64 and UInt64 operands go to integer_op, which receives
// both operands as that type and returns the same type. Float64 operands go to float_op. The operands always have the
// same type: a bound such as <T: Numeric> makes the checker reject mixed operands, and host arguments are checked
// against the same bounds when the host calls a function.
template <typename IntegerOp, typename FloatOp>
[[nodiscard]] inline auto same_type_numeric_binary(const Value& lhs, const Value& rhs, const char* op_name,
                                                   IntegerOp&& integer_op, FloatOp&& float_op) -> Value {
  (void)op_name;
  return as_number(lhs).Visit(
      [&](const Int lhs_value) -> Value { return make_integer<Int>(integer_op(lhs_value, integer_operand<Int>(rhs))); },
      [&](const UInt lhs_value) -> Value {
        return make_integer<UInt>(integer_op(lhs_value, integer_operand<UInt>(rhs)));
      },
      [&](const Float lhs_value) -> Value { return make_float(float_op(lhs_value, to_double(rhs))); });
}

// Integer-only variant for the explicit wrapping and saturating builtins.
template <typename IntegerOp>
[[nodiscard]] inline auto same_type_integer_binary(const Value& lhs, const Value& rhs, const char* op_name,
                                                   IntegerOp&& integer_op) -> Value {
  return same_type_numeric_binary(lhs, rhs, op_name, std::forward<IntegerOp>(integer_op),
                                  [op_name](const Float, const Float) -> Float {
                                    throw std::logic_error{std::string(op_name) +
                                                           " invariant violated: expected Int64 or UInt64 operands"};
                                  });
}

[[nodiscard]] inline auto AddBinary(const Value& lhs, const Value& rhs) -> Value {
  return same_type_numeric_binary(
      lhs, rhs, "Add",
      [](const auto a, const auto b) { return require_integer_result(integer::checked_add(a, b), "Add"); },
      [](const Float a, const Float b) -> Float { return a + b; });
}

[[nodiscard]] inline auto Add(Value arg) -> Value { return AddBinary(array_at(arg, 0), array_at(arg, 1)); }

[[nodiscard]] inline auto SubtractBinary(const Value& lhs, const Value& rhs) -> Value {
  return same_type_numeric_binary(
      lhs, rhs, "Subtract",
      [](const auto a, const auto b) { return require_integer_result(integer::checked_sub(a, b), "Subtract"); },
      [](const Float a, const Float b) -> Float { return a - b; });
}

[[nodiscard]] inline auto Subtract(Value arg) -> Value { return SubtractBinary(array_at(arg, 0), array_at(arg, 1)); }

[[nodiscard]] inline auto MultiplyBinary(const Value& lhs, const Value& rhs) -> Value {
  return same_type_numeric_binary(
      lhs, rhs, "Multiply",
      [](const auto a, const auto b) { return require_integer_result(integer::checked_mul(a, b), "Multiply"); },
      [](const Float a, const Float b) -> Float { return a * b; });
}

[[nodiscard]] inline auto Multiply(Value arg) -> Value { return MultiplyBinary(array_at(arg, 0), array_at(arg, 1)); }

// Integer division truncates toward zero. Float64 division follows IEEE-754 (x / 0.0 is infinite or NaN).
[[nodiscard]] inline auto DivideBinary(const Value& lhs, const Value& rhs) -> Value {
  return same_type_numeric_binary(
      lhs, rhs, "Divide",
      [](const auto a, const auto b) {
        require_nonzero_divisor(b, "Divide");
        return require_integer_result(integer::checked_div(a, b), "Divide");
      },
      [](const Float a, const Float b) -> Float { return a / b; });
}

[[nodiscard]] inline auto Divide(Value arg) -> Value { return DivideBinary(array_at(arg, 0), array_at(arg, 1)); }

// The remainder has the sign of the dividend, matching truncating division.
[[nodiscard]] inline auto ModBinary(const Value& lhs, const Value& rhs) -> Value {
  return same_type_numeric_binary(
      lhs, rhs, "Mod",
      [](const auto a, const auto b) {
        require_nonzero_divisor(b, "Mod");
        return integer::remainder(a, b);
      },
      [](const Float a, const Float b) -> Float { return std::fmod(a, b); });
}

[[nodiscard]] inline auto Mod(Value arg) -> Value { return ModBinary(array_at(arg, 0), array_at(arg, 1)); }

// Integer exponentiation is exact and requires a non-negative exponent.
[[nodiscard]] inline auto PowBinary(const Value& lhs, const Value& rhs) -> Value {
  return same_type_numeric_binary(
      lhs, rhs, "Pow",
      [](const auto base, const auto exponent) {
        using T = std::remove_cvref_t<decltype(base)>;
        if constexpr (std::is_signed_v<T>) {
          if (exponent < 0) {
            throw std::domain_error{"Pow: Int64 exponent must not be negative"};
          }
        }
        return require_integer_result(integer::checked_pow(base, exponent), "Pow");
      },
      [](const Float a, const Float b) -> Float { return std::pow(a, b); });
}

[[nodiscard]] inline auto Pow(Value arg) -> Value { return PowBinary(array_at(arg, 0), array_at(arg, 1)); }

[[nodiscard]] inline auto WrappingAdd(Value arg) -> Value {
  return same_type_integer_binary(array_at(arg, 0), array_at(arg, 1), "WrappingAdd",
                                  [](const auto a, const auto b) { return integer::wrapping_add(a, b); });
}

[[nodiscard]] inline auto WrappingSubtract(Value arg) -> Value {
  return same_type_integer_binary(array_at(arg, 0), array_at(arg, 1), "WrappingSubtract",
                                  [](const auto a, const auto b) { return integer::wrapping_sub(a, b); });
}

[[nodiscard]] inline auto WrappingMultiply(Value arg) -> Value {
  return same_type_integer_binary(array_at(arg, 0), array_at(arg, 1), "WrappingMultiply",
                                  [](const auto a, const auto b) { return integer::wrapping_mul(a, b); });
}

[[nodiscard]] inline auto SaturatingAdd(Value arg) -> Value {
  return same_type_integer_binary(array_at(arg, 0), array_at(arg, 1), "SaturatingAdd",
                                  [](const auto a, const auto b) { return integer::saturating_add(a, b); });
}

[[nodiscard]] inline auto SaturatingSubtract(Value arg) -> Value {
  return same_type_integer_binary(array_at(arg, 0), array_at(arg, 1), "SaturatingSubtract",
                                  [](const auto a, const auto b) { return integer::saturating_sub(a, b); });
}

[[nodiscard]] inline auto SaturatingMultiply(Value arg) -> Value {
  return same_type_integer_binary(array_at(arg, 0), array_at(arg, 1), "SaturatingMultiply",
                                  [](const auto a, const auto b) { return integer::saturating_mul(a, b); });
}

[[nodiscard]] inline auto BitAndBinary(const Value& lhs, const Value& rhs) -> Value {
  return make_int(int64_value(lhs) & int64_value(rhs));
}

[[nodiscard]] inline auto BitAnd(Value arg) -> Value {
  return BitAndBinary(array_at(arg, 0), array_at(arg, 1));
}

[[nodiscard]] inline auto BitOrBinary(const Value& lhs, const Value& rhs) -> Value {
  return make_int(int64_value(lhs) | int64_value(rhs));
}

[[nodiscard]] inline auto BitOr(Value arg) -> Value {
  return BitOrBinary(array_at(arg, 0), array_at(arg, 1));
}

[[nodiscard]] inline auto BitXorBinary(const Value& lhs, const Value& rhs) -> Value {
  return make_int(int64_value(lhs) ^ int64_value(rhs));
}

[[nodiscard]] inline auto BitXor(Value arg) -> Value {
  return BitXorBinary(array_at(arg, 0), array_at(arg, 1));
}

[[nodiscard]] inline auto BitNot(Value arg) -> Value {
  return make_int(~int64_value(unwrap_singleton_arg(std::move(arg))));
}

// Shifting an Int64 by 64 or more bits has no defined result in C++, so it is a runtime error. Shifts of negative
// values follow C++20: left shifts are two's complement and right shifts are arithmetic.
inline auto require_valid_shift(const UInt shift, const char* op_name) -> void {
  if (shift >= static_cast<UInt>(std::numeric_limits<Int>::digits + 1)) {
    throw std::out_of_range{std::format("{}: shift count {} is out of range for Int64 (0 to 63)", op_name, shift)};
  }
}

[[nodiscard]] inline auto BitShiftLeftBinary(const Value& lhs, const Value& rhs) -> Value {
  const Int value = int64_value(lhs);
  const UInt shift = uint64_value(rhs);
  require_valid_shift(shift, "BitShiftLeft");
  return make_int(value << shift);
}

[[nodiscard]] inline auto BitShiftLeft(Value arg) -> Value {
  return BitShiftLeftBinary(array_at(arg, 0), array_at(arg, 1));
}

[[nodiscard]] inline auto BitShiftRightBinary(const Value& lhs, const Value& rhs) -> Value {
  const Int value = int64_value(lhs);
  const UInt shift = uint64_value(rhs);
  require_valid_shift(shift, "BitShiftRight");
  return make_int(value >> shift);
}

[[nodiscard]] inline auto BitShiftRight(Value arg) -> Value {
  return BitShiftRightBinary(array_at(arg, 0), array_at(arg, 1));
}

// Negates an Int64 (failing on overflow) or a Float64.
[[nodiscard]] inline auto UnaryMinusValue(const Value& value) -> Value {
  return as_number(value).Visit(
      [](const Int signed_value) -> Value {
        return make_int(require_integer_result(integer::checked_neg(signed_value), "UnaryMinus"));
      },
      [](const UInt) -> Value { throw std::logic_error{"UnaryMinus invariant violated: expected Int64 or Float64"}; },
      [](const Float float_value) -> Value { return make_float(-float_value); });
}

[[nodiscard]] inline auto UnaryMinus(Value arg) -> Value { return UnaryMinusValue(unwrap_singleton_arg(std::move(arg))); }

[[nodiscard]] inline auto UnaryPlus(Value arg) -> Value {
  Value value = unwrap_singleton_arg(std::move(arg));
  (void)as_number(value);
  return value;
}

// Comparison & logical

[[nodiscard]] inline auto GreaterThanBinary(const Value& lhs, const Value& rhs) -> Value {
  return make_bool(compare_numbers(lhs, rhs) > 0);
}

[[nodiscard]] inline auto GreaterThan(Value arg) -> Value {
  return GreaterThanBinary(array_at(arg, 0), array_at(arg, 1));
}

[[nodiscard]] inline auto LessThanBinary(const Value& lhs, const Value& rhs) -> Value {
  return make_bool(compare_numbers(lhs, rhs) < 0);
}

[[nodiscard]] inline auto LessThan(Value arg) -> Value {
  return LessThanBinary(array_at(arg, 0), array_at(arg, 1));
}

[[nodiscard]] inline auto GreaterOrEqualBinary(const Value& lhs, const Value& rhs) -> Value {
  return make_bool(compare_numbers(lhs, rhs) >= 0);
}

[[nodiscard]] inline auto GreaterOrEqual(Value arg) -> Value {
  return GreaterOrEqualBinary(array_at(arg, 0), array_at(arg, 1));
}

[[nodiscard]] inline auto LessOrEqualBinary(const Value& lhs, const Value& rhs) -> Value {
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
  return make_string(type_name(unwrap_singleton_arg(std::move(arg))));
}

// Casts whose argument has static type Any are followed by a CheckType or TryCheckType instruction that checks
// the value; any other cast is already known to succeed.
[[nodiscard]] inline auto Cast(Value arg) -> Value { return unwrap_singleton_arg(std::move(arg)); }

[[nodiscard]] inline auto ResultOk(Value arg) -> Value;

[[nodiscard]] inline auto TryCast(Value arg) -> Value { return ResultOk(make_tuple(unwrap_singleton_arg(std::move(arg)))); }

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
    const Int code = int64_value(value);
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
  // arg = [condition, true_val, false_val], all Values
  return as_bool(array_at(arg, 0)) ? array_at(arg, 1) : array_at(arg, 2);
}

[[nodiscard]] inline auto Match(Value arg) -> Value {
  // arg = [value, [pattern, handler], [pattern, handler], ...]
  // Pattern wildcard is encoded as the lowering sentinel string.
  // Callable patterns are predicates: pattern(subject) -> Bool.
  // Std.Match is direct-call only, and the type checker enforces this shape at each call.
  const auto& args = as_array(arg);

  const Value subject = *args.TryGet(0);
  for (std::size_t case_index = 1; case_index < args.Size(); ++case_index) {
    const auto& case_tuple = as_array(*args.TryGet(case_index));

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

// A Result is stored as a (Bool tag, payload) tuple. Its shape is guaranteed by the Result<T, E> type, by the
// checked cast to Result, and by the host entry check, so the Result builtins do not check it again.
[[nodiscard]] inline auto ResultOk(Value arg) -> Value {
  return make_tuple(make_bool(true), unwrap_singleton_arg(std::move(arg)));
}

[[nodiscard]] inline auto ResultErr(Value arg) -> Value {
  return make_tuple(make_bool(false), unwrap_singleton_arg(std::move(arg)));
}

[[nodiscard]] inline auto ResultTag(Value arg) -> Value {
  const auto& tuple = as_array(arg);
  return *tuple.TryGet(0);
}

[[nodiscard]] inline auto ResultPayload(Value arg) -> Value {
  const auto& tuple = as_array(arg);
  return *tuple.TryGet(1);
}

[[nodiscard]] inline auto ResultIsOk(Value arg) -> Value {
  const auto& tuple = as_array(arg);
  return *tuple.TryGet(0);
}

[[nodiscard]] inline auto ResultIsErr(Value arg) -> Value {
  const auto& tuple = as_array(arg);
  return make_bool(!as_bool(*tuple.TryGet(0)));
}

[[nodiscard]] inline auto ResultUnwrap(Value arg) -> Value {
  const auto& tuple = as_array(arg);
  if (!as_bool(*tuple.TryGet(0))) {
    throw std::runtime_error{"Result.Unwrap expected Ok (true), got Err (false)"};
  }
  return *tuple.TryGet(1);
}

[[nodiscard]] inline auto ResultUnwrapErr(Value arg) -> Value {
  const auto& tuple = as_array(arg);
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
  Value state = std::move(mutable_array_at(arg, 0));
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
  Value state = std::move(mutable_array_at(arg, 0));
  const Value& continue_func = array_at(arg, 1);
  const Value& step_func = array_at(arg, 2);
  const std::size_t max_iters = uint64_index(array_at(arg, 3));
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
