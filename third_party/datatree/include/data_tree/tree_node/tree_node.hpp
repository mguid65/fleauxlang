/**
 * @brief Declarations for TreeNode
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

#ifndef DATATREE_TREE_NODE_HPP
#define DATATREE_TREE_NODE_HPP

#include <concepts>
#include <memory>
#include <optional>
#include <queue>
#include <ranges>
#include <type_traits>
#include <variant>

#include "data_tree/common/common.hpp"
#include "data_tree/error/error_type.hpp"
#include "data_tree/node_types/detail/value_types/value_types.hpp"

namespace mguid {

class ObjectNodeType;
class ArrayNodeType;
class ValueNodeType;

enum class NodeTypeTag : std::uint8_t { Object, Array, Value };

using NodeType = std::variant<ObjectNodeType, ArrayNodeType, ValueNodeType>;

/**
 * @brief Represents a node in the data tree that can be an Object, Array, or
 * Value
 */
class TreeNode {
  template <typename TCallable>
  struct RecursiveVisitHelper {
    TCallable visitor;

    /**
     * @brief Inner recursive visit function
     * @param node current node
     * @param depth current depth
     */
    void Visit(TreeNode& node, std::size_t depth);

    /**
     * @brief Inner recursive visit function
     * @param node current node
     * @param depth current depth
     */
    void Visit(const TreeNode& node, std::size_t depth) const;
  };

  /**
   * @brief Proxy class that provides access to unsafe TreeNode functionality
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
     * @brief Try to get the requested type from this Unsafe Proxy TreeNode
     *
     * Assumes that the TreeNode has the TRequestedType
     *
     * @tparam TRequestedType the type requested
     * @return The requested type if it is the type being held or exception
     */
    template <typename TRequestedType>
    [[nodiscard]] auto Get() const -> const TRequestedType& {
      return std::get<TRequestedType>(*m_node_ref.m_data_impl);
    }

    /**
     * @brief Try to get the requested type from this TreeNode
     *
     * Assumes that the TreeNode has the TRequestedType
     *
     * @tparam TRequestedType the type requested
     * @return The requested type if it is the type being held or exception
     */
    template <typename TRequestedType>
    [[nodiscard]] auto Get() -> TRequestedType& {
      return std::get<TRequestedType>(*m_node_ref.m_data_impl);
    }

    /**
     * @brief Get an ObjectNodeType from this node
     *
     * Assumes that the TreeNode has an ObjectNodeType
     *
     * @return reference to ObjectNodeType
     */
    [[nodiscard]] auto GetObject() const -> const ObjectNodeType& { return Get<ObjectNodeType>(); }

    /**
     * @brief Get an ObjectNodeType from this node
     *
     * Assumes that the TreeNode has an ObjectNodeType
     *
     * @return reference to ObjectNodeType
     */
    [[nodiscard]] auto GetObject() -> ObjectNodeType& { return Get<ObjectNodeType>(); }

    /**
     * @brief Get an ArrayNodeType from this node
     *
     * Assumes that the TreeNode has an ArrayNodeType
     *
     * @return reference to ArrayNodeType
     */
    [[nodiscard]] auto GetArray() const -> const ArrayNodeType& { return Get<ArrayNodeType>(); }

    /**
     * @brief Get an ArrayNodeType from this node
     *
     * Assumes that the TreeNode has an ArrayNodeType
     *
     * @return reference to ArrayNodeType
     */
    [[nodiscard]] auto GetArray() -> ArrayNodeType& { return Get<ArrayNodeType>(); }

    /**
     * @brief Get an ValueNodeType from this node
     * @return reference to ValueNodeType
     */
    [[nodiscard]] auto GetValue() const -> const ValueNodeType& { return Get<ValueNodeType>(); }

    /**
     * @brief Get an ValueNodeType from this node
     *
     * Assumes that the TreeNode has an ValueNodeType
     *
     * @return reference to ValueNodeType
     */
    [[nodiscard]] auto GetValue() -> ValueNodeType& { return Get<ValueNodeType>(); }

    /**
     * @brief Get Null value from this TreeNode
     *
     * Assumes that the TreeNode has an ValueNodeType
     *
     * @return Null value from this TreeNode
     */
    [[nodiscard]] auto GetNull() const -> const NullType& {
      return GetValue().ConstUnsafe(
          [](const auto&& unsafe) -> decltype(auto) { return unsafe.GetNull(); });
    }

    /**
     * @brief Get Bool value from this TreeNode
     * @return Bool value from this TreeNode
     */
    [[nodiscard]] auto GetBool() const -> const BoolType& {
      return GetValue().ConstUnsafe(
          [](const auto&& unsafe) -> decltype(auto) { return unsafe.GetBool(); });
    }

    /**
     * @brief Get Number value from this TreeNode
     * @return Number value from this TreeNode
     */
    [[nodiscard]] auto GetNumber() const -> const NumberType& {
      return GetValue().ConstUnsafe(
          [](const auto&& unsafe) -> decltype(auto) { return unsafe.GetNumber(); });
    }

    /**
     * @brief Get String value from this TreeNode
     * @return String value from this TreeNode
     */
    [[nodiscard]] auto GetString() const -> const std::string& {
      return GetValue().ConstUnsafe(
          [](const auto&& unsafe) -> decltype(auto) { return unsafe.GetString(); });
    }

    /**
     * @brief Get Null value from this TreeNode
     * @return Null value from this TreeNode
     */
    [[nodiscard]] auto GetNull() -> NullType& {
      return GetValue().Unsafe([](auto&& unsafe) -> decltype(auto) { return unsafe.GetNull(); });
    }

    /**
     * @brief Get Bool value from this TreeNode
     * @return Bool value from this TreeNode
     */
    [[nodiscard]] auto GetBool() -> BoolType& {
      return GetValue().Unsafe([](auto&& unsafe) -> decltype(auto) { return unsafe.GetBool(); });
    }

    /**
     * @brief Get Number value from this TreeNode
     * @return Number value from this TreeNode
     */
    [[nodiscard]] auto GetNumber() -> NumberType& {
      return GetValue().Unsafe([](auto&& unsafe) -> decltype(auto) { return unsafe.GetNumber(); });
    }

