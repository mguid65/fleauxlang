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

[[nodiscard]] inline auto random_seed_from_arg(Value arg, const std::string_view builtin_name) -> UInt {
  const Value seed = unwrap_singleton_arg(std::move(arg));
  return as_number(seed).Visit(
      [&](const Int signed_value) -> UInt {
        if (signed_value < 0) {
          throw std::invalid_argument(std::format("{}: seed must be non-negative", builtin_name));
        }
        return static_cast<UInt>(signed_value);
      },
      [](const UInt unsigned_value) -> UInt { return unsigned_value; },
      [&](const Float float_value) -> UInt {
        if (!std::isfinite(float_value) || std::floor(float_value) != float_value) {
          throw std::invalid_argument(std::format("{}: seed expects an integer value", builtin_name));
        }
        if (float_value < 0.0) {
          throw std::invalid_argument(std::format("{}: seed must be non-negative", builtin_name));
        }
        if (float_value > static_cast<double>(std::numeric_limits<UInt>::max())) {
          throw std::out_of_range(std::format("{}: seed out of UInt64 range", builtin_name));
        }
        return static_cast<UInt>(float_value);
      });
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

[[nodiscard]] inline auto random_generator_state_from_arg(Value arg, const std::string_view builtin_name) -> UInt {
  const Value generator = unwrap_singleton_arg(std::move(arg));
  const auto& token = generator.TryGetArray();
  if (!token) {
    throw std::invalid_argument(std::format("{}: expected RandomGenerator", builtin_name));
  }
  if (token->Size() != 3) {
    throw std::invalid_argument(std::format("{}: expected RandomGenerator", builtin_name));
  }

  const auto& tag = token->TryGet(0)->TryGetString();
  if (!tag || *tag != k_random_generator_tag) {
    throw std::invalid_argument(std::format("{}: expected RandomGenerator", builtin_name));
  }

  const UInt version = as_number(*token->TryGet(1)).Visit(
      [](const Int signed_value) -> UInt {
        if (signed_value < 0) {
          throw std::invalid_argument{"RandomGenerator: invalid version"};
        }
        return static_cast<UInt>(signed_value);
      },
      [](const UInt unsigned_value) -> UInt { return unsigned_value; },
      [](const Float float_value) -> UInt {
        if (!std::isfinite(float_value) || std::floor(float_value) != float_value || float_value < 0.0 ||
            float_value > static_cast<double>(std::numeric_limits<UInt>::max())) {
          throw std::invalid_argument{"RandomGenerator: invalid version"};
        }
        return static_cast<UInt>(float_value);
      });
  if (version != k_random_generator_version) {
    throw std::invalid_argument(std::format("{}: unsupported RandomGenerator version", builtin_name));
  }

  return as_number(*token->TryGet(2)).Visit(
      [](const Int signed_value) -> UInt {
        if (signed_value < 0) {
          throw std::invalid_argument{"RandomGenerator: invalid state"};
        }
        return static_cast<UInt>(signed_value);
      },
      [](const UInt unsigned_value) -> UInt { return unsigned_value; },
      [](const Float float_value) -> UInt {
        if (!std::isfinite(float_value) || std::floor(float_value) != float_value || float_value < 0.0 ||
            float_value > static_cast<double>(std::numeric_limits<UInt>::max())) {
          throw std::invalid_argument{"RandomGenerator: invalid state"};
        }
        return static_cast<UInt>(float_value);
      });
}

[[nodiscard]] inline auto random_step_pair(const UInt state) -> std::pair<UInt, Value> {
  const UInt next_state = random_seed_step(state);
  return {next_state, make_random_generator(next_state)};
}

}  // namespace detail

[[nodiscard]] inline auto RandomCreate(Value arg) -> Value {
  return detail::make_random_generator(detail::random_seed_from_arg(std::move(arg), "RandomCreate"));
}

[[nodiscard]] inline auto RandomNextUInt64(Value arg) -> Value {
  const auto [next_state, next_generator] =
      detail::random_step_pair(detail::random_generator_state_from_arg(std::move(arg), "RandomNextUInt64"));
  return make_tuple(make_uint(next_state), next_generator);
}

[[nodiscard]] inline auto RandomNextInt64(Value arg) -> Value {
  const auto [next_state, next_generator] =
      detail::random_step_pair(detail::random_generator_state_from_arg(std::move(arg), "RandomNextInt64"));
  return make_tuple(make_int(std::bit_cast<Int>(next_state)), next_generator);
}

[[nodiscard]] inline auto RandomNextFloat64(Value arg) -> Value {
  const auto [next_state, next_generator] =
      detail::random_step_pair(detail::random_generator_state_from_arg(std::move(arg), "RandomNextFloat64"));
  return make_tuple(make_float(detail::random_unit_float(next_state)), next_generator);
}

[[nodiscard]] inline auto RandomNextBool(Value arg) -> Value {
  const auto [next_state, next_generator] =
      detail::random_step_pair(detail::random_generator_state_from_arg(std::move(arg), "RandomNextBool"));
  return make_tuple(make_bool((next_state & 1ULL) != 0ULL), next_generator);
}

[[nodiscard]] inline auto RandomSplit(Value arg) -> Value {
  const UInt state = detail::random_generator_state_from_arg(std::move(arg), "RandomSplit");
  const UInt left_state = detail::random_seed_step(state ^ detail::k_random_split_left_salt);
  const UInt right_state = detail::random_seed_step(state ^ detail::k_random_split_right_salt);
  return make_tuple(detail::make_random_generator(left_state), detail::make_random_generator(right_state));
}

}  // namespace fleaux::runtime



