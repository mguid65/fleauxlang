/**
 * @brief Declarations for value node type
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

#ifndef DATATREE_VALUE_NODE_TYPE_HPP
#define DATATREE_VALUE_NODE_TYPE_HPP

#include <cstddef>
#include <utility>
#include <variant>

#include "data_tree/common/common.hpp"
#include "data_tree/node_types/detail/value_types/value_types.hpp"

namespace mguid {

/**
 * @brief A class to represent a node that stores a value
 */
class ValueNodeType {
  /**
   * @brief Proxy class that provides access to unsafe ValueNodeType functionality
   * @tparam TConst whether we are holding a const or non-const reference
   */
  template <bool TConst = false>
  class UnsafeProxyType {
  public:
    /**
     * @brief Delete Move/Copy Constructors/Assignment Operators
     */
    UnsafeProxyType(const UnsafeProxyType&) = delete;
    UnsafeProxyType& operator=(const UnsafeProxyType&) = delete;
    UnsafeProxyType(UnsafeProxyType&&) = delete;
    UnsafeProxyType& operator=(UnsafeProxyType&&) = delete;

    /**
     * @brief Get a NullType value from this value node type
     * @return  a NullType value
     */
    [[nodiscard]] auto GetNull() const -> const NullType& {
      return std::get<NullType>(m_node_ref.m_variant_value);
    }
    /**
     * @brief Get a StringType value from this value node type
     * @return  a StringType value
     */
    [[nodiscard]] auto GetString() const -> const StringType& {
      return std::get<StringType>(m_node_ref.m_variant_value);
    }
    /**
     * @brief Get a NumberType value from this value node type
     * @return  a NumberType value
     */
    [[nodiscard]] auto GetNumber() const -> const NumberType& {
      return std::get<NumberType>(m_node_ref.m_variant_value);
    }
    /**
     * @brief Get a BoolType value from this value node type
     * @return  a BoolType value
     */
    [[nodiscard]] auto GetBool() const -> const BoolType& {
      return std::get<BoolType>(m_node_ref.m_variant_value);
    }

    /**
     * @brief Get a NullType value from this value node type
     * @return  a NullType value
     */
    [[nodiscard]] auto GetNull() -> NullType& {
      return std::get<NullType>(m_node_ref.m_variant_value);
    }
    /**
     * @brief Get a StringType value from this value node type
     * @return  a StringType value
     */
    [[nodiscard]] auto GetString() -> StringType& {
      return std::get<StringType>(m_node_ref.m_variant_value);
    }
    /**
     * @brief Get a NumberType value from this value node type
     * @return  a NumberType value
     */
    [[nodiscard]] auto GetNumber() -> NumberType& {
      return std::get<NumberType>(m_node_ref.m_variant_value);
    }
    /**
     * @brief Get a BoolType value from this value node type
     * @return  a BoolType value
     */
    [[nodiscard]] auto GetBool() -> BoolType& {
      return std::get<BoolType>(m_node_ref.m_variant_value);
    }

    /**
     * @brief Get a reference to the held ValueNodeType
     * @return a reference to the held ValueNodeType
     */
    [[nodiscard]] auto Safe() -> ValueNodeType& { return m_node_ref; }

    /**
     * @brief Get a reference to the held ValueNodeType
     * @return a reference to the held ValueNodeType
     */
    [[nodiscard]] auto Safe() const -> const ValueNodeType& { return m_node_ref; }

  private:
    explicit UnsafeProxyType(std::conditional_t<TConst, const ValueNodeType&, ValueNodeType&> ref)
        : m_node_ref{ref} {}

    friend ValueNodeType;
    std::conditional_t<TConst, const ValueNodeType&, ValueNodeType&> m_node_ref;
  };

public:
  using ConstUnsafeProxy = const UnsafeProxyType<true>;
  using UnsafeProxy = UnsafeProxyType<false>;

  /**
   * @brief Default construct a ValueNode with value Null
   */
  ValueNodeType() noexcept = default;

  /**
   * @brief Explicit defaults for copy/move construction/assignment
   */
  ValueNodeType(const ValueNodeType&) = default;
  ValueNodeType(ValueNodeType&&) noexcept = default;
  ValueNodeType& operator=(const ValueNodeType&) = default;
  ValueNodeType& operator=(ValueNodeType&&) noexcept = default;

  /**
   * @brief Explicitly construct a null ValueNode
   */
  explicit ValueNodeType(std::nullptr_t) noexcept {}

  /**
   * @brief Construct a value node from a value
   * @tparam TValueType a valid value type
   * @param value some value
   */
  template <ValidValueNodeTypeValueType TValueType>
  explicit ValueNodeType(TValueType&& value) noexcept(!SatisfiesStringType<TValueType>) {
    // This is for the case where the value is a number primitive
    if constexpr (SatisfiesNumberType<TValueType>) {
      m_variant_value = NumberType(std::forward<TValueType>(value));
    } else {
      m_variant_value = std::forward<TValueType>(value);
    }
  }

  /**
   * @brief Assign a value to this value node type
   * @tparam TValueType type of value
   * @param value some value
   * @return reference to this value node type
   */
  template <ValidValueNodeTypeValueType TValueType>
  auto operator=(TValueType&& value) noexcept(!SatisfiesStringType<TValueType>) -> ValueNodeType& {
    if constexpr (SatisfiesNumberType<TValueType>) {
      m_variant_value = NumberType(std::forward<TValueType>(value));
    } else {
      m_variant_value = std::forward<TValueType>(value);
    }
    return *this;
  }

