#pragma once

#include "fleaux/runtime/value.hpp"

namespace fleaux::runtime {

[[nodiscard]] inline auto Sqrt(Value arg) -> Value {
  return num_result(std::sqrt(to_double(unwrap_singleton_arg(std::move(arg)))), false, true);
}

[[nodiscard]] inline auto Sin(Value arg) -> Value {
  return num_result(std::sin(to_double(unwrap_singleton_arg(std::move(arg)))), false, true);
}

[[nodiscard]] inline auto Cos(Value arg) -> Value {
  return num_result(std::cos(to_double(unwrap_singleton_arg(std::move(arg)))), false, true);
}

[[nodiscard]] inline auto Tan(Value arg) -> Value {
  return num_result(std::tan(to_double(unwrap_singleton_arg(std::move(arg)))), false, true);
}

[[nodiscard]] inline auto MathFloor(Value arg) -> Value {
  return num_result(std::floor(to_double(unwrap_singleton_arg(std::move(arg)))));
}

[[nodiscard]] inline auto MathCeil(Value arg) -> Value {
  return num_result(std::ceil(to_double(unwrap_singleton_arg(std::move(arg)))));
}

[[nodiscard]] inline auto MathAbs(Value arg) -> Value {
  return num_result(std::abs(to_double(unwrap_singleton_arg(std::move(arg)))));
}

[[nodiscard]] inline auto MathLog(Value arg) -> Value {
  return num_result(std::log(to_double(unwrap_singleton_arg(std::move(arg)))));
}

[[nodiscard]] inline auto MathClamp(Value arg) -> Value {
  const auto& args = as_array(arg);
  if (args.Size() != 3) {
    throw std::invalid_argument{"MathClamp expects 3 arguments"};
  }
  const double value_to_clamp = to_double(*args.TryGet(0));
  const double lower_bound = to_double(*args.TryGet(1));
  const double upper_bound = to_double(*args.TryGet(2));
  return num_result(std::clamp(value_to_clamp, lower_bound, upper_bound));
}
}  // namespace fleaux::runtime