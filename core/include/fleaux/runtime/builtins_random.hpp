#pragma once

#include <bit>

#include "fleaux/runtime/value.hpp"

namespace fleaux::runtime {

namespace detail {

inline constexpr UInt k_random_gamma = 0x9E3779B97F4A7C15ULL;
inline constexpr std::string_view k_random_generator_tag = "__fleaux_rng__";
inline constexpr UInt k_random_generator_version = 1ULL;
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
  return uint64_value(unwrap_singleton_arg(std::move(arg)));
}

[[nodiscard]] inline auto random_unit_float(const UInt seed) -> double {
  return std::ldexp(static_cast<double>(seed >> 11U), -53);
}

[[nodiscard]] inline auto make_random_generator(const UInt state) -> Value {
  Array token;
  token.Reserve(3);
  token.EmplaceBack(String{k_random_generator_tag});
  token.EmplaceBack(k_random_generator_version);
  token.EmplaceBack(state);
  return Value{std::move(token)};
}

// The generator is a RandomGenerator by type: every generator reaching a builtin was built by make_random_generator
// or passed is_valid_random_generator in a cast or host entry check.
[[nodiscard]] inline auto random_generator_state_from_arg(Value arg) -> UInt {
  const Value generator = unwrap_singleton_arg(std::move(arg));
  return uint64_value(*as_array(generator).TryGet(2));
}

// True for a well-formed generator token: (tag, version, state) with an unsigned version equal to the current one
// and an unsigned state, exactly as make_random_generator builds it.
[[nodiscard]] inline auto is_valid_random_generator(const Value& value) -> bool {
  const auto& token = value.TryGetArray();
  if (!token || token->Size() != 3U) {
    return false;
  }
  const auto& tag = token->TryGet(0)->TryGetString();
  const auto& version = token->TryGet(1)->TryGetNumber();
  const auto& state = token->TryGet(2)->TryGetNumber();
  return tag && *tag == k_random_generator_tag && version && version->IsUInt() &&
         *version->GetUInt() == k_random_generator_version && state && state->IsUInt();
}

[[nodiscard]] inline auto random_step_pair(const UInt state) -> std::pair<UInt, Value> {
  const UInt next_state = random_seed_step(state);
  return {next_state, make_random_generator(next_state)};
}

}  // namespace detail

[[nodiscard]] inline auto RandomCreate(Value arg) -> Value {
  return detail::make_random_generator(detail::random_seed_from_arg(std::move(arg)));
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
  return make_tuple(detail::make_random_generator(left_state), detail::make_random_generator(right_state));
}

}  // namespace fleaux::runtime