  /**
   * @brief Is this valid node type holding the requested type
   * @tparam TValueType requested type to check for
   * @return true if this value node type holding the requested type, otherwise false
   */
  template <ValidValueNodeTypeValueType TValueType>
  [[nodiscard]] constexpr auto Has() const noexcept -> bool {
    return std::holds_alternative<TValueType>(m_variant_value);
  }

  /**
   * @brief Is this value node type holding a NullType value
   * @return true if this value node type holding a NullType value, otherwise
   * false
   */
  [[nodiscard]] constexpr auto HasNull() const noexcept -> bool {
    return Has<NullType>();
  }
  /**
   * @brief Is this value node type holding a StringType value
   * @return true if this value node type holding a StringType value, otherwise
   * false
   */
  [[nodiscard]] constexpr auto HasString() const noexcept -> bool {
    return Has<StringType>();
  }
  /**
   * @brief Is this value node type holding a NumberType value
   * @return true if this value node type holding a NumberType value, otherwise
   * false
   */
  [[nodiscard]] constexpr auto HasNumber() const noexcept -> bool {
    return Has<NumberType>();
  }
  /**
   * @brief Is this value node type holding a BoolType value
   * @return true if this value node type holding a BoolType value, otherwise
   * false
   */
  [[nodiscard]] constexpr auto HasBool() const noexcept -> bool {
    return Has<BoolType>();
  }

  /**
   * @brief Try to get a NullType value from this value node type
   * @return Expected with a NullType value or an Error
   */
  [[nodiscard]] auto TryGetNull() const noexcept -> RefExpected<const NullType, Error> {
    if (auto* val = std::get_if<NullType>(&m_variant_value); val != nullptr) { return *val; }
    return make_unexpected(Error{.category = Error::Category::BadAccess});
  }
  /**
   * @brief Try to get a StringType value from this value node type
   * @return Expected with a StringType value or an Error
   */
  [[nodiscard]] auto TryGetString() const noexcept -> RefExpected<const StringType, Error> {
    if (auto* val = std::get_if<StringType>(&m_variant_value); val != nullptr) { return *val; }
    return make_unexpected(Error{.category = Error::Category::BadAccess});
  }
  /**
   * @brief Try to get a NumberType value from this value node type
   * @return Expected with a NumberType value or an Error
   */
  [[nodiscard]] auto TryGetNumber() const noexcept -> RefExpected<const NumberType, Error> {
    if (auto* val = std::get_if<NumberType>(&m_variant_value); val != nullptr) { return *val; }
    return make_unexpected(Error{.category = Error::Category::BadAccess});
  }
  /**
   * @brief Try to get a BoolType value from this value node type
   * @return Expected with a BoolType value or an Error
   */
  [[nodiscard]] auto TryGetBool() const noexcept -> RefExpected<const BoolType, Error> {
    if (auto* val = std::get_if<BoolType>(&m_variant_value); val != nullptr) { return *val; }
    return make_unexpected(Error{.category = Error::Category::BadAccess});
  }

  /**
   * @brief Try to get a NullType value from this value node type
   * @return Expected with a NullType value or an Error
   */
  [[nodiscard]] auto TryGetNull() noexcept -> RefExpected<NullType, Error> {
    if (auto* val = std::get_if<NullType>(&m_variant_value); val != nullptr) { return *val; }
    return make_unexpected(Error{.category = Error::Category::BadAccess});
  }
  /**
   * @brief Try to get a StringType value from this value node type
   * @return Expected with a StringType value or an Error
   */
  [[nodiscard]] auto TryGetString() noexcept -> RefExpected<StringType, Error> {
    if (auto* val = std::get_if<StringType>(&m_variant_value); val != nullptr) { return *val; }
    return make_unexpected(Error{.category = Error::Category::BadAccess});
  }
  /**
   * @brief Try to get a NumberType value from this value node type
   * @return Expected with a NumberType value or an Error
   */
  [[nodiscard]] auto TryGetNumber() noexcept -> RefExpected<NumberType, Error> {
    if (auto* val = std::get_if<NumberType>(&m_variant_value); val != nullptr) { return *val; }
    return make_unexpected(Error{.category = Error::Category::BadAccess});
  }
  /**
   * @brief Try to get a BoolType value from this value node type
   * @return Expected with a BoolType value or an Error
   */
  [[nodiscard]] auto TryGetBool() noexcept -> RefExpected<BoolType, Error> {
    if (auto* val = std::get_if<BoolType>(&m_variant_value); val != nullptr) { return *val; }
    return make_unexpected(Error{.category = Error::Category::BadAccess});
  }

  // VERY DRY SECTION, TRUST ME

  /**
   * @brief If the value held by this ValueNodeType is NullType then call the
   * provided callback function
   * @tparam TThenFunc callback function type
   * @param func callback function that takes a NullType and returns a
   * ValueNodeType
   * @return An ValueNodeType that was the result of calling the func or a
   * default constructed ValueNodeType
   */
  template <typename TThenFunc>
  auto IfNullThen(TThenFunc&& func) & -> ValueNodeType {
    auto* val = std::get_if<NullType>(&m_variant_value);
    if (val != nullptr) { return std::invoke(std::forward<TThenFunc>(func), *val); }
    return std::remove_cvref_t<std::invoke_result_t<TThenFunc, NullType&>>{};
  }

