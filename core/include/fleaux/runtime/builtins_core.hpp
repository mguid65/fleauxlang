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

#include "fleaux/common/match_wildcard.hpp"
#include "fleaux/runtime/integer_arithmetic.hpp"
#include "fleaux/runtime/value.hpp"

namespace fleaux::runtime {

// Messages of failures, without the name of the builtin that failed: the VM puts that in front.
//
// What Std.Match reports when no case matches, from the builtin and from a Match compiled in place.
inline constexpr std::string_view k_match_no_case_message = "no case matched, and there is no wildcard case";
inline constexpr std::string_view k_task_cancelled_message = "Task cancelled";
inline constexpr std::string_view k_task_timeout_message = "Std.Task.WithTimeout: timeout exceeded";

// A read past the end of a tuple, with the index as the program wrote it.
[[nodiscard]] inline auto index_out_of_range_message(const std::string_view index, const std::size_t size) -> std::string {
  return std::format("index {} is out of range for a tuple of {} item{}", index, size, size == 1U ? "" : "s");
}

[[nodiscard]] inline auto index_out_of_range_message(const std::uint64_t index, const std::size_t size) -> std::string {
  return index_out_of_range_message(std::to_string(index), size);
}

// A Std.LoopN that did not end within its limit.
[[nodiscard]] inline auto loop_limit_message(const std::size_t max_iters) -> std::string {
  return std::format("the loop did not end within max_iters ({}) steps", max_iters);
}

inline void append_sequence_range(TupleBuilder& out, const TupleView seq, const std::size_t start,
                                  const std::size_t stop, const std::size_t step = 1) {
  for (std::size_t index = start; index < stop; index += step) {
    out.push(seq[index]);
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
  const auto& args = as_tuple(value);
  if (args.size() != 1U) {
    throw std::logic_error{"internal error: Unwrap called with unexpected validated arity"};
  }
  return take_tuple_item(value, 0);
}

// The builtins that read one item out of a tuple move it out when the argument holds the only reference to the tuple,
// so that the item can go on to be changed in place.

[[nodiscard]] inline auto First(Value arg) -> Value { return take_tuple_item(arg, 0); }

[[nodiscard]] inline auto Second(Value arg) -> Value { return take_tuple_item(arg, 1); }

[[nodiscard]] inline auto ElementAtBinary(const Value& tuple, const Value& index) -> Value {
  const auto& seq = as_tuple(tuple);
  const std::size_t idx = position_index(index);
  if (idx >= seq.size()) {
    throw std::out_of_range{index_out_of_range_message(to_string(index), seq.size())};
  }
  return seq[idx];
}

// Item `index` of `tuple`, which the caller owns: moved out when nothing else refers to the tuple.
[[nodiscard]] inline auto take_element(Value& tuple, const Value& index) -> Value {
  const std::size_t size = as_tuple(tuple).size();
  if (const std::size_t idx = position_index(index); idx < size) {
    return take_tuple_item(tuple, idx);
  }
  throw std::out_of_range{index_out_of_range_message(to_string(index), size)};
}

[[nodiscard]] inline auto ElementAt(const Arguments& args) -> Value {
  Value tuple = args.take(0);
  return take_element(tuple, args[1]);
}

[[nodiscard]] inline auto Length(Value arg) -> Value { return make_int(static_cast<Int>(as_tuple(arg).size())); }

[[nodiscard]] inline auto Take(const Arguments& args) -> Value {
  Value seq = args.take(0);
  const std::size_t take_count = std::min(count_index(args[1], "count"), as_tuple(seq).size());
  return keep_tuple_items(std::move(seq), 0U, take_count);
}

[[nodiscard]] inline auto Drop(const Arguments& args) -> Value {
  Value seq = args.take(0);
  const std::size_t size = as_tuple(seq).size();
  const std::size_t clamped_start = std::min(count_index(args[1], "count"), size);
  return keep_tuple_items(std::move(seq), clamped_start, size);
}

// The items of `seq` from `real_start` up to `real_stop`, every `real_step`-th one. A tuple the argument held once is
// cut in its own block.
[[nodiscard]] inline auto slice_impl(Value seq, const std::size_t real_start, const std::size_t real_stop,
                                     const std::size_t real_step) -> Value {
  const std::size_t size = as_tuple(seq).size();
  const std::size_t end = std::min(real_stop, size);
  if (real_start >= end) {
    return make_tuple();
  }
  if (real_step == 1U) {
    return keep_tuple_items(std::move(seq), real_start, end);
  }

  const std::size_t count = (end - real_start - 1U) / real_step + 1U;
  if (!seq.object()->unique()) {
    TupleBuilder out(count);
    append_sequence_range(out, seq.tuple(), real_start, end, real_step);
    return std::move(out).finish();
  }
  // Each kept item moves down to its place; it never lands on an item still to be moved.
  TupleBuilder out = TupleBuilder::reopen(std::move(seq), size);
  const std::span<Value> items = out.items();
  for (std::size_t index = 0; index < count; ++index) {
    items[index] = std::move(items[real_start + index * real_step]);
  }
  out.erase(count, size);
  return std::move(out).finish();
}

[[nodiscard]] inline auto SliceTupleInt64(const Arguments& args) -> Value {
  Value seq = args.take(0);
  const std::size_t stop = count_index(args[1], "stop");
  return slice_impl(std::move(seq), 0, stop, 1);
}

[[nodiscard]] inline auto SliceTupleInt64Int64(const Arguments& args) -> Value {
  Value seq = args.take(0);
  const std::size_t start = count_index(args[1], "start");
  const std::size_t stop = count_index(args[2], "stop");
  return slice_impl(std::move(seq), start, stop, 1);
}

[[nodiscard]] inline auto SliceTupleInt64Int64Int64(const Arguments& args) -> Value {
  Value seq = args.take(0);
  const std::size_t start = count_index(args[1], "start");
  const std::size_t stop = count_index(args[2], "stop");
  const std::size_t step = count_index(args[3], "step");
  if (step == 0) {
    throw std::invalid_argument{"step cannot be 0"};
  }
  return slice_impl(std::move(seq), start, stop, step);
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
  return visit_number(value, [](const Int signed_value) -> T { return static_cast<T>(signed_value); },
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
[[nodiscard]] inline auto require_integer_result(const std::optional<T>& result) -> T {
  if (!result.has_value()) {
    throw std::overflow_error{std::string(integer_type_name<T>()) + " overflow"};
  }
  return *result;
}

template <std::integral T>
inline auto require_nonzero_divisor(const T divisor) -> void {
  if (divisor == 0) {
    throw std::domain_error{std::string(integer_type_name<T>()) + " division by zero"};
  }
}

// Applies an operation to two numbers of the same type. Int64 and UInt64 operands go to integer_op, which receives
// both operands as that type and returns the same type. Float64 operands go to float_op. The operands always have the
// same type: a bound such as <T: Numeric> makes the checker reject mixed operands, and host arguments are checked
// against the same bounds when the host calls a function.
template <typename IntegerOp, typename FloatOp>
[[nodiscard]] inline auto same_type_numeric_binary(const Value& lhs, const Value& rhs, IntegerOp&& integer_op, FloatOp&& float_op)
    -> Value {
  return visit_number(lhs, 
      [&](const Int lhs_value) -> Value { return make_integer<Int>(integer_op(lhs_value, integer_operand<Int>(rhs))); },
      [&](const UInt lhs_value) -> Value {
        return make_integer<UInt>(integer_op(lhs_value, integer_operand<UInt>(rhs)));
      },
      [&](const Float lhs_value) -> Value { return make_float(float_op(lhs_value, to_double(rhs))); });
}

// Integer-only variant for the explicit wrapping and saturating builtins.
template <typename IntegerOp>
[[nodiscard]] inline auto same_type_integer_binary(const Value& lhs, const Value& rhs, IntegerOp&& integer_op) -> Value {
  return same_type_numeric_binary(lhs, rhs, std::forward<IntegerOp>(integer_op), [](const Float, const Float) -> Float {
    throw std::logic_error{"invariant violated: expected Int64 or UInt64 operands"};
  });
}

[[nodiscard]] inline auto AddBinary(const Value& lhs, const Value& rhs) -> Value {
  return same_type_numeric_binary(
      lhs, rhs, [](const auto a, const auto b) { return require_integer_result(integer::checked_add(a, b)); },
      [](const Float a, const Float b) -> Float { return a + b; });
}

[[nodiscard]] inline auto Add(const Arguments& args) -> Value { return AddBinary(args[0], args[1]); }

[[nodiscard]] inline auto SubtractBinary(const Value& lhs, const Value& rhs) -> Value {
  return same_type_numeric_binary(
      lhs, rhs, [](const auto a, const auto b) { return require_integer_result(integer::checked_sub(a, b)); },
      [](const Float a, const Float b) -> Float { return a - b; });
}

[[nodiscard]] inline auto Subtract(const Arguments& args) -> Value { return SubtractBinary(args[0], args[1]); }

[[nodiscard]] inline auto MultiplyBinary(const Value& lhs, const Value& rhs) -> Value {
  return same_type_numeric_binary(
      lhs, rhs, [](const auto a, const auto b) { return require_integer_result(integer::checked_mul(a, b)); },
      [](const Float a, const Float b) -> Float { return a * b; });
}

[[nodiscard]] inline auto Multiply(const Arguments& args) -> Value { return MultiplyBinary(args[0], args[1]); }

// Integer division truncates toward zero. Float64 division follows IEEE-754 (x / 0.0 is infinite or NaN).
[[nodiscard]] inline auto DivideBinary(const Value& lhs, const Value& rhs) -> Value {
  return same_type_numeric_binary(
      lhs, rhs,
      [](const auto a, const auto b) {
        require_nonzero_divisor(b);
        return require_integer_result(integer::checked_div(a, b));
      },
      [](const Float a, const Float b) -> Float { return a / b; });
}

[[nodiscard]] inline auto Divide(const Arguments& args) -> Value { return DivideBinary(args[0], args[1]); }

// The remainder has the sign of the dividend, matching truncating division.
[[nodiscard]] inline auto ModBinary(const Value& lhs, const Value& rhs) -> Value {
  return same_type_numeric_binary(
      lhs, rhs,
      [](const auto a, const auto b) {
        require_nonzero_divisor(b);
        return integer::remainder(a, b);
      },
      [](const Float a, const Float b) -> Float { return std::fmod(a, b); });
}

[[nodiscard]] inline auto Mod(const Arguments& args) -> Value { return ModBinary(args[0], args[1]); }

// Integer exponentiation is exact and requires a non-negative exponent.
[[nodiscard]] inline auto PowBinary(const Value& lhs, const Value& rhs) -> Value {
  return same_type_numeric_binary(
      lhs, rhs,
      [](const auto base, const auto exponent) {
        using T = std::remove_cvref_t<decltype(base)>;
        if constexpr (std::is_signed_v<T>) {
          if (exponent < 0) {
            throw std::domain_error{"Int64 exponent must not be negative"};
          }
        }
        return require_integer_result(integer::checked_pow(base, exponent));
      },
      [](const Float a, const Float b) -> Float { return std::pow(a, b); });
}

[[nodiscard]] inline auto Pow(const Arguments& args) -> Value { return PowBinary(args[0], args[1]); }

[[nodiscard]] inline auto WrappingAdd(const Arguments& args) -> Value {
  return same_type_integer_binary(args[0], args[1], [](const auto a, const auto b) { return integer::wrapping_add(a, b); });
}

[[nodiscard]] inline auto WrappingSubtract(const Arguments& args) -> Value {
  return same_type_integer_binary(args[0], args[1], [](const auto a, const auto b) { return integer::wrapping_sub(a, b); });
}

[[nodiscard]] inline auto WrappingMultiply(const Arguments& args) -> Value {
  return same_type_integer_binary(args[0], args[1], [](const auto a, const auto b) { return integer::wrapping_mul(a, b); });
}

[[nodiscard]] inline auto SaturatingAdd(const Arguments& args) -> Value {
  return same_type_integer_binary(args[0], args[1], [](const auto a, const auto b) { return integer::saturating_add(a, b); });
}

[[nodiscard]] inline auto SaturatingSubtract(const Arguments& args) -> Value {
  return same_type_integer_binary(args[0], args[1], [](const auto a, const auto b) { return integer::saturating_sub(a, b); });
}

[[nodiscard]] inline auto SaturatingMultiply(const Arguments& args) -> Value {
  return same_type_integer_binary(args[0], args[1], [](const auto a, const auto b) { return integer::saturating_mul(a, b); });
}

[[nodiscard]] inline auto BitAndBinary(const Value& lhs, const Value& rhs) -> Value {
  return make_int(int64_value(lhs) & int64_value(rhs));
}

[[nodiscard]] inline auto BitAnd(const Arguments& args) -> Value {
  return BitAndBinary(args[0], args[1]);
}

[[nodiscard]] inline auto BitOrBinary(const Value& lhs, const Value& rhs) -> Value {
  return make_int(int64_value(lhs) | int64_value(rhs));
}

[[nodiscard]] inline auto BitOr(const Arguments& args) -> Value {
  return BitOrBinary(args[0], args[1]);
}

[[nodiscard]] inline auto BitXorBinary(const Value& lhs, const Value& rhs) -> Value {
  return make_int(int64_value(lhs) ^ int64_value(rhs));
}

[[nodiscard]] inline auto BitXor(const Arguments& args) -> Value {
  return BitXorBinary(args[0], args[1]);
}

[[nodiscard]] inline auto BitNot(Value arg) -> Value {
  return make_int(~int64_value(arg));
}

// Shifting an Int64 by 64 or more bits has no defined result in C++, so it is a runtime error. Shifts of negative
// values follow C++20: left shifts are two's complement and right shifts are arithmetic.
inline auto require_valid_shift(const Int shift) -> void {
  if (shift < 0 || shift > std::numeric_limits<Int>::digits) {
    throw std::out_of_range{std::format("shift count {} is out of range for Int64 (0 to 63)", shift)};
  }
}

[[nodiscard]] inline auto BitShiftLeftBinary(const Value& lhs, const Value& rhs) -> Value {
  const Int value = int64_value(lhs);
  const Int shift = as_int_value_strict(rhs);
  require_valid_shift(shift);
  return make_int(value << shift);
}

[[nodiscard]] inline auto BitShiftLeft(const Arguments& args) -> Value {
  return BitShiftLeftBinary(args[0], args[1]);
}

[[nodiscard]] inline auto BitShiftRightBinary(const Value& lhs, const Value& rhs) -> Value {
  const Int value = int64_value(lhs);
  const Int shift = as_int_value_strict(rhs);
  require_valid_shift(shift);
  return make_int(value >> shift);
}

[[nodiscard]] inline auto BitShiftRight(const Arguments& args) -> Value {
  return BitShiftRightBinary(args[0], args[1]);
}

// Negates an Int64 (failing on overflow) or a Float64.
[[nodiscard]] inline auto UnaryMinusValue(const Value& value) -> Value {
  return visit_number(
      value, [](const Int signed_value) -> Value { return make_int(require_integer_result(integer::checked_neg(signed_value))); },
      [](const UInt) -> Value { throw std::logic_error{"invariant violated: expected Int64 or Float64"}; },
      [](const Float float_value) -> Value { return make_float(-float_value); });
}

[[nodiscard]] inline auto UnaryMinus(Value arg) -> Value { return UnaryMinusValue(arg); }

[[nodiscard]] inline auto UnaryPlus(Value arg) -> Value {
  Value value = std::move(arg);
  if (!value.is_number()) {
    throw kind_error("Int64, UInt64, or Float64");
  }
  return value;
}

// Comparison & logical

[[nodiscard]] inline auto GreaterThanBinary(const Value& lhs, const Value& rhs) -> Value {
  return make_bool(compare_numbers(lhs, rhs) > 0);
}

[[nodiscard]] inline auto GreaterThan(const Arguments& args) -> Value {
  return GreaterThanBinary(args[0], args[1]);
}

[[nodiscard]] inline auto LessThanBinary(const Value& lhs, const Value& rhs) -> Value {
  return make_bool(compare_numbers(lhs, rhs) < 0);
}

[[nodiscard]] inline auto LessThan(const Arguments& args) -> Value {
  return LessThanBinary(args[0], args[1]);
}

[[nodiscard]] inline auto GreaterOrEqualBinary(const Value& lhs, const Value& rhs) -> Value {
  return make_bool(compare_numbers(lhs, rhs) >= 0);
}

[[nodiscard]] inline auto GreaterOrEqual(const Arguments& args) -> Value {
  return GreaterOrEqualBinary(args[0], args[1]);
}

[[nodiscard]] inline auto LessOrEqualBinary(const Value& lhs, const Value& rhs) -> Value {
  return make_bool(compare_numbers(lhs, rhs) <= 0);
}

[[nodiscard]] inline auto LessOrEqual(const Arguments& args) -> Value {
  return LessOrEqualBinary(args[0], args[1]);
}

[[nodiscard]] inline auto EqualBinary(const Value& lhs, const Value& rhs) -> Value { return make_bool(lhs == rhs); }

[[nodiscard]] inline auto Equal(const Arguments& args) -> Value { return EqualBinary(args[0], args[1]); }

[[nodiscard]] inline auto NotEqualBinary(const Value& lhs, const Value& rhs) -> Value { return make_bool(lhs != rhs); }

[[nodiscard]] inline auto NotEqual(const Arguments& args) -> Value { return NotEqualBinary(args[0], args[1]); }

[[nodiscard]] inline auto Not(Value arg) -> Value { return make_bool(!as_bool(arg)); }

[[nodiscard]] inline auto AndBinary(const Value& lhs, const Value& rhs) -> Value {
  return make_bool(as_bool(lhs) && as_bool(rhs));
}

[[nodiscard]] inline auto And(const Arguments& args) -> Value {
  return AndBinary(args[0], args[1]);
}

[[nodiscard]] inline auto OrBinary(const Value& lhs, const Value& rhs) -> Value { return make_bool(as_bool(lhs) || as_bool(rhs)); }

[[nodiscard]] inline auto Or(const Arguments& args) -> Value { return OrBinary(args[0], args[1]); }

// Output

[[nodiscard]] inline auto Println(Value arg) -> Value {
  // Prints the value, returns it unchanged.
  std::string text;
  append_value_varargs(text, arg);
  text += '\n';
  write_runtime_output(text, true);
  return arg;
}

[[nodiscard]] inline auto Printf(Value arg) -> Value {
  // arg = [format, arg0, arg1, ...]
  // Prints formatted text and returns the original argument tuple unchanged.
  if (!arg.is_tuple()) {
    const std::string fmt = to_string(arg);
    write_runtime_output(format_values(fmt, std::vector<Value>{}));
    return make_tuple(std::move(arg));
  }

  const auto& args = as_tuple(arg);
  if (args.size() == 0U) {
    throw std::logic_error{"internal error: Printf called with unexpected validated arity"};
  }
  const std::string fmt = to_string(tuple_at(arg, 0));
  std::vector<Value> values;
  values.reserve(args.size() - 1U);
  for (std::size_t arg_index = 1; arg_index < args.size(); ++arg_index) {
    values.push_back(tuple_at(arg, arg_index));
  }

  write_runtime_output(format_values(fmt, values));
  return arg;
}

[[nodiscard]] inline auto read_input_line() -> Value {
  std::string line;
  if (!std::getline(runtime_input_stream(), line)) {
    return make_string("");
  }
  return make_string(line);
}

[[nodiscard]] inline auto Input_Void(const Arguments& /*args*/) -> Value {
  return read_input_line();
}

[[nodiscard]] inline auto Input_String(Value arg) -> Value {
  write_runtime_output(to_string(arg), true);
  return read_input_line();
}

[[nodiscard]] inline auto GetArgs(const Arguments& /*args*/) -> Value {
  const auto args = get_process_args();
  TupleBuilder out(args.size());
  for (const auto& process_arg : args) {
    out.push(make_string(process_arg));
  }
  return std::move(out).finish();
}

[[nodiscard]] inline auto Type(Value arg) -> Value {
  return make_string(type_name(arg));
}

// Casts whose argument has static type Any are followed by a CheckType or TryCheckType instruction that checks
// the value; any other cast is already known to succeed.
[[nodiscard]] inline auto Cast(Value arg) -> Value { return arg; }

[[nodiscard]] inline auto ResultOk(Value arg) -> Value;

[[nodiscard]] inline auto TryCast(Value arg) -> Value { return ResultOk(std::move(arg)); }

[[nodiscard]] inline auto ToInt64(Value arg) -> Value { return make_int(as_int_value_strict(arg)); }

[[nodiscard]] inline auto ToUInt64(Value arg) -> Value {
  const Value value = std::move(arg);
  return visit_number(value, 
      [](const Int signed_value) -> Value {
        if (signed_value < 0) {
          throw std::invalid_argument{std::format("cannot convert the negative Int64 {} to UInt64", signed_value)};
        }
        return make_uint(static_cast<UInt>(signed_value));
      },
      [](const UInt unsigned_value) -> Value { return make_uint(unsigned_value); },
      [](const Float float_value) -> Value {
        if (!std::isfinite(float_value) || std::floor(float_value) != float_value || float_value < 0.0) {
          throw std::invalid_argument{std::format("cannot convert the Float64 {} to UInt64", float_value)};
        }
        if (float_value > static_cast<double>(std::numeric_limits<UInt>::max())) {
          throw std::out_of_range{std::format("the Float64 {} is out of UInt64 range", float_value)};
        }
        return make_uint(static_cast<UInt>(float_value));
      });
}

[[nodiscard]] inline auto ToFloat64(Value arg) -> Value {
  return make_float(to_double(arg));
}

[[nodiscard]] inline auto Exit_Void(const Arguments& /*args*/) -> Value {
  std::exit(0);
}

[[nodiscard]] inline auto Exit_Int64(Value arg) -> Value {
  auto to_exit_code = [](const Value& value) -> int {
    const Int code = int64_value(value);
    if (code < static_cast<Int>(std::numeric_limits<int>::min()) ||
        code > static_cast<Int>(std::numeric_limits<int>::max())) {
      throw std::invalid_argument{std::format("exit code {} is out of range for this host", code)};
    }
    return static_cast<int>(code);
  };

  std::exit(to_exit_code(arg));
}

// Control flow (templated: functions remain concrete C++ callables)

[[nodiscard]] inline auto Select(const Arguments& args) -> Value {
  // args = [condition, true_val, false_val], all Values
  return as_bool(args[0]) ? args[1] : args[2];
}

[[nodiscard]] inline auto Match(const Arguments& args) -> Value {
  // args = [value, [pattern, handler], [pattern, handler], ...]
  // Pattern wildcard is encoded as the lowering sentinel string.
  // Callable patterns are predicates: pattern(subject) -> Bool.
  // Std.Match is direct-call only, and the type checker enforces this shape at each call.
  const Value& subject = args[0];
  for (std::size_t case_index = 1; case_index < args.size(); ++case_index) {
    const Value& pattern = tuple_at(args[case_index], 0);
    const Value& handler = tuple_at(args[case_index], 1);

    const bool wildcard_match = pattern.is_string() && as_string(pattern) == common::kMatchWildcardSentinel;
    bool predicate_match = false;
    if (!wildcard_match && pattern.is_function()) {
      predicate_match = as_bool(invoke_callable(pattern, subject));
    }

    if (wildcard_match || predicate_match || pattern == subject) {
      return invoke_callable(handler, subject);
    }
  }

  throw std::runtime_error{std::string{k_match_no_case_message}};
}

// A Result is stored as a (Bool tag, payload) tuple. Its shape is guaranteed by the Result<T, E> type, by the
// checked cast to Result, and by the host entry check, so the Result builtins do not check it again.
[[nodiscard]] inline auto ResultOk(Value arg) -> Value {
  return make_tuple(make_bool(true), std::move(arg));
}

[[nodiscard]] inline auto ResultErr(Value arg) -> Value {
  return make_tuple(make_bool(false), std::move(arg));
}

[[nodiscard]] inline auto ResultPayload(Value arg) -> Value { return tuple_at(arg, 1); }

[[nodiscard]] inline auto ResultIsOk(Value arg) -> Value { return tuple_at(arg, 0); }

[[nodiscard]] inline auto ResultIsErr(Value arg) -> Value { return make_bool(!as_bool(tuple_at(arg, 0))); }

[[nodiscard]] inline auto ResultUnwrap(Value arg) -> Value {
  if (!as_bool(tuple_at(arg, 0))) {
    throw std::runtime_error{"expected Ok, got Err: " + to_string(tuple_at(arg, 1))};
  }
  return tuple_at(arg, 1);
}

[[nodiscard]] inline auto ResultUnwrapErr(Value arg) -> Value {
  if (as_bool(tuple_at(arg, 0))) {
    throw std::runtime_error{"expected Err, got Ok: " + to_string(tuple_at(arg, 1))};
  }
  return tuple_at(arg, 1);
}

// The functions over a Result. A Result that the argument holds alone gives up its payload rather than copying it.

// args = [result, func]: Ok(func(value)) for an Ok, and an Err as it is.
[[nodiscard]] inline auto ResultMap(const Arguments& args) -> Value {
  Value result = args.take(0);
  if (!as_bool(tuple_at(result, 0))) {
    return result;
  }
  return ResultOk(invoke_callable(args[1], take_tuple_item(result, 1)));
}

// args = [result, func]: Err(func(error)) for an Err, and an Ok as it is.
[[nodiscard]] inline auto ResultMapErr(const Arguments& args) -> Value {
  Value result = args.take(0);
  if (as_bool(tuple_at(result, 0))) {
    return result;
  }
  return ResultErr(invoke_callable(args[1], take_tuple_item(result, 1)));
}

// args = [result, func]: func(value), itself a Result, for an Ok, and an Err as it is.
[[nodiscard]] inline auto ResultAndThen(const Arguments& args) -> Value {
  Value result = args.take(0);
  if (!as_bool(tuple_at(result, 0))) {
    return result;
  }
  return invoke_callable(args[1], take_tuple_item(result, 1));
}

// args = [result, func]: func(error), itself a Result, for an Err, and an Ok as it is.
[[nodiscard]] inline auto ResultOrElse(const Arguments& args) -> Value {
  Value result = args.take(0);
  if (as_bool(tuple_at(result, 0))) {
    return result;
  }
  return invoke_callable(args[1], take_tuple_item(result, 1));
}

// args = [result, default]: the value of an Ok, and the default for an Err.
[[nodiscard]] inline auto ResultUnwrapOr(const Arguments& args) -> Value {
  Value result = args.take(0);
  if (!as_bool(tuple_at(result, 0))) {
    return args.take(1);
  }
  return take_tuple_item(result, 1);
}

// A failure the program reports itself, with Std.Panic, Std.Assert, or Std.AssertEqual. The VM shows its message as it
// is, without the builtin's name in front: the message is the program's.
class ProgramFailure : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

[[noreturn]] inline auto Panic(Value arg) -> Value { throw ProgramFailure{as_string(arg)}; }

// args = [condition, message]
[[nodiscard]] inline auto Assert(const Arguments& args) -> Value {
  if (!as_bool(args[0])) {
    throw ProgramFailure{as_string(args[1])};
  }
  return make_tuple();
}

// args = [actual, expected]
[[nodiscard]] inline auto AssertEqual(const Arguments& args) -> Value {
  if (!(args[0] == args[1])) {
    throw ProgramFailure{std::format("expected {}, got {}", to_string(args[1]), to_string(args[0]))};
  }
  return make_tuple();
}

// The VM's function values hand Std.Try their failure as a value (call_or_fail), which saves throwing it again; a
// function made from C++ throws it, and the catch takes that.
[[nodiscard]] inline auto Try(const Arguments& args) -> Value {
  const Value& value = args[0];
  const Value& function_ref = args[1];
  try {
    std::string error;
    if (auto result = as_callable(function_ref).call_or_fail(value, error); result.has_value()) {
      return ResultOk(std::move(*result));
    }
    return ResultErr(make_string(std::move(error)));
  } catch (const std::exception& ex) {
    return ResultErr(make_string(ex.what()));
  }
}

[[nodiscard]] inline auto Apply(const Arguments& args) -> Value {
  // args = [value, func_ref]
  return invoke_callable(args[1], args[0]);
}

[[nodiscard]] inline auto Branch(const Arguments& args) -> Value {
  // args = [condition, value, true_func_ref, false_func_ref]
  const Value& condition = args[0];
  const Value& value = args[1];
  const Value& true_func = args[2];
  const Value& false_func = args[3];
  return as_bool(condition) ? invoke_callable(true_func, value) : invoke_callable(false_func, value);
}

[[nodiscard]] inline auto Loop(const Arguments& args) -> Value {
  // args = [state, continue_func_ref, step_func_ref]
  Value state = args.take(0);
  const Callable& continue_callable = as_callable(args[1]);
  const Callable& step_callable = as_callable(args[2]);
  while (as_bool(continue_callable.call(state))) {
    state = step_callable.call(std::move(state));
  }
  return state;
}

[[nodiscard]] inline auto LoopN(const Arguments& args) -> Value {
  // args = [state, continue_func_ref, step_func_ref, max_iters]
  Value state = args.take(0);
  const Callable& continue_callable = as_callable(args[1]);
  const Callable& step_callable = as_callable(args[2]);
  const std::size_t max_iters = count_index(args[3], "max_iters");

  std::size_t steps = 0;
  while (as_bool(continue_callable.call(state))) {
    if (steps >= max_iters) {
      throw std::runtime_error{loop_limit_message(max_iters)};
    }
    state = step_callable.call(std::move(state));
    ++steps;
  }
  return state;
}

}  // namespace fleaux::runtime