    /**
     * @brief Get String value from this TreeNode
     * @return String value from this TreeNode
     */
    [[nodiscard]] auto GetString() -> std::string& {
      return GetValue().Unsafe([](auto&& unsafe) -> decltype(auto) { return unsafe.GetString(); });
    }

    /**
     * @brief Get a reference to the TreeNode that is mapped to the key/idx
     * equivalent to `key_or_idx`
     *
     * Assumes that this object is the proper type and the key/index exists
     *
     * @param key_or_idx the key/idx of the TreeNode to find
     * @return A reference to the requested element
     */
    [[nodiscard]] auto operator[](const KeyOrIdxType& key_or_idx) const
        -> const UnsafeProxyType<true> {
      return UnsafeProxyType<true>{key_or_idx.Visit(
          [&](const StringKeyType& key) -> const TreeNode& {
            return std::as_const(std::get<ObjectNodeType>(*m_node_ref.m_data_impl))
                .ConstUnsafe(
                    [&key](const auto&& unsafe) -> decltype(auto) { return unsafe.At(key); });
          },
          [&](const IntegerKeyType& idx) -> const TreeNode& {
            return std::as_const(std::get<ArrayNodeType>(*m_node_ref.m_data_impl))
                .ConstUnsafe(
                    [&idx](const auto&& unsafe) -> decltype(auto) { return unsafe.At(idx); });
          })};
    }

    /**
     * @brief Get a reference to the TreeNode that is mapped to the key/idx
     * equivalent to `key_or_idx`
     *
     * Assumes that this object is the proper type and the key/index exists
     *
     * @param key_or_idx the key/idx of the TreeNode to find
     * @return A reference to the requested element
     */
    [[nodiscard]] auto operator[](const KeyOrIdxType& key_or_idx) -> UnsafeProxyType<false> {
      return UnsafeProxyType<false>{key_or_idx.Visit(
          [&](const StringKeyType& key) -> const TreeNode& {
            return std::get<ObjectNodeType>(*m_node_ref.m_data_impl)
                .Unsafe([&key](auto&& unsafe) -> decltype(auto) { return unsafe.At(key); });
          },
          [&](const IntegerKeyType& idx) -> const TreeNode& {
            return std::get<ArrayNodeType>(*m_node_ref.m_data_impl)
                .Unsafe([&idx](auto&& unsafe) -> decltype(auto) { return unsafe.At(idx); });
          })};
    }

    /**
     * @brief Recursively visit a tree node with a visitor overload set
     *
     * TODO: Replace with iterative version, until then the recursion limit is 512
     *
     * @tparam TCallables set of non final callable types
     * @param callables set of non final callables
     */
    template <typename... TCallables>
    void RecursiveVisit(TCallables&&... callables) {
      auto visit_helper = RecursiveVisitHelper{Overload{std::forward<TCallables>(callables)...}};
      visit_helper.Visit(m_node_ref, 0);
    }

    /**
     * @brief Recursively visit a tree node with a visitor overload set
     *
     * TODO: Replace with iterative version, until then the recursion limit is 512
     *
     * @tparam TCallables set of non final callable types
     * @param callables set of non final callables
     */
    template <typename... TCallables>
    void RecursiveVisit(TCallables&&... callables) const {
      const auto visit_helper =
          RecursiveVisitHelper{Overload{std::forward<TCallables>(callables)...}};
      visit_helper.Visit(m_node_ref, 0);
    }

    /**
     * @brief Get a reference to the held TreeNode
     * @return a reference to the held TreeNode
     */
    [[nodiscard]] auto Safe() -> TreeNode& { return m_node_ref; }

    /**
     * @brief Get a reference to the held TreeNode
     * @return a reference to the held TreeNode
     */
    [[nodiscard]] auto Safe() const -> const TreeNode& { return m_node_ref; }

  private:
    explicit UnsafeProxyType(std::conditional_t<TConst, const TreeNode&, TreeNode&> ref)
        : m_node_ref{ref} {}

    friend TreeNode;
    std::conditional_t<TConst, const TreeNode&, TreeNode&> m_node_ref;
  };

