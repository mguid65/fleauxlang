#pragma once

#include <algorithm>
#include <cmath>

#include "fleaux/runtime/value.hpp"

namespace fleaux::runtime {

// Every Std.Math builtin here is declared to return Float64, so each result is a Float64 even when it is a whole
// number.

[[nodiscard]] inline auto Sqrt(Value arg) -> Value {
  return make_float(std::sqrt(to_double(unwrap_singleton_arg(std::move(arg)))));
}

[[nodiscard]] inline auto Sin(Value arg) -> Value {
  return make_float(std::sin(to_double(unwrap_singleton_arg(std::move(arg)))));
}

[[nodiscard]] inline auto Cos(Value arg) -> Value {
  return make_float(std::cos(to_double(unwrap_singleton_arg(std::move(arg)))));
}

[[nodiscard]] inline auto Tan(Value arg) -> Value {
  return make_float(std::tan(to_double(unwrap_singleton_arg(std::move(arg)))));
}

[[nodiscard]] inline auto MathFloor(Value arg) -> Value {
  return make_float(std::floor(to_double(unwrap_singleton_arg(std::move(arg)))));
}

[[nodiscard]] inline auto MathCeil(Value arg) -> Value {
  return make_float(std::ceil(to_double(unwrap_singleton_arg(std::move(arg)))));
}

[[nodiscard]] inline auto MathAbs(Value arg) -> Value {
  return make_float(std::abs(to_double(unwrap_singleton_arg(std::move(arg)))));
}

[[nodiscard]] inline auto MathLog(Value arg) -> Value {
  return make_float(std::log(to_double(unwrap_singleton_arg(std::move(arg)))));
}

[[nodiscard]] inline auto MathClamp(Value arg) -> Value {
  const double value_to_clamp = to_double(array_at(arg, 0));
  const double lower_bound = to_double(array_at(arg, 1));
  const double upper_bound = to_double(array_at(arg, 2));
  return make_float(std::clamp(value_to_clamp, lower_bound, upper_bound));
}
}  // namespace fleaux::runtime
