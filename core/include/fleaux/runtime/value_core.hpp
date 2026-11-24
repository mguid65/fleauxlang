#pragma once

// The runtime's value type.
//
// A Value is 16 bytes: a kind and a payload. Null, Bool, Int64, UInt64, Float64, and RandomGenerator values live in
// the payload and never allocate. Every other kind points to an immutable heap object that counts its references
// atomically, so values can be copied freely, also between threads, and are freed with their last copy. A value that
// is about to change a tuple, dict, or string it holds alone changes it in place; one that shares it copies it first.

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "fleaux/runtime/heap.hpp"

namespace fleaux::runtime {

using Int = std::int64_t;
using UInt = std::uint64_t;
using Float = double;
using Bool = bool;
using String = std::string;

enum class ValueKind : std::uint8_t {
  // In the payload.
  kNull,
  kBool,
  kInt,
  kUInt,
  kFloat,
  kRandomGenerator,
  // On the heap.
  kString,
  kTuple,
  kDict,
  kFunction,
  kFileHandle,
  kTask,
};

[[nodiscard]] constexpr auto is_heap_kind(const ValueKind kind) noexcept -> bool { return kind >= ValueKind::kString; }

// The reference count every heap object starts with: the object belongs to the Value that creates it. Heap objects
// come from the runtime's allocator (heap.hpp); a function value's virtual destructor hands back its own size.
class HeapObject {
public:
  [[nodiscard]] static auto operator new(const std::size_t bytes) -> void* { return heap::allocate(bytes); }
  static void operator delete(void* block, const std::size_t bytes) noexcept { heap::deallocate(block, bytes); }

  HeapObject() = default;
  HeapObject(const HeapObject&) = delete;
  auto operator=(const HeapObject&) -> HeapObject& = delete;
  HeapObject(HeapObject&&) = delete;
  auto operator=(HeapObject&&) -> HeapObject& = delete;

  void retain() const noexcept { references_.fetch_add(1, std::memory_order_relaxed); }

  // Drops one reference; true when it was the last one.
  [[nodiscard]] auto release() const noexcept -> bool {
    return references_.fetch_sub(1, std::memory_order_acq_rel) == 1;
  }

  // True when exactly one Value refers to the object, so that Value may change it in place.
  [[nodiscard]] auto unique() const noexcept -> bool { return references_.load(std::memory_order_acquire) == 1; }

protected:
  ~HeapObject() = default;

private:
  mutable std::atomic<std::uint32_t> references_{1};
};

class Value;
class Dict;
class Callable;
class FileHandleObject;
class TaskState;

// A tuple's items, read where the tuple keeps them. The view is valid while a value holds the tuple.
using TupleView = std::span<const Value>;

class Value {
public:
  Value() noexcept = default;

  Value(const Value& other) noexcept : kind_{other.kind_}, payload_{other.payload_} {
    if (is_heap_kind(kind_)) {
      payload_.object->retain();
    }
  }

  Value(Value&& other) noexcept : kind_{other.kind_}, payload_{other.payload_} {
    other.kind_ = ValueKind::kNull;
    other.payload_.unsigned_integer = 0;
  }

  // The assignments take the new contents first and release the old object last, so that assigning a value that the
  // old one holds (a tuple's own item, say) is safe. They work on the kind and pointer directly rather than through a
  // temporary Value, which would have to live in memory for the release; the VM assigns on most instructions.
  auto operator=(const Value& other) noexcept -> Value& {
    if (is_heap_kind(other.kind_)) {
      other.payload_.object->retain();
    }
    const ValueKind old_kind = kind_;
    HeapObject* const old_object = payload_.object;
    kind_ = other.kind_;
    payload_ = other.payload_;
    if (is_heap_kind(old_kind)) {
      release(old_kind, old_object);
    }
    return *this;
  }

  auto operator=(Value&& other) noexcept -> Value& {
    if (this == &other) {
      return *this;
    }
    const ValueKind old_kind = kind_;
    HeapObject* const old_object = payload_.object;
    kind_ = other.kind_;
    payload_ = other.payload_;
    other.kind_ = ValueKind::kNull;
    other.payload_.unsigned_integer = 0;
    if (is_heap_kind(old_kind)) {
      release(old_kind, old_object);
    }
    return *this;
  }

