/**
 * @brief Generic node type for opaque runtime-only map containers
 * @author Matthew Guidry (github: mguid65)
 * @date 2024-02-05
 */

#ifndef DATATREE_GENERIC_NODE_TYPE_HPP
#define DATATREE_GENERIC_NODE_TYPE_HPP

#include <string>
#include <unordered_map>

#include "data_tree/common/common.hpp"
#include "data_tree/error/error_type.hpp"

namespace mguid {

class TreeNode;

/**
 * @brief Opaque runtime-only map node used for special host-managed references
 *
 * GenericNodeType is designed for storing special types like functions, file handles,
 * and other opaque references that are managed by the host system. Unlike ObjectNodeType
 * which is used for user-facing associative arrays, GenericNodeType is intended for
 * internal system use to track and manage special object references.
 *
 * The key difference from ObjectNodeType:
 * - Generic: For special internal types (functions, handles, etc.)
 * - Object: For user-facing associative arrays/objects
 *
 * Both use the same underlying unordered_map<string, TreeNode> structure but serve
 * different purposes in the data model.
 */
class GenericNodeType {
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
   * @brief Proxy class that provides access to unsafe GenericNodeType functionality
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
     * @brief Get a reference to the held GenericNodeType
     * @return a reference to the held GenericNodeType
     */
    [[nodiscard]] auto Safe() -> GenericNodeType& { return m_node_ref; }

    /**
     * @brief Get a reference to the held GenericNodeType
     * @return a reference to the held GenericNodeType
     */
    [[nodiscard]] auto Safe() const -> const GenericNodeType& { return m_node_ref; }

  private:
    explicit UnsafeProxyType(std::conditional_t<TConst, const GenericNodeType&, GenericNodeType&> ref)
        : m_node_ref{ref} {}

    friend GenericNodeType;
    std::conditional_t<TConst, const GenericNodeType&, GenericNodeType&> m_node_ref;
  };

public:
  using ConstUnsafeProxy = const UnsafeProxyType<true>;
  using UnsafeProxy = UnsafeProxyType<false>;

  /**
   * @brief Default construct a GenericNodeType
   */
  GenericNodeType() = default;

  /**
   * @brief Explicit defaults for copy/move construction/assignment
   */
  GenericNodeType(const GenericNodeType&) = default;
  GenericNodeType(GenericNodeType&&) noexcept = default;
  GenericNodeType& operator=(const GenericNodeType&) = default;
  GenericNodeType& operator=(GenericNodeType&&) noexcept = default;

  /**
   * @brief Construct a GenericNodeType from an initializer list
   * @param init_list an initializer list of key-value pairs
   */
  GenericNodeType(std::initializer_list<ValueType> init_list) : m_children{init_list} {}

  /**
   * @brief Assign an initializer list of ValueType to this
   * @param init_list an initializer list of ValueType
   * @return reference to this
   */
  GenericNodeType& operator=(std::initializer_list<ValueType> init_list) {
    m_children = init_list;
    return *this;
  }

  /**
   * @brief Construct a GenericNodeType from an existing map
   * @param init_mapping an initial mapping to initialize this with
   */
  explicit GenericNodeType(MapType init_mapping) : m_children{std::move(init_mapping)} {}

  /**
   * @brief Try to get the value associated with the specified key
   * @param key the key of the value to find
   * @return the associated TreeNode if the key exists; otherwise KeyError
   */
  [[nodiscard]] auto TryGet(const KeyType& key) const -> ConstExpectedRType {
    if (auto iter = m_children.find(key); iter != m_children.end()) {
      return iter->second;
    }
    return make_unexpected(Error{.category = Error::Category::KeyError});
  }

  /**
   * @brief Try to get the value associated with the specified key
   * @param key the key of the value to find
   * @return the associated TreeNode if the key exists; otherwise KeyError
   */
  [[nodiscard]] auto TryGet(const KeyType& key) -> ExpectedRType {
    if (auto iter = m_children.find(key); iter != m_children.end()) {
      return iter->second;
    }
    return make_unexpected(Error{.category = Error::Category::KeyError});
  }

  /**
   * @brief Returns a reference to the value mapped to a key, inserting a default if needed
   * @param key the key of the element to find
   * @return A reference to the mapped value of the requested element
   */
  auto operator[](const KeyType& key) -> MappedType& { return m_children[key]; }

  /**
   * @brief Returns a reference to the value mapped to a key, inserting a default if needed
   * @param key the key of the element to find (will be moved)
   * @return A reference to the mapped value of the requested element
   */
  auto operator[](KeyType&& key) -> MappedType& { return m_children[std::move(key)]; }