  /**
   * @brief If the value held by this ValueNodeType is NullType then call the
   * provided callback function
   * @tparam TThenFunc callback function type
   * @param func callback function that takes a NullType and returns a
   * ValueNodeType
   * @return An ValueNodeType that was the result of calling the func or a
   * default constructed ValueNodeType
   */
  template <typename TThenFunc>
  auto IfNullThen(TThenFunc&& func) const& -> ValueNodeType {
    auto* val = std::get_if<NullType>(&m_variant_value);
    if (val != nullptr) { return std::invoke(std::forward<TThenFunc>(func), *val); }
    return std::remove_cvref_t<std::invoke_result_t<TThenFunc, const NullType&>>{};
  }

  /**
   * @brief If the value held by this ValueNodeType is NullType then call the
   * provided callback function
   * @tparam TThenFunc callback function type
   * @param func callback function that takes a NullType and returns a
   * ValueNodeType
   * @return An ValueNodeType that was the result of calling the func or a
   * default constructed ValueNodeType
   */
  template <typename TThenFunc>
  auto IfNullThen(TThenFunc&& func) && -> ValueNodeType {
    auto* val = std::get_if<NullType>(&m_variant_value);
    if (val != nullptr) { return std::invoke(std::forward<TThenFunc>(func), *val); }
    return std::remove_cvref_t<std::invoke_result_t<TThenFunc, NullType>>{};
  }

  /**
   * @brief If the value held by this ValueNodeType is NullType then call the
   * provided callback function
   * @tparam TThenFunc callback function type
   * @param func callback function that takes a NullType and returns a
   * ValueNodeType
   * @return An ValueNodeType that was the result of calling the func or a
   * default constructed ValueNodeType
   */
  template <typename TThenFunc>
  auto IfNullThen(TThenFunc&& func) const&& -> ValueNodeType {
    auto* val = std::get_if<NullType>(&m_variant_value);
    if (val != nullptr) { return std::invoke(std::forward<TThenFunc>(func), *val); }
    return std::remove_cvref_t<std::invoke_result_t<TThenFunc, const NullType>>{};
  }

  /**
   * @brief If the value held by this ValueNodeType is StringType then call the
   * provided callback function
   * @tparam TThenFunc callback function type
   * @param func callback function that takes a StringType and returns a
   * ValueNodeType
   * @return An ValueNodeType that was the result of calling the func or a
   * default constructed ValueNodeType
   */
  template <typename TThenFunc>
  auto IfStringThen(TThenFunc&& func) & -> ValueNodeType {
    auto* val = std::get_if<StringType>(&m_variant_value);
    if (val != nullptr) { return std::invoke(std::forward<TThenFunc>(func), *val); }
    return std::remove_cvref_t<std::invoke_result_t<TThenFunc, StringType&>>{};
  }

  /**
   * @brief If the value held by this ValueNodeType is StringType then call the
   * provided callback function
   * @tparam TThenFunc callback function type
   * @param func callback function that takes a StringType and returns a
   * ValueNodeType
   * @return An ValueNodeType that was the result of calling the func or a
   * default constructed ValueNodeType
   */
  template <typename TThenFunc>
  auto IfStringThen(TThenFunc&& func) const& -> ValueNodeType {
    auto* val = std::get_if<StringType>(&m_variant_value);
    if (val != nullptr) { return std::invoke(std::forward<TThenFunc>(func), *val); }
    return std::remove_cvref_t<std::invoke_result_t<TThenFunc, const StringType&>>{};
  }

  /**
   * @brief If the value held by this ValueNodeType is StringType then call the
   * provided callback function
   * @tparam TThenFunc callback function type
   * @param func callback function that takes a StringType and returns a
   * ValueNodeType
   * @return An ValueNodeType that was the result of calling the func or a
   * default constructed ValueNodeType
   */
  template <typename TThenFunc>
  auto IfStringThen(TThenFunc&& func) && -> ValueNodeType {
    auto* val = std::get_if<StringType>(&m_variant_value);
    if (val != nullptr) { return std::invoke(std::forward<TThenFunc>(func), std::move(*val)); }
    return std::remove_cvref_t<std::invoke_result_t<TThenFunc, StringType>>{};
  }

  /**
   * @brief If the value held by this ValueNodeType is StringType then call the
   * provided callback function
   * @tparam TThenFunc callback function type
   * @param func callback function that takes a StringType and returns a
   * ValueNodeType
   * @return An ValueNodeType that was the result of calling the func or a
   * default constructed ValueNodeType
   */
  template <typename TThenFunc>
  auto IfStringThen(TThenFunc&& func) const&& -> ValueNodeType {
    auto* val = std::get_if<StringType>(&m_variant_value);
    if (val != nullptr) { return std::invoke(std::forward<TThenFunc>(func), *val); }
    return std::remove_cvref_t<std::invoke_result_t<TThenFunc, const StringType>>{};
  }

  /**
   * @brief If the value held by this ValueNodeType is NumberType then call the
   * provided callback function
   * @tparam TThenFunc callback function type
   * @param func callback function that takes a NumberType and returns a
   * ValueNodeType
   * @return An ValueNodeType that was the result of calling the func or a
   * default constructed ValueNodeType
   */
  template <typename TThenFunc>
  auto IfNumberThen(TThenFunc&& func) & -> ValueNodeType {
    auto* val = std::get_if<NumberType>(&m_variant_value);
    if (val != nullptr) { return std::invoke(std::forward<TThenFunc>(func), *val); }
    return std::remove_cvref_t<std::invoke_result_t<TThenFunc, NumberType&>>{};
  }

