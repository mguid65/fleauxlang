// Foundation: type aliases, registries, construction/extraction helpers, printing/format utilities.
// Part of the split runtime support layer; included by fleaux/runtime/runtime_support.hpp.

#pragma once

// Fleaux runtime layer built on mguid::DataTree.
//
// All runtime builtins are callable objects with the signature:
//    Value operator()(Value arg) const;
//
// The pipeline operator   value | Builtin{}   returns a Value.
//
// Fleaux tuples are represented as Value holding an ArrayNodeType.
// Fleaux primitives are Value holding a ValueNodeType (Int64/UInt64/Float64/Bool/String/Null).
//
// Control-flow builtins (Loop, LoopN, Apply, Branch, Select) remain templated
// because their function arguments are C++ callables, not Values.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <concepts>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#if __has_include(<format>)
#include <format>
#define FLEAUX_HAS_STD_FORMAT 1
#else
#define FLEAUX_HAS_STD_FORMAT 0
#endif
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "data_tree/data_tree.hpp"

namespace fleaux::runtime {

// Core type aliases

using Value = mguid::DataTree;
using Array = mguid::ArrayNodeType;
using Object = mguid::ObjectNodeType;
using Generic = mguid::GenericNodeType;
using ValueNode = mguid::ValueNodeType;
using Number = mguid::NumberType;
using Null = mguid::NullType;
using Bool = mguid::BoolType;
using String = mguid::StringType;

using Int = mguid::IntegerType;           // int64_t
using UInt = mguid::UnsignedIntegerType;  // uint64_t
using Float = mguid::DoubleType;          // double

using RuntimeCallable = std::function<Value(Value)>;

// ============================================================================
// ScopedRegistry<T>
//
// General slot+generation+free-list+registration-log registry.
// Supports:
//   - Transient registration (logged; cleaned up by ScopedRegistryCheckpoint).
//   - Pinned registration (not logged; caller owns lifetime).
//   - Generation-safe lookup: stale ids are always rejected.
//   - Slot reuse: freed slots are recycled, generation counter prevents ABA.
// ============================================================================

struct RegistryId {
  UInt slot;
  UInt generation;
};

template <typename T>
struct ScopedRegistryEntry {
  T value{};
  UInt generation{0};
  bool occupied{false};
};

template <typename T>
struct ScopedRegistry {
  using Entry = ScopedRegistryEntry<T>;

  std::vector<Entry> entries;
  std::vector<UInt> free_slots;
  std::vector<UInt> registration_log;
  std::size_t active_count{0};

  // Insert a value and optionally log it for scope cleanup.
  auto insert(T val, const bool logged) -> RegistryId {
    UInt slot = 0;
    if (!free_slots.empty()) {
      slot = free_slots.back();
      free_slots.pop_back();
      auto& entry = entries.at(static_cast<std::size_t>(slot));
      entry.value = std::move(val);
      entry.occupied = true;
    } else {
      entries.push_back(Entry{.value = std::move(val), .generation = 0, .occupied = true});
      slot = static_cast<UInt>(entries.size() - 1);
    }
    if (logged) { registration_log.push_back(slot); }
    ++active_count;
    return RegistryId{.slot = slot, .generation = entries[static_cast<std::size_t>(slot)].generation};
  }

  // Retire a slot; bumps generation so existing ids become stale.
  void retire(const UInt slot) {
    const auto idx = static_cast<std::size_t>(slot);
    if (idx >= entries.size()) { return; }
    auto& entry = entries[idx];
    if (!entry.occupied) { return; }
    entry.value = T{};
    ++entry.generation;
    entry.occupied = false;
    free_slots.push_back(slot);
    if (active_count > 0) { --active_count; }
  }

  // Look up a value by id; returns nullptr if id is stale or invalid.
  [[nodiscard]] auto get(const RegistryId& id) const -> const T* {
    const auto idx = static_cast<std::size_t>(id.slot);
    if (idx >= entries.size()) { return nullptr; }
    const auto& entry = entries[idx];
    if (!entry.occupied || entry.generation != id.generation) { return nullptr; }
    return &entry.value;
  }

  void clear() {
    entries.clear();
    free_slots.clear();
    registration_log.clear();
    active_count = 0;
  }
};

// ============================================================================
// ScopedRegistryCheckpoint
//
// RAII scope guard for a ScopedRegistry: snapshots the registration log size
// on construction, retires all entries added after the checkpoint on destruction.
// Pinned entries (not in the log) are unaffected.
// ============================================================================

template <typename T>
class ScopedRegistryCheckpoint {
public:
  explicit ScopedRegistryCheckpoint(ScopedRegistry<T>& registry, std::mutex& mutex)
      : registry_(&registry), mutex_(&mutex) {
    std::scoped_lock lock(*mutex_);
    checkpoint_ = registry_->registration_log.size();
  }

  ScopedRegistryCheckpoint(const ScopedRegistryCheckpoint&) = delete;
  auto operator=(const ScopedRegistryCheckpoint&) -> ScopedRegistryCheckpoint& = delete;
  ScopedRegistryCheckpoint(ScopedRegistryCheckpoint&&) = delete;
  auto operator=(ScopedRegistryCheckpoint&&) -> ScopedRegistryCheckpoint& = delete;

  ~ScopedRegistryCheckpoint() {
    std::scoped_lock lock(*mutex_);
    const auto log_size = registry_->registration_log.size();
    for (std::size_t i = checkpoint_; i < log_size; ++i) {
      registry_->retire(registry_->registration_log[i]);
    }
    registry_->registration_log.resize(checkpoint_);
  }

private:
  ScopedRegistry<T>* registry_;
  std::mutex* mutex_;
  std::size_t checkpoint_{0};
};

// ============================================================================
// PinnedRegistryRef<T>
//
// RAII owner for a pinned (non-logged) registry entry.
// Releases the slot on destruction or explicit release().
// ============================================================================

template <typename T>
class PinnedRegistryRef {
public:
  PinnedRegistryRef() = default;

  PinnedRegistryRef(ScopedRegistry<T>& registry, std::mutex& mutex, T val)
      : registry_(&registry), mutex_(&mutex) {
    std::scoped_lock lock(*mutex_);
    id_ = registry_->insert(std::move(val), /*logged=*/false);
    valid_ = true;
  }

  PinnedRegistryRef(const PinnedRegistryRef&) = delete;
  auto operator=(const PinnedRegistryRef&) -> PinnedRegistryRef& = delete;

  PinnedRegistryRef(PinnedRegistryRef&& other) noexcept
      : registry_(other.registry_), mutex_(other.mutex_), id_(other.id_), valid_(other.valid_) {
    other.valid_ = false;
  }

  auto operator=(PinnedRegistryRef&& other) noexcept -> PinnedRegistryRef& {
    if (this != &other) {
      if (valid_) { do_release(); }
      registry_ = other.registry_;
      mutex_ = other.mutex_;
      id_ = other.id_;
      valid_ = other.valid_;
      other.valid_ = false;
    }
    return *this;
  }

  ~PinnedRegistryRef() { if (valid_) { do_release(); } }

