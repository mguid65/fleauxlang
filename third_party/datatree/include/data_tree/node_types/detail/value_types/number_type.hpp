/**
 * @brief A number holder abstraction
 * @author Matthew Guidry (github: mguid65)
 * @date 2024-02-05
 *
 * @cond IGNORE_LICENSE
 *
 * MIT License
 *
 * Copyright (c) 2024 Matthew Guidry
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * @endcond
 */

#ifndef DATATREE_NUMBER_TYPE_HPP
#define DATATREE_NUMBER_TYPE_HPP

#include <cmath>
#include <compare>
#include <concepts>
#include <cstdint>
#include <functional>
#include <utility>
#include <ostream>

#include "data_tree/common/common.hpp"
#include "data_tree/error/error_type.hpp"

#ifdef _MSC_VER
#define BEGIN_SUPPRESS_C4146 \
_Pragma("warning(push, 0)")  \
_Pragma("warning(disable : 4146)")

#define END_SUPPRESS_C4146 \
_Pragma("warning(pop)")
#else
#define BEGIN_SUPPRESS_C4146
#define END_SUPPRESS_C4146
#endif

namespace mguid {

using IntegerType = std::int64_t;
using UnsignedIntegerType = std::uint64_t;
using DoubleType = double;

namespace detail {
/**
 * @brief A type satisfies this concept if it is an integral or floating point
 * but not a bool
 * @tparam TType A type we are attempting to constrain
 */
template <typename TType>
concept AllowedNumericType = (!std::same_as<std::remove_cvref_t<TType>, bool> &&
                              (std::integral<std::remove_cvref_t<TType>> ||
                               std::floating_point<std::remove_cvref_t<TType>>));

/**
 * @brief A type satisfies this concept if it is a signed integer type but not
 * boolean
 * @tparam TType A type we are attempting to constrain
 */
template <typename TType>
concept SignedIntegerLike = (!std::same_as<std::remove_cvref_t<TType>, bool> &&
                             (std::integral<std::remove_cvref_t<TType>>)) &&
                            std::is_signed_v<std::remove_cvref_t<TType>>;

/**
 * @brief A type satisfies this concept if it is an unsigned integer type but
 * not boolean
 * @tparam TType A type we are attempting to constrain
 */
template <typename TType>
concept UnsignedIntegerLike = (!std::same_as<std::remove_cvref_t<TType>, bool> &&
                               (std::integral<std::remove_cvref_t<TType>>)) &&
                              std::is_unsigned_v<std::remove_cvref_t<TType>>;
}  // namespace detail

/**
 * @brief A class that holds numeric values
 *
 * NOTE: Maybe use tl::expected everywhere
 */
class NumberType {
public:
  // The ordering of these enums matter
  // It affects the three-way comparison
  enum class TypeTag : std::uint8_t { Int, UInt, Double };

  /**
   * @brief Default construct a number with no value
   *
   * Sets the tag to None
   */
  constexpr NumberType() noexcept = default;

  /**
   * @brief Explicit defaults for copy/move construction/assignment
   */
  constexpr NumberType(const NumberType&) noexcept = default;
  constexpr NumberType(NumberType&&) noexcept = default;
  constexpr NumberType& operator=(const NumberType&) noexcept = default;
  constexpr NumberType& operator=(NumberType&&) noexcept = default;

  /**
   * @brief Construct a NumberType from a value
   * @tparam TValueType type of value constrained by AllowedNumericType
   * @param val numeric value
   */
  template <detail::AllowedNumericType TValueType>
  constexpr explicit NumberType(TValueType&& val) noexcept {
    this->operator=(val);
  }

  /**
   * @brief Is this number holding a double value
   * @return True if it is a double, otherwise false
   */
  [[nodiscard]] constexpr auto IsDouble() const noexcept -> bool {
    return m_tag == TypeTag::Double;
  }

  /**
   * @brief Is this number holding an integer value
   * @return True if it is an integer, otherwise false
   */
  [[nodiscard]] constexpr auto IsInt() const noexcept -> bool { return m_tag == TypeTag::Int; }

  /**
   * @brief Is this number holding an unsigned integer value
   * @return True if it is an unsigned integer, otherwise false
   */
  [[nodiscard]] constexpr auto IsUInt() const noexcept -> bool { return m_tag == TypeTag::UInt; }

  /**
   * @brief Try to get a double from this number type
   * @return the double value if it exists, otherwise error
   */
  [[nodiscard]] auto GetDouble() const -> expected<DoubleType, Error> {
    if (m_tag == TypeTag::Double) return m_union.f_value;
    return make_unexpected(Error{.category = Error::Category::BadAccess});
  }

  /**
   * @brief Try to get an integer from this number type
   * @return the integer value if it exists, otherwise error
   */
  [[nodiscard]] auto GetInt() const -> expected<IntegerType, Error> {
    if (m_tag == TypeTag::Int) return m_union.i_value;
    return make_unexpected(Error{.category = Error::Category::BadAccess});
  }