  /**
   * @brief Check if there is a key equivalent to the provided key in this GenericNodeType
   * @param key key value to search for
   * @return true if there is such a key, otherwise false
   */
  [[nodiscard]] auto Contains(const KeyType& key) const -> bool {
    return m_children.contains(key);
  }

  /**
   * @brief Returns the number of elements in this GenericNodeType
   * @return the number of elements
   */
  [[nodiscard]] auto Size() const noexcept -> SizeType { return m_children.size(); }

  /**
   * @brief Check if this GenericNodeType is empty
   * @return true if empty, otherwise false
   */
  [[nodiscard]] auto Empty() const noexcept -> bool { return m_children.empty(); }

  /**
   * @brief Find a value with key equivalent to the provided key
   * @param key key value to search for
   * @return An iterator to the found value. If not found, past-the-end iterator.
   */
  [[nodiscard]] auto Find(const KeyType& key) -> Iterator { return m_children.find(key); }

  /**
   * @brief Find a value with key equivalent to the provided key
   * @param key key value to search for
   * @return An iterator to the found value. If not found, past-the-end iterator.
   */
  [[nodiscard]] auto Find(const KeyType& key) const -> ConstIterator {
    return m_children.find(key);
  }

  /**
   * @brief Erase all elements from this GenericNodeType
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
   * @param hint iterator to position before which insertion will occur
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
   * @param hint iterator to position before which insertion will occur
   * @param value value to insert
   * @return An iterator to the inserted value, or to the value that
   * prevented the insertion.
   */
  auto InsertHint(ConstIterator hint, ValueType&& value) -> Iterator {
    return m_children.insert(hint, value);
  }

  /**
   * @brief Inserts value, using hint as a non-binding suggestion
   * @tparam TConvertibleToValueType type of value to insert
   * @param hint iterator to position before which insertion will occur
   * @param value value to insert
   * @return An iterator to the inserted value, or to the value that
   * prevented the insertion.
   */
  template <typename TConvertibleToValueType>
  auto InsertHint(ConstIterator hint, TConvertibleToValueType&& value) -> Iterator {
    return m_children.insert(hint, value);
  }

  /**
   * @brief Inserts elements from initializer list
   * @param init_list initializer list to insert the values from
   */
  void Insert(std::initializer_list<ValueType> init_list) { m_children.insert(init_list); }

  /**
   * @brief Insert a new element or assign to an existing element if found
   * @tparam TValue type of value to insert
   * @param key the key used both to look up and to insert if not found
   * @param obj the value to insert or assign
   * @return The bool component is true if insertion took place, false if
   * assignment took place. The iterator component points at the element.
   */
  template <typename TValue>
  auto InsertOrAssign(const KeyType& key, TValue&& obj) -> std::pair<Iterator, bool> {
    return m_children.insert_or_assign(key, std::forward<TValue>(obj));
  }

  /**
   * @brief Insert a new element or assign to an existing element if found
   * @tparam TValue type of value to insert
   * @param key the key used both to look up and to insert if not found
   * @param obj the value to insert or assign
   * @return The bool component is true if insertion took place, false if
   * assignment took place. The iterator component points at the element.
   */
  template <typename TValue>
  auto InsertOrAssign(KeyType&& key, TValue&& obj) -> std::pair<Iterator, bool> {
    return m_children.insert_or_assign(key, std::forward<TValue>(obj));
  }

  /**
   * @brief Insert a new element or assign to an existing element if found
   * @tparam TValue type of value to insert
   * @param hint iterator hint
   * @param key the key used both to look up and to insert if not found
   * @param obj the value to insert or assign
   * @return An iterator to the element that was inserted or updated
   */
  template <typename TValue>
  auto InsertOrAssignHint(ConstIterator hint, const KeyType& key, TValue&& obj) -> Iterator {
    return m_children.insert_or_assign(hint, key, std::forward<TValue>(obj));
  }

  /**
   * @brief Insert a new element or assign to an existing element if found
   * @tparam TValue type of value to insert
   * @param hint iterator hint
   * @param key the key used both to look up and to insert if not found
   * @param obj the value to insert or assign
   * @return An iterator to the element that was inserted or updated
   */
  template <typename TValue>
  auto InsertOrAssignHint(ConstIterator hint, KeyType&& key, TValue&& obj) -> Iterator {
    return m_children.insert_or_assign(hint, key, std::forward<TValue>(obj));
  }

  /**
   * @brief Inserts a new element into the container constructed in-place
   * @tparam TArgs type of arguments to forward
   * @param args arguments to forward to the constructor of the element
   * @return A pair consisting of an iterator to the inserted element and a bool
   * denoting whether the insertion took place
   */
  template <typename... TArgs>
  auto Emplace(TArgs&&... args) -> std::pair<Iterator, bool> {
    return m_children.emplace(std::forward<TArgs>(args)...);
  }