  ~Value() {
    if (is_heap_kind(kind_)) {
      release(kind_, payload_.object);
    }
  }

  void swap(Value& other) noexcept {
    std::swap(kind_, other.kind_);
    std::swap(payload_, other.payload_);
  }

  [[nodiscard]] static auto from_bool(const Bool value) noexcept -> Value {
    Value result;
    result.kind_ = ValueKind::kBool;
    result.payload_.boolean = value;
    return result;
  }

  [[nodiscard]] static auto from_int(const Int value) noexcept -> Value {
    Value result;
    result.kind_ = ValueKind::kInt;
    result.payload_.integer = value;
    return result;
  }

  [[nodiscard]] static auto from_uint(const UInt value) noexcept -> Value {
    Value result;
    result.kind_ = ValueKind::kUInt;
    result.payload_.unsigned_integer = value;
    return result;
  }

  [[nodiscard]] static auto from_float(const Float value) noexcept -> Value {
    Value result;
    result.kind_ = ValueKind::kFloat;
    result.payload_.floating = value;
    return result;
  }

  [[nodiscard]] static auto from_random_state(const UInt state) noexcept -> Value {
    Value result;
    result.kind_ = ValueKind::kRandomGenerator;
    result.payload_.unsigned_integer = state;
    return result;
  }

  [[nodiscard]] static auto from_string(String text) -> Value;
  [[nodiscard]] static auto from_dict(Dict dict) -> Value;

  // A value of a heap kind that takes over the one reference a newly created object starts with.
  [[nodiscard]] static auto adopt(const ValueKind kind, HeapObject* object) noexcept -> Value {
    Value result;
    result.kind_ = kind;
    result.payload_.object = object;
    return result;
  }

  [[nodiscard]] auto kind() const noexcept -> ValueKind { return kind_; }
  [[nodiscard]] auto is_null() const noexcept -> bool { return kind_ == ValueKind::kNull; }
  [[nodiscard]] auto is_bool() const noexcept -> bool { return kind_ == ValueKind::kBool; }
  [[nodiscard]] auto is_int() const noexcept -> bool { return kind_ == ValueKind::kInt; }
  [[nodiscard]] auto is_uint() const noexcept -> bool { return kind_ == ValueKind::kUInt; }
  [[nodiscard]] auto is_float() const noexcept -> bool { return kind_ == ValueKind::kFloat; }
  [[nodiscard]] auto is_number() const noexcept -> bool {
    return kind_ == ValueKind::kInt || kind_ == ValueKind::kUInt || kind_ == ValueKind::kFloat;
  }
  [[nodiscard]] auto is_random_generator() const noexcept -> bool { return kind_ == ValueKind::kRandomGenerator; }
  [[nodiscard]] auto is_string() const noexcept -> bool { return kind_ == ValueKind::kString; }
  [[nodiscard]] auto is_tuple() const noexcept -> bool { return kind_ == ValueKind::kTuple; }
  [[nodiscard]] auto is_dict() const noexcept -> bool { return kind_ == ValueKind::kDict; }
  [[nodiscard]] auto is_function() const noexcept -> bool { return kind_ == ValueKind::kFunction; }
  [[nodiscard]] auto is_file_handle() const noexcept -> bool { return kind_ == ValueKind::kFileHandle; }
  [[nodiscard]] auto is_task() const noexcept -> bool { return kind_ == ValueKind::kTask; }

  // The accessors below assume the kind: check it first, or rely on the type checker. The checked helpers in
  // value.hpp (as_bool, as_tuple, ...) throw on a wrong kind instead.
  [[nodiscard]] auto bool_value() const noexcept -> Bool { return payload_.boolean; }
  [[nodiscard]] auto int_value() const noexcept -> Int { return payload_.integer; }
  [[nodiscard]] auto uint_value() const noexcept -> UInt { return payload_.unsigned_integer; }
  [[nodiscard]] auto float_value() const noexcept -> Float { return payload_.floating; }
  [[nodiscard]] auto random_state() const noexcept -> UInt { return payload_.unsigned_integer; }
  [[nodiscard]] auto string_value() const noexcept -> const String&;
  [[nodiscard]] auto tuple() const noexcept -> TupleView;
  [[nodiscard]] auto dict() const noexcept -> const Dict&;
  [[nodiscard]] auto callable() const noexcept -> const Callable&;
  [[nodiscard]] auto file_handle() const noexcept -> FileHandleObject&;
  [[nodiscard]] auto task() const noexcept -> TaskState&;

