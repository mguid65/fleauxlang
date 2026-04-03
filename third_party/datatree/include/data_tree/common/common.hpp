/**
 * @brief Common utilities
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

#ifndef DATATREE_COMMON_HPP
#define DATATREE_COMMON_HPP

#include <algorithm>
#include <cstdint>
#include <ostream>
#include <string>
#include <type_traits>
#include <variant>
#include <array>

#include <tl/expected.hpp>

namespace mguid {

/**
 * @brief Prettify a JSON string
 * @param json a JSON string
 * @param tab_width width of indentation in pretty printed output
 * @return pretty formatted json
 */
[[nodiscard]] inline std::string PrettifyJson(const std::string& json, std::size_t tab_width = 2) {
  std::size_t indent{0};
  std::string result;

  auto append_indent = [&]() { result.append(indent * tab_width, ' '); };

  for (const auto& ch : json) {
    switch (ch) {
      case '{':
        [[fallthrough]];
      case '[': {
        indent += 1;
        result += ch;
        result += '\n';
        append_indent();
        break;
      }
      case '}':
        [[fallthrough]];
      case ']': {
        indent -= 1;
        result += '\n';
        append_indent();
        result += ch;
        break;
      }
      case ',': {
        result += ch;
        result += '\n';
        append_indent();
        break;
      }
      default:
        result += ch;
    }
  }

  return result;
}

/**
 * @brief A type to hold a set of callable overloads
 * @tparam TNonFinalCallables a set of non-final callables
 */
template <typename... TNonFinalCallables>
struct Overload : TNonFinalCallables... {
  using TNonFinalCallables::operator()...;
};

/**
 * @brief Deduction guide for Overload
 */
template <class... TNonFinalCallables>
Overload(TNonFinalCallables...) -> Overload<TNonFinalCallables...>;

/**
 * @brief Doing this to get expected-lite things
 */
using tl::expected;
using tl::make_unexpected;

/**
 * @brief Extension for expected that hides the usage of a reference wrapper for
 * reference types
 * @tparam TExpectedType reference value type
 * @tparam TErrorType error type
 */
template <typename TExpectedType, typename TErrorType>
struct RefExpected : private expected<std::reference_wrapper<TExpectedType>, TErrorType> {
  using BaseType = expected<std::reference_wrapper<TExpectedType>, TErrorType>;
  using ValueType = TExpectedType;

  using BaseType::BaseType;
  using BaseType::operator=;
  using BaseType::operator bool;
  using BaseType::emplace;
  using BaseType::error;
  using BaseType::has_value;
  using BaseType::swap;

  /**
   * @brief Construct a RefExpected from a TExpectedType reference
   * @param ref a TExpectedType reference
   */
  constexpr RefExpected(TExpectedType& ref) : BaseType(std::ref(ref)) {}

  /**
   * @brief Get const reference to value from this
   *
   * The normal expected::value overloads are private
   *
   * @return const reference to ValueType
   */
  [[nodiscard]] constexpr const ValueType& value() const& { return this->BaseType::value().get(); }

  /**
   * @brief Get value from this
   *
   * The normal expected::value overloads are private
   *
   * @return reference to ValueType
   */
  [[nodiscard]] ValueType& value() & { return this->BaseType::value().get(); }

  /**
   * @brief Get const pointer to value from this
   *
   * The normal expected::operator-> overloads are private
   *
   * @return const pointer to ValueType
   */
  [[nodiscard]] constexpr const ValueType* operator->() const {
    return &(this->BaseType::value().get());
  }

  /**
   * @brief Get pointer to value from this
   *
   * The normal expected::operator-> overloads are private
   *
   * @return pointer to ValueType
   */
  [[nodiscard]] ValueType* operator->() { return &(this->BaseType::value().get()); }

  /**
   * @brief Get const reference to value from this
   *
   * The normal expected::operator* overloads are private
   *
   * @return const reference to ValueType
   */
  [[nodiscard]] constexpr const ValueType& operator*() const& {
    return this->BaseType::value().get();
  }