  void release() { if (valid_) { do_release(); valid_ = false; } }
  [[nodiscard]] auto id() const -> const RegistryId& { return id_; }
  [[nodiscard]] auto is_valid() const -> bool { return valid_; }

private:
  void do_release() {
    std::scoped_lock lock(*mutex_);
    registry_->retire(id_.slot);
  }

  ScopedRegistry<T>* registry_{nullptr};
  std::mutex* mutex_{nullptr};
  RegistryId id_{.slot = 0, .generation = 0};
  bool valid_{false};
};

// ============================================================================
// Callable registry (ScopedRegistry<RuntimeCallable>)
// ============================================================================

inline constexpr std::string_view k_callable_tag = "__fleaux_callable__";
inline constexpr std::string_view k_handle_tag = "__fleaux_handle__";
inline constexpr std::string_view k_value_ref_tag = "__fleaux_ref__";

// Legacy alias kept so external code using CallableId or RegistryId compiles.
using CallableId = RegistryId;

inline auto callable_registry() -> ScopedRegistry<RuntimeCallable>& {
  static ScopedRegistry<RuntimeCallable> registry;
  return registry;
}

inline auto callable_registry_mutex() -> std::mutex& {
  static std::mutex mutex;
  return mutex;
}

// Process arguments

inline auto process_args_mutex() -> std::mutex& {
  static std::mutex mutex;
  return mutex;
}

inline auto process_args_storage() -> std::vector<std::string>& {
  static std::vector<std::string> args;
  return args;
}

inline void set_process_args(const int argc, char** argv) {
  std::scoped_lock lock(process_args_mutex());
  auto& args = process_args_storage();
  args.clear();
  if (argc <= 0) { return; }
  args.reserve(static_cast<std::size_t>(argc));
  for (int arg_index = 0; arg_index < argc; ++arg_index) {
    args.emplace_back((argv != nullptr && argv[arg_index] != nullptr) ? argv[arg_index] : "");
  }
}

[[nodiscard]] inline auto get_process_args() -> std::vector<std::string> {
  std::scoped_lock lock(process_args_mutex());
  return process_args_storage();
}

// Callable/Function handling

inline auto register_callable(RuntimeCallable fn) -> CallableId {
  std::scoped_lock lock(callable_registry_mutex());
  return callable_registry().insert(std::move(fn), /*logged=*/true);
}

[[nodiscard]] inline auto callable_registry_size() -> std::size_t {
  std::scoped_lock lock(callable_registry_mutex());
  return callable_registry().active_count;
}

// Discard all registered callables; intended for test isolation only.
// Any live callable-ref Values become invalid after this call.
inline void reset_callable_registry() {
  std::scoped_lock lock(callable_registry_mutex());
  callable_registry().clear();
}

// Registers a callable outside the registration log so it survives
// CallableRegistryScope teardown. The caller owns the returned CallableId and
// must eventually call release_callable_ref to retire it.
inline auto register_callable_pinned(RuntimeCallable fn) -> CallableId {
  std::scoped_lock lock(callable_registry_mutex());
  return callable_registry().insert(std::move(fn), /*logged=*/false);
}

// Snapshots callable-registry log position and retires transient entries on scope exit.
// This is safe for execution scopes where callable-ref Values do not escape.
class CallableRegistryScope {
public:
  CallableRegistryScope() {
    std::scoped_lock lock(callable_registry_mutex());
    checkpoint_log_size_ = callable_registry().registration_log.size();
  }

  CallableRegistryScope(const CallableRegistryScope&) = delete;
  auto operator=(const CallableRegistryScope&) -> CallableRegistryScope& = delete;
  CallableRegistryScope(CallableRegistryScope&&) = delete;
  auto operator=(CallableRegistryScope&&) -> CallableRegistryScope& = delete;

  ~CallableRegistryScope() {
    std::scoped_lock lock(callable_registry_mutex());
    auto& call_reg = callable_registry();
    const auto log_size = call_reg.registration_log.size();
    for (std::size_t i = checkpoint_log_size_; i < log_size; ++i) {
      call_reg.retire(call_reg.registration_log[i]);
    }
    call_reg.registration_log.resize(checkpoint_log_size_);
  }

private:
  std::size_t checkpoint_log_size_{0};
};

// Global function registry stored as GenericNodeType
inline auto function_registry() -> Value& {
  static Value registry{mguid::NodeTypeTag::Generic};
  return registry;
}

// Explicitly retires a pinned (or any) callable slot by its CallableId.
// Safe to call multiple times; subsequent calls for the same id are no-ops.
inline void release_callable_ref(const CallableId& id) {
  std::scoped_lock lock(callable_registry_mutex());
  callable_registry().retire(id.slot);
}

// Forward declaration; full definition follows callable_id_from_value below.
inline void release_callable_ref(const Value& ref);

// RAII wrapper that pins a callable for its lifetime and releases it on destruction.
// Construct with make_pinned_callable_ref; do not copy (move is allowed).
class PinnedCallableRef {
public:
  PinnedCallableRef() = default;

  explicit PinnedCallableRef(RuntimeCallable fn) {
    id_ = register_callable_pinned(std::move(fn));
    ref_ = [&] {
      Array out;
      out.Reserve(3);
      out.PushBack(Value{String{k_callable_tag}});
      out.PushBack(Value{id_.slot});
      out.PushBack(Value{id_.generation});
      return Value{std::move(out)};
    }();
    valid_ = true;
  }

  PinnedCallableRef(const PinnedCallableRef&) = delete;
  auto operator=(const PinnedCallableRef&) -> PinnedCallableRef& = delete;

  PinnedCallableRef(PinnedCallableRef&& other) noexcept
      : id_(other.id_), ref_(std::move(other.ref_)), valid_(other.valid_) {
    other.valid_ = false;
  }

  auto operator=(PinnedCallableRef&& other) noexcept -> PinnedCallableRef& {
    if (this != &other) {
      if (valid_) { release_callable_ref(id_); }
      id_ = other.id_;
      ref_ = std::move(other.ref_);
      valid_ = other.valid_;
      other.valid_ = false;
    }
    return *this;
  }

  ~PinnedCallableRef() {
    if (valid_) { release_callable_ref(id_); }
  }

  // Returns the Value token that can be passed into the runtime.
  [[nodiscard]] auto token() const -> const Value& { return ref_; }

  // Explicitly release the pinned ref before destruction.
  void release() {
    if (valid_) {
      release_callable_ref(id_);
      valid_ = false;
    }
  }