  // The items of a tuple, dict, or string this value can change: shared data is copied first.
  [[nodiscard]] auto mutable_tuple() -> std::span<Value>;
  [[nodiscard]] auto mutable_dict() -> Dict&;
  [[nodiscard]] auto mutable_string() -> String&;

  // The heap object of a heap kind, for identity comparisons.
  [[nodiscard]] auto object() const noexcept -> const HeapObject* {
    return is_heap_kind(kind_) ? payload_.object : nullptr;
  }

private:
  // A builder continues a tuple this value alone refers to by taking over its reference (TupleBuilder::reopen).
  friend class TupleBuilder;

  // Drops a reference to `object`, a heap object of `kind`, and destroys it when that was the last one.
  static void release(const ValueKind kind, HeapObject* object) noexcept {
    if (object->release()) {
      destroy_object(kind, object);
    }
  }
  static void destroy_object(ValueKind kind, HeapObject* object) noexcept;
  // The content of this value's heap object, copied into an object of its own first when other values share it.
  template <typename Object, typename Content>
  [[nodiscard]] auto detached(Content Object::* content) -> Content&;

  ValueKind kind_{ValueKind::kNull};
  union Payload {
    Bool boolean;
    Int integer;
    UInt unsigned_integer;
    Float floating;
    HeapObject* object;
  } payload_{.unsigned_integer = 0};
};

static_assert(sizeof(Value) == 16U);

// Equality is by kind and contents: numbers of different kinds are never equal, NaN equals nothing, tuples compare
// item by item, and dicts compare their entries regardless of order. Functions, handles, and tasks are equal only to
// themselves.
[[nodiscard]] auto operator==(const Value& lhs, const Value& rhs) noexcept -> bool;

// Dict keys hash and compare by kind and contents. Floats compare by their bits here, so a NaN key finds itself.
struct DictKeyHash {
  [[nodiscard]] auto operator()(const Value& key) const noexcept -> std::size_t;
};

struct DictKeyEqual {
  [[nodiscard]] auto operator()(const Value& lhs, const Value& rhs) const noexcept -> bool;
};

// A dict's entries, in the order their keys were first set. A dict of up to kLinearEntries entries finds a key by
// comparing it with each entry's; a bigger one keeps a hash index from key to position. Most dicts are small, and an
// index costs a bucket array and a node per key, each its own allocation.
class Dict {
public:
  using Entry = std::pair<Value, Value>;
  using Entries = std::vector<Entry, heap::Allocator<Entry>>;

  static constexpr std::size_t kLinearEntries = 8U;

  [[nodiscard]] auto size() const noexcept -> std::size_t { return entries_.size(); }
  [[nodiscard]] auto empty() const noexcept -> bool { return entries_.empty(); }
  [[nodiscard]] auto entries() const noexcept -> const Entries& { return entries_; }
  [[nodiscard]] auto begin() const noexcept { return entries_.begin(); }
  [[nodiscard]] auto end() const noexcept { return entries_.end(); }

  void reserve(const std::size_t count) {
    entries_.reserve(count);
    if (count > kLinearEntries) {
      index_.reserve(count);
    }
  }

  // The value at `key`, or nullptr when the dict has no such key.
  [[nodiscard]] auto find(const Value& key) const -> const Value* {
    const std::size_t position = position_of(key);
    return position == npos ? nullptr : &entries_[position].second;
  }

  [[nodiscard]] auto contains(const Value& key) const -> bool { return position_of(key) != npos; }

