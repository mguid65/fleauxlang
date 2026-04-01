/**
 * @brief Value type declarations and concepts
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

#ifndef DATATREE_VALUE_TYPES_HPP
#define DATATREE_VALUE_TYPES_HPP

#include <string>
#include <type_traits>
#include <variant>

#include "data_tree/node_types/detail/value_types/null_type.hpp"
#include "data_tree/node_types/detail/value_types/number_type.hpp"

namespace mguid {

using BoolType = bool;

using StringType = std::string;
//    NumberType
//    NullType

/**
 * @brief A type satisfies this concept if it is the same as bool without
 * cvref qualifiers
 * @tparam TType type to constrain
 */
template <typename TType>
concept SatisfiesBoolType = std::same_as<std::remove_cvref_t<TType>, bool>;

/**
 * @brief A type satisfies this concept if it is the convertible to
 * std::string without cvref qualifiers
 * @tparam TType type to constrain
 */
template <typename TType>
concept SatisfiesStringType = !std::same_as<std::remove_cvref_t<TType>, std::nullptr_t> &&
                              std::convertible_to<std::remove_cvref_t<TType>, std::string>;

/**
 * @brief A type satisfies this concept if it satisfies AllowedNumericType
 * or whose underlying type is a NumberType
 * @tparam TType type to constrain
 */
template <typename TType>
concept SatisfiesNumberType =
    detail::AllowedNumericType<TType> || std::same_as<std::remove_cvref_t<TType>, NumberType>;

/**
 * @brief A type satisfies this concept if it is the same as NullType without
 * cvref qualifiers
 * @tparam TType type to constrain
 */
template <typename TType>
concept SatisfiesNullType = std::same_as<std::remove_cvref_t<TType>, NullType>;

/**
 * @brief A variant type of allowed value node value types
 */
using VariantValueType = std::variant<NullType, BoolType, NumberType, StringType>;

/**
 * @brief A type satisfies this concept if it satisfies one of the value node
 * value type concepts
 *
 * This is named like this because I think its funny
 *
 * @tparam TType type to constrain
 */
template <typename TType>
concept ValidValueNodeTypeValueType = SatisfiesBoolType<TType> || SatisfiesStringType<TType> ||
                                      SatisfiesNumberType<TType> || SatisfiesNullType<TType>;

}  // namespace mguid

#endif  // DATATREE_VALUE_TYPES_HPP
