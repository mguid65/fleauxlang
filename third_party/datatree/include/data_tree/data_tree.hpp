/**
 * @brief Declarations for datatree
 * @author Matthew Guidry(github: mguid65)
 * @date 2024-02-04
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

#ifndef DATATREE_DATATREE_HPP
#define DATATREE_DATATREE_HPP

#include <charconv>
#include <string_view>

#include "data_tree/common/common.hpp"
#include "data_tree/tree_node/tree_node.hpp"

namespace mguid {

using DataTree = TreeNode;

/*
 * @brief Define some constant values for convenience
 */
static const auto NullValue = ValueNodeType{Null};
static const auto TrueValue = ValueNodeType{true};
static const auto FalseValue = ValueNodeType{false};

/*
 * @brief Define some constant value typed trees for convenience
 */
static const auto NullValueTree = DataTree{ValueNodeType{Null}};
static const auto TrueValueTree = DataTree{ValueNodeType{true}};
static const auto FalseValueTree = DataTree{ValueNodeType{false}};

namespace literals {

/**
 * @brief UDL operator to create a value typed DataTree from an int64
 * @param value some string representation of a number
 * @return a DataTree created from the parsed value
 */
inline mguid::DataTree operator""_DT_I64(unsigned long long int val) {
  return mguid::DataTree{mguid::ValueNodeType{static_cast<IntegerType>(val)}};
}

/**
 * @brief UDL operator to create a value typed DataTree from an int64
 * @param value some string representation of a number
 * @return a DataTree created from the parsed value
 */
inline mguid::DataTree operator""_DT_U64(unsigned long long int val) {
  return mguid::DataTree{mguid::ValueNodeType{static_cast<UnsignedIntegerType>(val)}};
}

/**
 * @brief UDL operator to create a value typed DataTree from an int64
 * @param value some string representation of a number
 * @return a DataTree created from the parsed value
 */
inline mguid::DataTree operator""_DT_F64(long double val) {
  return mguid::DataTree{mguid::ValueNodeType{static_cast<DoubleType>(val)}};
}

/**
 * @brief UDL operator to create a DataTree from a string
 * @param value string data
 * @param sz string size
 * @return a DataTree created with the string value
 */
mguid::DataTree operator""_DT_STR(const char* value, std::size_t sz) {
  return mguid::DataTree(mguid::ValueNodeType{mguid::StringType{value, sz}});
}

}  // namespace literals

/**
 * @brief Stream insertion operator overload for DataTree
 * @param os some ostream
 * @param dt a DataTree
 * @return reference to os
 */
inline std::ostream& operator<<(std::ostream& os, const DataTree& dt) {
  dt.Visit(
      [&os](const mguid::ObjectNodeType& obj) {
        os << '{';
        bool first{true};
        for (const auto& [key, node] : obj) {
          auto comma = [](bool& val) {
            if (val) {
              val = false;
              return "";
            } else {
              return ",";
            }
          };
          os << comma(first) << "\"" << key << "\":" << node;
        }
        os << '}';
      },
      [&os](const mguid::ArrayNodeType& arr) {
        os << '[';
        bool first{true};
        for (const auto& item : arr) {
          auto comma = [](bool& val) {
            if (val) {
              val = false;
              return " ";
            } else {
              return ",";
            }
          };
          os << comma(first) << item;
        }
        os << ']';
      },
      [&os](const mguid::ValueNodeType& val) {
        val.Visit(
            [&os](const mguid::BoolType& value) {
              os << std::boolalpha << value << std::noboolalpha;
            },
            [&os](const mguid::StringType& value) { os << '"' << value << '"'; },
            [&os](const auto& value) { os << value; });
      });
  return os;
}

}  // namespace mguid

#endif  // DATATREE_DATATREE_HPP