  // Sets the value at `key`. A key already present keeps its place.
  void set(Value key, Value value) {
    if (const std::size_t position = position_of(key); position != npos) {
      entries_[position].second = std::move(value);
      return;
    }
    if (entries_.capacity() == 0U) {
      entries_.reserve(4U);
    }
    if (indexed()) {
      index_.emplace(key, entries_.size());
    }
    entries_.emplace_back(std::move(key), std::move(value));
    if (!indexed() && entries_.size() > kLinearEntries) {
      rebuild_index();
    }
  }

  // Removes `key`; false when the dict has no such key.
  auto erase(const Value& key) -> bool {
    const std::size_t position = position_of(key);
    if (position == npos) {
      return false;
    }
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(position));
    if (entries_.size() <= kLinearEntries) {
      index_ = {};
    } else {
      rebuild_index();
    }
    return true;
  }

private:
  static constexpr std::size_t npos = static_cast<std::size_t>(-1);

  [[nodiscard]] auto indexed() const noexcept -> bool { return !index_.empty(); }

  [[nodiscard]] auto position_of(const Value& key) const -> std::size_t {
    if (indexed()) {
      const auto found = index_.find(key);
      return found == index_.end() ? npos : found->second;
    }
    const DictKeyEqual equal;
    for (std::size_t position = 0; position < entries_.size(); ++position) {
      if (equal(entries_[position].first, key)) {
        return position;
      }
    }
    return npos;
  }

  void rebuild_index() {
    index_.clear();
    index_.reserve(entries_.size());
    for (std::size_t position = 0; position < entries_.size(); ++position) {
      index_.emplace(entries_[position].first, position);
    }
  }

  Entries entries_;
  std::unordered_map<Value, std::size_t, DictKeyHash, DictKeyEqual, heap::Allocator<std::pair<const Value, std::size_t>>>
      index_;
};

class StringObject final : public HeapObject {
public:
  explicit StringObject(String content) : text(std::move(content)) {}
  String text;
};

// A tuple: a header (the reference count, the size, and the capacity) and the items in one heap block, which holds the
// items right after the header. A TupleBuilder makes the block and constructs the items; the size counts the items
// constructed so far, and the capacity the items the block has room for.
class TupleObject final : public HeapObject {
public:
  TupleObject(const TupleObject&) = delete;
  auto operator=(const TupleObject&) -> TupleObject& = delete;
  TupleObject(TupleObject&&) = delete;
  auto operator=(TupleObject&&) -> TupleObject& = delete;

  // The most items a tuple holds: the size and capacity are 32-bit, and the block's size must not overflow.
  [[nodiscard]] static constexpr auto max_items() noexcept -> std::size_t;

  // A block with room for `capacity` items, holding none yet.
  [[nodiscard]] static auto allocate(std::size_t capacity) -> TupleObject*;

  // Destroys the items and frees the block.
  static void destroy(TupleObject* tuple) noexcept;

  [[nodiscard]] auto size() const noexcept -> std::size_t { return size_; }
  [[nodiscard]] auto capacity() const noexcept -> std::size_t { return capacity_; }
  [[nodiscard]] auto items() noexcept -> Value*;
  [[nodiscard]] auto items() const noexcept -> const Value*;

private:
  friend class TupleBuilder;

  TupleObject() = default;
  ~TupleObject() = default;

  std::uint32_t size_{0};
  std::uint32_t capacity_{0};
};

// Where a tuple's items start in its block: after the header, aligned for a Value.
inline constexpr std::size_t kTupleItemsOffset = (sizeof(TupleObject) + alignof(Value) - 1U) / alignof(Value) * alignof(Value);
static_assert(kTupleItemsOffset == 16U, "a tuple's header is 16 bytes");

constexpr auto TupleObject::max_items() noexcept -> std::size_t {
  return std::min<std::size_t>(std::numeric_limits<std::uint32_t>::max(),
                               (std::numeric_limits<std::size_t>::max() - kTupleItemsOffset) / sizeof(Value));
}

// The bytes of a tuple's block with room for `capacity` items.
[[nodiscard]] constexpr auto tuple_block_bytes(const std::size_t capacity) noexcept -> std::size_t {
  return kTupleItemsOffset + capacity * sizeof(Value);
}

