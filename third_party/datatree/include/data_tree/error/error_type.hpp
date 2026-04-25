/**
 * @brief Defines an error type to use with expected in interfaces
 * @author Matthew Guidry (github: mguid65)
 * @date 2024-02-06
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
