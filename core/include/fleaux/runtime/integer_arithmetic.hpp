#pragma once

#include <concepts>
#include <limits>
#include <optional>
#include <type_traits>

// Exact fixed-width integer arithmetic used by the Fleaux runtime for Int64 and UInt64 values.
//
// Three overflow behaviors are provided for addition, subtraction, and multiplication:
// - checked_*: returns std::nullopt when the exact result does not fit in T
// - wrapping_*: returns the result modulo 2^N (two's complement for signed types)
// - saturating_*: clamps the result to the nearest representable value
//
// Only standard C++20 is used. Wrapping relies on the C++20 guarantee that converting an unsigned value to a signed
// type of the same width is modular.
namespace fleaux::runtime::integer {

template <std::integral T>
[[nodiscard]] constexpr auto wrapping_add(const T lhs, const T rhs) -> T {
  using U = std::make_unsigned_t<T>;
  return static_cast<T>(static_cast<U>(lhs) + static_cast<U>(rhs));
}

template <std::integral T>
[[nodiscard]] constexpr auto wrapping_sub(const T lhs, const T rhs) -> T {
  using U = std::make_unsigned_t<T>;
  return static_cast<T>(static_cast<U>(lhs) - static_cast<U>(rhs));
}

template <std::integral T>
[[nodiscard]] constexpr auto wrapping_mul(const T lhs, const T rhs) -> T {
  using U = std::make_unsigned_t<T>;
  return static_cast<T>(static_cast<U>(lhs) * static_cast<U>(rhs));
}

template <std::integral T>
[[nodiscard]] constexpr auto add_overflows(const T lhs, const T rhs) -> bool {
  if constexpr (std::is_signed_v<T>) {
    return (rhs > 0 && lhs > std::numeric_limits<T>::max() - rhs) ||
           (rhs < 0 && lhs < std::numeric_limits<T>::min() - rhs);
  } else {
    return lhs > std::numeric_limits<T>::max() - rhs;
  }
}

template <std::integral T>
[[nodiscard]] constexpr auto sub_overflows(const T lhs, const T rhs) -> bool {
  if constexpr (std::is_signed_v<T>) {
    return (rhs < 0 && lhs > std::numeric_limits<T>::max() + rhs) ||
           (rhs > 0 && lhs < std::numeric_limits<T>::min() + rhs);
  } else {
    return lhs < rhs;
  }
}

template <std::integral T>
[[nodiscard]] constexpr auto mul_overflows(const T lhs, const T rhs) -> bool {
  if (lhs == 0 || rhs == 0) {
    return false;
  }
  if constexpr (std::is_signed_v<T>) {
    constexpr T kMax = std::numeric_limits<T>::max();
    constexpr T kMin = std::numeric_limits<T>::min();
    if (lhs > 0) {
      return rhs > 0 ? lhs > kMax / rhs : rhs < kMin / lhs;
    }
    return rhs > 0 ? lhs < kMin / rhs : lhs < kMax / rhs;
  } else {
    return rhs > std::numeric_limits<T>::max() / lhs;
  }
}

template <std::integral T>
[[nodiscard]] constexpr auto checked_add(const T lhs, const T rhs) -> std::optional<T> {
  if (add_overflows(lhs, rhs)) {
    return std::nullopt;
  }
  return static_cast<T>(lhs + rhs);
}

template <std::integral T>
[[nodiscard]] constexpr auto checked_sub(const T lhs, const T rhs) -> std::optional<T> {
  if (sub_overflows(lhs, rhs)) {
    return std::nullopt;
  }
  return static_cast<T>(lhs - rhs);
}

template <std::integral T>
[[nodiscard]] constexpr auto checked_mul(const T lhs, const T rhs) -> std::optional<T> {
  if (mul_overflows(lhs, rhs)) {
    return std::nullopt;
  }
  return static_cast<T>(lhs * rhs);
}

template <std::integral T>
[[nodiscard]] constexpr auto saturating_add(const T lhs, const T rhs) -> T {
  if (!add_overflows(lhs, rhs)) {
    return static_cast<T>(lhs + rhs);
  }
  if constexpr (std::is_signed_v<T>) {
    return rhs > 0 ? std::numeric_limits<T>::max() : std::numeric_limits<T>::min();
  } else {
    return std::numeric_limits<T>::max();
  }
}

template <std::integral T>
[[nodiscard]] constexpr auto saturating_sub(const T lhs, const T rhs) -> T {
  if (!sub_overflows(lhs, rhs)) {
    return static_cast<T>(lhs - rhs);
  }
  if constexpr (std::is_signed_v<T>) {
    return rhs < 0 ? std::numeric_limits<T>::max() : std::numeric_limits<T>::min();
  } else {
    return std::numeric_limits<T>::min();
  }
}

template <std::integral T>
[[nodiscard]] constexpr auto saturating_mul(const T lhs, const T rhs) -> T {
  if (!mul_overflows(lhs, rhs)) {
    return static_cast<T>(lhs * rhs);
  }
  if constexpr (std::is_signed_v<T>) {
    return (lhs < 0) != (rhs < 0) ? std::numeric_limits<T>::min() : std::numeric_limits<T>::max();
  } else {
    return std::numeric_limits<T>::max();
  }
}

// Truncating division. Precondition: rhs != 0. Returns std::nullopt only for the signed overflow min / -1.
template <std::integral T>
[[nodiscard]] constexpr auto checked_div(const T lhs, const T rhs) -> std::optional<T> {
  if constexpr (std::is_signed_v<T>) {
    if (lhs == std::numeric_limits<T>::min() && rhs == -1) {
      return std::nullopt;
    }
  }
  return static_cast<T>(lhs / rhs);
}

// Remainder with the sign of the dividend, matching truncating division. Precondition: rhs != 0.
template <std::integral T>
[[nodiscard]] constexpr auto remainder(const T lhs, const T rhs) -> T {
  if constexpr (std::is_signed_v<T>) {
    if (rhs == -1) {
      return 0;  // Avoids the undefined min % -1.
    }
  }
  return static_cast<T>(lhs % rhs);
}

// Exponentiation by squaring. Precondition: exponent >= 0. Returns std::nullopt on overflow.
template <std::integral T>
[[nodiscard]] constexpr auto checked_pow(T base, T exponent) -> std::optional<T> {
  T result = 1;
  while (exponent > 0) {
    if ((exponent & 1) != 0) {
      const auto next = checked_mul(result, base);
      if (!next.has_value()) {
        return std::nullopt;
      }
      result = *next;
    }
    exponent = static_cast<T>(exponent / 2);
    if (exponent > 0) {
      const auto squared = checked_mul(base, base);
      if (!squared.has_value()) {
        return std::nullopt;
      }
      base = *squared;
    }
  }
  return result;
}

template <std::signed_integral T>
[[nodiscard]] constexpr auto checked_neg(const T value) -> std::optional<T> {
  if (value == std::numeric_limits<T>::min()) {
    return std::nullopt;
  }
  return static_cast<T>(-value);
}

}  // namespace fleaux::runtime::integer