  [[nodiscard]] auto id() const -> const CallableId& { return id_; }
  [[nodiscard]] auto is_valid() const -> bool { return valid_; }

private:
  CallableId id_{.slot = 0, .generation = 0};
  Value ref_;
  bool valid_{false};
};

template <typename F>
[[nodiscard]] auto make_pinned_callable_ref(F&& fn) -> PinnedCallableRef {
  return PinnedCallableRef(RuntimeCallable(std::forward<F>(fn)));
}

[[nodiscard]] inline auto make_callable_ref(RuntimeCallable fn) -> Value {
  const CallableId id = register_callable(std::move(fn));

  // Return simple array with type tag, slot, and generation for passing around.
  Array out;
  out.Reserve(3);
  out.PushBack(Value{String{k_callable_tag}});
  out.PushBack(Value{id.slot});
  out.PushBack(Value{id.generation});
  return Value{std::move(out)};
}

template <typename F>
auto make_callable_ref(F&& fn) -> Value {
  return make_callable_ref(RuntimeCallable(std::forward<F>(fn)));
}

[[nodiscard]] inline auto callable_id_from_value(const Value& ref) -> std::optional<CallableId> {
  const auto& arr = ref.TryGetArray();
  if (!arr || (arr->Size() != 2 && arr->Size() != 3)) { return std::nullopt; }

  const auto& tag = arr->TryGet(0)->TryGetString();
  if (!tag || *tag != k_callable_tag) { return std::nullopt; }

  const auto as_uint = [](const Number& number_value) -> std::optional<UInt> {
    return number_value.Visit(
        [](const Int signed_value) -> std::optional<UInt> {
          if (signed_value < 0) return std::nullopt;
          return static_cast<UInt>(signed_value);
        },
        [](const UInt unsigned_value) -> std::optional<UInt> { return unsigned_value; },
        [](const Float float_value) -> std::optional<UInt> {
          if (float_value < 0.0 || std::floor(float_value) != float_value) return std::nullopt;
          return static_cast<UInt>(float_value);
        });
  };

  const auto& slot_num = arr->TryGet(1)->TryGetNumber();
  if (!slot_num) { return std::nullopt; }
  const auto slot = as_uint(*slot_num);
  if (!slot) { return std::nullopt; }

  UInt generation = 0;
  if (arr->Size() == 3) {
    const auto& generation_num = arr->TryGet(2)->TryGetNumber();
    if (!generation_num) { return std::nullopt; }
    const auto parsed_generation = as_uint(*generation_num);
    if (!parsed_generation) { return std::nullopt; }
    generation = *parsed_generation;
  }

  return CallableId{.slot = *slot, .generation = generation};
}

// Overload of release_callable_ref that accepts a Value token (defined here after
// callable_id_from_value is available).
inline void release_callable_ref(const Value& ref) {
  const auto id = callable_id_from_value(ref);
  if (!id) { return; }
  std::scoped_lock lock(callable_registry_mutex());
  // Only retire if the generation still matches (idempotent if already retired).
  auto& call_reg = callable_registry();
  const auto* ptr = call_reg.get(*id);
  if (ptr) { call_reg.retire(id->slot); }
}

// ============================================================================
// ValueRegistry - ScopedRegistry<Value>
//
// Stores arbitrary Fleaux Values by reference using slot+generation tokens.
// Tokens can be passed through the pipeline without deep-copying the stored
// Value; callers deref when they need the actual value.
// Use make_value_ref (transient) or make_pinned_value_ref (persistent).
// ============================================================================

inline auto value_registry() -> ScopedRegistry<Value>& {
  static ScopedRegistry<Value> registry;
  return registry;
}

inline auto value_registry_mutex() -> std::mutex& {
  static std::mutex mutex;
  return mutex;
}

struct ValueRegistryTelemetry {
  std::size_t active_count{0};
  std::size_t peak_active_count{0};
  std::size_t rejected_allocations{0};
  std::size_t stale_deref_rejections{0};
  std::optional<std::size_t> transient_cap{};
};

inline auto value_registry_telemetry_state() -> ValueRegistryTelemetry& {
  static ValueRegistryTelemetry state;
  return state;
}

inline void set_value_registry_transient_cap(const std::optional<std::size_t> cap) {
  std::scoped_lock lock(value_registry_mutex());
  value_registry_telemetry_state().transient_cap = cap;
}

[[nodiscard]] inline auto value_registry_telemetry() -> ValueRegistryTelemetry {
  std::scoped_lock lock(value_registry_mutex());
  auto snapshot = value_registry_telemetry_state();
  snapshot.active_count = value_registry().active_count;
  return snapshot;
}

inline void reset_value_registry_telemetry_for_tests() {
  std::scoped_lock lock(value_registry_mutex());
  value_registry_telemetry_state() = ValueRegistryTelemetry{};
}

inline void reset_value_registry_for_tests() {
  std::scoped_lock lock(value_registry_mutex());
  value_registry().clear();
  value_registry_telemetry_state() = ValueRegistryTelemetry{};
}

// Returns a Value token for a stored value (transient; cleaned up by ValueRegistryScope).
[[nodiscard]] inline auto make_value_ref(Value val) -> Value {
  RegistryId id;
  {
    std::scoped_lock lock(value_registry_mutex());
    auto& telemetry = value_registry_telemetry_state();
    auto& reg = value_registry();
    if (telemetry.transient_cap.has_value() && reg.active_count >= *telemetry.transient_cap) {
      ++telemetry.rejected_allocations;
      throw std::runtime_error{"make_value_ref: value-ref transient cap reached"};
    }
    id = reg.insert(std::move(val), /*logged=*/true);
    telemetry.peak_active_count = std::max(telemetry.peak_active_count, reg.active_count);
  }
  Array out;
  out.Reserve(3);
  out.PushBack(Value{String{k_value_ref_tag}});
  out.PushBack(Value{id.slot});
  out.PushBack(Value{id.generation});
  return Value{std::move(out)};
}

[[nodiscard]] inline auto value_ref_id_from_token(const Value& token) -> std::optional<RegistryId> {
  const auto& arr = token.TryGetArray();
  if (!arr || arr->Size() != 3) { return std::nullopt; }
  const auto& tag = arr->TryGet(0)->TryGetString();
  if (!tag || *tag != k_value_ref_tag) { return std::nullopt; }
  const auto as_uint = [](const Number& n) -> std::optional<UInt> {
    return n.Visit(
        [](const Int i) -> std::optional<UInt> { return i >= 0 ? std::optional<UInt>(static_cast<UInt>(i)) : std::nullopt; },
        [](const UInt u) -> std::optional<UInt> { return u; },
        [](const Float f) -> std::optional<UInt> {
          return f >= 0 && std::floor(f) == f ? std::optional<UInt>(static_cast<UInt>(f)) : std::nullopt;
        });
  };
  const auto& sn = arr->TryGet(1)->TryGetNumber();
  const auto& gn = arr->TryGet(2)->TryGetNumber();
  if (!sn || !gn) { return std::nullopt; }
  const auto slot = as_uint(*sn);
  const auto gen = as_uint(*gn);
  if (!slot || !gen) { return std::nullopt; }
  return RegistryId{.slot = *slot, .generation = *gen};
}

// Returns a copy of the stored value; throws if token is stale.
[[nodiscard]] inline auto deref_value_ref(const Value& token) -> Value {
  const auto id = value_ref_id_from_token(token);
  if (!id) { throw std::runtime_error{"deref_value_ref: not a value-ref token"}; }
  std::scoped_lock lock(value_registry_mutex());
  auto& telemetry = value_registry_telemetry_state();
  const Value* ptr = value_registry().get(*id);
  if (!ptr) {
    ++telemetry.stale_deref_rejections;
    throw std::runtime_error{"deref_value_ref: stale or unknown value-ref token"};
  }
  return *ptr;
}

// Explicitly retire a value ref token. Idempotent.
inline void release_value_ref(const Value& token) {
  const auto id = value_ref_id_from_token(token);
  if (!id) { return; }
  std::scoped_lock lock(value_registry_mutex());
  auto& reg = value_registry();
  if (reg.get(*id)) { reg.retire(id->slot); }
}

// RAII scope: retires all transient value-refs created within this scope on exit.
class ValueRegistryScope {
public:
  ValueRegistryScope() {
    std::scoped_lock lock(value_registry_mutex());
    checkpoint_ = value_registry().registration_log.size();
  }