inline auto TupleObject::allocate(const std::size_t capacity) -> TupleObject* {
  if (capacity > max_items()) {
    throw std::bad_array_new_length();
  }
  void* block = heap::allocate(tuple_block_bytes(capacity));
  // The global placement form: HeapObject's operator new hides it.
  auto* tuple = ::new (block) TupleObject;
  tuple->capacity_ = static_cast<std::uint32_t>(capacity);
  return tuple;
}

inline void TupleObject::destroy(TupleObject* tuple) noexcept {
  const std::size_t bytes = tuple_block_bytes(tuple->capacity_);
  std::destroy_n(tuple->items(), tuple->size_);
  tuple->~TupleObject();
  heap::deallocate(tuple, bytes);
}

inline auto TupleObject::items() noexcept -> Value* {
  return reinterpret_cast<Value*>(reinterpret_cast<std::byte*>(this) + kTupleItemsOffset);
}

inline auto TupleObject::items() const noexcept -> const Value* {
  return reinterpret_cast<const Value*>(reinterpret_cast<const std::byte*>(this) + kTupleItemsOffset);
}

// The empty tuple (defined after TupleBuilder). Every empty tuple is this one value.
[[nodiscard]] auto empty_tuple() -> Value;

// Builds a tuple in the block it keeps: the items are pushed, inserted, or erased, then finish() makes the tuple value.
// The block grows by doubling when it lacks room.
class TupleBuilder {
public:
  explicit TupleBuilder(const std::size_t capacity) : tuple_(TupleObject::allocate(capacity)) {}

  // Continues `tuple`, with room for at least `capacity` items. A tuple that `tuple` alone refers to is changed in its
  // own block, which moves to a bigger one when it lacks the room; a shared one is copied into a new block first.
  [[nodiscard]] static auto reopen(Value tuple, std::size_t capacity) -> TupleBuilder;

  ~TupleBuilder() {
    if (tuple_ != nullptr) {
      TupleObject::destroy(tuple_);
    }
  }

  TupleBuilder(const TupleBuilder&) = delete;
  auto operator=(const TupleBuilder&) -> TupleBuilder& = delete;
  TupleBuilder(TupleBuilder&& other) noexcept : tuple_(std::exchange(other.tuple_, nullptr)) {}
  auto operator=(TupleBuilder&&) -> TupleBuilder& = delete;

  void push(const Value& item) {
    if (tuple_->size_ == tuple_->capacity_) {
      push_growing(Value{item});
      return;
    }
    std::construct_at(tuple_->items() + tuple_->size_, item);
    ++tuple_->size_;
  }

  void push(Value&& item) {
    if (tuple_->size_ == tuple_->capacity_) {
      push_growing(std::move(item));
      return;
    }
    std::construct_at(tuple_->items() + tuple_->size_, std::move(item));
    ++tuple_->size_;
  }

  // Inserts `item` before the item at `index` (at most size()), moving that item and the ones after it up one.
  void insert(const std::size_t index, Value item) {
    reserve(size() + 1U);
    Value* items = tuple_->items();
    const std::size_t count = tuple_->size_;
    if (index == count) {
      std::construct_at(items + count, std::move(item));
    } else {
      std::construct_at(items + count, std::move(items[count - 1U]));
      std::move_backward(items + index, items + count - 1U, items + count);
      items[index] = std::move(item);
    }
    ++tuple_->size_;
  }

  // Removes the items from `first` up to `last` (first <= last <= size()), moving the ones after them down.
  void erase(const std::size_t first, const std::size_t last) {
    if (first == last) {
      return;
    }
    Value* items = tuple_->items();
    const std::size_t count = tuple_->size_;
    std::move(items + last, items + count, items + first);
    const std::size_t kept = count - (last - first);
    std::destroy(items + kept, items + count);
    tuple_->size_ = static_cast<std::uint32_t>(kept);
  }

  [[nodiscard]] auto size() const noexcept -> std::size_t { return tuple_->size_; }

  // The items so far, which may still change before finish().
  [[nodiscard]] auto items() noexcept -> std::span<Value> { return {tuple_->items(), tuple_->size_}; }