  /**
   * @brief Get reference to value from this
   *
   * The normal expected::operator* overloads are private
   *
   * @return reference to ValueType
   */
  [[nodiscard]] ValueType& operator*() & { return this->BaseType::value().get(); }
};

using StringKeyType = std::string;
using IntegerKeyType = std::size_t;

/**
 * @brief Key type to use to access paths, string for maps or int for arrays
 *
 * For example, if I had a json object `j`:
 *
 * ```
 * {
 *   "key1" :
 *   {
 *     "key2" : [ ... ]
 *   }
 * }
 * ```
 *
 * ```
 *         j["key1"]["key2"][0];
 *         ___|        |    |___________
 *        |            |               |
 * StringKeyType  StringKeyType  IntegerKeyType
 * ```
 *
 * The same applies for this DataTree
 */
struct KeyOrIdxType : std::variant<StringKeyType, IntegerKeyType> {
  using BaseType = std::variant<StringKeyType, IntegerKeyType>;

  KeyOrIdxType() = default;
  KeyOrIdxType(const KeyOrIdxType&) = default;
  KeyOrIdxType(KeyOrIdxType&&) = default;
  KeyOrIdxType& operator=(const KeyOrIdxType&) = default;
  KeyOrIdxType& operator=(KeyOrIdxType&&) = default;

  KeyOrIdxType(const std::string& key) : BaseType{key} {}
  KeyOrIdxType(std::string&& key) : BaseType{std::move(key)} {}

  template <std::size_t NSize>
  KeyOrIdxType(const char (&key)[NSize]) : BaseType{std::string(key)} {}

  KeyOrIdxType(const std::size_t& idx) : BaseType{idx} {}

  /**
   * @brief Visit a value node type with a visitor overload set
   * @tparam TCallables set of non final callable types
   * @param callables set of non final callables
   * @return the common return type of all callables provided
   */
  template <typename... TCallables>
  decltype(auto) Visit(TCallables&&... callables) {
    auto overload_set = Overload{std::forward<TCallables>(callables)...};
    return std::visit(overload_set, *this);
  }

  /**
   * @brief Visit a value node type with a visitor overload set
   * @tparam TCallables set of non final callable types
   * @param callables set of non final callables
   * @return the common return type of all callables provided
   */
  template <typename... TCallables>
  decltype(auto) Visit(TCallables&&... callables) const {
    auto overload_set = Overload{std::forward<TCallables>(callables)...};
    return std::visit(overload_set, *this);
  }
};

namespace key_literals {
/**
 * @brief UDL to create a KeyType from an index
 * @param idx an index
 * @return a KeyType created from an index
 */
inline KeyOrIdxType operator""_k(unsigned long long idx) { return KeyOrIdxType{idx}; }

/**
 * @brief Helper for string literal KeyType UDL
 * @tparam NSize size of string literal
 */
template <std::size_t NSize>
struct KeyTypeStringLiteralHelper {
  char data[NSize]{};
  /**
   * @brief Construct a KeyTypeStringLiteralHelper from a string literal
   * @param str a string literal
   */
  constexpr KeyTypeStringLiteralHelper(const char (&str)[NSize]) {
    std::copy(std::begin(str), std::end(str), std::begin(data));
  }
};

/**
 * @brief UDL to create a KeyType from a string literal
 * @param idx an index
 * @return a KeyType created from a string literal
 */
template <KeyTypeStringLiteralHelper TLiteralStrKey>
KeyOrIdxType operator""_k() {
  return KeyOrIdxType{TLiteralStrKey.data};
}
}  // namespace key_literals

/**
 * @brief Determine the first type in the type list that is convertible to TType
 * @tparam TType type to check against list
 * @tparam TFirst first type in list
 * @tparam TOther the rest of the type list
 */
template <typename TType, typename TFirst, typename... TOther>
struct PickFirstConvertible {
private:
  using rhs_chain_type = typename PickFirstConvertible<TType, TOther...>::type;

public:
  using type = typename std::conditional<std::is_convertible<TType, TFirst>::value, TFirst,
                                         rhs_chain_type>::type;
};

