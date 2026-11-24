#pragma once

#include <bit>

#include "fleaux/runtime/value.hpp"

namespace fleaux::runtime {

namespace detail {

inline constexpr UInt k_random_gamma = 0x9E3779B97F4A7C15ULL;
inline constexpr UInt k_random_split_left_salt = 0xA0761D6478BD642FULL;
inline constexpr UInt k_random_split_right_salt = 0xE7037ED1A0B428DBULL;

[[nodiscard]] inline auto random_seed_step(const UInt seed) -> UInt {
  UInt mixed = seed + k_random_gamma;
  mixed = (mixed ^ (mixed >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  mixed = (mixed ^ (mixed >> 27U)) * 0x94D049BB133111EBULL;
  return mixed ^ (mixed >> 31U);
}

// The seed is a UInt64 by type.
[[nodiscard]] inline auto random_seed_from_arg(Value arg) -> UInt {
  return uint64_value(arg);
}

[[nodiscard]] inline auto random_unit_float(const UInt seed) -> double {
  return std::ldexp(static_cast<double>(seed >> 11U), -53);
}

// The generator is a RandomGenerator by type, which the type checker or a host entry check guarantees.
[[nodiscard]] inline auto random_generator_state_from_arg(Value arg) -> UInt {
  const Value generator = std::move(arg);
  if (!generator.is_random_generator()) {
    throw kind_error("RandomGenerator");
  }
  return generator.random_state();
}

[[nodiscard]] inline auto random_step_pair(const UInt state) -> std::pair<UInt, Value> {
  const UInt next_state = random_seed_step(state);
  return {next_state, make_random_generator(next_state)};
}

}  // namespace detail

[[nodiscard]] inline auto RandomCreate(Value arg) -> Value {
  return make_random_generator(detail::random_seed_from_arg(std::move(arg)));
}

[[nodiscard]] inline auto RandomNextUInt64(Value arg) -> Value {
  const auto [next_state, next_generator] =
      detail::random_step_pair(detail::random_generator_state_from_arg(std::move(arg)));
  return make_tuple(make_uint(next_state), next_generator);
}

[[nodiscard]] inline auto RandomNextInt64(Value arg) -> Value {
  const auto [next_state, next_generator] =
      detail::random_step_pair(detail::random_generator_state_from_arg(std::move(arg)));
  return make_tuple(make_int(std::bit_cast<Int>(next_state)), next_generator);
}

[[nodiscard]] inline auto RandomNextFloat64(Value arg) -> Value {
  const auto [next_state, next_generator] =
      detail::random_step_pair(detail::random_generator_state_from_arg(std::move(arg)));
  return make_tuple(make_float(detail::random_unit_float(next_state)), next_generator);
}

[[nodiscard]] inline auto RandomNextBool(Value arg) -> Value {
  const auto [next_state, next_generator] =
      detail::random_step_pair(detail::random_generator_state_from_arg(std::move(arg)));
  return make_tuple(make_bool((next_state & 1ULL) != 0ULL), next_generator);
}

[[nodiscard]] inline auto RandomSplit(Value arg) -> Value {
  const UInt state = detail::random_generator_state_from_arg(std::move(arg));
  const UInt left_state = detail::random_seed_step(state ^ detail::k_random_split_left_salt);
  const UInt right_state = detail::random_seed_step(state ^ detail::k_random_split_right_salt);
  return make_tuple(make_random_generator(left_state), make_random_generator(right_state));
}

}  // namespace fleaux::runtime