  // The tuple of the items. No items make the empty tuple. A block more than half unused is traded for one of the
  // exact size, so that a tuple built or cut with room to spare does not keep the spare room. Growth by doubling never
  // leaves more than half unused.
  [[nodiscard]] auto finish() && -> Value {
    if (tuple_->size_ == 0U) {
      TupleObject::destroy(std::exchange(tuple_, nullptr));
      return empty_tuple();
    }
    if (tuple_->capacity_ / 2U > tuple_->size_) {
      reallocate(tuple_->size_);
    }
    return Value::adopt(ValueKind::kTuple, std::exchange(tuple_, nullptr));
  }

private:
  explicit TupleBuilder(TupleObject* tuple) noexcept : tuple_(tuple) {}

  // Room for at least `needed` items: when the block lacks it, a block twice the size, or bigger if that is not
  // enough.
  void reserve(const std::size_t needed) {
    const std::size_t capacity = tuple_->capacity_;
    if (needed <= capacity) {
      return;
    }
    const std::size_t doubled = std::min(capacity < 2U ? 4U : capacity * 2U, TupleObject::max_items());
    reallocate(std::max(needed, doubled));
  }

  // Pushes into a bigger block. The caller has already taken the item, which may be one of the old block's own items,
  // so freeing that block cannot destroy it.
  void push_growing(Value item) {
    reserve(size() + 1U);
    std::construct_at(tuple_->items() + tuple_->size_, std::move(item));
    ++tuple_->size_;
  }

  void reallocate(const std::size_t capacity) {
    TupleObject* moved = TupleObject::allocate(capacity);
    std::uninitialized_move_n(tuple_->items(), tuple_->size_, moved->items());
    moved->size_ = tuple_->size_;
    TupleObject::destroy(tuple_);
    tuple_ = moved;
  }

  TupleObject* tuple_;
};

class DictObject final : public HeapObject {
public:
  explicit DictObject(Dict content) : dict(std::move(content)) {}
  Dict dict;
};

// A function value. The VM, builtins, and hosts implement it; calling it never needs a lookup.
class Callable : public HeapObject {
public:
  Callable() = default;
  virtual ~Callable() = default;
  Callable(const Callable&) = delete;
  auto operator=(const Callable&) -> Callable& = delete;
  Callable(Callable&&) = delete;
  auto operator=(Callable&&) -> Callable& = delete;

  // Calls with the argument the function's caller built: one value, or a tuple of several.
  [[nodiscard]] virtual auto call(Value argument) const -> Value = 0;

  // Calls with two arguments that are still apart. By default they become one tuple.
  [[nodiscard]] virtual auto call_binary(Value lhs, Value rhs) const -> Value {
    TupleBuilder items(2U);
    items.push(std::move(lhs));
    items.push(std::move(rhs));
    return call(std::move(items).finish());
  }

  // Calls as call() does, but a function value whose failures arrive as values (the VM's builtins and compiled
  // functions) reports one in `error` and returns no value instead of throwing it. Others throw as call() does.
  [[nodiscard]] virtual auto call_or_fail(Value argument, std::string& error) const -> std::optional<Value> {
    (void)error;
    return call(std::move(argument));
  }

  // The running program a function value belongs to, when the VM made it for that program: the VM enters such a
  // function in its own loop instead of calling call(). Null for every other function value.
  [[nodiscard]] auto owner() const noexcept -> const void* { return owner_; }

protected:
  explicit Callable(const void* owner) noexcept : owner_(owner) {}

private:
  const void* owner_{nullptr};
};

// A function value made from any C++ callable that takes and returns a Value.
template <typename Function>
class FunctionCallable final : public Callable {
public:
  explicit FunctionCallable(Function function) : function_(std::move(function)) {}
  [[nodiscard]] auto call(Value argument) const -> Value override { return function_(std::move(argument)); }

private:
  Function function_;
};

// Every file handle that exists, so that a file can be closed before it is removed: Windows does not remove a file
// that is open.
class OpenFiles {
public:
  void add(FileHandleObject* handle) {
    const std::scoped_lock lock(mutex_);
    handles_.push_back(handle);
  }

  void remove(FileHandleObject* handle) {
    const std::scoped_lock lock(mutex_);
    std::erase(handles_, handle);
  }