public:
  using ConstUnsafeProxy = const UnsafeProxyType<true>;
  using UnsafeProxy = UnsafeProxyType<false>;

  /**
   * @brief Default construct a TreeNode
   */
  inline TreeNode();

  /**
   * @brief Copy construct a TreeNode
   */
  inline TreeNode(const TreeNode&);
  /**
   * @brief Move construct a TreeNode
   */
  inline TreeNode(TreeNode&&) noexcept;
  /**
   * @brief Copy assign a TreeNode
   * @return reference to this TreeNode
   */
  inline TreeNode& operator=(const TreeNode&);
  /**
   * @brief Move assign a TreeNode
   * @return reference to this TreeNode
   */
  inline TreeNode& operator=(TreeNode&&) noexcept;

  /**
   * @brief Destroy a TreeNode
   */
  inline ~TreeNode();

  /**
   * @brief Construct a TreeNode from an ObjectNodeType
   * @param node_data an ObjectNodeType
   */
  explicit inline TreeNode(const ObjectNodeType& node_data);

  /**
   * @brief Construct a TreeNode from an ObjectNodeType
   * @param node_data an ObjectNodeType
   */
  explicit inline TreeNode(ObjectNodeType&& node_data);

  /**
   * @brief Construct a TreeNode from an ArrayNodeType
   * @param node_data an ArrayNodeType
   */
  explicit inline TreeNode(const ArrayNodeType& node_data);

  /**
   * @brief Construct a TreeNode from an ArrayNodeType
   * @param node_data an ArrayNodeType
   */
  explicit inline TreeNode(ArrayNodeType&& node_data);

  /**
   * @brief Construct a TreeNode from an ValueNodeType
   * @param node_data an ValueNodeType
   */
  explicit inline TreeNode(const ValueNodeType& node_data);
  /**
   * @brief Construct a TreeNode from an ValueNodeType
   * @param node_data an ValueNodeType
   */
  explicit inline TreeNode(ValueNodeType&& node_data);

  /**
   * @brief Construct from a value that satisfies ValidValueNodeTypeValueType
   * @tparam TValueType type of value that satisfies ValidValueNodeTypeValueType
   * @param value value to use to construct inner value node
   */
  template <ValidValueNodeTypeValueType TValueType>
  explicit TreeNode(TValueType&& value);

  /**
   * @brief Assign a value that satisfies ValidValueNodeTypeValueType to this
   *
   * If this TreeNode is not a ValueNodeType, reset it to ValueNodeType then
   * assign the value
   *
   * @tparam TValueType type of value that satisfies ValidValueNodeTypeValueType
   * @param value value to assign
   * @return reference to this TreeNode
   */
  template <ValidValueNodeTypeValueType TValueType>
  TreeNode& operator=(TValueType&& value);

  /**
   * @brief Construct a TreeNode with the proper alternative given the tag
   *
   * NodeTypeTag::Object, NodeTypeTag::Array, or NodeTypeTag::Value
   *
   * @tparam tag tag corresponding with one of the node types
   */
  explicit inline TreeNode(NodeTypeTag tag);

  /**
   * @brief Copy assign a TreeNode from an ObjectNodeType
   * @param node_data an ObjectNodeType
   * @return reference to this TreeNode
   */
  inline TreeNode& operator=(const ObjectNodeType& node_data);
  /**
   * @brief Move assign a TreeNode from an ObjectNodeType
   * @param node_data an ObjectNodeType
   * @return reference to this TreeNode
   */
  inline TreeNode& operator=(ObjectNodeType&& node_data);
  /**
   * @brief Copy assign a TreeNode from an ArrayNodeType
   * @param node_data an ArrayNodeType
   * @return reference to this TreeNode
   */
  inline TreeNode& operator=(const ArrayNodeType& node_data);
  /**
   * @brief Move assign a TreeNode from an ArrayNodeType
   * @param node_data an ArrayNodeType
   * @return reference to this TreeNode
   */
  inline TreeNode& operator=(ArrayNodeType&& node_data);
  /**
   * @brief Copy assign a TreeNode from an ValueNodeType
   * @param node_data an ValueNodeType
   * @return reference to this TreeNode
   */
  inline TreeNode& operator=(const ValueNodeType& node_data);
  /**
   * @brief Move assign a TreeNode from an ValueNodeType
   * @param node_data an ValueNodeType
   * @return reference to this TreeNode
   */
  inline TreeNode& operator=(ValueNodeType&& node_data);

  /**
   * @brief Get type tag for this tree node
   * @return type tag
   */
  [[nodiscard]] inline NodeTypeTag Tag() const noexcept;

  /**
   * @brief Try to get the requested type from this TreeNode
   * @tparam TRequestedType the type requested
   * @return The requested type if it is the type being held, otherwise Error
   */
  template <ValidValueNodeTypeValueType TRequestedType>
  [[nodiscard]] auto Has() const noexcept -> bool;

  /**
   * @brief Try to get the requested type from this TreeNode
   * @tparam TRequestedType the type requested
   * @return The requested type if it is the type being held, otherwise Error
   */
  template <typename TRequestedType>
    requires(!ValidValueNodeTypeValueType<TRequestedType>)
  [[nodiscard]] auto Has() const noexcept -> bool;

  /**
   * @brief Is this node holding an ObjectNodeType
   * @return true if holding an ObjectNodeType, otherwise false
   */
  [[nodiscard]] inline auto HasObject() const noexcept -> bool;

  /**
   * @brief Is this node holding an ArrayNodeType
   * @return true if holding an ArrayNodeType, otherwise false
   */
  [[nodiscard]] inline auto HasArray() const noexcept -> bool;

  /**
   * @brief Is this node holding an ValueNodeType
   * @return true if holding an ValueNodeType, otherwise false
   */
  [[nodiscard]] inline auto HasValue() const noexcept -> bool;

  /**
   * @brief Check if this TreeNode is holding a ValueNodeType and if it is Null
   * type
   * @return
   */
  [[nodiscard]] inline auto HasNull() const noexcept -> bool;
  /**
   * @brief Check if this TreeNode is holding a ValueNodeType and if it is
   * Boolean type
   * @return
   */
  [[nodiscard]] inline auto HasBool() const noexcept -> bool;
  /**
   * @brief Check if this TreeNode is holding a ValueNodeType and if it is
   * Number type
   * @return
   */
  [[nodiscard]] inline auto HasNumber() const noexcept -> bool;
  /**
   * @brief Check if this TreeNode is holding a ValueNodeType and if it is
   * String type
   * @return
   */
  [[nodiscard]] inline auto HasString() const noexcept -> bool;

  /**
   * @brief Try to get the child corresponding to id from this
   * @param id id of child to find
   * @return child node or error
   */
  [[nodiscard]] inline auto TryGet(const KeyOrIdxType& id) -> RefExpected<TreeNode, Error>;

  /**
   * @brief Try to get the child corresponding to id from this
   * @param id id of child to find
   * @return child node or error
   */
  [[nodiscard]] inline auto TryGet(const KeyOrIdxType& id) const
      -> RefExpected<const TreeNode, Error>;

  /**
   * @brief Determine if the provided path exists in this data tree
   * @param key_or_idx path to check
   * @return true if the path exists, otherwise false
   */
  template <std::size_t NLength>
  [[nodiscard]] inline auto Exists(const Path<NLength>& path) const noexcept -> bool;

  /**
   * @brief Determine if the provided path exists in this data tree
   * @param key_or_idx path to check
   * @return true if the path exists, otherwise false
   */
  template <typename TRange>
    requires std::ranges::range<TRange> &&
             std::convertible_to<std::ranges::range_value_t<TRange>, KeyOrIdxType>
  [[nodiscard]] inline auto Exists(const TRange& ids) const noexcept -> bool;

  /**
   * @brief Determine if the provided path exists in this data tree and is a ValueNodeType
   * and has the proper value type
   * @param key_or_idx path to check
   * @return true if the path exists, otherwise false
   */
  template <ValidValueNodeTypeValueType TValueType, std::size_t NLength>
  [[nodiscard]] inline auto ContainsType(const Path<NLength>& path) const noexcept -> bool;

  /**
   * @brief Determine if the provided path exists in this data tree
   * @param key_or_idx path to check
   * @return true if the path exists, otherwise false
   */
  template <ValidValueNodeTypeValueType TValueType, typename TRange>
    requires std::ranges::range<TRange> &&
             std::convertible_to<std::ranges::range_value_t<TRange>, KeyOrIdxType>
  [[nodiscard]] inline auto ContainsType(const TRange& ids) const noexcept -> bool;

  /**
   * @brief Determine if the provided path exists in this data tree and is a ValueNodeType
   * and has the proper value type
   * @param key_or_idx path to check
   * @return true if the path exists, otherwise false
   */
  template <typename TNodeType, std::size_t NLength>
    requires(!ValidValueNodeTypeValueType<TNodeType>)
  [[nodiscard]] inline auto ContainsNodeType(const Path<NLength>& path) const noexcept -> bool;

  /**
   * @brief Determine if the provided path exists in this data tree
   * @param key_or_idx path to check
   * @return true if the path exists, otherwise false
   */
  template <typename TNodeType, typename TRange>
    requires std::ranges::range<TRange> &&
             std::convertible_to<std::ranges::range_value_t<TRange>, KeyOrIdxType> &&
             (!ValidValueNodeTypeValueType<TNodeType>)
  [[nodiscard]] inline auto ContainsNodeType(const TRange& ids) const noexcept -> bool;

  /**
   * @brief Try to get the requested type from this TreeNode
   * @tparam TRequestedType the type requested
   * @return The requested type if it is the type being held, otherwise Error
   */
  template <typename TRequestedType>
  [[nodiscard]] auto TryGet() const -> RefExpected<const TRequestedType, Error>;

  /**
   * @brief Try to get the requested type from this TreeNode
   * @tparam TRequestedType the type requested
   * @return The requested type if it is the type being held, otherwise Error
   */
  template <typename TRequestedType>
  [[nodiscard]] auto TryGet() -> RefExpected<TRequestedType, Error>;

  /**
   * @brief Try to get an ObjectNodeType from this node
   * @return ObjectNodeType if holding an ObjectNodeType, otherwise Error
   */
  [[nodiscard]] inline auto TryGetObject() const -> RefExpected<const ObjectNodeType, Error>;

  /**
   * @brief Try to get an ArrayNodeType from this node
   * @return ArrayNodeType if holding an ArrayNodeType, otherwise Error
   */
  [[nodiscard]] inline auto TryGetArray() const -> RefExpected<const ArrayNodeType, Error>;

  /**
   * @brief Try to get an ValueNodeType from this node
   * @return ValueNodeType if holding an ValueNodeType, otherwise Error
   */
  [[nodiscard]] inline auto TryGetValue() const -> RefExpected<const ValueNodeType, Error>;

  /**
   * @brief Try to get an ObjectNodeType from this node
   * @return ObjectNodeType if holding an ObjectNodeType, otherwise Error
   */
  [[nodiscard]] inline auto TryGetObject() -> RefExpected<ObjectNodeType, Error>;

  /**
   * @brief Try to get an ArrayNodeType from this node
   * @return ArrayNodeType if holding an ArrayNodeType, otherwise Error
   */
  [[nodiscard]] inline auto TryGetArray() -> RefExpected<ArrayNodeType, Error>;

  /**
   * @brief Try to get an ValueNodeType from this node
   * @return ValueNodeType if holding an ValueNodeType, otherwise Error
   */
  [[nodiscard]] inline auto TryGetValue() -> RefExpected<ValueNodeType, Error>;

  /**
   * @brief Try to get NullType value from this TreeNode
   * @return NullType value from this TreeNode
   */
  [[nodiscard]] inline auto TryGetNull() const -> RefExpected<const NullType, Error>;

  /**
   * @brief Try to get boolean value from this node
   * @return boolean value from this node
   */
  [[nodiscard]] inline auto TryGetBool() const -> RefExpected<const BoolType, Error>;

  /**
   * @brief Try to get integer value from this node
   * @return integer value from this node
   */
  [[nodiscard]] inline auto TryGetNumber() const -> RefExpected<const NumberType, Error>;

  /**
   * @brief Try to get string value from this node
   * @return string value from this node
   */
  [[nodiscard]] inline auto TryGetString() const -> RefExpected<const StringType, Error>;

  /**
   * @brief Try to get Null value from this TreeNode
   * @return Null value from this TreeNode
   */
  [[nodiscard]] inline auto TryGetNull() -> RefExpected<NullType, Error>;

  /**
   * @brief Try to get Bool value from this TreeNode
   * @return Bool value from this TreeNode
   */
  [[nodiscard]] inline auto TryGetBool() -> RefExpected<BoolType, Error>;

  /**
   * @brief Try to get Number value from this TreeNode
   * @return Number value from this TreeNode
   */
  [[nodiscard]] inline auto TryGetNumber() -> RefExpected<NumberType, Error>;

  /**
   * @brief Try to get String value from this TreeNode
   * @return String value from this TreeNode
   */
  [[nodiscard]] inline auto TryGetString() -> RefExpected<StringType, Error>;

  /**
   * @brief Set this TreeNode to an TNodeType
   * @tparam TNodeType type restricted by a concept to be ObjectNodeType,
   * ArrayNodeType, or ValueNodeType so we can use perfect forwarding
   * @param node_data one of the valid node types
   */
  template <typename TNodeType>
  void Set(TNodeType&& node_data);

  /**
   * @brief Reset this node, optionally specifying a new default node type by
   * tag
   * @tparam TTag tag corresponding with one of the node types
   */
  template <NodeTypeTag TTag = NodeTypeTag::Object>
    requires(static_cast<std::uint8_t>(TTag) < 3)
  void Reset();

  /**
   * @brief Reset this node, optionally specifying a new default node type by
   * tag
   * @param tag tag corresponding with one of the node types
   */
  inline void Reset(NodeTypeTag tag);

  /**
   * @brief Visit a tree node with a visitor overload set
   * @tparam TCallables set of non final callable types
   * @param callables set of non final callables
   * @return the common return type of all callables provided
   */
  template <typename... TCallables>
  decltype(auto) Visit(TCallables&&... callables);

  /**
   * @brief Visit a tree node with a visitor overload set
   * @tparam TCallables set of non final callable types
   * @param callables set of non final callables
   * @return the common return type of all callables provided
   */
  template <typename... TCallables>
  decltype(auto) Visit(TCallables&&... callables) const;

  /**
   * @brief Get a reference to the TreeNode that is mapped to the key equivalent
   * to `key_or_idx`
   *
   * If an TreeNode corresponding to key in the path is not the correct type for
   * the key or does not exist, reset or create that node
   *
   * @param key_or_idx the key of the TreeNode to find
   * @return A reference to the requested element
   */
  inline auto operator[](const KeyOrIdxType& key_or_idx) -> TreeNode&;

  /**
   * @brief Equality compare this TreeNode to another
   * @return true if they are equal, otherwise false
   */
  [[nodiscard]] inline bool operator==(const TreeNode& other) const noexcept;

  /**
   * @brief Erase TreeNode that is mapped to the key/idx or value equivalent to
   * `key_or_idx`
   * @param key_or_idx the key/idx of the TreeNode to erase
   * @return true if element was removed, otherwise false
   */
  inline bool Erase(const KeyOrIdxType& key_or_idx);

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
    requires(
        std::is_invocable_v<TFunc, decltype(std::declval<TreeNode::UnsafeProxy>()), TreeNode&> &&
        !std::is_same_v<std::decay_t<std::invoke_result_t<
                            TFunc, decltype(std::declval<TreeNode::UnsafeProxy>()), TreeNode&>>,
                        TreeNode::UnsafeProxy>)
  auto Unsafe(TFunc&& func)
      -> std::invoke_result_t<TFunc, decltype(std::declval<TreeNode::UnsafeProxy>()), TreeNode&>;

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
    requires(std::is_invocable_v<TFunc, decltype(std::declval<TreeNode::UnsafeProxy>())> &&
             !std::is_same_v<std::decay_t<std::invoke_result_t<
                                 TFunc, decltype(std::declval<TreeNode::UnsafeProxy>())>>,
                             TreeNode::UnsafeProxy>)
  auto Unsafe(TFunc&& func)
      -> std::invoke_result_t<TFunc, decltype(std::declval<TreeNode::UnsafeProxy>())>;

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
    requires(std::is_invocable_v<TFunc, decltype(std::declval<TreeNode::ConstUnsafeProxy>()),
                                 const TreeNode&> &&
             !std::is_same_v<
                 std::decay_t<std::invoke_result_t<
                     TFunc, decltype(std::declval<TreeNode::ConstUnsafeProxy>()), const TreeNode&>>,
                 TreeNode::ConstUnsafeProxy>)
  auto ConstUnsafe(TFunc&& func) const
      -> std::invoke_result_t<TFunc, decltype(std::declval<TreeNode::ConstUnsafeProxy>()),
                              const TreeNode&>;

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
    requires(std::is_invocable_v<TFunc, decltype(std::declval<TreeNode::ConstUnsafeProxy>())> &&
             !std::is_same_v<std::decay_t<std::invoke_result_t<
                                 TFunc, decltype(std::declval<TreeNode::ConstUnsafeProxy>())>>,
                             TreeNode::ConstUnsafeProxy>)
  auto ConstUnsafe(TFunc&& func) const
      -> std::invoke_result_t<TFunc, decltype(std::declval<TreeNode::ConstUnsafeProxy>())>;

  /**
   * @brief This is only to assist with negating a ValueNodeType with a NumberType
   * Does nothing if the type is incorrect
   * @return A TreeNode with a negated value or copy of this
   */
  [[nodiscard]] inline TreeNode operator-() const;