  /**
   * @brief If the value held by this ValueNodeType is NumberType then call the
   * provided callback function
   * @tparam TThenFunc callback function type
   * @param func callback function that takes a NumberType and returns a
   * ValueNodeType
   * @return An ValueNodeType that was the result of calling the func or a
   * default constructed ValueNodeType
   */
  template <typename TThenFunc>
  auto IfNumberThen(TThenFunc&& func) const& -> ValueNodeType {
    auto* val = std::get_if<NumberType>(&m_variant_value);
    if (val != nullptr) { return std::invoke(std::forward<TThenFunc>(func), *val); }
    return std::remove_cvref_t<std::invoke_result_t<TThenFunc, const NumberType&>>{};
  }

  /**
   * @brief If the value held by this ValueNodeType is NumberType then call the
   * provided callback function
   * @tparam TThenFunc callback function type
   * @param func callback function that takes a NumberType and returns a
   * ValueNodeType
   * @return An ValueNodeType that was the result of calling the func or a
   * default constructed ValueNodeType
   */
  template <typename TThenFunc>
  auto IfNumberThen(TThenFunc&& func) && -> ValueNodeType {
    auto* val = std::get_if<NumberType>(&m_variant_value);
    if (val != nullptr) { return std::invoke(std::forward<TThenFunc>(func), *val); }
    return std::remove_cvref_t<std::invoke_result_t<TThenFunc, NumberType>>{};
  }

  /**
   * @brief If the value held by this ValueNodeType is NumberType then call the
   * provided callback function
   * @tparam TThenFunc callback function type
   * @param func callback function that takes a NumberType and returns a
   * ValueNodeType
   * @return An ValueNodeType that was the result of calling the func or a
   * default constructed ValueNodeType
   */
  template <typename TThenFunc>
  auto IfNumberThen(TThenFunc&& func) const&& -> ValueNodeType {
    auto* val = std::get_if<NumberType>(&m_variant_value);
    if (val != nullptr) { return std::invoke(std::forward<TThenFunc>(func), *val); }
    return std::remove_cvref_t<std::invoke_result_t<TThenFunc, const NumberType>>{};
  }

  /**
   * @brief If the value held by this ValueNodeType is BoolType then call the
   * provided callback function
   * @tparam TThenFunc callback function type
   * @param func callback function that takes a BoolType and returns a
   * ValueNodeType
   * @return An ValueNodeType that was the result of calling the func or a
   * default constructed ValueNodeType
   */
  template <typename TThenFunc>
  auto IfBoolThen(TThenFunc&& func) & -> ValueNodeType {
    auto* val = std::get_if<BoolType>(&m_variant_value);
    if (val != nullptr) { return std::invoke(std::forward<TThenFunc>(func), *val); }
    return std::remove_cvref_t<std::invoke_result_t<TThenFunc, BoolType&>>{};
  }

  /**
   * @brief If the value held by this ValueNodeType is BoolType then call the
   * provided callback function
   * @tparam TThenFunc callback function type
   * @param func callback function that takes a BoolType and returns a
   * ValueNodeType
   * @return An ValueNodeType that was the result of calling the func or a
   * default constructed ValueNodeType
   */
  template <typename TThenFunc>
  auto IfBoolThen(TThenFunc&& func) const& -> ValueNodeType {
    auto* val = std::get_if<BoolType>(&m_variant_value);
    if (val != nullptr) { return std::invoke(std::forward<TThenFunc>(func), *val); }
    return std::remove_cvref_t<std::invoke_result_t<TThenFunc, const BoolType&>>{};
  }

  /**
   * @brief If the value held by this ValueNodeType is BoolType then call the
   * provided callback function
   * @tparam TThenFunc callback function type
   * @param func callback function that takes a BoolType and returns a
   * ValueNodeType
   * @return An ValueNodeType that was the result of calling the func or a
   * default constructed ValueNodeType
   */
  template <typename TThenFunc>
  auto IfBoolThen(TThenFunc&& func) && -> ValueNodeType {
    auto* val = std::get_if<BoolType>(&m_variant_value);
    if (val != nullptr) { return std::invoke(std::forward<TThenFunc>(func), *val); }
    return std::remove_cvref_t<std::invoke_result_t<TThenFunc, BoolType>>{};
  }

  /**
   * @brief If the value held by this ValueNodeType is BoolType then call the
   * provided callback function
   * @tparam TThenFunc callback function type
   * @param func callback function that takes a BoolType and returns a
   * ValueNodeType
   * @return An ValueNodeType that was the result of calling the func or a
   * default constructed ValueNodeType
   */
  template <typename TThenFunc>
  auto IfBoolThen(TThenFunc&& func) const&& -> ValueNodeType {
    auto* val = std::get_if<BoolType>(&m_variant_value);
    if (val != nullptr) { return std::invoke(std::forward<TThenFunc>(func), *val); }
    return std::remove_cvref_t<std::invoke_result_t<TThenFunc, const BoolType>>{};
  }

  /**
   * @brief If the value held by this ValueNodeType is NullType then call the
   * provided callback function
   * @tparam TTransformFunc callback function type
   * @param func callback function that takes a NullType and returns something
   * convertible to ValueNodeType
   * @return the result of constructing a ValueNodeType with the default
   * constructed return value type of TTransformFunc
   */
  template <typename TTransformFunc>
  auto IfNullTransform(TTransformFunc&& func) & -> ValueNodeType {
    auto* val = std::get_if<NullType>(&m_variant_value);
    if (val != nullptr) {
      return ValueNodeType{std::invoke(std::forward<TTransformFunc>(func), *val)};
    }
    return ValueNodeType{std::remove_cvref_t<std::invoke_result_t<TTransformFunc, NullType&>>{}};
  }