  // Calls `visit` with each handle, holding the lock.
  template <typename Visit>
  void for_each(Visit&& visit) {
    const std::scoped_lock lock(mutex_);
    for (FileHandleObject* handle : handles_) {
      visit(*handle);
    }
  }

private:
  std::mutex mutex_;
  std::vector<FileHandleObject*> handles_;
};

[[nodiscard]] inline auto open_files() -> OpenFiles& {
  static OpenFiles files;
  return files;
}

class FileHandleObject final : public HeapObject {
public:
  FileHandleObject() { open_files().add(this); }
  ~FileHandleObject() { open_files().remove(this); }
  FileHandleObject(const FileHandleObject&) = delete;
  auto operator=(const FileHandleObject&) -> FileHandleObject& = delete;
  FileHandleObject(FileHandleObject&&) = delete;
  auto operator=(FileHandleObject&&) -> FileHandleObject& = delete;

  std::fstream stream;
  std::string path;
  std::string mode;
  bool closed{false};
};

// The state of a task started by Std.Task; builtins_parallel.hpp defines it.
class TaskState : public HeapObject {
public:
  TaskState() = default;
  virtual ~TaskState() = default;
  TaskState(const TaskState&) = delete;
  auto operator=(const TaskState&) -> TaskState& = delete;
  TaskState(TaskState&&) = delete;
  auto operator=(TaskState&&) -> TaskState& = delete;
};

inline auto Value::from_string(String text) -> Value {
  return adopt(ValueKind::kString, new StringObject(std::move(text)));
}

inline auto Value::from_dict(Dict dict) -> Value { return adopt(ValueKind::kDict, new DictObject(std::move(dict))); }

inline auto Value::string_value() const noexcept -> const String& {
  return static_cast<const StringObject*>(payload_.object)->text;
}

inline auto Value::tuple() const noexcept -> TupleView {
  const auto* object = static_cast<const TupleObject*>(payload_.object);
  return {object->items(), object->size()};
}

inline auto Value::dict() const noexcept -> const Dict& { return static_cast<const DictObject*>(payload_.object)->dict; }

inline auto Value::callable() const noexcept -> const Callable& {
  return *static_cast<const Callable*>(payload_.object);
}

inline auto Value::file_handle() const noexcept -> FileHandleObject& {
  return *static_cast<FileHandleObject*>(payload_.object);
}

inline auto Value::task() const noexcept -> TaskState& { return *static_cast<TaskState*>(payload_.object); }

template <typename Object, typename Content>
auto Value::detached(Content Object::* content) -> Content& {
  auto* object = static_cast<Object*>(payload_.object);
  if (!object->unique()) {
    auto* copy = new Object(object->*content);
    release(kind_, object);
    payload_.object = copy;
    object = copy;
  }
  return object->*content;
}

inline auto TupleBuilder::reopen(Value tuple, const std::size_t capacity) -> TupleBuilder {
  auto* object = static_cast<TupleObject*>(tuple.payload_.object);
  if (!object->unique()) {
    const TupleView shared = tuple.tuple();
    TupleBuilder copy(std::max(capacity, shared.size()));
    for (const Value& item : shared) {
      copy.push(item);
    }
    return copy;
  }
  // The builder takes over the reference `tuple` held.
  tuple.kind_ = ValueKind::kNull;
  tuple.payload_.unsigned_integer = 0;
  TupleBuilder builder(object);
  builder.reserve(capacity);
  return builder;
}

// Making an empty tuple allocates nothing: it is another reference to this one. The static reference keeps it alive,
// so nothing ever holds it only once, and it is never changed in place.
inline auto empty_tuple() -> Value {
  static const Value empty = Value::adopt(ValueKind::kTuple, TupleObject::allocate(0U));
  return empty;
}

inline auto Value::mutable_tuple() -> std::span<Value> {
  if (!payload_.object->unique()) {
    const TupleView shared = tuple();
    TupleBuilder copy(shared.size());
    for (const Value& item : shared) {
      copy.push(item);
    }
    *this = std::move(copy).finish();
  }
  auto* object = static_cast<TupleObject*>(payload_.object);
  return {object->items(), object->size()};
}

inline auto Value::mutable_dict() -> Dict& { return detached(&DictObject::dict); }