  /**
   * @brief Inserts a new element into the container, using hint as a suggestion
   * @tparam TArgs type of arguments to forward
   * @param hint iterator, used as a suggestion as to where to insert
   * @param args arguments to forward to the constructor of the element
   * @return An iterator to the newly inserted element
   */
  template <typename... TArgs>
  auto EmplaceHint(ConstIterator hint, TArgs&&... args) -> Iterator {
    return m_children.emplace_hint(hint, std::forward<TArgs>(args)...);
  }

  /**
   * @brief If a key equivalent to key already exists, does nothing.
   * Otherwise, inserts a new element with key and value constructed with args.
   * @tparam TArgs type of arguments to forward
   * @param key the key used both to look up and to insert if not found
   * @param args arguments to forward to the constructor of the element
   * @return The bool component is true if insertion took place, false if
   * assignment took place. The iterator component points at the element.
   */
  template <typename... TArgs>
  auto TryEmplace(const KeyType& key, TArgs&&... args) -> std::pair<Iterator, bool> {
    return m_children.try_emplace(key, std::forward<TArgs>(args)...);
  }

  /**
   * @brief If a key equivalent to key already exists, does nothing.
   * Otherwise, inserts a new element with key and value constructed with args.
   * @tparam TArgs type of arguments to forward
   * @param key the key used both to look up and to insert if not found
   * @param args arguments to forward to the constructor of the element
   * @return The bool component is true if insertion took place, false if
   * assignment took place. The iterator component points at the element.
   */
  template <typename... TArgs>
  auto TryEmplace(KeyType&& key, TArgs&&... args) -> std::pair<Iterator, bool> {
    return m_children.try_emplace(key, std::forward<TArgs>(args)...);
  }

  /**
   * @brief If a key equivalent to key already exists, does nothing.
   * Otherwise, inserts a new element with key and value constructed with args.
   * @tparam TArgs type of arguments to forward
   * @param hint iterator hint
   * @param key the key used both to look up and to insert if not found
   * @param args arguments to forward to the constructor of the element
   * @return An iterator to the element that was inserted or already existed
   */
  template <typename... TArgs>
  auto TryEmplaceHint(ConstIterator hint, const KeyType& key, TArgs&&... args) -> Iterator {
    return m_children.try_emplace(hint, key, std::forward<TArgs>(args)...);
  }

  /**
   * @brief If a key equivalent to key already exists, does nothing.
   * Otherwise, inserts a new element with key and value constructed with args.
   * @tparam TArgs type of arguments to forward
   * @param hint iterator hint
   * @param key the key used both to look up and to insert if not found
   * @param args arguments to forward to the constructor of the element
   * @return An iterator to the element that was inserted or already existed
   */
  template <typename... TArgs>
  auto TryEmplaceHint(ConstIterator hint, KeyType&& key, TArgs&&... args) -> Iterator {
    return m_children.try_emplace(hint, key, std::forward<TArgs>(args)...);
  }

  /**
   * @brief Removes the element at pos
   * @param pos iterator to the element to remove
   * @return Iterator following the last removed element
   */
  auto Erase(Iterator pos) -> Iterator { return m_children.erase(pos); }

  /**
   * @brief Removes the element at pos
   * @param pos iterator to the element to remove
   * @return Iterator following the last removed element
   */
  auto Erase(ConstIterator pos) -> Iterator { return m_children.erase(pos); }

  /**
   * @brief Removes the element with key equivalent to the provided key
   * @param key key value of the element to remove
   * @return Number of elements removed (0 or 1)
   */
  auto Erase(const KeyType& key) -> SizeType { return m_children.erase(key); }

  /**
   * @brief Get an iterator to the first element of the underlying map
   * @return Iterator to the first element
   */
  [[nodiscard]] auto Begin() noexcept -> Iterator { return m_children.begin(); }

  /**
   * @brief Get an iterator to the first element of the underlying map
   * @return Iterator to the first element
   */
  [[nodiscard]] auto Begin() const noexcept -> ConstIterator { return m_children.begin(); }

  /**
   * @brief Get a const iterator to the first element of the underlying map
   * @return Const iterator to the first element
   */
  [[nodiscard]] auto CBegin() const noexcept -> ConstIterator { return m_children.cbegin(); }

  /**
   * @brief Get an iterator to the element following the last element of the map
   * @return Iterator to the element following the last element
   */
  [[nodiscard]] auto End() noexcept -> Iterator { return m_children.end(); }

  /**
   * @brief Get an iterator to the element following the last element of the map
   * @return Iterator to the element following the last element
   */
  [[nodiscard]] auto End() const noexcept -> ConstIterator { return m_children.end(); }