  ValueRegistryScope(const ValueRegistryScope&) = delete;
  auto operator=(const ValueRegistryScope&) -> ValueRegistryScope& = delete;
  ValueRegistryScope(ValueRegistryScope&&) = delete;
  auto operator=(ValueRegistryScope&&) -> ValueRegistryScope& = delete;

  ~ValueRegistryScope() {
    std::scoped_lock lock(value_registry_mutex());
    auto& reg = value_registry();
    const auto log_size = reg.registration_log.size();
    for (std::size_t i = checkpoint_; i < log_size; ++i) { reg.retire(reg.registration_log[i]); }
    reg.registration_log.resize(checkpoint_);
  }

private:
  std::size_t checkpoint_{0};
};

// RAII pinned value ref: stored value outlives any ValueRegistryScope.
class PinnedValueRef {
public:
  PinnedValueRef() = default;

  explicit PinnedValueRef(Value val) {
    RegistryId id;
    {
      std::scoped_lock lock(value_registry_mutex());
      id = value_registry().insert(std::move(val), /*logged=*/false);
    }
    Array out;
    out.Reserve(3);
    out.PushBack(Value{String{k_value_ref_tag}});
    out.PushBack(Value{id.slot});
    out.PushBack(Value{id.generation});
    token_ = Value{std::move(out)};
    id_ = id;
    valid_ = true;
  }

  PinnedValueRef(const PinnedValueRef&) = delete;
  auto operator=(const PinnedValueRef&) -> PinnedValueRef& = delete;

  PinnedValueRef(PinnedValueRef&& other) noexcept
      : id_(other.id_), token_(std::move(other.token_)), valid_(other.valid_) {
    other.valid_ = false;
  }

  auto operator=(PinnedValueRef&& other) noexcept -> PinnedValueRef& {
    if (this != &other) {
      if (valid_) { do_release(); }
      id_ = other.id_;
      token_ = std::move(other.token_);
      valid_ = other.valid_;
      other.valid_ = false;
    }
    return *this;
  }

  ~PinnedValueRef() { if (valid_) { do_release(); } }

  [[nodiscard]] auto token() const -> const Value& { return token_; }
  [[nodiscard]] auto id() const -> const RegistryId& { return id_; }
  [[nodiscard]] auto is_valid() const -> bool { return valid_; }

  void release() { if (valid_) { do_release(); valid_ = false; } }

  // Returns a copy of the stored value.
  [[nodiscard]] auto get() const -> Value {
    if (!valid_) { throw std::runtime_error{"PinnedValueRef::get: already released"}; }
    return deref_value_ref(token_);
  }

private:
  void do_release() {
    std::scoped_lock lock(value_registry_mutex());
    value_registry().retire(id_.slot);
  }

  RegistryId id_{.slot = 0, .generation = 0};
  Value token_;
  bool valid_{false};
};

// File handle registry

struct HandleEntry {
  std::fstream stream;
  std::string path;
  std::string mode;
  bool closed{false};
  UInt generation{0};
};

struct HandleRegistry {
  std::vector<HandleEntry> entries;
  std::vector<UInt> registration_log;
  std::mutex mtx;

  auto open(const std::string& path, const std::string& mode) -> UInt {
    std::scoped_lock lock(mtx);
    // find a closed slot to reuse
    for (std::size_t slot_index = 0; slot_index < entries.size(); ++slot_index) {
      if (entries[slot_index].closed) {
        auto& entry = entries[slot_index];
        entry.generation++;
        entry.closed = false;
        entry.path = path;
        entry.mode = mode;
        open_stream(entry);
        registration_log.push_back(static_cast<UInt>(slot_index));
        return static_cast<UInt>(slot_index);
      }
    }

    entries.push_back({});
    auto& entry = entries.back();
    entry.path = path;
    entry.mode = mode;
    entry.generation = 0;
    open_stream(entry);
    const auto slot = static_cast<UInt>(entries.size() - 1);
    registration_log.push_back(slot);
    return slot;
  }

  static void open_stream(HandleEntry& entry) {
    std::ios::openmode flags{};

    const bool is_read = (entry.mode.find('r') != std::string::npos);
    const bool is_write = (entry.mode.find('w') != std::string::npos);
    const bool is_append = (entry.mode.find('a') != std::string::npos);
    const bool is_binary = (entry.mode.find('b') != std::string::npos);

    if (is_read) flags |= std::ios::in;
    if (is_write) flags |= std::ios::out | std::ios::trunc;
    if (is_append) flags |= std::ios::out | std::ios::app;
    if (is_binary) flags |= std::ios::binary;
    if (!is_read && !is_write && !is_append) flags |= std::ios::in;

    entry.stream.open(entry.path, flags);
    if (!entry.stream.is_open()) {
      throw std::runtime_error{"FileOpen: cannot open '" + entry.path + "' with mode '" + entry.mode + "'"};
    }
  }

  // Returns a raw pointer to the entry; callers must not concurrently call
  // open() or close() while the pointer is in use.
  [[nodiscard]] auto get(const UInt slot, const UInt gen) -> HandleEntry* {
    if (slot >= entries.size()) return nullptr;
    auto& entry = entries[slot];
    if (entry.closed || entry.generation != gen) return nullptr;
    return &entry;
  }

  auto close(const UInt slot, const UInt gen) -> bool {
    std::scoped_lock lock(mtx);
    if (slot >= entries.size()) return false;
    auto& entry = entries[slot];
    if (entry.closed || entry.generation != gen) return false;
    entry.stream.close();
    entry.closed = true;
    return true;
  }
};

inline auto handle_registry() -> HandleRegistry& {
  static HandleRegistry reg;
  return reg;
}

// RAII scope: closes any file handles opened after the checkpoint on scope exit.
// Handles explicitly closed before scope exit are skipped (idempotent).
class HandleRegistryScope {
public:
  HandleRegistryScope() {
    auto& reg = handle_registry();
    std::scoped_lock lock(reg.mtx);
    checkpoint_ = reg.registration_log.size();
  }

  HandleRegistryScope(const HandleRegistryScope&) = delete;
  auto operator=(const HandleRegistryScope&) -> HandleRegistryScope& = delete;
  HandleRegistryScope(HandleRegistryScope&&) = delete;
  auto operator=(HandleRegistryScope&&) -> HandleRegistryScope& = delete;

