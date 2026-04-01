/**
 * @brief Declarations for object node type
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

#ifndef DATATREE_OBJECT_NODE_TYPE_HPP
#define DATATREE_OBJECT_NODE_TYPE_HPP

// Oh no, horrible bad map type
// Could use martinus/robin-hood-hashing unordered_node_map which provides
// similar reference and pointer stability as std::unordered_map but it is no
// longer being developed in favor of another project: martinus/unordered_dense
// problems with unordered_dense while I sit here at midnight is that I don't
// know if I will need strong reference stability because I haven't
// written any code yet. So for now I will just use std::unordered_map.
// I will eventually create some benchmarks and compare at least
// std::unordered_map vs robin-hood-hashing::unordered_node_map vs
// folly::F14NodeMap
#include <string>
#include <unordered_map>

#include "data_tree/common/common.hpp"
#include "data_tree/error/error_type.hpp"

namespace mguid {

class TreeNode;

/**
 * @brief Map like class that defines an object like node with children
 *
 * Most of the functions here just forward to the underlying map
 */
class ObjectNodeType {
public:
  using MapType = std::unordered_map<std::string, TreeNode>;
  using ValueType = MapType::value_type;
  using KeyType = MapType::key_type;
  using MappedType = MapType::mapped_type;
  using SizeType = MapType::size_type;
  using Iterator = MapType::iterator;
  using ConstIterator = MapType::const_iterator;

  using ExpectedRType = RefExpected<MappedType, Error>;
  using ConstExpectedRType = RefExpected<const MappedType, Error>;

private:
  /**
   * @brief Proxy class that provides access to unsafe ObjectNodeType functionality
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
     * @brief Returns a reference to the mapped value of the element with
     * specified key.
     *
     * @throws std::out_of_range if key doesnt exist
     * @param key the key of the element to find
     * @return A reference to the mapped value of the requested element.
     */
    [[nodiscard]] auto At(const KeyType& key) -> MappedType& {
      return m_node_ref.m_children.at(key);
    }

    /**
     * @brief Returns a reference to the mapped value of the element with
     * specified key.
     *
     * @throws std::out_of_range if key doesnt exist
     * @param key the key of the element to find
     * @return A reference to the mapped value of the requested element.
     */
    [[nodiscard]] auto At(const KeyType& key) const -> const MappedType& {
      return m_node_ref.m_children.at(key);
    }

    /**
     * @brief Returns a reference to the value that is mapped to a key
     * equivalent to `key`
     * @throws std::out_of_range if key doesnt exist
     * @param key the key of the element to find
     * @return A reference to the mapped value of the requested element.
     */
    [[nodiscard]] auto operator[](const KeyType& key) const -> const MappedType& {
      return m_node_ref.m_children.at(key);
    }

    /**
     * @brief Get a reference to the held ObjectNodeType
     * @return a reference to the held ObjectNodeType
     */
    [[nodiscard]] auto Safe() -> ObjectNodeType& { return m_node_ref; }

    /**
     * @brief Get a reference to the held ObjectNodeType
     * @return a reference to the held ObjectNodeType
     */
    [[nodiscard]] auto Safe() const -> const ObjectNodeType& { return m_node_ref; }

  private:
    explicit UnsafeProxyType(std::conditional_t<TConst, const ObjectNodeType&, ObjectNodeType&> ref)
        : m_node_ref{ref} {}

    friend ObjectNodeType;
    std::conditional_t<TConst, const ObjectNodeType&, ObjectNodeType&> m_node_ref;
  };