  /**
   * @brief Try to get an integer from this number type
   * @return the integer value if it exists, otherwise error
   */
  [[nodiscard]] auto GetUInt() const -> expected<UnsignedIntegerType, Error> {
    if (m_tag == TypeTag::UInt) return m_union.u_value;
    return make_unexpected(Error{.category = Error::Category::BadAccess});
  }

  /**
   * @brief Assign a value to this number container
   * @tparam TValueType type of value constrained by AllowedNumericType
   * @param val numeric value
   * @return reference to this number instance
   */
  template <detail::AllowedNumericType TValueType>
  constexpr auto operator=(TValueType&& val) noexcept -> NumberType& {
    SetValue(std::forward<TValueType>(val));
    return *this;
  }

  /**
   * @brief Get the type tag of this number container
   * @return the type tag of this number container
   */
  [[nodiscard]] constexpr auto GetTypeTag() const noexcept -> TypeTag { return m_tag; }

  /**
   * @brief Set the value of this numeric type
   * @tparam TValueType type of value constrained by AllowedNumericType
   * @param val numeric value
   */
  template <detail::AllowedNumericType TValueType>
  constexpr void SetValue(TValueType&& val) noexcept {
    if constexpr (std::floating_point<std::remove_cvref_t<TValueType>>) {
      m_union.f_value = std::forward<TValueType>(val);
      m_tag = TypeTag::Double;
    } else if constexpr (detail::SignedIntegerLike<TValueType>) {
      m_union.i_value = std::forward<TValueType>(val);
      m_tag = TypeTag::Int;
    } else if constexpr (detail::UnsignedIntegerLike<TValueType>) {
      m_union.u_value = std::forward<TValueType>(val);
      m_tag = TypeTag::UInt;
    } else {
      // The concept should prevent this from happening but just in case,
      // we put a throw here which isn't allowed in constexpr contexts so this
      // will fail to compile
      Unreachable();
    }
  }

  /**
   * @brief Reset the state of this number container
   */
  constexpr void Reset() noexcept {
    m_tag = TypeTag::Int;
    m_union.i_value = IntegerType{0};
  }

  /**
   * @brief Visit a tree node with a visitor overload set
   *
   * if an `auto&&` overload is provided, then it will be preferred.
   * Use `auto` as a catch all overload
   *
   * @tparam TCallables set of non final callable types
   * @param callables set of non final callables
   * @return the common return type of all callables provided
   */
  template <typename... TCallables>
  decltype(auto) Visit(TCallables&&... callables)
    requires(std::is_invocable_v<decltype(Overload{std::forward<TCallables>(callables)...}),
                                 IntegerType&> &&
             std::is_invocable_v<decltype(Overload{std::forward<TCallables>(callables)...}),
                                 UnsignedIntegerType&> &&
             std::is_invocable_v<decltype(Overload{std::forward<TCallables>(callables)...}),
                                 DoubleType&>)
  {
    auto overload_set = Overload{std::forward<TCallables>(callables)...};
    switch (m_tag) {
      case TypeTag::Int: {
        return std::invoke(overload_set, m_union.i_value);
      }
      case TypeTag::UInt: {
        return std::invoke(overload_set, m_union.u_value);
      }
      case TypeTag::Double: {
        return std::invoke(overload_set, m_union.f_value);
      }
    }
    Unreachable();
  }

  /**
   * @brief Visit a tree node with a visitor overload set
   *
   * if an `auto&&` overload is provided, then it will be preferred.
   * Use `auto` as a catch all overload
   *
   * @tparam TCallables set of non final callable types
   * @param callables set of non final callables
   * @return the common return type of all callables provided
   */
  template <typename... TCallables>
  decltype(auto) Visit(TCallables&&... callables) const
    requires(std::is_invocable_v<decltype(Overload{std::forward<TCallables>(callables)...}),
                                 const IntegerType&> &&
             std::is_invocable_v<decltype(Overload{std::forward<TCallables>(callables)...}),
                                 const UnsignedIntegerType&> &&
             std::is_invocable_v<decltype(Overload{std::forward<TCallables>(callables)...}),
                                 const DoubleType&>)
  {
    auto overload_set = Overload{std::forward<TCallables>(callables)...};
    switch (m_tag) {
      case TypeTag::Int: {
        return std::invoke(overload_set, m_union.i_value);
      }
      case TypeTag::UInt: {
        return std::invoke(overload_set, m_union.u_value);
      }
      case TypeTag::Double: {
        return std::invoke(overload_set, m_union.f_value);
      }
    }
    Unreachable();
  }

