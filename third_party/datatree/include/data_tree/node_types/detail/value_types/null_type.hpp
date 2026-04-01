/**
 * @brief Definition of NullType value type
 *
 * Based on the libc++ implementation of std::monostate
 *
 * src:
 * https://github.com/llvm/llvm-project/blob/main/libcxx/include/__variant/monostate.h
 *
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

#ifndef DATATREE_NULL_TYPE_HPP
#define DATATREE_NULL_TYPE_HPP

#include <compare>
#include <cstdint>
#include <functional>

namespace mguid {

/**
 * @brief Class that represents a well behaved null state similar
 * to std::monostate
 */
struct NullType {};

/**
 * @brief Equality operator for NullType
 * @return Always returns true
 */
inline constexpr bool operator==(NullType, NullType) noexcept { return true; }

/**
 * @brief Comparison operator for NullType
 * @return Always returns equal
 */
inline constexpr std::strong_ordering operator<=>(NullType, NullType) noexcept {
  return std::strong_ordering::equal;
}

/**
 * @brief Ostream overload for NullType
 * @param os reference to an ostream
 * @return reference to an ostream
 */
inline std::ostream& operator<<(std::ostream& os, const mguid::NullType&) {
  return os << "null";
}

constexpr static auto Null = NullType{};

}  // namespace mguid

namespace std {
/**
 * @brief Specialization of std::hash for NullType
 */
template <>
struct hash<mguid::NullType> {
  using argument_type = mguid::NullType;
  using result_type = std::size_t;

  /**
   * @brief Hash function for NullType
   *
   * The value returned is the same as the value returned by the
   * std::hash<std::monostate> specialization in llvm/libc++.
   *
   * In the comments there it claims that
   * this is "a fundamentally attractive random value" but it is really just the
   * fundamental gravitational constant
   *
   * src:
   * https://github.com/llvm/llvm-project/blob/main/libcxx/include/__variant/monostate.h#L56C32-L56C71
   *
   * @return always returns the same unsigned integer value
   */
  inline result_type operator()(const argument_type&) const noexcept {
    return 66740831;
  }
};
}  // namespace std

#endif  // DATATREE_NULL_TYPE_HPP