  /**
   * @brief Get a const iterator to the element following the last element
   * @return Const iterator to the element following the last element
   */
  [[nodiscard]] auto CEnd() const noexcept -> ConstIterator { return m_children.cend(); }

  /**
   * @brief Get an iterator to the first element (STL-style)
   * @return Iterator to the first element
   */
  [[nodiscard]] auto begin() noexcept -> Iterator { return Begin(); }

  /**
   * @brief Get an iterator to the first element (STL-style)
   * @return Iterator to the first element
   */
  [[nodiscard]] auto begin() const noexcept -> ConstIterator { return Begin(); }

  /**
   * @brief Get a const iterator to the first element (STL-style)
   * @return Const iterator to the first element
   */
  [[nodiscard]] auto cbegin() const noexcept -> ConstIterator { return CBegin(); }

  /**
   * @brief Get an iterator to the element following the last element (STL-style)
   * @return Iterator to the element following the last element
   */
  [[nodiscard]] auto end() noexcept -> Iterator { return End(); }

  /**
   * @brief Get an iterator to the element following the last element (STL-style)
   * @return Iterator to the element following the last element
   */
  [[nodiscard]] auto end() const noexcept -> ConstIterator { return End(); }

  /**
   * @brief Get a const iterator to the element following the last element (STL-style)
   * @return Const iterator to the element following the last element
   */
  [[nodiscard]] auto cend() const noexcept -> ConstIterator { return CEnd(); }

  /**
   * @brief Compares the contents of two GenericNodeTypes
   * @param other GenericNodeType to compare against
   * @return true if the contents are equal, false otherwise
   */
  [[nodiscard]] auto operator==(const GenericNodeType& other) const -> bool = default;

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
        std::is_invocable_v<TFunc, decltype(std::declval<GenericNodeType::UnsafeProxy>()),
                            GenericNodeType&> &&
        !std::is_same_v<
            std::decay_t<std::invoke_result_t<
                TFunc, decltype(std::declval<GenericNodeType::UnsafeProxy>()), GenericNodeType&>>,
            GenericNodeType::UnsafeProxy>)
  auto Unsafe(TFunc&& func)
      -> std::invoke_result_t<TFunc, decltype(std::declval<GenericNodeType::UnsafeProxy>()),
                              GenericNodeType&> {
    return std::invoke(std::forward<TFunc>(func), GenericNodeType::UnsafeProxy{*this}, *this);
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
    requires(std::is_invocable_v<TFunc, decltype(std::declval<GenericNodeType::UnsafeProxy>())> &&
             !std::is_same_v<std::decay_t<std::invoke_result_t<
                                 TFunc, decltype(std::declval<GenericNodeType::UnsafeProxy>())>>,
                             GenericNodeType::UnsafeProxy>)
  auto Unsafe(TFunc&& func)
      -> std::invoke_result_t<TFunc, decltype(std::declval<GenericNodeType::UnsafeProxy>())> {
    return std::invoke(std::forward<TFunc>(func), GenericNodeType::UnsafeProxy{*this});
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
    requires(std::is_invocable_v<TFunc, decltype(std::declval<GenericNodeType::ConstUnsafeProxy>()),
                                 const GenericNodeType&> &&
             !std::is_same_v<std::decay_t<std::invoke_result_t<
                                 TFunc, decltype(std::declval<GenericNodeType::ConstUnsafeProxy>()),
                                 const GenericNodeType&>>,
                             GenericNodeType::ConstUnsafeProxy>)
  auto ConstUnsafe(TFunc&& func) const
      -> std::invoke_result_t<TFunc, decltype(std::declval<GenericNodeType::ConstUnsafeProxy>()),
                              const GenericNodeType&> {
    return std::invoke(std::forward<TFunc>(func), GenericNodeType::ConstUnsafeProxy{*this}, *this);
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
        std::is_invocable_v<TFunc, decltype(std::declval<GenericNodeType::ConstUnsafeProxy>())> &&
        !std::is_same_v<std::decay_t<std::invoke_result_t<
                            TFunc, decltype(std::declval<GenericNodeType::ConstUnsafeProxy>())>>,
                        GenericNodeType::ConstUnsafeProxy>)
  auto ConstUnsafe(TFunc&& func) const
      -> std::invoke_result_t<TFunc, decltype(std::declval<GenericNodeType::ConstUnsafeProxy>())> {
    return std::invoke(std::forward<TFunc>(func), GenericNodeType::ConstUnsafeProxy{*this});
  }

private:
  MapType m_children;
};

}  // namespace mguid

#endif  // DATATREE_GENERIC_NODE_TYPE_HPP