  /**
   * @brief Compare this NumberType with another NumberType
   * @param other another NumberType
   * @return comparison category
   */
  [[nodiscard]] constexpr auto operator<=>(const NumberType& other) const -> std::partial_ordering {
    if (m_tag != other.m_tag) {
      return static_cast<std::uint8_t>(m_tag) <=> static_cast<std::uint8_t>(other.m_tag);
    }
    switch (m_tag) {
      case TypeTag::Double:
        return m_union.f_value <=> other.m_union.f_value;
      case TypeTag::UInt:
        return m_union.u_value <=> other.m_union.u_value;
      case TypeTag::Int:
        return m_union.i_value <=> other.m_union.i_value;
      default:
        return std::partial_ordering::unordered;
    }
  }

  /**
   * @brief Equality compare this NumberType with another NumberType
   * @param other Another NumberType to compare
   * @return true if equal, otherwise false
   */
  [[nodiscard]] constexpr auto operator==(const NumberType& other) const -> bool {
    if (m_tag != other.m_tag) { return false; }
    switch (m_tag) {
      case TypeTag::Double:
        return m_union.f_value == other.m_union.f_value;
      case TypeTag::UInt:
        return m_union.u_value == other.m_union.u_value;
      case TypeTag::Int:
        return m_union.i_value == other.m_union.i_value;
      default:
        // This is what happens if the user does bad things and somehow sets a
        // tag value outside the range of the enum although, they would have to
        // do something bad like
        //
        //  char bytes[16] = {0, 0, 0, 0, 0, 0, 0, 0, 97, 0, 0, 0, 0, 0, 0, 0};
        //  reinterpret_cast<mguid::NumberType*>(&bytes);
        return false;
    }
  }

  /**
   * @brief Negate the held value
   * @return A new NumberType with the negated value
   */
  [[nodiscard]] NumberType operator-() const {
    return Visit(
        [](const UnsignedIntegerType val) {
          BEGIN_SUPPRESS_C4146
          return NumberType{std::negate<void>{}(val)};
          END_SUPPRESS_C4146
        },
        [](const auto val) { return NumberType{std::negate<void>{}(val)}; });
  }

  /**
   * @brief Promote the held value
   * @return A new NumberType with the promoted value
   */
  [[nodiscard]] NumberType operator+() const {
    return Visit([](const auto val) { return NumberType{+val}; });
  }

  /**
   * @brief Add this NumberType to another NumberType
   * @return A new NumberType with the result of the plus operation
   */
  [[nodiscard]] NumberType operator+(const NumberType& other) const {
    return Visit([&other](const auto lhs) {
      return other.Visit([lhs](const auto rhs) { return NumberType{std::plus<void>{}(lhs, rhs)}; });
    });
  }

  /**
   * @brief Subtract a NumberType from this NumberType
   * @return A new NumberType with the result of the minus operation
   */
  [[nodiscard]] NumberType operator-(const NumberType& other) const {
    return Visit([&other](const auto lhs) {
      return other.Visit(
          [lhs](const auto rhs) { return NumberType{std::minus<void>{}(lhs, rhs)}; });
    });
  }

  /**
   * @brief Multiply this NumberType with another NumberType
   * @return A new NumberType with the result of the multiplication operation
   */
  [[nodiscard]] NumberType operator*(const NumberType& other) const {
    return Visit([&other](const auto lhs) {
      return other.Visit(
          [lhs](const auto rhs) { return NumberType{std::multiplies<void>{}(lhs, rhs)}; });
    });
  }

  /**
   * @brief Divide this NumberType by another NumberType
   * @return A new NumberType with the result of the division operation
   */
  [[nodiscard]] NumberType operator/(const NumberType& other) const {
    return Visit([&other](const auto lhs) {
      return other.Visit(
          [lhs](const auto rhs) { return NumberType{std::divides<void>{}(lhs, rhs)}; });
    });
  }

  /**
   * @brief Modulo this NumberType by another NumberType
   * @return A new NumberType with the result of the modulus operation
   */
  [[nodiscard]] NumberType operator%(const NumberType& other) const {
    return Visit(
        [&other](const mguid::DoubleType lhs) {
          return other.Visit([lhs](const auto rhs) { return NumberType{std::fmod(lhs, rhs)}; });
        },
        [&other](const auto lhs) {
          return other.Visit(
              [lhs](const mguid::DoubleType rhs) { return NumberType{std::fmod(lhs, rhs)}; },
              [lhs](const auto rhs) { return NumberType{std::modulus<void>{}(lhs, rhs)}; });
        });
  }

  /**
   * @brief Stream insertion operator overload
   * @return reference to ostream
   */
  friend std::ostream& operator<<(std::ostream& os, const NumberType& nt) {
    nt.Visit([&os](const auto val) { os << val; });
    return os;
  }

private:
  union {
    IntegerType i_value{0};
    UnsignedIntegerType u_value;
    DoubleType f_value;
  } m_union;
  TypeTag m_tag{TypeTag::Int};
};

}  // namespace mguid

#ifdef BEGIN_SUPPRESS_C4146
#undef BEGIN_SUPPRESS_C4146
#endif

#ifdef END_SUPPRESS_C4146
#undef END_SUPPRESS_C4146
#endif

#endif  // DATATREE_NUMBER_TYPE_HPP