  /**
   * @brief If the value held by this ValueNodeType is NullType then call the
   * provided callback function
   * @tparam TTransformFunc callback function type
   * @param func callback function that takes a NullType and returns something
   * convertible to ValueNodeType
   * @return the result of constructing a ValueNodeType with the default
   * constructed return value type of TTransformFunc
   */
  template <typename TTransformFunc>
  auto IfNullTransform(TTransformFunc&& func) const& -> ValueNodeType {
    auto* val = std::get_if<NullType>(&m_variant_value);
    if (val != nullptr) {
      return ValueNodeType{std::invoke(std::forward<TTransformFunc>(func), *val)};
    }
    return ValueNodeType{
        std::remove_cvref_t<std::invoke_result_t<TTransformFunc, const NullType&>>{}};
  }

  /**
   * @brief If the value held by this ValueNodeType is NullType then call the
   * provided callback function
   * @tparam TTransformFunc callback function type
   * @param func callback function that takes a NullType and returns something
   * convertible to ValueNodeType
   * @return the result of constructing a ValueNodeType with the default
   * constructed return value type of TTransformFunc
   */
  template <typename TTransformFunc>
  auto IfNullTransform(TTransformFunc&& func) && -> ValueNodeType {
    auto* val = std::get_if<NullType>(&m_variant_value);
    if (val != nullptr) {
      return ValueNodeType{std::invoke(std::forward<TTransformFunc>(func), *val)};
    }
    return ValueNodeType{std::remove_cvref_t<std::invoke_result_t<TTransformFunc, NullType>>{}};
  }

  /**
   * @brief If the value held by this ValueNodeType is NullType then call the
   * provided callback function
   * @tparam TTransformFunc callback function type
   * @param func callback function that takes a NullType and returns something
   * convertible to ValueNodeType
   * @return the result of constructing a ValueNodeType with the default
   * constructed return value type of TTransformFunc
   */
  template <typename TTransformFunc>
  auto IfNullTransform(TTransformFunc&& func) const&& -> ValueNodeType {
    auto* val = std::get_if<NullType>(&m_variant_value);
    if (val != nullptr) {
      return ValueNodeType{std::invoke(std::forward<TTransformFunc>(func), *val)};
    }
    return ValueNodeType{
        std::remove_cvref_t<std::invoke_result_t<TTransformFunc, const NullType>>{}};
  }

  /**
   * @brief If the value held by this ValueNodeType is StringType then call the
   * provided callback function
   * @tparam TTransformFunc callback function type
   * @param func callback function that takes a StringType and returns something
   * convertible to ValueNodeType
   * @return the result of constructing a ValueNodeType with the default
   * constructed return value type of TTransformFunc
   */
  template <typename TTransformFunc>
  auto IfStringTransform(TTransformFunc&& func) & -> ValueNodeType {
    auto* val = std::get_if<StringType>(&m_variant_value);
    if (val != nullptr) {
      return ValueNodeType{std::invoke(std::forward<TTransformFunc>(func), *val)};
    }
    return ValueNodeType{std::remove_cvref_t<std::invoke_result_t<TTransformFunc, StringType&>>{}};
  }

  /**
   * @brief If the value held by this ValueNodeType is StringType then call the
   * provided callback function
   * @tparam TTransformFunc callback function type
   * @param func callback function that takes a StringType and returns something
   * convertible to ValueNodeType
   * @return the result of constructing a ValueNodeType with the default
   * constructed return value type of TTransformFunc
   */
  template <typename TTransformFunc>
  auto IfStringTransform(TTransformFunc&& func) const& -> ValueNodeType {
    auto* val = std::get_if<StringType>(&m_variant_value);
    if (val != nullptr) {
      return ValueNodeType{std::invoke(std::forward<TTransformFunc>(func), *val)};
    }
    return ValueNodeType{
        std::remove_cvref_t<std::invoke_result_t<TTransformFunc, const StringType&>>{}};
  }

  /**
   * @brief If the value held by this ValueNodeType is StringType then call the
   * provided callback function
   * @tparam TTransformFunc callback function type
   * @param func callback function that takes a StringType and returns something
   * convertible to ValueNodeType
   * @return the result of constructing a ValueNodeType with the default
   * constructed return value type of TTransformFunc
   */
  template <typename TTransformFunc>
  auto IfStringTransform(TTransformFunc&& func) && -> ValueNodeType {
    auto* val = std::get_if<StringType>(&m_variant_value);
    if (val != nullptr) {
      return ValueNodeType{std::invoke(std::forward<TTransformFunc>(func), std::move(*val))};
    }
    return ValueNodeType{std::remove_cvref_t<std::invoke_result_t<TTransformFunc, StringType>>{}};
  }

  /**
   * @brief If the value held by this ValueNodeType is StringType then call the
   * provided callback function
   * @tparam TTransformFunc callback function type
   * @param func callback function that takes a StringType and returns something
   * convertible to ValueNodeType
   * @return the result of constructing a ValueNodeType with the default
   * constructed return value type of TTransformFunc
   */
  template <typename TTransformFunc>
  auto IfStringTransform(TTransformFunc&& func) const&& -> ValueNodeType {
    auto* val = std::get_if<StringType>(&m_variant_value);
    if (val != nullptr) {
      return ValueNodeType{std::invoke(std::forward<TTransformFunc>(func), *val)};
    }
    return ValueNodeType{
        std::remove_cvref_t<std::invoke_result_t<TTransformFunc, const StringType>>{}};
  }