  ~HandleRegistryScope() {
    auto& reg = handle_registry();
    std::scoped_lock lock(reg.mtx);
    const auto log_size = reg.registration_log.size();
    for (std::size_t i = checkpoint_; i < log_size; ++i) {
      const auto slot = static_cast<std::size_t>(reg.registration_log[i]);
      if (slot < reg.entries.size()) {
        auto& entry = reg.entries[slot];
        if (!entry.closed) {
          entry.stream.close();
          entry.closed = true;
        }
      }
    }
    reg.registration_log.resize(checkpoint_);
  }

private:
  std::size_t checkpoint_{0};
};

// Global handle registry stored as GenericNodeType
inline auto file_handle_registry() -> Value& {
  static Value registry{mguid::NodeTypeTag::Generic};
  return registry;
}

// Handle token with type checking
struct HandleId {
  UInt slot;
  UInt gen;
};

[[nodiscard]] inline auto make_handle_token(UInt slot, UInt gen) -> Value {
  // Store in Generic node with metadata
  auto& gen_node = file_handle_registry().Unsafe([](auto&& proxy) -> decltype(auto) { return proxy.GetGeneric(); });

  Object handle_entry;
  handle_entry["type"] = Value{String{k_handle_tag}};
  handle_entry["slot"] = Value{slot};
  handle_entry["gen"] = Value{gen};

  gen_node["handle_" + std::to_string(slot) + "_" + std::to_string(gen)] = Value{std::move(handle_entry)};

  // Return array with type tag, slot, and gen for passing around
  Array token;
  token.Reserve(3);
  token.PushBack(Value{String{k_handle_tag}});
  token.PushBack(Value{slot});
  token.PushBack(Value{gen});
  return Value{std::move(token)};
}

[[nodiscard]] inline auto handle_id_from_value(const Value& value) -> std::optional<HandleId> {
  const auto& arr = value.TryGetArray();
  if (!arr || arr->Size() != 3) return std::nullopt;
  const auto& tag = arr->TryGet(0)->TryGetString();
  if (!tag || *tag != k_handle_tag) return std::nullopt;
  const auto& sn = arr->TryGet(1)->TryGetNumber();
  const auto& gn = arr->TryGet(2)->TryGetNumber();
  if (!sn || !gn) return std::nullopt;
  auto as_uint = [](const Number& number_value) -> std::optional<UInt> {
    return number_value.Visit(
        [](const Int signed_value) -> std::optional<UInt> {
          return signed_value >= 0 ? std::optional<UInt>(static_cast<UInt>(signed_value)) : std::nullopt;
        },
        [](const UInt unsigned_value) -> std::optional<UInt> { return unsigned_value; },
        [](const Float float_value) -> std::optional<UInt> {
          return float_value >= 0 && std::floor(float_value) == float_value
                     ? std::optional<UInt>(static_cast<UInt>(float_value))
                     : std::nullopt;
        });
  };
  const auto slot = as_uint(*sn);
  const auto gen = as_uint(*gn);
  if (!slot || !gen) return std::nullopt;
  return HandleId{.slot = *slot, .gen = *gen};
}

[[nodiscard]] inline auto require_handle(const Value& token, const char* op) -> HandleEntry& {
  const auto id = handle_id_from_value(token);
  if (!id) throw std::runtime_error{std::string(op) + ": not a valid handle token"};
  HandleEntry* entry = handle_registry().get(id->slot, id->gen);
  if (!entry) throw std::runtime_error{std::string(op) + ": handle is closed or invalid"};
  return *entry;
}

[[nodiscard]] inline auto invoke_callable_ref(const Value& ref, Value arg) -> Value {
  const auto id = callable_id_from_value(ref);
  if (!id) { throw std::runtime_error{"Expected callable reference"}; }

  RuntimeCallable callable;
  {
    std::scoped_lock lock(callable_registry_mutex());
    const RuntimeCallable* ptr = callable_registry().get(*id);
    if (!ptr) { throw std::runtime_error{"Unknown callable reference"}; }
    callable = *ptr;
  }

  return callable(std::move(arg));
}

/**
 * @brief A node has a call operator that accepts a Value and returns a Value
 */
template <typename Node>
concept NodeLike = requires(Node&& node, Value arg) {
  { std::invoke(std::forward<Node>(node), std::move(arg)) } -> std::same_as<Value>;
};

// Pipeline for builtins that receive a Value and return a Value.
template <NodeLike Node>
auto operator|(Value arg, Node&& node) -> Value {
  return std::invoke(std::forward<Node>(node), std::move(arg));
}

inline auto operator|(Value arg, const Value& callable_ref) -> Value {
  return invoke_callable_ref(callable_ref, std::move(arg));
}

// Construction helpers

inline auto make_null() -> Value { return Value{Null{}}; }
inline auto make_bool(bool value) -> Value { return Value{value}; }
inline auto make_int(Int value) -> Value { return Value{value}; }
inline auto make_uint(UInt value) -> Value { return Value{value}; }
inline auto make_float(Float value) -> Value { return Value{value}; }
inline auto make_string(String value) -> Value { return Value{std::move(value)}; }
inline auto make_array() -> Value { return Value{Array{}}; }

// Build a tuple Value from a variadic list of Values.

template <typename MaybeValue>
concept ValueLike = requires(MaybeValue&& vals) { requires(std::same_as<std::remove_cvref_t<MaybeValue>, Value>); };

template <ValueLike... Values>
auto make_tuple(Values&&... vals) -> Value {
  Array arr;
  arr.Reserve(sizeof...(Values));
  (arr.PushBack(std::forward<Values>(vals)), ...);
  return Value{std::move(arr)};
}

// Extraction helpers

[[nodiscard]] inline auto as_array(const Value& value) -> const Array& {
  auto result = value.TryGetArray();
  if (!result) throw std::runtime_error{"fleaux::runtime: expected Array"};
  return *result;
}

[[nodiscard]] inline auto as_array(Value& value) -> Array& {
  auto result = value.TryGetArray();
  if (!result) throw std::runtime_error{"fleaux::runtime: expected Array"};
  return *result;
}

[[nodiscard]] inline auto as_number(const Value& value) -> const Number& {
  auto result = value.TryGetNumber();
  if (!result) throw std::runtime_error{"fleaux::runtime: expected Int64, UInt64, or Float64"};
  return *result;
}

[[nodiscard]] inline auto as_bool(const Value& value) -> Bool {
  auto result = value.TryGetBool();
  if (!result) throw std::runtime_error{"fleaux::runtime: expected Bool"};
  return *result;
}

[[nodiscard]] inline auto as_string(const Value& value) -> const String& {
  auto result = value.TryGetString();
  if (!result) throw std::runtime_error{"fleaux::runtime: expected String"};
  return *result;
}

[[nodiscard]] inline auto as_object(const Value& value) -> const Object& {
  auto result = value.TryGetObject();
  if (!result) throw std::runtime_error{"fleaux::runtime: expected Object"};
  return *result;
}

[[nodiscard]] inline auto as_object(Value& value) -> Object& {
  auto result = value.TryGetObject();
  if (!result) throw std::runtime_error{"fleaux::runtime: expected Object"};
  return *result;
}

// Get the Nth element of an Array Value (throws on out-of-range).
[[nodiscard]] inline auto array_at(const Value& value, const std::size_t index) -> const Value& {
  auto result = as_array(value).TryGet(index);
  if (!result) throw std::out_of_range{"fleaux::runtime: array index out of range"};
  return *result;
}

[[nodiscard]] inline auto array_at(Value& value, const std::size_t index) -> Value& {
  auto result = value.TryGetArray();
  if (!result) throw std::runtime_error{"fleaux::runtime: expected Array"};
  Array& arr = *result;
  if (index >= arr.Size()) throw std::out_of_range{"fleaux::runtime: array index out of range"};
  return arr[index];
}

// Convert any numeric Value to double.
[[nodiscard]] inline auto to_double(const Value& val) -> double {
  return as_number(val).Visit([](const Int signed_value) -> double { return static_cast<double>(signed_value); },
                              [](const UInt unsigned_value) -> double {
                                return static_cast<double>(unsigned_value);
                              },
                              [](const Float float_value) -> double { return float_value; });
}

// Convert a double result back to the most correct numeric Value (Int64, UInt64, or Float64).
[[nodiscard]] inline auto num_result(const double val, const bool prefer_unsigned = false) -> Value {
  if (val == std::floor(val) && std::isfinite(val)) {
    if (prefer_unsigned && val >= 0.0 && val <= static_cast<double>(std::numeric_limits<UInt>::max())) {
      return make_uint(static_cast<UInt>(val));
    }
    if (val >= static_cast<double>(std::numeric_limits<Int>::min()) &&
        val <= static_cast<double>(std::numeric_limits<Int>::max())) {
      return make_int(static_cast<Int>(val));
    }
    if (!prefer_unsigned && val >= 0.0 && val <= static_cast<double>(std::numeric_limits<UInt>::max())) {
      return make_uint(static_cast<UInt>(val));
    }
  }
  return make_float(val);
}

[[nodiscard]] inline auto is_uint_number(const Value& val) -> bool {
  return as_number(val).Visit([](const Int) -> bool { return false; }, [](const UInt) -> bool { return true; },
                              [](const Float) -> bool { return false; });
}

[[nodiscard]] inline auto is_int_number(const Value& val) -> bool {
  return as_number(val).Visit([](const Int) -> bool { return true; }, [](const UInt) -> bool { return false; },
                              [](const Float) -> bool { return false; });
}

[[nodiscard]] inline auto is_float_number(const Value& val) -> bool {
  return as_number(val).Visit([](const Int) -> bool { return false; }, [](const UInt) -> bool { return false; },
                              [](const Float) -> bool { return true; });
}

[[nodiscard]] inline auto is_mixed_signed_unsigned_integer_pair(const Value& lhs, const Value& rhs) -> bool {
  if (is_float_number(lhs) || is_float_number(rhs)) { return false; }
  return (is_int_number(lhs) && is_uint_number(rhs)) || (is_uint_number(lhs) && is_int_number(rhs));
}

[[nodiscard]] inline auto compare_numbers(const Value& lhs, const Value& rhs) -> int {
  const bool lhs_float = is_float_number(lhs);
  const bool rhs_float = is_float_number(rhs);
  if (lhs_float || rhs_float) {
    const double lhs_value = to_double(lhs);
    const double rhs_value = to_double(rhs);
    return (lhs_value < rhs_value) ? -1 : ((lhs_value > rhs_value) ? 1 : 0);
  }

  if (is_int_number(lhs) && is_int_number(rhs)) {
    const Int lhs_value = as_number(lhs).Visit([](const Int signed_value) -> Int { return signed_value; },
                                               [](const UInt) -> Int { return 0; },
                                               [](const Float) -> Int { return 0; });
    const Int rhs_value = as_number(rhs).Visit([](const Int signed_value) -> Int { return signed_value; },
                                               [](const UInt) -> Int { return 0; },
                                               [](const Float) -> Int { return 0; });
    return (lhs_value < rhs_value) ? -1 : ((lhs_value > rhs_value) ? 1 : 0);
  }

  if (is_uint_number(lhs) && is_uint_number(rhs)) {
    const UInt lhs_value = as_number(lhs).Visit([](const Int) -> UInt { return 0; },
                                                [](const UInt unsigned_value) -> UInt { return unsigned_value; },
                                                [](const Float) -> UInt { return 0; });
    const UInt rhs_value = as_number(rhs).Visit([](const Int) -> UInt { return 0; },
                                                [](const UInt unsigned_value) -> UInt { return unsigned_value; },
                                                [](const Float) -> UInt { return 0; });
    return (lhs_value < rhs_value) ? -1 : ((lhs_value > rhs_value) ? 1 : 0);
  }

  if (is_int_number(lhs) && is_uint_number(rhs)) {
    const Int lhs_value = as_number(lhs).Visit([](const Int signed_value) -> Int { return signed_value; },
                                               [](const UInt) -> Int { return 0; },
                                               [](const Float) -> Int { return 0; });
    const UInt rhs_value = as_number(rhs).Visit([](const Int) -> UInt { return 0; },
                                                [](const UInt unsigned_value) -> UInt { return unsigned_value; },
                                                [](const Float) -> UInt { return 0; });
    if (lhs_value < 0) { return -1; }
    const UInt lhs_as_uint = static_cast<UInt>(lhs_value);
    return (lhs_as_uint < rhs_value) ? -1 : ((lhs_as_uint > rhs_value) ? 1 : 0);
  }

  const UInt lhs_value = as_number(lhs).Visit([](const Int) -> UInt { return 0; },
                                              [](const UInt unsigned_value) -> UInt { return unsigned_value; },
                                              [](const Float) -> UInt { return 0; });
  const Int rhs_value = as_number(rhs).Visit([](const Int signed_value) -> Int { return signed_value; },
                                             [](const UInt) -> Int { return 0; },
                                             [](const Float) -> Int { return 0; });
  if (rhs_value < 0) { return 1; }
  const UInt rhs_as_uint = static_cast<UInt>(rhs_value);
  return (lhs_value < rhs_as_uint) ? -1 : ((lhs_value > rhs_as_uint) ? 1 : 0);
}

// Extract the integer index embedded in a numeric (Int64/UInt64/Float64) Value.
[[nodiscard]] inline auto as_index(const Value& val) -> std::size_t {
  return as_number(val).Visit(
      [](const Int signed_value) -> std::size_t { return static_cast<std::size_t>(signed_value); },
      [](const UInt unsigned_value) -> std::size_t { return static_cast<std::size_t>(unsigned_value); },
      [](const Float float_value) -> std::size_t { return static_cast<std::size_t>(float_value); });
}

[[nodiscard]] inline auto as_int_value(const Value& val) -> Int {
  return as_number(val).Visit([](const Int signed_value) -> Int { return signed_value; },
                              [](const UInt unsigned_value) -> Int { return static_cast<Int>(unsigned_value); },
                              [](const Float float_value) -> Int { return static_cast<Int>(float_value); });
}

[[nodiscard]] inline auto as_int_value_strict(const Value& val, const std::string_view name) -> Int {
  return as_number(val).Visit(
      [&](const Int signed_value) -> Int { return signed_value; },
      [&](const UInt unsigned_value) -> Int {
        if (unsigned_value > static_cast<UInt>(std::numeric_limits<Int>::max())) {
          throw std::out_of_range(std::format("{} out of Int64 range", name));
        }
        return static_cast<Int>(unsigned_value);
      },
      [&](const Float float_value) -> Int {
        if (!std::isfinite(float_value) || std::floor(float_value) != float_value) {
          throw std::invalid_argument(std::format("{} expects an integer value", name));
        }
        if (float_value < static_cast<double>(std::numeric_limits<Int>::min()) ||
            float_value > static_cast<double>(std::numeric_limits<Int>::max())) {
          throw std::out_of_range(std::format("{} out of Int64 range", name));
        }
        return static_cast<Int>(float_value);
      });
}

[[nodiscard]] inline auto as_index_strict(const Value& val, const std::string_view name) -> std::size_t {
  const Int index_value = as_int_value_strict(val, name);
  if (index_value < 0) { throw std::out_of_range(std::format("{} expects non-negative integer", name)); }
  return static_cast<std::size_t>(index_value);
}

// Python make_node semantics: when one argument is provided, the callee sees
// the scalar value instead of a 1-tuple wrapper.
[[nodiscard]] inline auto unwrap_singleton_arg(Value val) -> Value {
  if (val.HasArray()) {
    if (const auto& arr = as_array(val); arr.Size() == 1) { return *arr.TryGet(0); }
  }
  return val;
}

// Value printing

[[nodiscard]] inline auto format_number(const Number& number_value) -> std::string {
  return number_value.Visit([](const Int signed_value) -> std::string { return std::format("{}", signed_value); },
                            [](const UInt unsigned_value) -> std::string {
                              return std::format("{}", unsigned_value);
                            },
                            [](const Float float_value) -> std::string {
                              return std::format("{}", float_value);
                            });
}

// Print a Value as a scalar/tuple repr for the C++ runtime.
inline void print_value_repr(std::ostream& os, const Value& value) {
  value.Visit(
      [&](const Array& arr) -> void {
        os << '(';
        for (std::size_t element_index = 0; element_index < arr.Size(); ++element_index) {
          if (element_index > 0) { os << ", "; }
          print_value_repr(os, *arr.TryGet(element_index));
        }
        if (arr.Size() == 1) { os << ','; }
        os << ')';
      },
      [&](const Object& obj) -> void {
        // Objects are the backing store for Fleaux dicts.
        // Keys carry a type prefix ("s:", "n:", "b:", "z:") so that
        // different key types never collide (e.g. int 1 vs string "1").
        os << '{';
        // Collect and sort internal keys so output is deterministic.
        std::vector<std::string> sorted_keys;
        sorted_keys.reserve(obj.Size());
        for (const auto& internal_key : obj | std::views::keys) sorted_keys.push_back(internal_key);
        std::ranges::sort(sorted_keys);
        bool first = true;
        for (const auto& ikey : sorted_keys) {
          if (!first) os << ", ";
          first = false;
          // Strip the type prefix and render the original key value.
          if (ikey.size() >= 2 && ikey[1] == ':') {
            const char tag = ikey[0];
            const auto& payload = ikey.substr(2);
            if (tag == 'b')
              os << (payload == "true" ? "True" : "False");
            else if (tag == 'z')
              os << "Null";
            else
              os << payload;  // 'n' (number) or unknown
          } else {
            os << ikey;  // legacy key without a prefix
          }
          os << ": ";
          if (const auto got = obj.TryGet(ikey)) print_value_repr(os, *got);
        }
        os << '}';
      },
      [&](const Generic& /*gen*/) -> void { os << "<generic>"; },
      [&](const ValueNode& vn) -> void {
        vn.Visit([&](const Null&) -> void { os << "Null"; },
                 [&](const Bool val) -> void { os << (val ? "True" : "False"); },
                 [&](const Number& val) -> void { os << format_number(val); },
                 [&](const String& val) -> void { os << val; });
      });
}

// Tuple args are printed as varargs, space-separated.
inline void print_value_varargs(std::ostream& os, const Value& value) {
  if (!value.HasArray()) {
    print_value_repr(os, value);
    return;
  }

  const auto& arr = as_array(value);
  for (std::size_t element_index = 0; element_index < arr.Size(); ++element_index) {
    if (element_index > 0) { os << ' '; }
    print_value_repr(os, *arr.TryGet(element_index));
  }
}

inline auto to_string(const Value& value) -> std::string {
  std::ostringstream oss;
  print_value_repr(oss, value);
  return oss.str();
}

[[nodiscard]] inline auto type_name(const Value& value) -> std::string {
  if (callable_id_from_value(value).has_value()) { return "Callable"; }
  if (handle_id_from_value(value).has_value()) { return "Handle"; }

  return value.Visit(
      [](const Array&) -> std::string { return "Tuple"; },
      [](const Generic&) -> std::string { return "Generic"; },
      [](const Object&) -> std::string { return "Dict"; },
      [](const ValueNode& vn) -> std::string {
        return vn.Visit(
            [](const Null&) -> std::string { return "Null"; },
            [](const Bool&) -> std::string { return "Bool"; },
            [](const Number& number_value) -> std::string {
              return number_value.Visit([](const Int) -> std::string { return "Int64"; },
                                        [](const UInt) -> std::string { return "UInt64"; },
                                        [](const Float) -> std::string { return "Float64"; });
            },
            [](const String&) -> std::string { return "String"; });
      });
}

enum class SortTag {
  Array,
  Generic,
  Object,
  Null,
  Bool,
  Number,
  String,
};

[[nodiscard]] inline auto sort_tag_of(const Value& value) -> SortTag {
  const auto tag = value.Visit([](const Array&) -> SortTag { return SortTag::Array; },
                           [](const Generic&) -> SortTag { return SortTag::Generic; },
                           [](const Object&) -> SortTag { return SortTag::Object; },
                           [](const ValueNode& vn) -> SortTag {
                             return vn.Visit([](const Null&) -> SortTag { return SortTag::Null; },
                                             [](const Bool&) -> SortTag { return SortTag::Bool; },
                                             [](const Number&) -> SortTag { return SortTag::Number; },
                                             [](const String&) -> SortTag { return SortTag::String; });
                           });
  return tag;
}

[[nodiscard]] inline auto compare_values_for_sort(const Value& lhs, const Value& rhs) -> int;

[[nodiscard]] inline auto compare_arrays_for_sort(const Array& lhs, const Array& rhs) -> int {
  const std::size_t min_size = std::min(lhs.Size(), rhs.Size());
  for (std::size_t element_index = 0; element_index < min_size; ++element_index) {
    if (const int cmp = compare_values_for_sort(*lhs.TryGet(element_index), *rhs.TryGet(element_index)); cmp != 0) {
      return cmp;
    }
  }
  if (lhs.Size() < rhs.Size()) { return -1; }
  if (lhs.Size() > rhs.Size()) { return 1; }
  return 0;
}

[[nodiscard]] inline auto compare_values_for_sort(const Value& lhs, const Value& rhs) -> int {
  const SortTag lhs_tag = sort_tag_of(lhs);
  if (const SortTag rhs_tag = sort_tag_of(rhs); lhs_tag != rhs_tag) {
    throw std::invalid_argument{"TupleSort supports homogeneous comparable values only"};
  }

  switch (lhs_tag) {
    case SortTag::Null:
      return 0;
    case SortTag::Bool: {
      const bool lhs_bool = as_bool(lhs);
      const bool rhs_bool = as_bool(rhs);
      return (lhs_bool < rhs_bool) ? -1 : ((lhs_bool > rhs_bool) ? 1 : 0);
    }
    case SortTag::Number: {
      return compare_numbers(lhs, rhs);
    }
    case SortTag::String: {
      const String& lhs_string = as_string(lhs);
      const String& rhs_string = as_string(rhs);
      return (lhs_string < rhs_string) ? -1 : ((lhs_string > rhs_string) ? 1 : 0);
    }
    case SortTag::Array:
      return compare_arrays_for_sort(as_array(lhs), as_array(rhs));
    case SortTag::Generic:
      throw std::invalid_argument{"TupleSort does not support sorting generic values"};
    case SortTag::Object:
      throw std::invalid_argument{"TupleSort does not support sorting object values"};
  }
  throw std::invalid_argument{"TupleSort internal error"};
}

[[nodiscard]] inline auto require_args(const Value& arg, std::size_t expected_count, const char* name) -> const Array& {
  const auto& args = as_array(arg);
  if (args.Size() != expected_count) {
    throw std::invalid_argument(std::format("{} expects {} arguments", name, expected_count));
  }
  return args;
}

[[nodiscard]] inline auto as_path_string_unary(Value arg) -> std::string {
  return to_string(unwrap_singleton_arg(std::move(arg)));
}

[[nodiscard]] inline auto random_suffix(const std::size_t size = 12) -> std::string {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  static constexpr auto alphabet = std::to_array("abcdefghijklmnopqrstuvwxyz0123456789");
  std::uniform_int_distribution<std::size_t> dist(0, sizeof(alphabet) - 2);
  std::string out;
  out.reserve(size);
  for (std::size_t char_index = 0; char_index < size; ++char_index) {
    out.push_back(alphabet[dist(rng)]);
  }
  return out;
}

inline void throw_if_filesystem_error(const std::error_code& ec, std::string_view op) {
  if (ec) { throw std::runtime_error(std::format("{} failed: {}", op, ec.message())); }
}

[[nodiscard]] inline auto trim_left(std::string text) -> std::string {
  const auto it =
      std::ranges::find_if_not(text, [](const unsigned char ch) -> bool { return std::isspace(ch) != 0; });
  text.erase(text.begin(), it);
  return text;
}

[[nodiscard]] inline auto trim_right(std::string text) -> std::string {
  const auto it = std::ranges::find_if_not(std::views::reverse(text),
                                           [](const unsigned char ch) -> bool { return std::isspace(ch) != 0; });
  text.erase(it.base(), text.end());
  return text;
}

[[nodiscard]] inline auto format_value_plain(const Value& value) -> std::string {
#if FLEAUX_HAS_STD_FORMAT
  if (const auto num = value.TryGetNumber()) {
    return num->Visit(
        [&](const Int signed_value) -> std::string {
          return std::vformat("{}", std::make_format_args(signed_value));
        },
        [&](const UInt unsigned_value) -> std::string {
          return std::vformat("{}", std::make_format_args(unsigned_value));
        },
        [&](const Float float_value) -> std::string {
          return std::vformat("{}", std::make_format_args(float_value));
        });
  }
  if (const auto bool_value = value.TryGetBool()) {
    return std::vformat("{}", std::make_format_args(*bool_value));
  }
  if (const auto string_value = value.TryGetString()) {
    return std::vformat("{}", std::make_format_args(*string_value));
  }
#endif
  return to_string(value);
}

[[nodiscard]] inline auto format_value_with_spec(const Value& value, const std::string& spec) -> std::string {
#if FLEAUX_HAS_STD_FORMAT
  const std::string single_fmt = std::format("{{0:{}}}", spec);
  if (const auto num = value.TryGetNumber()) {
    return num->Visit(
        [&](const Int signed_value) -> std::string {
          return std::vformat(single_fmt, std::make_format_args(signed_value));
        },
        [&](const UInt unsigned_value) -> std::string {
          return std::vformat(single_fmt, std::make_format_args(unsigned_value));
        },
        [&](const Float float_value) -> std::string {
          return std::vformat(single_fmt, std::make_format_args(float_value));
        });
  }
  if (const auto bool_value = value.TryGetBool()) {
    return std::vformat(single_fmt, std::make_format_args(*bool_value));
  }
  if (const auto string_value = value.TryGetString()) {
    return std::vformat(single_fmt, std::make_format_args(*string_value));
  }
  const std::string repr = to_string(value);
  return std::vformat(single_fmt, std::make_format_args(repr));
#else
  throw std::invalid_argument{"Format specs require <format> support"};
#endif
}

[[nodiscard]] inline auto format_values_fallback(const std::string& fmt, const std::vector<Value>& values)
    -> std::string {
  std::string out;
  out.reserve(fmt.size());

  std::size_t next_auto_index = 0;
  bool saw_auto_index = false;
  bool saw_manual_index = false;

  for (std::size_t cursor = 0; cursor < fmt.size();) {
    const std::size_t next_special = fmt.find_first_of("{}", cursor);
    if (next_special == std::string::npos) {
      out.append(fmt, cursor, fmt.size() - cursor);
      break;
    }

    if (next_special > cursor) {
      out.append(fmt, cursor, next_special - cursor);
      cursor = next_special;
      continue;
    }

    const char ch = fmt[cursor];
    if (ch == '{') {
      if (cursor + 1 < fmt.size() && fmt[cursor + 1] == '{') {
        out.push_back('{');
        cursor += 2;
        continue;
      }

      const std::size_t close = fmt.find('}', cursor + 1);
      if (close == std::string::npos) { throw std::invalid_argument{"Printf format string has unmatched '{'"}; }

      const std::string field = fmt.substr(cursor + 1, close - (cursor + 1));
      std::size_t index = 0;
      std::string spec;

      const std::size_t colon = field.find(':');
      const std::string index_part = (colon == std::string::npos) ? field : field.substr(0, colon);
      if (colon != std::string::npos) { spec = field.substr(colon + 1); }

      if (index_part.empty()) {
        if (saw_manual_index) {
          throw std::invalid_argument{"Printf format cannot mix automatic and manual field numbering"};
        }
        saw_auto_index = true;
        index = next_auto_index++;
      } else {
        if (saw_auto_index) {
          throw std::invalid_argument{"Printf format cannot mix automatic and manual field numbering"};
        }
        saw_manual_index = true;
        for (const char c : index_part) {
          if (!std::isdigit(static_cast<unsigned char>(c))) {
            throw std::invalid_argument{"Format supports only '{}'/'{N}' with optional ':spec'"};
          }
        }
        index = static_cast<std::size_t>(std::stoull(index_part));
      }

      if (index >= values.size()) { throw std::invalid_argument{"Format string references a missing argument"}; }

      if (spec.empty()) {
        out += format_value_plain(values[index]);
      } else {
        out += format_value_with_spec(values[index], spec);
      }

      cursor = close + 1;
      continue;
    }

    if (ch == '}') {
      if (cursor + 1 < fmt.size() && fmt[cursor + 1] == '}') {
        out.push_back('}');
        cursor += 2;
        continue;
      }
      throw std::invalid_argument{"Printf format string has unmatched '}'"};
    }
  }
  return out;
}

[[nodiscard]] inline auto format_values(const std::string& fmt, const std::vector<Value>& values) -> std::string {
  return format_values_fallback(fmt, values);
}

[[nodiscard]] inline auto format_values(const std::string& fmt, const std::vector<std::string>& values) -> std::string {
  std::vector<Value> wrapped;
  wrapped.reserve(values.size());
  for (const auto& s : values) { wrapped.push_back(make_string(s)); }
  return format_values(fmt, wrapped);
}

}  // namespace fleaux::runtime