public:
  using ConstUnsafeProxy = const UnsafeProxyType<true>;
  using UnsafeProxy = UnsafeProxyType<false>;

  /**
   * @brief Default construct an ObjectNodeType
   */
  ObjectNodeType() = default;

  /**
   * @brief Explicit defaults for copy/move construction/assignment
   */
  ObjectNodeType(const ObjectNodeType&) = default;
  ObjectNodeType(ObjectNodeType&&) noexcept = default;
  ObjectNodeType& operator=(const ObjectNodeType&) = default;
  ObjectNodeType& operator=(ObjectNodeType&&) noexcept = default;

  /**
   * @brief Construct an ObjectNodeType from an initializer list of ValueType
   * @param init_list an initializer list of ValueType
   */
  ObjectNodeType(std::initializer_list<ValueType> init_list) : m_children{init_list} {}

  /**
   * @brief Assign an initializer list of ValueType to this
   * @param init_list an initializer list of ValueType
   * @return reference to this
   */
  ObjectNodeType& operator=(std::initializer_list<ValueType> init_list) {
    m_children = init_list;
    return *this;
  }

  /**
   * @brief Construct an ObjectNodeType from an existing map
   * @param init_mapping an initial mapping to initialize this with
   */
  explicit ObjectNodeType(MapType init_mapping) : m_children{std::move(init_mapping)} {}

  /**
   * @brief Try to get a copy of the node id with the specified key
   * @param key the key of the node id to find
   * @return the associated node id if the key it exists; otherwise BadAccess
   * error
   */
  [[nodiscard]] auto TryGet(const KeyType& key) const -> ConstExpectedRType {
    if (auto iter = m_children.find(key); iter != m_children.end()) { return iter->second; }
    return make_unexpected(Error{.category = Error::Category::KeyError});
  }

  /**
   * @brief Try to get a copy of the node id with the specified key
   * @param key the key of the node id to find
   * @return the associated node id if the key it exists; otherwise BadAccess
   * error
   */
  [[nodiscard]] auto TryGet(const KeyType& key) -> ExpectedRType {
    if (auto iter = m_children.find(key); iter != m_children.end()) { return iter->second; }
    return make_unexpected(Error{.category = Error::Category::KeyError});
  }

  /**
   * @brief Erase all elements from this ObjectNodeType
   */
  void Clear() noexcept { m_children.clear(); }

  /**
   * @brief Insert value if the container doesn't already contain an element
   * with an equivalent key
   * @param value value to insert
   * @return A std::pair consisting of an Iterator to the inserted value (or
   * to the element that prevented the insertion) and a bool denoting whether
   * the insertion took place (true if insertion happened, false if it did not).
   */
  auto Insert(const ValueType& value) -> std::pair<Iterator, bool> {
    return m_children.insert(value);
  }

  /**
   * @brief Insert value if the container doesn't already contain an element
   * with an equivalent key
   * @param value value to insert
   * @return A std::pair consisting of an Iterator to the inserted value (or
   * to the element that prevented the insertion) and a bool denoting whether
   * the insertion took place (true if insertion happened, false if it did not).
   */
  auto Insert(ValueType&& value) -> std::pair<Iterator, bool> { return m_children.insert(value); }

  /**
   * @brief Insert value if the container doesn't already contain an element
   * with an equivalent key
   *
   * This overload is equivalent to
   * Emplace(std::forward<TConvertibleToValueType>(value)) and only participates
   * in overload resolution if std::is_constructible_v<ValueType,
   * TConvertibleToValueType&&> == true.
   *
   * @tparam TConvertibleToValueType type of value to insert
   * @param value value to insert
   * @return A std::pair consisting of an Iterator to the inserted value (or
   * to the element that prevented the insertion) and a bool denoting whether
   * the insertion took place (true if insertion happened, false if it did not).
   */
  template <typename TConvertibleToValueType>
  auto Insert(TConvertibleToValueType&& value) -> std::pair<Iterator, bool> {
    return m_children.insert(std::forward<TConvertibleToValueType>(value));
  }

  /**
   * @brief Inserts value, using hint as a non-binding suggestion to where the
   * search should start.
   * @param value value to insert
   * @return An iterator to the inserted value, or to the value that
   * prevented the insertion.
   */
  auto InsertHint(ConstIterator hint, const ValueType& value) -> Iterator {
    return m_children.insert(hint, value);
  }

  /**
   * @brief Inserts value, using hint as a non-binding suggestion to where the
   * search should start.
   * @param value value to insert
   * @return An iterator to the inserted value, or to the value that
   * prevented the insertion.
   */
  auto InsertHint(ConstIterator hint, ValueType&& value) -> Iterator {
    return m_children.insert(hint, value);
  }

  /**
   * @brief Inserts value, using hint as a non-binding suggestion to where the
   * search should start.
   *
   * This overload is equivalent to
   * EmplaceHint(hint, std::forward<TConvertibleToValueType>(value)) and only
   * participates in overload resolution if std::is_constructible_v<ValueType,
   * TConvertibleToValueType&&> == true.
   *
   * @tparam TConvertibleToValueType type of value to insert
   *
   * @param value value to insert
   * @return An iterator to the inserted value, or to the value that
   * prevented the insertion.
   */
  template <typename TConvertibleToValueType>
  auto InsertHint(ConstIterator hint, TConvertibleToValueType&& value) -> Iterator {
    return m_children.insert(hint, value);
  }

  /**
   * @brief Inserts elements from initializer list init_list.
   *
   * If multiple elements in
   * the range have keys that compare equivalent, it is unspecified which
   * element is inserted
   *
   * @param init_list initializer list to insert the values from
   */
  void Insert(std::initializer_list<ValueType> init_list) { m_children.insert(init_list); }

  /**
   * @brief Insert a new element or assign to an existing element if found
   *
   * If a key equivalent to key already exists in the container, assigns
   * std::forward<TValue>(obj) to the MappedType corresponding to the key key.
   * If the key does not exist, inserts the new value as if by Insert,
   * constructing it from ValueType(key, std::forward<TValue>(obj)).
   *
   * @tparam TValue type of value to insert
   * @param key the key used both to look up and to insert if not found
   * @param obj the value to insert or assign
   * @return The bool component is true if the insertion took place and false if
   * the assignment took place. The iterator component is pointing at the
   * element that was inserted or updated.
   */
  template <typename TValue>
  auto InsertOrAssign(const KeyType& key, TValue&& obj) -> std::pair<Iterator, bool> {
    return m_children.insert_or_assign(key, std::forward<TValue>(obj));
  }

  /**
   * @brief Insert a new element or assign to an existing element if found
   *
   * If a key equivalent to key already exists in the container, assigns
   * std::forward<TValue>(obj) to the MappedType corresponding to the key key.
   * If the key does not exist, inserts the new value as if by Insert,
   * constructing it from ValueType(std::move(key), std::forward<TValue>(obj)).
   *
   * @tparam TValue type of value to insert
   * @param key the key used both to look up and to insert if not found
   * @param obj the value to insert or assign
   * @return The bool component is true if the insertion took place and false if
   * the assignment took place. The iterator component is pointing at the
   * element that was inserted or updated.
   */
  template <typename TValue>
  auto InsertOrAssign(KeyType&& key, TValue&& obj) -> std::pair<Iterator, bool> {
    return m_children.insert_or_assign(key, std::forward<TValue>(obj));
  }

  /**
   * @brief Insert a new element or assign to an existing element if found
   *
   * If a key equivalent to key already exists in the container, assigns
   * std::forward<TValue>(obj) to the MappedType corresponding to the key key.
   * If the key does not exist, constructs an object u of ValueType with
   * std::forward<KeyType>(key), std::forward<TValue>(obj)), then inserts u into
   * the underlying unordered_map. If hash_function()(u.first) !=
   * hash_function()(k) || contains(u.first) is true, the behavior is undefined.
   * The ValueType must be EmplaceConstructible into the underlying
   * unordered_map from std::forward<KeyType>(key), std::forward<TValue>(obj).
   * This overload participates in overload resolution only if
   * Hash::is_transparent and KeyEqual::is_transparent are valid and each
   * denotes a type. This assumes that such Hash is callable with both KeyType
   * and Key type, and that the KeyEqual is transparent, which, together, allows
   * calling this function without constructing an instance of Key.
   *
   * @tparam TValue type of value to insert
   * @param hint iterator to the position before which the new element will be
   * inserted
   * @param key the key used both to look up and to insert if not found
   * @param obj the value to insert or assign
   * @return The bool component is true if the insertion took place and false if
   * the assignment took place. The iterator component is pointing at the
   * element that was inserted or updated.
   */
  template <typename TValue>
  auto InsertOrAssignHint(ConstIterator hint, const KeyType& key, TValue&& obj) -> Iterator {
    return m_children.insert_or_assign(hint, key, std::forward<TValue>(obj));
  }

  /**
   * @brief Insert a new element or assign to an existing element if found
   *
   * If a key equivalent to key already exists in the container, assigns
   * std::forward<TValue>(obj) to the MappedType corresponding to the key key.
   * If the key does not exist, constructs an object u of ValueType with
   * std::forward<KeyType>(key), std::forward<TValue>(obj)), then inserts u into
   * the underlying unordered_map. If hash_function()(u.first) !=
   * hash_function()(k) || contains(u.first) is true, the behavior is undefined.
   * The ValueType must be EmplaceConstructible into the underlying
   * unordered_map from std::forward<KeyType>(key), std::forward<TValue>(obj).
   * This overload participates in overload resolution only if
   * Hash::is_transparent and KeyEqual::is_transparent are valid and each
   * denotes a type. This assumes that such Hash is callable with both KeyType
   * and Key type, and that the KeyEqual is transparent, which, together, allows
   * calling this function without constructing an instance of Key.
   *
   * @tparam TValue type of value to insert
   * @param hint iterator to the position before which the new element will be
   * inserted
   * @param key the key used both to look up and to insert if not found
   * @param obj the value to insert or assign
   * @return The bool component is true if the insertion took place and false if
   * the assignment took place. The iterator component is pointing at the
   * element that was inserted or updated.
   */
  template <typename TValue>
  auto InsertOrAssignHint(ConstIterator hint, KeyType&& key, TValue&& obj) -> Iterator {
    return m_children.insert_or_assign(hint, key, std::forward<TValue>(obj));
  }

  /**
   * @brief Inserts a new element into the container constructed in-place with
   * the given args if there is no element with the key in the container.
   * @tparam TArgs type of arguments to forward
   * @param args arguments to forward to the constructor of the element
   * @return Returns a pair consisting of an iterator to the inserted element,
   * or the already-existing element if no insertion happened, and a bool
   * denoting whether the insertion took place (true if insertion happened,
   * false if it did not).
   */
  template <typename... TArgs>
  auto Emplace(TArgs&&... args) -> std::pair<Iterator, bool> {
    return m_children.emplace(std::forward<TArgs>(args)...);
  }

  /**
   * @brief Inserts a new element into the container, using hint as a suggestion
   * where the element should go.
   * @tparam TArgs type of arguments to forward
   * @param hint iterator, used as a suggestion as to where to insert the new
   * element
   * @param args arguments to forward to the constructor of the element
   * @return Returns an iterator to the newly inserted element. If the insertion
   * failed because the element already exists, returns an iterator to the
   * already existing element with the equivalent key.
   */
  template <typename... TArgs>
  auto EmplaceHint(ConstIterator hint, TArgs&&... args) -> Iterator {
    return m_children.emplace_hint(hint, std::forward<TArgs>(args)...);
  }

  /**
   * @brief If a key equivalent to key already exists in the container, does
   * nothing. Otherwise, inserts a new element into the container with key k and
   * value constructed with args.
   *
   * In such case that the key does not exist in the container:
   *
   * Behaves like Emplace except that the element is constructed as
   * value_type(std::piecewise_construct,
   *            std::forward_as_tuple(k),
   *            std::forward_as_tuple(std::forward<Args>(args)...))
   *
   * @tparam TArgs type of arguments to forward
   * @param key the key used both to look up and to insert if not found
   * @param args arguments to forward to the constructor of the element
   * @return The bool component is true if the insertion took place and false if
   * the assignment took place. The iterator component is pointing at the
   * element that was inserted or updated.
   */
  template <typename... TArgs>
  auto TryEmplace(const KeyType& key, TArgs&&... args) -> std::pair<Iterator, bool> {
    return m_children.try_emplace(key, std::forward<TArgs>(args)...);
  }

  /**
   * @brief If a key equivalent to key already exists in the container, does
   * nothing. Otherwise, inserts a new element into the container with key k and
   * value constructed with args.
   *
   * In such case that the key does not exist in the container:
   *
   * Behaves like emplace except that the element is constructed as
   * value_type(std::piecewise_construct,
   *            std::forward_as_tuple(std::move(k)),
   *            std::forward_as_tuple(std::forward<Args>(args)...))
   *
   * @tparam TArgs type of arguments to forward
   * @param key the key used both to look up and to insert if not found
   * @param args arguments to forward to the constructor of the element
   * @return The bool component is true if the insertion took place and false if
   * the assignment took place. The iterator component is pointing at the
   * element that was inserted or updated.
   */
  template <typename... TArgs>
  auto TryEmplace(KeyType&& key, TArgs&&... args) -> std::pair<Iterator, bool> {
    return m_children.try_emplace(key, std::forward<TArgs>(args)...);
  }

  /**
   * @brief If a key equivalent to key already exists in the container, does
   * nothing. Otherwise, inserts a new element into the container with key k and
   * value constructed with args.
   *
   * In such case that the key does not exist in the container:
   *
   * Behaves like EmplaceHint except that the element is constructed as
   * value_type(std::piecewise_construct,
   *            std::forward_as_tuple(k),
   *            std::forward_as_tuple(std::forward<Args>(args)...))
   *
   * @tparam TArgs type of arguments to forward
   * @param hint iterator to the position before which the new element will be
   * inserted
   * @param key the key used both to look up and to insert if not found
   * @param args arguments to forward to the constructor of the element
   * @return The bool component is true if the insertion took place and false if
   * the assignment took place. The iterator component is pointing at the
   * element that was inserted or updated.
   */
  template <typename... TArgs>
  auto TryEmplaceHint(ConstIterator hint, const KeyType& key, TArgs&&... args) -> Iterator {
    return m_children.try_emplace(hint, key, std::forward<TArgs>(args)...);
  }

  /**
   * @brief If a key equivalent to key already exists in the container, does
   * nothing. Otherwise, inserts a new element into the container with key k and
   * value constructed with args.
   *
   * In such case that the key does not exist in the container:
   *
   * Behaves like EmplaceHint except that the element is constructed as
   * value_type(std::piecewise_construct,
   *            std::forward_as_tuple(std::move(k)),
   *            std::forward_as_tuple(std::forward<Args>(args)...))
   *
   * @tparam TArgs type of arguments to forward
   * @param hint iterator to the position before which the new element will be
   * inserted
   * @param key the key used both to look up and to insert if not found
   * @param args arguments to forward to the constructor of the element
   * @return The bool component is true if the insertion took place and false if
   * the assignment took place. The iterator component is pointing at the
   * element that was inserted or updated.
   */
  template <typename... TArgs>
  auto TryEmplaceHint(ConstIterator hint, KeyType&& key, TArgs&&... args) -> Iterator {
    return m_children.try_emplace(hint, key, std::forward<TArgs>(args)...);
  }

  // Note: I haven't exposed the ranged erase:
  // Iterator Erase(ConstIterator first, ConstIterator last);
  // because I don't like it.
  // Since unordered_map is *unordered* it is unpredictable what is within the
  // range. It is hard to see how this could be useful.

  /**
   * @brief Removes the element at pos
   * @param pos iterator to the element to remove
   * @return Iterator following the last removed element.
   */
  auto Erase(Iterator pos) -> Iterator { return m_children.erase(pos); }

  /**
   * @brief Removes the element at pos
   * @param pos iterator to the element to remove
   * @return Iterator following the last removed element.
   */
  auto Erase(ConstIterator pos) -> Iterator { return m_children.erase(pos); }

  /**
   * @brief Removes the element (if one exists) with the key equivalent to key.
   * @param key key value of the elements to remove
   * @return Number of elements removed (0 or 1).
   */
  auto Erase(const KeyType& key) -> SizeType { return m_children.erase(key); }

  /**
   * @brief Returns a reference to the value that is mapped to a key equivalent
   * to `key`, performing an insertion if such key does not already exist.
   * @param key the key of the element to find
   * @return A reference to the mapped value of the requested element.
   */
  auto operator[](const KeyType& key) -> MappedType& { return m_children[key]; }

  /**
   * @brief Returns a reference to the value that is mapped to a key equivalent
   * to `key`, performing an insertion if such key does not already exist.
   * @param key the key of the element to find
   * @return A reference to the mapped value of the requested element.
   */
  auto operator[](KeyType&& key) -> MappedType& { return m_children[std::move(key)]; }

  /**
   * @brief Check if there is a key equivalent to the provided key in this
   * ObjectNodeType
   * @param key key value of the node id to search for
   * @return true if there is such a node id, otherwise false
   */
  [[nodiscard]] auto Contains(const KeyType& key) const -> bool { return m_children.contains(key); }

  /**
   * @brief Returns the number of children in this ObjectNodeType
   * @return the number of children in this ObjectNodeType
   */
  [[nodiscard]] auto Size() const noexcept -> SizeType { return m_children.size(); }

  /**
   * @brief Check if this ObjectNodeType is empty
   * @return true if empty, otherwise false
   */
  [[nodiscard]] auto Empty() const noexcept -> bool { return m_children.empty(); }

  /**
   * @brief Find a node id with key equivalent to the provided key
   * @param key key value of the node id to search for
   * @return An iterator to the requested node id. If no such node id is found,
   * past-the-end (see end()) iterator is returned.
   */
  [[nodiscard]] auto Find(const KeyType& key) -> Iterator { return m_children.find(key); }

  /**
   * @brief Find a node id with key equivalent to the provided key
   * @param key key value of the node id to search for
   * @return An iterator to the requested node id. If no such node id is found,
   * past-the-end (see end()) iterator is returned.
   */
  [[nodiscard]] auto Find(const KeyType& key) const -> ConstIterator {
    return m_children.find(key);
  }

  /**
   * @brief Get an iterator to the first element of the underlying map.
   * @return Iterator to the first element.
   */
  [[nodiscard]] auto Begin() noexcept -> Iterator { return m_children.begin(); }

  /**
   * @brief Get an iterator to the first element of the underlying map.
   * @return Iterator to the first element.
   */
  [[nodiscard]] auto Begin() const noexcept -> ConstIterator { return m_children.begin(); }

  /**
   * @brief Get an iterator to the first element of the underlying map.
   * @return Iterator to the first element.
   */
  [[nodiscard]] auto CBegin() const noexcept -> ConstIterator { return m_children.cbegin(); }

  /**
   * @brief Get an iterator to the first element of the underlying map.
   * @return Iterator to the first element.
   */
  [[nodiscard]] auto begin() noexcept -> Iterator { return Begin(); }

  /**
   * @brief Get an iterator to the first element of the underlying map.
   * @return Iterator to the first element.
   */
  [[nodiscard]] auto begin() const noexcept -> ConstIterator { return Begin(); }

  /**
   * @brief Get an iterator to the first element of the underlying map.
   * @return Iterator to the first element.
   */
  [[nodiscard]] auto cbegin() const noexcept -> ConstIterator { return CBegin(); }

  /**
   * @brief Get an iterator to the element following the last element of the
   * unordered_map.
   * @return Iterator to the element following the last element.
   */
  [[nodiscard]] auto End() noexcept -> Iterator { return m_children.end(); }

  /**
   * @brief Get an iterator to the element following the last element of the
   * unordered_map.
   * @return Iterator to the element following the last element.
   */
  [[nodiscard]] auto End() const noexcept -> ConstIterator { return m_children.end(); }

  /**
   * @brief Get an iterator to the element following the last element of the
   * unordered_map.
   * @return Iterator to the element following the last element.
   */
  [[nodiscard]] auto CEnd() const noexcept -> ConstIterator { return m_children.cend(); }

  /**
   * @brief Get an iterator to the element following the last element of the
   * unordered_map.
   * @return Iterator to the element following the last element.
   */
  [[nodiscard]] auto end() noexcept -> Iterator { return End(); }

  /**
   * @brief Get an iterator to the element following the last element of the
   * unordered_map.
   * @return Iterator to the element following the last element.
   */
  [[nodiscard]] auto end() const noexcept -> ConstIterator { return End(); }

  /**
   * @brief Get an iterator to the element following the last element of the
   * unordered_map.
   * @return Iterator to the element following the last element.
   */
  [[nodiscard]] auto cend() const noexcept -> ConstIterator { return CEnd(); }

  /**
   * @brief Compares the contents of two ObjectNodeTypes.
   * @param other ObjectNodeType to compare against
   * @return true if the contents of the containers are equal, false otherwise.
   */
  [[nodiscard]] auto operator==(const ObjectNodeType& other) const -> bool = default;

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
        std::is_invocable_v<TFunc, decltype(std::declval<ObjectNodeType::UnsafeProxy>()),
                            ObjectNodeType&> &&
        !std::is_same_v<
            std::decay_t<std::invoke_result_t<
                TFunc, decltype(std::declval<ObjectNodeType::UnsafeProxy>()), ObjectNodeType&>>,
            ObjectNodeType::UnsafeProxy>)
  auto Unsafe(TFunc&& func)
      -> std::invoke_result_t<TFunc, decltype(std::declval<ObjectNodeType::UnsafeProxy>()),
                              ObjectNodeType&> {
    return std::invoke(std::forward<TFunc>(func), ObjectNodeType::UnsafeProxy{*this}, *this);
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
    requires(std::is_invocable_v<TFunc, decltype(std::declval<ObjectNodeType::UnsafeProxy>())> &&
             !std::is_same_v<std::decay_t<std::invoke_result_t<
                                 TFunc, decltype(std::declval<ObjectNodeType::UnsafeProxy>())>>,
                             ObjectNodeType::UnsafeProxy>)
  auto Unsafe(TFunc&& func)
      -> std::invoke_result_t<TFunc, decltype(std::declval<ObjectNodeType::UnsafeProxy>())> {
    return std::invoke(std::forward<TFunc>(func), ObjectNodeType::UnsafeProxy{*this});
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
    requires(std::is_invocable_v<TFunc, decltype(std::declval<ObjectNodeType::ConstUnsafeProxy>()),
                                 const ObjectNodeType&> &&
             !std::is_same_v<std::decay_t<std::invoke_result_t<
                                 TFunc, decltype(std::declval<ObjectNodeType::ConstUnsafeProxy>()),
                                 const ObjectNodeType&>>,
                             ObjectNodeType::ConstUnsafeProxy>)
  auto ConstUnsafe(TFunc&& func) const
      -> std::invoke_result_t<TFunc, decltype(std::declval<ObjectNodeType::ConstUnsafeProxy>()),
                              const ObjectNodeType&> {
    return std::invoke(std::forward<TFunc>(func), ObjectNodeType::ConstUnsafeProxy{*this}, *this);
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
        std::is_invocable_v<TFunc, decltype(std::declval<ObjectNodeType::ConstUnsafeProxy>())> &&
        !std::is_same_v<std::decay_t<std::invoke_result_t<
                            TFunc, decltype(std::declval<ObjectNodeType::ConstUnsafeProxy>())>>,
                        ObjectNodeType::ConstUnsafeProxy>)
  auto ConstUnsafe(TFunc&& func) const
      -> std::invoke_result_t<TFunc, decltype(std::declval<ObjectNodeType::ConstUnsafeProxy>())> {
    return std::invoke(std::forward<TFunc>(func), ObjectNodeType::ConstUnsafeProxy{*this});
  }

private:
  MapType m_children;
};

}  // namespace mguid

#endif  // DATATREE_OBJECT_NODE_TYPE_HPP