inline auto Value::mutable_string() -> String& { return detached(&StringObject::text); }

inline void Value::destroy_object(const ValueKind kind, HeapObject* object) noexcept {
  switch (kind) {
    case ValueKind::kString:
      delete static_cast<StringObject*>(object);
      break;
    case ValueKind::kTuple:
      TupleObject::destroy(static_cast<TupleObject*>(object));
      break;
    case ValueKind::kDict:
      delete static_cast<DictObject*>(object);
      break;
    case ValueKind::kFunction:
      delete static_cast<Callable*>(object);
      break;
    case ValueKind::kFileHandle:
      delete static_cast<FileHandleObject*>(object);
      break;
    case ValueKind::kTask:
      delete static_cast<TaskState*>(object);
      break;
    default:
      break;
  }
}

inline auto operator==(const Value& lhs, const Value& rhs) noexcept -> bool {
  if (lhs.kind() != rhs.kind()) {
    return false;
  }
  switch (lhs.kind()) {
    case ValueKind::kNull:
      return true;
    case ValueKind::kBool:
      return lhs.bool_value() == rhs.bool_value();
    case ValueKind::kInt:
      return lhs.int_value() == rhs.int_value();
    case ValueKind::kUInt:
      return lhs.uint_value() == rhs.uint_value();
    case ValueKind::kFloat:
      return lhs.float_value() == rhs.float_value();
    case ValueKind::kRandomGenerator:
      return lhs.random_state() == rhs.random_state();
    case ValueKind::kString:
      return lhs.string_value() == rhs.string_value();
    case ValueKind::kTuple:
      return std::ranges::equal(lhs.tuple(), rhs.tuple());
    case ValueKind::kDict: {
      const Dict& left = lhs.dict();
      const Dict& right = rhs.dict();
      if (left.size() != right.size()) {
        return false;
      }
      for (const auto& [key, value] : left) {
        const Value* other = right.find(key);
        if (other == nullptr || !(*other == value)) {
          return false;
        }
      }
      return true;
    }
    case ValueKind::kFunction:
    case ValueKind::kFileHandle:
    case ValueKind::kTask:
      return lhs.object() == rhs.object();
  }
  return false;
}

inline auto DictKeyHash::operator()(const Value& key) const noexcept -> std::size_t {
  const auto mix = [&key](const std::size_t hash) -> std::size_t {
    return hash ^ (static_cast<std::size_t>(key.kind()) * 0x9E3779B97F4A7C15ULL);
  };
  switch (key.kind()) {
    case ValueKind::kNull:
      return mix(0);
    case ValueKind::kBool:
      return mix(key.bool_value() ? 1U : 0U);
    case ValueKind::kInt:
      return mix(std::hash<Int>{}(key.int_value()));
    case ValueKind::kUInt:
      return mix(std::hash<UInt>{}(key.uint_value()));
    case ValueKind::kFloat:
      return mix(std::hash<UInt>{}(std::bit_cast<UInt>(key.float_value())));
    case ValueKind::kRandomGenerator:
      return mix(std::hash<UInt>{}(key.random_state()));
    case ValueKind::kString:
      return mix(std::hash<String>{}(key.string_value()));
    case ValueKind::kTuple: {
      std::size_t hash = mix(key.tuple().size());
      for (const Value& item : key.tuple()) {
        hash = hash * 31U + operator()(item);
      }
      return hash;
    }
    default:
      return mix(std::hash<const void*>{}(key.object()));
  }
}

inline auto DictKeyEqual::operator()(const Value& lhs, const Value& rhs) const noexcept -> bool {
  if (lhs.kind() != rhs.kind()) {
    return false;
  }
  if (lhs.is_float()) {
    return std::bit_cast<UInt>(lhs.float_value()) == std::bit_cast<UInt>(rhs.float_value());
  }
  if (lhs.is_tuple()) {
    const TupleView left = lhs.tuple();
    const TupleView right = rhs.tuple();
    if (left.size() != right.size()) {
      return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
      if (!operator()(left[index], right[index])) {
        return false;
      }
    }
    return true;
  }
  return lhs == rhs;
}

}  // namespace fleaux::runtime