private:
  /**
   * @brief Based on a tag, create the corresponding node type
   *
   * This is effectively a static factory function
   *
   * @param tag tag corresponding with one of the node types
   */
  [[nodiscard]] static inline NodeType FromTag(NodeTypeTag tag);
  /**
   * @brief Based on a tag, create the corresponding node type
   *
   * This is effectively a static factory function
   *
   * @tparam TTag tag corresponding with one of the node types
   */
  template <NodeTypeTag TTag>
  [[nodiscard]] static inline auto FromTagTemplate();

  std::unique_ptr<NodeType> m_data_impl;
};

}  // namespace mguid

// Get implementations of ObjectNodeType, ArrayNodeType, and ValueNodeType
#include "data_tree/node_types/node_types.inc"

namespace mguid {

template <typename TFunc>
  requires(std::is_invocable_v<TFunc, decltype(std::declval<TreeNode::UnsafeProxy>()), TreeNode&> &&
           !std::is_same_v<std::decay_t<std::invoke_result_t<
                               TFunc, decltype(std::declval<TreeNode::UnsafeProxy>()), TreeNode&>>,
                           TreeNode::UnsafeProxy>)
auto TreeNode::Unsafe(TFunc&& func)
    -> std::invoke_result_t<TFunc, decltype(std::declval<TreeNode::UnsafeProxy>()), TreeNode&> {
  return std::invoke(std::forward<TFunc>(func), TreeNode::UnsafeProxy{*this}, *this);
}

template <typename TFunc>
  requires(std::is_invocable_v<TFunc, decltype(std::declval<TreeNode::UnsafeProxy>())> &&
           !std::is_same_v<std::decay_t<std::invoke_result_t<
                               TFunc, decltype(std::declval<TreeNode::UnsafeProxy>())>>,
                           TreeNode::UnsafeProxy>)
auto TreeNode::Unsafe(TFunc&& func)
    -> std::invoke_result_t<TFunc, decltype(std::declval<TreeNode::UnsafeProxy>())> {
  return std::invoke(std::forward<TFunc>(func), TreeNode::UnsafeProxy{*this});
}

template <typename TFunc>
  requires(std::is_invocable_v<TFunc, decltype(std::declval<TreeNode::ConstUnsafeProxy>()),
                               const TreeNode&> &&
           !std::is_same_v<
               std::decay_t<std::invoke_result_t<
                   TFunc, decltype(std::declval<TreeNode::ConstUnsafeProxy>()), const TreeNode&>>,
               TreeNode::ConstUnsafeProxy>)
auto TreeNode::ConstUnsafe(TFunc&& func) const
    -> std::invoke_result_t<TFunc, decltype(std::declval<TreeNode::ConstUnsafeProxy>()),
                            const TreeNode&> {
  return std::invoke(std::forward<TFunc>(func), TreeNode::ConstUnsafeProxy{*this}, *this);
}

template <typename TFunc>
  requires(std::is_invocable_v<TFunc, decltype(std::declval<TreeNode::ConstUnsafeProxy>())> &&
           !std::is_same_v<std::decay_t<std::invoke_result_t<
                               TFunc, decltype(std::declval<TreeNode::ConstUnsafeProxy>())>>,
                           TreeNode::ConstUnsafeProxy>)
auto TreeNode::ConstUnsafe(TFunc&& func) const
    -> std::invoke_result_t<TFunc, decltype(std::declval<TreeNode::ConstUnsafeProxy>())> {
  return std::invoke(std::forward<TFunc>(func), TreeNode::ConstUnsafeProxy{*this});
}

template <ValidValueNodeTypeValueType TValueType>
TreeNode::TreeNode(TValueType&& value)
    : m_data_impl{std::make_unique<NodeType>(ValueNodeType{std::forward<TValueType>(value)})} {}

template <ValidValueNodeTypeValueType TValueType>
TreeNode& TreeNode::operator=(TValueType&& value) {
  if (!HasValue()) { Reset<NodeTypeTag::Value>(); }
  Unsafe([&value](auto&& unsafe) { unsafe.GetValue() = std::forward<TValueType>(value); });
  return *this;
}

template <ValidValueNodeTypeValueType TRequestedType>
auto TreeNode::Has() const noexcept -> bool {
  return HasValue() && ConstUnsafe([](const auto&& unsafe) {
           return unsafe.GetValue().template Has<TRequestedType>();
         });
}

template <typename TRequestedType>
  requires(!ValidValueNodeTypeValueType<TRequestedType>)
auto TreeNode::Has() const noexcept -> bool {
  return std::holds_alternative<TRequestedType>(*m_data_impl);
}

template <typename TRequestedType>
auto TreeNode::TryGet() const -> RefExpected<const TRequestedType, Error> {
  if (auto* result = std::get_if<TRequestedType>(&(*m_data_impl)); result != nullptr) {
    return *result;
  }
  return make_unexpected(Error{.category = Error::Category::BadAccess});
}

template <typename TRequestedType>
auto TreeNode::TryGet() -> RefExpected<TRequestedType, Error> {
  if (auto* result = std::get_if<TRequestedType>(&(*m_data_impl)); result != nullptr) {
    return *result;
  }
  return make_unexpected(Error{.category = Error::Category::BadAccess});
}

template <typename TNodeType>
void TreeNode::Set(TNodeType&& node_data) {
  *m_data_impl = std::forward<TNodeType>(node_data);
}

template <NodeTypeTag TTag>
  requires(static_cast<std::uint8_t>(TTag) < 3)
void TreeNode::Reset() {
  *m_data_impl = FromTagTemplate<TTag>();
}

template <typename... TCallables>
decltype(auto) TreeNode::Visit(TCallables&&... callables) {
  auto overload_set = Overload{std::forward<TCallables>(callables)...};
  return std::visit(overload_set, *m_data_impl);
}

template <typename... TCallables>
decltype(auto) TreeNode::Visit(TCallables&&... callables) const {
  auto overload_set = Overload{std::forward<TCallables>(callables)...};
  return std::visit(overload_set, *m_data_impl);
}

template <typename TCallable>
void TreeNode::RecursiveVisitHelper<TCallable>::Visit(const TreeNode& node,
                                                      std::size_t depth) const {
  if (depth == 1024) { throw std::runtime_error{"Recursion depth exceeded"}; }
  std::visit(visitor, *node.m_data_impl);
  node.Visit(
      [this, depth](const ObjectNodeType& obj) mutable {
        for (auto& kv : obj) { Visit(kv.second, ++depth); }
      },
      [this, depth](const ArrayNodeType& arr) mutable {
        for (auto& item : arr) { Visit(item, ++depth); }
      },
      [](const ValueNodeType&) {});
}

template <typename TCallable>
void TreeNode::RecursiveVisitHelper<TCallable>::Visit(TreeNode& node, std::size_t depth) {
  std::queue<std::reference_wrapper<TreeNode>> to_visit;
  if (depth == 1024) { throw std::runtime_error{"Recursion depth exceeded"}; }
  std::visit(visitor, *node.m_data_impl);
  node.Visit(
      [this, depth](ObjectNodeType& obj) mutable {
        for (auto& kv : obj) { Visit(kv.second, ++depth); }
      },
      [this, depth](ArrayNodeType& arr) mutable {
        for (auto& item : arr) { Visit(item, ++depth); }
      },
      [](ValueNodeType&) {});
}

[[nodiscard]] inline NodeType TreeNode::FromTag(NodeTypeTag tag) {
  switch (tag) {
    case NodeTypeTag::Array:
      return {ArrayNodeType{}};
    case NodeTypeTag::Value:
      return {ValueNodeType{}};
    default:
      return {ObjectNodeType{}};
  }
}

template <NodeTypeTag TTag>
[[nodiscard]] inline auto TreeNode::FromTagTemplate() {
  if constexpr (TTag == NodeTypeTag::Array) {
    return ArrayNodeType{};
  } else if constexpr (TTag == NodeTypeTag::Value) {
    return ValueNodeType{};
  } else {
    return ObjectNodeType{};
  }
}

inline TreeNode::TreeNode() : m_data_impl{std::make_unique<NodeType>(ObjectNodeType{})} {}

inline TreeNode::TreeNode(const TreeNode& other)
    : m_data_impl{std::make_unique<NodeType>(*other.m_data_impl)} {}

inline TreeNode& TreeNode::operator=(const TreeNode& other) {
  if (&other != this) { m_data_impl = std::make_unique<NodeType>(*other.m_data_impl); }
  return *this;
}

inline TreeNode::TreeNode(TreeNode&&) noexcept = default;

inline TreeNode& TreeNode::operator=(TreeNode&&) noexcept = default;

inline TreeNode::TreeNode(const ObjectNodeType& node_data)
    : m_data_impl{std::make_unique<NodeType>(node_data)} {}

inline TreeNode::TreeNode(ObjectNodeType&& node_data)
    : m_data_impl{std::make_unique<NodeType>(node_data)} {}

inline TreeNode::TreeNode(const ArrayNodeType& node_data)
    : m_data_impl{std::make_unique<NodeType>(node_data)} {}

inline TreeNode::TreeNode(ArrayNodeType&& node_data)
    : m_data_impl{std::make_unique<NodeType>(node_data)} {}

inline TreeNode::TreeNode(const ValueNodeType& node_data)
    : m_data_impl{std::make_unique<NodeType>(node_data)} {}

inline TreeNode::TreeNode(ValueNodeType&& node_data)
    : m_data_impl{std::make_unique<NodeType>(node_data)} {}

inline TreeNode& TreeNode::operator=(const ObjectNodeType& node_data) {
  *m_data_impl = node_data;
  return *this;
}

inline TreeNode& TreeNode::operator=(ObjectNodeType&& node_data) {
  *m_data_impl = node_data;
  return *this;
}

inline TreeNode& TreeNode::operator=(const ArrayNodeType& node_data) {
  *m_data_impl = node_data;
  return *this;
}

inline TreeNode& TreeNode::operator=(ArrayNodeType&& node_data) {
  *m_data_impl = node_data;
  return *this;
}

inline TreeNode& TreeNode::operator=(const ValueNodeType& node_data) {
  *m_data_impl = node_data;
  return *this;
}

inline TreeNode& TreeNode::operator=(ValueNodeType&& node_data) {
  *m_data_impl = node_data;
  return *this;
}

inline TreeNode::TreeNode(NodeTypeTag tag)
    : m_data_impl{std::make_unique<NodeType>(FromTag(tag))} {}

inline NodeTypeTag TreeNode::Tag() const noexcept {
  switch (m_data_impl->index()) {
    case 0:
      return NodeTypeTag::Object;
    case 1:
      return NodeTypeTag::Array;
    case 2:
      return NodeTypeTag::Value;
    default:
      Unreachable();
  }
}

inline auto TreeNode::HasObject() const noexcept -> bool { return Has<ObjectNodeType>(); }

inline auto TreeNode::HasArray() const noexcept -> bool { return Has<ArrayNodeType>(); }

inline auto TreeNode::HasValue() const noexcept -> bool { return Has<ValueNodeType>(); }

inline auto TreeNode::HasNull() const noexcept -> bool { return Has<NullType>(); }

inline auto TreeNode::HasBool() const noexcept -> bool { return Has<BoolType>(); }

inline auto TreeNode::HasNumber() const noexcept -> bool { return Has<NumberType>(); }

inline auto TreeNode::HasString() const noexcept -> bool { return Has<StringType>(); }

inline auto TreeNode::TryGet(const KeyOrIdxType& key_or_idx) -> RefExpected<TreeNode, Error> {
  if (const auto* key_ptr = std::get_if<StringKeyType>(&key_or_idx); key_ptr != nullptr) {
    const auto& key = *key_ptr;
    if (auto obj = TryGetObject(); obj.has_value() && obj->Contains(key)) {
      return obj.value()[key];
    }
    return make_unexpected(Error{.category = Error::Category::KeyError});
  } else {
    const auto& idx = std::get<IntegerKeyType>(key_or_idx);
    if (auto arr = TryGetArray(); arr.has_value() && arr->Size() > idx) { return arr.value()[idx]; }
    return make_unexpected(Error{.category = Error::Category::KeyError});
  }
}

inline auto TreeNode::TryGet(const KeyOrIdxType& key_or_idx) const
    -> RefExpected<const TreeNode, Error> {
  if (const auto* key_ptr = std::get_if<StringKeyType>(&key_or_idx); key_ptr != nullptr) {
    const auto& key = *key_ptr;
    if (const auto obj = TryGetObject(); obj.has_value() && obj->Contains(key)) {
      return obj.value().ConstUnsafe(
          [&key](const auto& unsafe) -> decltype(auto) { return unsafe[key]; });
    }
    return make_unexpected(Error{.category = Error::Category::KeyError});
  } else {
    const auto& idx = std::get<IntegerKeyType>(key_or_idx);
    if (const auto arr = TryGetArray(); arr.has_value() && arr->Size() > idx) {
      return arr.value().ConstUnsafe(
          [&idx](const auto& unsafe) -> decltype(auto) { return unsafe[idx]; });
    }
    return make_unexpected(Error{.category = Error::Category::KeyError});
  }
}

template <std::size_t NLength>
auto TreeNode::Exists(const Path<NLength>& path) const noexcept -> bool {
  return [this]<std::size_t... NIdxs>(const auto& container_inner, std::index_sequence<NIdxs...>) {
    RefExpected<const TreeNode, Error> ref{*this};
    return ([&ref](const auto& item) {
      ref = ref->TryGet(item);
      return ref.has_value();
    }(container_inner[NIdxs]) &&
            ...);
  }(path.Items(), std::make_index_sequence<NLength>{});
}

template <typename TRange>
  requires std::ranges::range<TRange> &&
           std::convertible_to<std::ranges::range_value_t<TRange>, KeyOrIdxType>
auto TreeNode::Exists(const TRange& ids) const noexcept -> bool {
  RefExpected<const TreeNode, Error> ref{*this};
  for (const auto& item : ids) {
    ref = ref->TryGet(item);
    if (!ref.has_value()) { return false; }
  }
  return true;
}

template <ValidValueNodeTypeValueType TValueType, std::size_t NLength>
auto TreeNode::ContainsType(const Path<NLength>& path) const noexcept -> bool {
  return [this]<std::size_t... NIdxs>(const auto& container_inner, std::index_sequence<NIdxs...>) {
    RefExpected<const TreeNode, Error> ref{*this};
    return ([&ref](const auto& item) {
             ref = ref->TryGet(item);
             return ref.has_value();
           }(container_inner[NIdxs]) &&
            ...) &&
           ref->Has<TValueType>();
  }(path.Items(), std::make_index_sequence<NLength>{});
}

template <ValidValueNodeTypeValueType TValueType, typename TRange>
  requires std::ranges::range<TRange> &&
           std::convertible_to<std::ranges::range_value_t<TRange>, KeyOrIdxType>
auto TreeNode::ContainsType(const TRange& ids) const noexcept -> bool {
  RefExpected<const TreeNode, Error> ref{*this};
  for (const auto& item : ids) {
    ref = ref->TryGet(item);
    if (!ref.has_value()) { return false; }
  }
  return ref->Has<TValueType>();
}

template <typename TNodeType, std::size_t NLength>
  requires(!ValidValueNodeTypeValueType<TNodeType>)
auto TreeNode::ContainsNodeType(const Path<NLength>& path) const noexcept -> bool {
  return [this]<std::size_t... NIdxs>(const auto& container_inner, std::index_sequence<NIdxs...>) {
    RefExpected<const TreeNode, Error> ref{*this};
    return ([&ref](const auto& item) {
             ref = ref->TryGet(item);
             return ref.has_value();
           }(container_inner[NIdxs]) &&
            ...) &&
           ref->Has<TNodeType>();
  }(path.Items(), std::make_index_sequence<NLength>{});
}

template <typename TNodeType, typename TRange>
  requires std::ranges::range<TRange> &&
           std::convertible_to<std::ranges::range_value_t<TRange>, KeyOrIdxType> &&
           (!ValidValueNodeTypeValueType<TNodeType>)
auto TreeNode::ContainsNodeType(const TRange& ids) const noexcept -> bool {
  RefExpected<const TreeNode, Error> ref{*this};
  for (const auto& item : ids) {
    ref = ref->TryGet(item);
    if (!ref.has_value()) { return false; }
  }
  return ref->Has<TNodeType>();
}

inline auto TreeNode::TryGetObject() const -> RefExpected<const ObjectNodeType, Error> {
  return TryGet<ObjectNodeType>();
}

inline auto TreeNode::TryGetArray() const -> RefExpected<const ArrayNodeType, Error> {
  return TryGet<ArrayNodeType>();
}

inline auto TreeNode::TryGetValue() const -> RefExpected<const ValueNodeType, Error> {
  return TryGet<ValueNodeType>();
}

inline auto TreeNode::TryGetObject() -> RefExpected<ObjectNodeType, Error> {
  return TryGet<ObjectNodeType>();
}

inline auto TreeNode::TryGetArray() -> RefExpected<ArrayNodeType, Error> {
  return TryGet<ArrayNodeType>();
}

inline auto TreeNode::TryGetValue() -> RefExpected<ValueNodeType, Error> {
  return TryGet<ValueNodeType>();
}

inline auto TreeNode::TryGetNull() const -> RefExpected<const NullType, Error> {
  if (!HasValue()) { return make_unexpected(Error{.category = Error::Category::BadAccess}); }
  return ConstUnsafe([](const auto&& unsafe) { return unsafe.GetValue().TryGetNull(); });
}

inline auto TreeNode::TryGetBool() const -> RefExpected<const BoolType, Error> {
  if (!HasValue()) { return make_unexpected(Error{.category = Error::Category::BadAccess}); }
  return ConstUnsafe([](const auto&& unsafe) { return unsafe.GetValue().TryGetBool(); });
}

inline auto TreeNode::TryGetNumber() const -> RefExpected<const NumberType, Error> {
  if (!HasValue()) { return make_unexpected(Error{.category = Error::Category::BadAccess}); }
  return ConstUnsafe([](const auto&& unsafe) { return unsafe.GetValue().TryGetNumber(); });
}

inline auto TreeNode::TryGetString() const -> RefExpected<const StringType, Error> {
  if (!HasValue()) { return make_unexpected(Error{.category = Error::Category::BadAccess}); }
  return ConstUnsafe([](const auto&& unsafe) { return unsafe.GetValue().TryGetString(); });
}

inline auto TreeNode::TryGetNull() -> RefExpected<NullType, Error> {
  if (!HasValue()) { return make_unexpected(Error{.category = Error::Category::BadAccess}); }
  return Unsafe([](auto&& unsafe) { return unsafe.GetValue().TryGetNull(); });
}

inline auto TreeNode::TryGetBool() -> RefExpected<BoolType, Error> {
  if (!HasValue()) { return make_unexpected(Error{.category = Error::Category::BadAccess}); }
  return Unsafe([](auto&& unsafe) { return unsafe.GetValue().TryGetBool(); });
}

inline auto TreeNode::TryGetNumber() -> RefExpected<NumberType, Error> {
  if (!HasValue()) { return make_unexpected(Error{.category = Error::Category::BadAccess}); }
  return Unsafe([](auto&& unsafe) { return unsafe.GetValue().TryGetNumber(); });
}

inline auto TreeNode::TryGetString() -> RefExpected<StringType, Error> {
  if (!HasValue()) { return make_unexpected(Error{.category = Error::Category::BadAccess}); }
  return Unsafe([](auto&& unsafe) { return unsafe.GetValue().TryGetString(); });
}

inline void TreeNode::Reset(NodeTypeTag tag) { *m_data_impl = FromTag(tag); }

inline auto TreeNode::operator[](const KeyOrIdxType& key_or_idx) -> TreeNode& {
  if (const auto* key_ptr = std::get_if<StringKeyType>(&key_or_idx); key_ptr != nullptr) {
    const auto& key = *key_ptr;
    if (!HasObject()) { *m_data_impl = ObjectNodeType{}; }
    return std::get<ObjectNodeType>(*m_data_impl)[key];
  } else {
    const auto& idx = std::get<IntegerKeyType>(key_or_idx);
    if (!HasArray()) { *m_data_impl = ArrayNodeType{}; }
    return std::get<ArrayNodeType>(*m_data_impl)[idx];
  }
}

inline bool TreeNode::Erase(const KeyOrIdxType& key_or_idx) {
  if (const auto* key_ptr = std::get_if<StringKeyType>(&key_or_idx); key_ptr != nullptr) {
    const auto& key = *key_ptr;
    if (!HasObject()) { return false; }
    auto& obj = std::get<ObjectNodeType>(*m_data_impl);
    return static_cast<bool>(obj.Erase(key));
  } else {
    const auto& idx = std::get<IntegerKeyType>(key_or_idx);
    if (!HasArray()) { return false; }
    auto& arr = std::get<ArrayNodeType>(*m_data_impl);
    if (idx >= arr.Size()) { return false; }
    arr.Erase(std::next(arr.Begin(), static_cast<ArrayNodeType::DifferenceType>(idx)));
    return true;
  }
}

inline bool TreeNode::operator==(const TreeNode& other) const noexcept {
  return *m_data_impl == *other.m_data_impl;
}

[[nodiscard]] inline TreeNode TreeNode::operator-() const {
  if (HasNumber()) {
    return ConstUnsafe(
        [](const auto&& unsafe) { return TreeNode{ValueNodeType{-unsafe.GetNumber()}}; });
  }
  return *this;
}

inline TreeNode::~TreeNode() = default;

}  // namespace mguid

#endif  // DATATREE_TREE_NODE_HPP
