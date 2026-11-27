#pragma once

#include <algorithm>
#include <cmath>
#include <format>

#include "fleaux/runtime/builtins_core.hpp"
#include "fleaux/runtime/value.hpp"

namespace fleaux::runtime {

// Every Std.Math builtin here is declared to return Float64, so each result is a Float64 even when it is a whole
// number.

[[nodiscard]] inline auto Sqrt(Value arg) -> Value {
  return make_float(std::sqrt(to_double(arg)));
}

[[nodiscard]] inline auto Sin(Value arg) -> Value {
  return make_float(std::sin(to_double(arg)));
}

[[nodiscard]] inline auto Cos(Value arg) -> Value {
  return make_float(std::cos(to_double(arg)));
}

[[nodiscard]] inline auto Tan(Value arg) -> Value {
  return make_float(std::tan(to_double(arg)));
}

[[nodiscard]] inline auto MathFloor(Value arg) -> Value {
  return make_float(std::floor(to_double(arg)));
}

[[nodiscard]] inline auto MathCeil(Value arg) -> Value {
  return make_float(std::ceil(to_double(arg)));
}

// Std.Math.Abs<T: Signed>: an Int64's absolute value, which overflows for the smallest Int64, or a Float64's.
[[nodiscard]] inline auto MathAbs(Value arg) -> Value {
  if (arg.is_int()) {
    const Int value = arg.int_value();
    return make_int(value < 0 ? require_integer_result(integer::checked_neg(value)) : value);
  }
  return make_float(std::abs(to_double(arg)));
}

[[nodiscard]] inline auto MathLog(Value arg) -> Value {
  return make_float(std::log(to_double(arg)));
}

// Std.Math.Clamp<T: Numeric>(x, lo, hi): x, but no less than lo and no more than hi, in x's type.
[[nodiscard]] inline auto MathClamp(const Arguments& args) -> Value {
  const Value& value = args[0];
  const Value& lower_bound = args[1];
  const Value& upper_bound = args[2];
  if (compare_values_for_sort(lower_bound, upper_bound) > 0) {
    throw std::invalid_argument{std::format("lo {} is greater than hi {}", to_string(lower_bound), to_string(upper_bound))};
  }
  if (compare_values_for_sort(value, lower_bound) < 0) {
    return lower_bound;
  }
  return compare_values_for_sort(value, upper_bound) > 0 ? upper_bound : value;
}

[[nodiscard]] inline auto MathExp(Value arg) -> Value { return make_float(std::exp(to_double(arg))); }

[[nodiscard]] inline auto MathLog2(Value arg) -> Value { return make_float(std::log2(to_double(arg))); }

[[nodiscard]] inline auto MathLog10(Value arg) -> Value { return make_float(std::log10(to_double(arg))); }

// Rounds halfway cases away from zero.
[[nodiscard]] inline auto MathRound(Value arg) -> Value { return make_float(std::round(to_double(arg))); }

[[nodiscard]] inline auto MathTrunc(Value arg) -> Value { return make_float(std::trunc(to_double(arg))); }

[[nodiscard]] inline auto MathAsin(Value arg) -> Value { return make_float(std::asin(to_double(arg))); }

[[nodiscard]] inline auto MathAcos(Value arg) -> Value { return make_float(std::acos(to_double(arg))); }

[[nodiscard]] inline auto MathAtan(Value arg) -> Value { return make_float(std::atan(to_double(arg))); }

// args = [y, x]: the angle of the point (x, y), from -pi to pi.
[[nodiscard]] inline auto MathAtan2(const Arguments& args) -> Value {
  return make_float(std::atan2(to_double(args[0]), to_double(args[1])));
}

// Std.Min and Std.Max over two Comparable values of one type; the first on a tie.
[[nodiscard]] inline auto Min(const Arguments& args) -> Value {
  return compare_values_for_sort(args[1], args[0]) < 0 ? args.take(1) : args.take(0);
}

[[nodiscard]] inline auto Max(const Arguments& args) -> Value {
  return compare_values_for_sort(args[1], args[0]) > 0 ? args.take(1) : args.take(0);
}
}  // namespace fleaux::runtime