  /**
   * @brief If the value held by this ValueNodeType is NumberType then call the
   * provided callback function
   * @tparam TTransformFunc callback function type
   * @param func callback function that takes a NumberType and returns something
   * convertible to ValueNodeType
   * @return the result of constructing a ValueNodeType with the default
   * constructed return value type of TTransformFunc
   */
  template <typename TTransformFunc>
  auto IfNumberTransform(TTransformFunc&& func) & -> ValueNodeType {
    auto* val = std::get_if<NumberType>(&m_variant_value);
    if (val != nullptr) {
      return ValueNodeType{std::invoke(std::forward<TTransformFunc>(func), *val)};
    }
    return ValueNodeType{std::remove_cvref_t<std::invoke_result_t<TTransformFunc, NumberType&>>{}};
  }

  /**
   * @brief If the value held by this ValueNodeType is NumberType then call the
   * provided callback function
   * @tparam TTransformFunc callback function type
   * @param func callback function that takes a NumberType and returns something
   * convertible to ValueNodeType
   * @return the result of constructing a ValueNodeType with the default
   * constructed return value type of TTransformFunc
   */
  template <typename TTransformFunc>
  auto IfNumberTransform(TTransformFunc&& func) const& -> ValueNodeType {
    auto* val = std::get_if<NumberType>(&m_variant_value);
    if (val != nullptr) {
      return ValueNodeType{std::invoke(std::forward<TTransformFunc>(func), *val)};
    }
    return ValueNodeType{
        std::remove_cvref_t<std::invoke_result_t<TTransformFunc, const NumberType&>>{}};
  }

  /**
   * @brief If the value held by this ValueNodeType is NumberType then call the
   * provided callback function
   * @tparam TTransformFunc callback function type
   * @param func callback function that takes a NumberType and returns something
   * convertible to ValueNodeType
   * @return the result of constructing a ValueNodeType with the default
   * constructed return value type of TTransformFunc
   */
  template <typename TTransformFunc>
  auto IfNumberTransform(TTransformFunc&& func) && -> ValueNodeType {
    auto* val = std::get_if<NumberType>(&m_variant_value);
    if (val != nullptr) {
      return ValueNodeType{std::invoke(std::forward<TTransformFunc>(func), *val)};
    }
    return ValueNodeType{std::remove_cvref_t<std::invoke_result_t<TTransformFunc, NumberType>>{}};
  }

  /**
   * @brief If the value held by this ValueNodeType is NumberType then call the
   * provided callback function
   * @tparam TTransformFunc callback function type
   * @param func callback function that takes a NumberType and returns something
   * convertible to ValueNodeType
   * @return the result of constructing a ValueNodeType with the default
   * constructed return value type of TTransformFunc
   */
  template <typename TTransformFunc>
  auto IfNumberTransform(TTransformFunc&& func) const&& -> ValueNodeType {
    auto* val = std::get_if<NumberType>(&m_variant_value);
    if (val != nullptr) {
      return ValueNodeType{std::invoke(std::forward<TTransformFunc>(func), *val)};
    }
    return ValueNodeType{
        std::remove_cvref_t<std::invoke_result_t<TTransformFunc, const NumberType>>{}};
  }

  /**
   * @brief If the value held by this ValueNodeType is BoolType then call the
   * provided callback function
   * @tparam TTransformFunc callback function type
   * @param func callback function that takes a BoolType and returns something
   * convertible to ValueNodeType
   * @return the result of constructing a ValueNodeType with the default
   * constructed return value type of TTransformFunc
   */
  template <typename TTransformFunc>
  auto IfBoolTransform(TTransformFunc&& func) & -> ValueNodeType {
    auto* val = std::get_if<BoolType>(&m_variant_value);
    if (val != nullptr) {
      return ValueNodeType{std::invoke(std::forward<TTransformFunc>(func), *val)};
    }
    return ValueNodeType{std::remove_cvref_t<std::invoke_result_t<TTransformFunc, BoolType&>>{}};
  }

  /**
   * @brief If the value held by this ValueNodeType is BoolType then call the
   * provided callback function
   * @tparam TTransformFunc callback function type
   * @param func callback function that takes a BoolType and returns something
   * convertible to ValueNodeType
   * @return the result of constructing a ValueNodeType with the default
   * constructed return value type of TTransformFunc
   */
  template <typename TTransformFunc>
  auto IfBoolTransform(TTransformFunc&& func) const& -> ValueNodeType {
    auto* val = std::get_if<BoolType>(&m_variant_value);
    if (val != nullptr) {
      return ValueNodeType{std::invoke(std::forward<TTransformFunc>(func), *val)};
    }
    return ValueNodeType{
        std::remove_cvref_t<std::invoke_result_t<TTransformFunc, const BoolType&>>{}};
  }

  /**
   * @brief If the value held by this ValueNodeType is BoolType then call the
   * provided callback function
   * @tparam TTransformFunc callback function type
   * @param func callback function that takes a BoolType and returns something
   * convertible to ValueNodeType
   * @return the result of constructing a ValueNodeType with the default
   * constructed return value type of TTransformFunc
   */
  template <typename TTransformFunc>
  auto IfBoolTransform(TTransformFunc&& func) && -> ValueNodeType {
    auto* val = std::get_if<BoolType>(&m_variant_value);
    if (val != nullptr) {
      return ValueNodeType{std::invoke(std::forward<TTransformFunc>(func), *val)};
    }
    return ValueNodeType{std::remove_cvref_t<std::invoke_result_t<TTransformFunc, BoolType>>{}};
  }