/**
 * @brief Determine the first type in the type list that is convertible to TType
 * @tparam TType type to check against list
 * @tparam TFirst first type in list
 */
template <typename TType, typename TFirst>
struct PickFirstConvertible<TType, TFirst> {
  using type = std::conditional<std::is_convertible<TType, TFirst>::value, TFirst, void>::type;
};

/**
 * @brief Represents a path to a node in the data tree
 * @tparam NLength path length
 */
template <std::size_t NLength>
struct Path {
  /**
   * @brief Construct a Path from variadic path items
   * @tparam TArgs path item types
   * @param path_items path items
   */
  template <typename... TArgs>
  Path(TArgs&&... path_items)
      : items{
            {static_cast<typename PickFirstConvertible<TArgs, StringKeyType, IntegerKeyType>::type>(
                path_items)...}} {}

  /**
   * @brief Get array of KeyType path items
   * @return array of KeyType path items
   */
  [[nodiscard]] const std::array<KeyOrIdxType, NLength>& Items() const { return items; }

private:
  std::array<KeyOrIdxType, NLength> items;
};

/**
 * @brief Requires that a type has a subscript operator that accepts std::size_t
 * @tparam TContainer some container type
 */
template <typename TContainer>
concept IntegerIndexable = requires(TContainer container, std::size_t idx) {
  { container[idx] };
};

/**
 * @brief Compile time for loop for a container with an integer subscript
 * operator
 * @tparam NCount Number of iterations
 * @tparam TContainer Type of container with an integer subscript operator
 * @tparam TFunc Type of function to apply at each "iteration"
 * @param container container with a integer subscript operator
 * @param func function to apply at each "iteration"
 */
template <std::size_t NCount, IntegerIndexable TContainer, typename TFunc>
  requires std::invocable<TFunc, decltype(std::declval<TContainer>()[0])>
constexpr void For(TContainer&& container, TFunc&& func) noexcept(
    std::is_nothrow_invocable_v<TFunc, decltype(std::declval<TContainer>()[0])>) {
  auto ForImpl = []<std::size_t... NIdxs>(TContainer&& container_inner, TFunc&& func_inner,
                                          std::index_sequence<NIdxs...>) {
    (std::invoke(func_inner, container_inner[NIdxs]), ...);
  };
  ForImpl(std::forward<TContainer>(container), std::forward<TFunc>(func),
          std::make_index_sequence<NCount>{});
}

/**
 * @brief Ostream overload to print a Path
 * @tparam NLength path length
 * @param os reference to some ostream object
 * @param path a path object
 * @return reference to ostream
 */
template <std::size_t NLength>
std::ostream& operator<<(std::ostream& os, const Path<NLength>& path) {
  For<NLength>(path.Items(), [&os](auto&& arg) {
    std::visit(
        Overload{[&os](const StringKeyType& key_or_idx) { os << "[\"" << key_or_idx << "\"]"; },
                 [&os](const IntegerKeyType& key_or_idx) { os << "[" << key_or_idx << "]"; }},
        KeyOrIdxType{arg});
  });
  return os;
}

/**
 * @brief Deduction guide to create a path from key path parameters
 */
template <typename... TArgs>
Path(TArgs...) -> Path<sizeof...(TArgs)>;

/**
 * @brief Invokes undefined behavior. An implementation may use this to optimize
 * impossible code branches away (typically, in optimized builds) or to trap
 * them to prevent further execution (typically, in debug builds).
 *
 * From "Possible implementation" section for `std::unreachable` here:
 * https://en.cppreference.com/w/cpp/utility/unreachable
 */
[[noreturn]] inline void Unreachable() {
  // Uses compiler specific extensions if possible.
  // Even if no extension is used, undefined behavior is still raised by
  // an empty function body and the noreturn attribute.
#if defined(_MSC_VER) && !defined(__clang__)  // MSVC
  __assume(false);
#else  // GCC, Clang
  __builtin_unreachable();
#endif
}

}  // namespace mguid

#endif  // DATATREE_COMMON_HPP
