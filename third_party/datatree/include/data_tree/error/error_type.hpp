/**
 * @brief Defines an error type to use with expected in interfaces
 * @author Matthew Guidry (github: mguid65)
 * @date 2024-02-06
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

#ifndef DATATREE_ERROR_TYPE_HPP
#define DATATREE_ERROR_TYPE_HPP

#include <string>

namespace mguid {
/**
 * @brief Simple aggregate error class
 */
struct Error {
  enum class Category : std::uint8_t {
    OutOfRange,
    BadAccess,
    KeyError,
    Generic
  } category{Category::Generic};

  /**
   * @brief Get category as a string
   *
   * In case of an out of bounds category, "Category::Unknown" will be returned.
   *
   * @param cat category value
   * @return category as a string
   */
  [[nodiscard]] static auto CategoryToString(Category cat) noexcept -> std::string {
    switch (cat) {
      case Category::OutOfRange:
        return "Category::OutOfRange";
      case Category::BadAccess:
        return "Category::BadAccess";
      case Category::KeyError:
        return "Category::KeyError";
      case Category::Generic:
        return "Category::Generic";
      default:
        return "Category::Unknown";
    }
  }
};
}  // namespace mguid

#endif  // DATATREE_ERROR_TYPE_HPP