  /**
   * @brief If the value held by this ValueNodeType is BoolType then call the
   * provided callback function
   * @tparam TTransformFunc callback function type
   * @param func callback function that takes a BoolType and returns something
   * convertible to ValueNodeType
   * @return the result of constructing a ValueNodeType with the default
   * constructed return value type of TTransformFunc
   */
  template <typename TTransformFunc>
  auto IfBoolTransform(TTransformFunc&& func) const&& -> ValueNodeType {
    auto* val = std::get_if<BoolType>(&m_variant_value);
    if (val != nullptr) {
      return ValueNodeType{std::invoke(std::forward<TTransformFunc>(func), *val)};
    }
    return ValueNodeType{
        std::remove_cvref_t<std::invoke_result_t<TTransformFunc, const BoolType>>{}};
  }

  /**
   * @brief If the value held by this ValueNodeType is not NullType then call
   * the provided callback function
   * @tparam TElseFunc callback function type
   * @param func callback function that returns a ValueNodeType
   * @return *this, or the result of calling TElseFunc function
   */
  template <typename TElseFunc>
    requires std::same_as<std::remove_cvref_t<std::invoke_result_t<TElseFunc>>, ValueNodeType>
  auto IfNotNull(TElseFunc&& func) const& -> ValueNodeType {
    auto* val = std::get_if<NullType>(&m_variant_value);
    if (val != nullptr) { return *this; }
    return std::invoke(std::forward<TElseFunc>(func));
  }

  /**
   * @brief If the value held by this ValueNodeType is not NullType then call
   * the provided callback function
   * @tparam TElseFunc callback function type
   * @param func callback function that returns a ValueNodeType
   * @return *this, or the result of calling TElseFunc function
   */
  template <typename TElseFunc>
    requires std::same_as<std::remove_cvref_t<std::invoke_result_t<TElseFunc>>, ValueNodeType>
  auto IfNotNull(TElseFunc&& func) && -> ValueNodeType {
    auto* val = std::get_if<NullType>(&m_variant_value);
    if (val != nullptr) { return std::move(*this); }
    return std::invoke(std::forward<TElseFunc>(func));
  }

  /**
   * @brief If the value held by this ValueNodeType is not StringType then call
   * the provided callback function
   * @tparam TElseFunc callback function type
   * @param func callback function that returns a ValueNodeType
   * @return *this, or the result of calling TElseFunc function
   */
  template <typename TElseFunc>
    requires std::same_as<std::remove_cvref_t<std::invoke_result_t<TElseFunc>>, ValueNodeType>
  auto IfNotString(TElseFunc&& func) const& -> ValueNodeType {
    auto* val = std::get_if<StringType>(&m_variant_value);
    if (val != nullptr) { return *this; }
    return std::invoke(std::forward<TElseFunc>(func));
  }

  /**
   * @brief If the value held by this ValueNodeType is not StringType then call
   * the provided callback function
   * @tparam TElseFunc callback function type
   * @param func callback function that returns a ValueNodeType
   * @return *this, or the result of calling TElseFunc function
   */
  template <typename TElseFunc>
    requires std::same_as<std::remove_cvref_t<std::invoke_result_t<TElseFunc>>, ValueNodeType>
  auto IfNotString(TElseFunc&& func) && -> ValueNodeType {
    auto* val = std::get_if<StringType>(&m_variant_value);
    if (val != nullptr) { return std::move(*this); }
    return std::invoke(std::forward<TElseFunc>(func));
  }

  /**
   * @brief If the value held by this ValueNodeType is not NumberType then call
   * the provided callback function
   * @tparam TElseFunc callback function type
   * @param func callback function that returns a ValueNodeType
   * @return *this, or the result of calling TElseFunc function
   */
  template <typename TElseFunc>
    requires std::same_as<std::remove_cvref_t<std::invoke_result_t<TElseFunc>>, ValueNodeType>
  auto IfNotNumber(TElseFunc&& func) const& -> ValueNodeType {
    auto* val = std::get_if<NumberType>(&m_variant_value);
    if (val != nullptr) { return *this; }
    return std::invoke(std::forward<TElseFunc>(func));
  }

  /**
   * @brief If the value held by this ValueNodeType is not NumberType then call
   * the provided callback function
   * @tparam TElseFunc callback function type
   * @param func callback function that returns a ValueNodeType
   * @return *this, or the result of calling TElseFunc function
   */
  template <typename TElseFunc>
    requires std::same_as<std::remove_cvref_t<std::invoke_result_t<TElseFunc>>, ValueNodeType>
  auto IfNotNumber(TElseFunc&& func) && -> ValueNodeType {
    auto* val = std::get_if<NumberType>(&m_variant_value);
    if (val != nullptr) { return std::move(*this); }
    return std::invoke(std::forward<TElseFunc>(func));
  }

  /**
   * @brief If the value held by this ValueNodeType is not BoolType then call
   * the provided callback function
   * @tparam TElseFunc callback function type
   * @param func callback function that returns a ValueNodeType
   * @return *this, or the result of calling TElseFunc function
   */
  template <typename TElseFunc>
    requires std::same_as<std::remove_cvref_t<std::invoke_result_t<TElseFunc>>, ValueNodeType>
  auto IfNotBool(TElseFunc&& func) const& -> ValueNodeType {
    auto* val = std::get_if<BoolType>(&m_variant_value);
    if (val != nullptr) { return *this; }
    return std::invoke(std::forward<TElseFunc>(func));
  }

