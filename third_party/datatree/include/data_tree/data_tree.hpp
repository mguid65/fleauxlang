/**
 * @brief Declarations for datatree
 * @author Matthew Guidry(github: mguid65)
 * @date 2024-02-04
 */

#ifndef DATATREE_DATATREE_HPP
#define DATATREE_DATATREE_HPP

#include <charconv>
#include <string_view>

#include "data_tree/common/common.hpp"
#include "data_tree/tree_node/tree_node.hpp"

namespace mguid {

/**
 * @brief Main DataTree type alias for TreeNode
 *
 * DataTree (TreeNode) can hold one of four node types:
 * 1. ObjectNodeType   - For user-facing associative arrays/dictionaries
 * 2. ArrayNodeType    - For ordered collections/arrays
 * 3. ValueNodeType    - For primitive values (null, bool, numbers, strings)
 * 4. GenericNodeType  - For special internal types (functions, file handles, etc.)
 *
 * Usage patterns:
 * - Use ObjectNodeType for creating JSON-like structures and user data
 * - Use ArrayNodeType for ordered lists of elements
 * - Use ValueNodeType for primitive data
 * - Use GenericNodeType for host-managed special types that shouldn't be
 *   exposed to user code (functions, file handles, system resources)
 */
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
inline mguid::DataTree operator""_DT_STR(const char* value, std::size_t sz) {
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
      },
      [&os](const mguid::GenericNodeType&) {
        os << "<generic>";
      });
  return os;
}

}  // namespace mguid

#endif  // DATATREE_DATATREE_HPP