  /**
   * @brief If the value held by this ValueNodeType is not BoolType then call
   * the provided callback function
   * @tparam TElseFunc callback function type
   * @param func callback function that returns a ValueNodeType
   * @return *this, or the result of calling TElseFunc function
   */
  template <typename TElseFunc>
    requires std::same_as<std::remove_cvref_t<std::invoke_result_t<TElseFunc>>, ValueNodeType>
  auto IfNotBool(TElseFunc&& func) && -> ValueNodeType {
    auto* val = std::get_if<BoolType>(&m_variant_value);
    if (val != nullptr) { return std::move(*this); }
    return std::invoke(std::forward<TElseFunc>(func));
  }

  /**
   * @brief Visit a value node type with a visitor overload set
   * @tparam TCallables set of non final callable types
   * @param callables set of non final callables
   * @return the common return type of all callables provided
   */
  template <typename... TCallables>
  decltype(auto) Visit(TCallables&&... callables) {
    auto overload_set = Overload{std::forward<TCallables>(callables)...};
    return std::visit(overload_set, m_variant_value);
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
    return std::visit(overload_set, m_variant_value);
  }

  /**
   * @brief Compare this ValueNodeType with another ValueNodeType
   * @return comparison category
   */
  [[nodiscard]] constexpr auto operator<=>(const ValueNodeType&) const = default;

  /**
   * @brief Use the unsafe API within a lambda function
   *
   * The UnsafeProxy cannot be returned from the lambda function
   *
   * @tparam TFunc type of function
   * @param func unsafe block function
   * @return value returned by provided lambda function
   */
  template <typename TFunc>
    requires(std::is_invocable_v<TFunc, decltype(std::declval<ValueNodeType::UnsafeProxy>()),
                                 ValueNodeType&> &&
             !std::is_same_v<
                 std::decay_t<std::invoke_result_t<
                     TFunc, decltype(std::declval<ValueNodeType::UnsafeProxy>()), ValueNodeType&>>,
                 ValueNodeType::UnsafeProxy>)
  auto Unsafe(TFunc&& func)
      -> std::invoke_result_t<TFunc, decltype(std::declval<ValueNodeType::UnsafeProxy>()),
                              ValueNodeType&> {
    return std::invoke(std::forward<TFunc>(func), ValueNodeType::UnsafeProxy{*this}, *this);
  }

  /**
   * @brief Use the unsafe API within a lambda function
   *
   * The UnsafeProxy cannot be returned from the lambda function
   *
   * @tparam TFunc type of function
   * @param func unsafe block function
   * @return value returned by provided lambda function
   */
  template <typename TFunc>
    requires(std::is_invocable_v<TFunc, decltype(std::declval<ValueNodeType::UnsafeProxy>())> &&
             !std::is_same_v<std::decay_t<std::invoke_result_t<
                                 TFunc, decltype(std::declval<ValueNodeType::UnsafeProxy>())>>,
                             ValueNodeType::UnsafeProxy>)
  auto Unsafe(TFunc&& func)
      -> std::invoke_result_t<TFunc, decltype(std::declval<ValueNodeType::UnsafeProxy>())> {
    return std::invoke(std::forward<TFunc>(func), ValueNodeType::UnsafeProxy{*this});
  }

  /**
   * @brief Use the unsafe API within a lambda function
   *
   * The ConstUnsafeProxy cannot be returned from the lambda function
   *
   * @tparam TFunc type of function
   * @param func unsafe block function
   * @return value returned by provided lambda function
   */
  template <typename TFunc>
    requires(std::is_invocable_v<TFunc, decltype(std::declval<ValueNodeType::ConstUnsafeProxy>()),
                                 const ValueNodeType&> &&
             !std::is_same_v<std::decay_t<std::invoke_result_t<
                                 TFunc, decltype(std::declval<ValueNodeType::ConstUnsafeProxy>()),
                                 const ValueNodeType&>>,
                             ValueNodeType::ConstUnsafeProxy>)
  auto ConstUnsafe(TFunc&& func) const
      -> std::invoke_result_t<TFunc, decltype(std::declval<ValueNodeType::ConstUnsafeProxy>()),
                              const ValueNodeType&> {
    return std::invoke(std::forward<TFunc>(func), ValueNodeType::ConstUnsafeProxy{*this}, *this);
  }

  /**
   * @brief Use the unsafe API within a lambda function
   *
   * The ConstUnsafeProxy cannot be returned from the lambda function
   *
   * @tparam TFunc type of function
   * @param func unsafe block function
   * @return value returned by provided lambda function
   */
  template <typename TFunc>
    requires(
        std::is_invocable_v<TFunc, decltype(std::declval<ValueNodeType::ConstUnsafeProxy>())> &&
        !std::is_same_v<std::decay_t<std::invoke_result_t<
                            TFunc, decltype(std::declval<ValueNodeType::ConstUnsafeProxy>())>>,
                        ValueNodeType::ConstUnsafeProxy>)
  auto ConstUnsafe(TFunc&& func) const
      -> std::invoke_result_t<TFunc, decltype(std::declval<ValueNodeType::ConstUnsafeProxy>())> {
    return std::invoke(std::forward<TFunc>(func), ValueNodeType::ConstUnsafeProxy{*this});
  }

private:
  VariantValueType m_variant_value{NullType{}};
};

}  // namespace mguid

#endif  // DATATREE_VALUE_NODE_TYPE_HPP
