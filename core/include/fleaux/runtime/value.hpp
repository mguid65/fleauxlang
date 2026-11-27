#pragma once

// The runtime layer: the helpers builtins and the VM use to build, read, print, and format Values.
//
// Builtins take their arguments as one Value and return one: a Fleaux call with several arguments passes them as a
// tuple. value_core.hpp defines Value itself.

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <cmath>
#include <concepts>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <random>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "fleaux/common/overloaded.hpp"
#include "fleaux/runtime/value_core.hpp"

namespace fleaux::runtime {

// An extension function bound to a native function declaration. It receives the declared parameters as one tuple and
// throws std::exception on failure.
using NativeFunction = std::function<Value(const Value&)>;

// Execution state

// What one run of a module needs beyond its values: the native functions it declares, bound before it starts, and
// the tasks it spawns, which are settled when it ends.
struct RuntimeExecutionState {
  RuntimeExecutionState() = default;
  RuntimeExecutionState(const RuntimeExecutionState&) = delete;
  auto operator=(const RuntimeExecutionState&) -> RuntimeExecutionState& = delete;
  RuntimeExecutionState(RuntimeExecutionState&&) = delete;
  auto operator=(RuntimeExecutionState&&) -> RuntimeExecutionState& = delete;
  ~RuntimeExecutionState() = default;

  // The running module's native functions, parallel to Module::native_functions.
  std::vector<NativeFunction> native_functions{};

  // Tasks started with Std.Task.Spawn; see TaskScope in builtins_parallel.hpp.
  std::mutex task_mutex{};
  std::vector<Value> spawned_tasks{};
};

[[nodiscard]] inline auto default_runtime_execution_state() -> RuntimeExecutionState& {
  static RuntimeExecutionState state;
  return state;
}

struct RuntimeExecutionStateThreadContext {
  std::reference_wrapper<RuntimeExecutionState> active;
};

[[nodiscard]] auto runtime_execution_state_thread_context() -> RuntimeExecutionStateThreadContext&;

[[nodiscard]] inline auto runtime_execution_state() -> RuntimeExecutionState& {
  return runtime_execution_state_thread_context().active.get();
}

class ActiveRuntimeExecutionStateScope {
public:
  explicit ActiveRuntimeExecutionStateScope(RuntimeExecutionState& state)
      : previous_(runtime_execution_state_thread_context().active) {
    runtime_execution_state_thread_context().active = std::ref(state);
  }

  ActiveRuntimeExecutionStateScope(const ActiveRuntimeExecutionStateScope&) = delete;
  auto operator=(const ActiveRuntimeExecutionStateScope&) -> ActiveRuntimeExecutionStateScope& = delete;

  ~ActiveRuntimeExecutionStateScope() { runtime_execution_state_thread_context().active = previous_; }

private:
  std::reference_wrapper<RuntimeExecutionState> previous_;
};

// Streams and process arguments

struct RuntimeThreadContext {
  std::reference_wrapper<std::ostream> output{std::cout};
  std::reference_wrapper<std::istream> input{std::cin};
  std::vector<std::string> process_args{};
  std::optional<std::reference_wrapper<const std::vector<std::string>>> process_args_override{};
  // True on the task pool's workers, which run a task they wait for themselves (await_task in builtins_parallel.hpp).
  bool pool_worker{false};
};

[[nodiscard]] auto runtime_thread_context() -> RuntimeThreadContext&;

[[nodiscard]] inline auto runtime_output_stream() -> std::ostream& { return runtime_thread_context().output.get(); }

// Writes text to the running code's output stream, under one lock for the process. Parallel items and tasks print to
// the stream of the run that started them, so several threads can print to one stream at once; the lock keeps the
// stream whole, and each call's text in one piece.
inline void write_runtime_output(const std::string_view text, const bool flush = false) {
  static std::mutex mutex;
  const std::scoped_lock lock(mutex);
  std::ostream& output = runtime_output_stream();
  output << text;
  if (flush) {
    output.flush();
  }
}

[[nodiscard]] inline auto runtime_input_stream() -> std::istream& { return runtime_thread_context().input.get(); }

class RuntimeOutputStreamScope {
public:
  explicit RuntimeOutputStreamScope(std::ostream& stream) : previous_(runtime_thread_context().output) {
    runtime_thread_context().output = std::ref(stream);
  }

  RuntimeOutputStreamScope(const RuntimeOutputStreamScope&) = delete;
  auto operator=(const RuntimeOutputStreamScope&) -> RuntimeOutputStreamScope& = delete;

  ~RuntimeOutputStreamScope() { runtime_thread_context().output = previous_; }

private:
  std::reference_wrapper<std::ostream> previous_;
};

class RuntimeInputStreamScope {
public:
  explicit RuntimeInputStreamScope(std::istream& stream) : previous_(runtime_thread_context().input) {
    runtime_thread_context().input = std::ref(stream);
  }

  RuntimeInputStreamScope(const RuntimeInputStreamScope&) = delete;
  auto operator=(const RuntimeInputStreamScope&) -> RuntimeInputStreamScope& = delete;

  ~RuntimeInputStreamScope() { runtime_thread_context().input = previous_; }

private:
  std::reference_wrapper<std::istream> previous_;
};

inline auto process_args_storage() -> std::vector<std::string>& { return runtime_thread_context().process_args; }

inline void set_process_args(std::vector<std::string> args) { process_args_storage() = std::move(args); }

inline void set_process_args(const int argc, char** argv) {
  std::vector<std::string> args;
  if (argc <= 0) {
    set_process_args(std::move(args));
    return;
  }
  args.reserve(static_cast<std::size_t>(argc));
  for (int arg_index = 0; arg_index < argc; ++arg_index) {
    args.emplace_back((argv != nullptr && argv[arg_index] != nullptr) ? argv[arg_index] : "");
  }
  set_process_args(std::move(args));
}

[[nodiscard]] inline auto get_process_args() -> std::vector<std::string> {
  if (const auto& args = runtime_thread_context().process_args_override; args.has_value()) {
    return args->get();
  }
  return process_args_storage();
}

class RuntimeProcessArgsOverrideScope {
public:
  explicit RuntimeProcessArgsOverrideScope(const std::vector<std::string>& args)
      : previous_(runtime_thread_context().process_args_override) {
    runtime_thread_context().process_args_override = std::cref(args);
  }

  RuntimeProcessArgsOverrideScope(const RuntimeProcessArgsOverrideScope&) = delete;
  auto operator=(const RuntimeProcessArgsOverrideScope&) -> RuntimeProcessArgsOverrideScope& = delete;

  ~RuntimeProcessArgsOverrideScope() { runtime_thread_context().process_args_override = previous_; }

private:
  std::optional<std::reference_wrapper<const std::vector<std::string>>> previous_;
};

class RuntimeProcessArgsScope {
public:
  explicit RuntimeProcessArgsScope(std::vector<std::string> args) : previous_(get_process_args()) {
    set_process_args(std::move(args));
  }

  RuntimeProcessArgsScope(const RuntimeProcessArgsScope&) = delete;
  auto operator=(const RuntimeProcessArgsScope&) -> RuntimeProcessArgsScope& = delete;

  ~RuntimeProcessArgsScope() { set_process_args(std::move(previous_)); }

private:
  std::vector<std::string> previous_{};
};

// Construction

[[nodiscard]] inline auto make_null() -> Value { return Value{}; }
[[nodiscard]] inline auto make_bool(const Bool value) -> Value { return Value::from_bool(value); }
[[nodiscard]] inline auto make_int(const Int value) -> Value { return Value::from_int(value); }
[[nodiscard]] inline auto make_uint(const UInt value) -> Value { return Value::from_uint(value); }
[[nodiscard]] inline auto make_float(const Float value) -> Value { return Value::from_float(value); }
[[nodiscard]] inline auto make_string(String value) -> Value { return Value::from_string(std::move(value)); }
[[nodiscard]] inline auto make_random_generator(const UInt state) -> Value { return Value::from_random_state(state); }
[[nodiscard]] inline auto make_dict(Dict dict = {}) -> Value { return Value::from_dict(std::move(dict)); }

// A tuple of copies of the items of a sized range of values, such as a TupleView or a slice of one. A range of move
// iterators moves the items instead.
template <std::ranges::sized_range Items>
  requires std::constructible_from<Value, std::ranges::range_reference_t<Items>>
[[nodiscard]] auto make_tuple_from(Items&& items) -> Value {
  const auto count = static_cast<std::size_t>(std::ranges::size(items));
  if (count == 0U) {
    return empty_tuple();
  }
  TupleBuilder tuple(count);
  for (auto&& item : items) {
    tuple.push(std::forward<decltype(item)>(item));
  }
  return std::move(tuple).finish();
}

// A vector would be copied item by item here: build the tuple with a TupleBuilder instead, or move the items with
// make_tuple_moved.
auto make_tuple_from(std::vector<Value>&& items) -> Value = delete;

// A tuple of `items`, moved out of where they are.
[[nodiscard]] inline auto make_tuple_moved(const std::span<Value> items) -> Value {
  if (items.empty()) {
    return empty_tuple();
  }
  TupleBuilder tuple(items.size());
  for (Value& item : items) {
    tuple.push(std::move(item));
  }
  return std::move(tuple).finish();
}

// Pushes the items of `tuple`: moved out of a tuple `tuple` alone refers to, copied out of a shared one.
inline void push_tuple_items(TupleBuilder& out, Value tuple) {
  if (tuple.object()->unique()) {
    for (Value& item : tuple.mutable_tuple()) {
      out.push(std::move(item));
    }
    return;
  }
  for (const Value& item : tuple.tuple()) {
    out.push(item);
  }
}

// The items of `sequence` from `first` up to `last` (first <= last <= size), as a tuple. A tuple `sequence` alone
// refers to is cut in its own block; only the kept range of a shared one is copied.
[[nodiscard]] inline auto keep_tuple_items(Value sequence, const std::size_t first, const std::size_t last) -> Value {
  if (!sequence.object()->unique()) {
    return make_tuple_from(sequence.tuple().subspan(first, last - first));
  }
  const std::size_t size = sequence.tuple().size();
  TupleBuilder out = TupleBuilder::reopen(std::move(sequence), size);
  out.erase(last, size);
  out.erase(0, first);
  return std::move(out).finish();
}

// The items of `sequence` without those from `first` up to `last` (first <= last <= size), as a tuple. A tuple
// `sequence` alone refers to is cut in its own block; the rest of a shared one is copied.
[[nodiscard]] inline auto remove_tuple_items(Value sequence, const std::size_t first, const std::size_t last) -> Value {
  if (sequence.object()->unique()) {
    TupleBuilder out = TupleBuilder::reopen(std::move(sequence), 0U);
    out.erase(first, last);
    return std::move(out).finish();
  }
  const TupleView items = sequence.tuple();
  TupleBuilder out(items.size() - (last - first));
  for (const Value& item : items.first(first)) {
    out.push(item);
  }
  for (const Value& item : items.subspan(last)) {
    out.push(item);
  }
  return std::move(out).finish();
}

template <typename MaybeValue>
concept ValueLike = std::same_as<std::remove_cvref_t<MaybeValue>, Value>;

// A tuple of the arguments, each one a Value.
template <ValueLike... Values>
[[nodiscard]] auto make_tuple(Values&&... values) -> Value {
  if constexpr (sizeof...(Values) == 0U) {
    return empty_tuple();
  } else {
    TupleBuilder tuple(sizeof...(Values));
    (tuple.push(std::forward<Values>(values)), ...);
    return std::move(tuple).finish();
  }
}

// A function value that calls `function`, any C++ callable taking and returning a Value.
template <typename Function>
[[nodiscard]] auto make_function(Function&& function) -> Value {
  using Stored = std::decay_t<Function>;
  return Value::adopt(ValueKind::kFunction, new FunctionCallable<Stored>(std::forward<Function>(function)));
}

// A function value for a Callable allocated with new, whose one reference the value takes over.
[[nodiscard]] inline auto adopt_function(Callable* callable) -> Value {
  return Value::adopt(ValueKind::kFunction, callable);
}

// Reading values. Each helper throws when the value is not of the kind it reads.

[[nodiscard]] inline auto kind_error(const std::string_view expected) -> std::runtime_error {
  return std::runtime_error{std::format("fleaux::runtime: expected {}", expected)};
}

[[nodiscard]] inline auto as_bool(const Value& value) -> Bool {
  if (!value.is_bool()) {
    throw kind_error("Bool");
  }
  return value.bool_value();
}

[[nodiscard]] inline auto as_string(const Value& value) -> const String& {
  if (!value.is_string()) {
    throw kind_error("String");
  }
  return value.string_value();
}

[[nodiscard]] inline auto as_tuple(const Value& value) -> TupleView {
  if (!value.is_tuple()) {
    throw kind_error("Tuple");
  }
  return value.tuple();
}

[[nodiscard]] inline auto as_dict(const Value& value) -> const Dict& {
  if (!value.is_dict()) {
    throw kind_error("Dict");
  }
  return value.dict();
}

[[nodiscard]] inline auto as_callable(const Value& value) -> const Callable& {
  if (const Callable* callable = value.callable_if_function(); callable != nullptr) {
    return *callable;
  }
  throw kind_error("a function");
}

// Item `index` of a tuple.
[[nodiscard]] inline auto tuple_at(const Value& value, const std::size_t index) -> const Value& {
  const TupleView items = as_tuple(value);
  if (index >= items.size()) {
    throw std::out_of_range{"fleaux::runtime: tuple index out of range"};
  }
  return items[index];
}

// Item `index` of a tuple the caller owns: moved out when nothing else refers to the tuple, and copied when other
// values share it.
[[nodiscard]] inline auto take_tuple_item(Value& value, const std::size_t index) -> Value {
  const Value& item = tuple_at(value, index);
  if (!value.object()->unique()) {
    return item;
  }
  return std::move(value.mutable_tuple()[index]);
}

// The arguments of a call of a builtin that takes other than one: values the call owns, which the builtin reads
// (args[i]) or moves out (args.take(i)). The VM passes the values on its stack. A call that has its arguments as one
// tuple (an argument list with spreads, a builtin called as a function value) passes the tuple, which the arguments
// keep alive and lend the items of: a tuple nothing else refers to lends its own, and a shared one is copied first.
// A value that is not a tuple is one argument.
class Arguments {
public:
  explicit Arguments(const std::span<Value> values) noexcept : values_(values) {}

  // NOLINTNEXTLINE(google-explicit-constructor): a builtin taking Arguments is called with an argument tuple as well.
  Arguments(Value tuple) : held_(std::move(tuple)) {
    values_ = held_.is_tuple() ? held_.mutable_tuple() : std::span<Value>{&held_, 1U};
  }

  Arguments(const Arguments&) = delete;
  auto operator=(const Arguments&) -> Arguments& = delete;
  Arguments(Arguments&& other) noexcept : held_(std::move(other.held_)), values_(other.values_) {
    // A lone value is held here, so the view follows it.
    if (other.values_.data() == &other.held_) {
      values_ = std::span<Value>{&held_, 1U};
    }
  }
  auto operator=(Arguments&&) -> Arguments& = delete;
  ~Arguments() = default;

  [[nodiscard]] auto size() const noexcept -> std::size_t { return values_.size(); }

  [[nodiscard]] auto operator[](const std::size_t index) const -> const Value& { return at(index); }

  // Moves argument `index` out; the builtin must not read it again.
  [[nodiscard]] auto take(const std::size_t index) const -> Value { return std::move(at(index)); }

  [[nodiscard]] auto items() const noexcept -> TupleView { return values_; }

private:
  [[nodiscard]] auto at(const std::size_t index) const -> Value& {
    if (index >= values_.size()) {
      throw std::out_of_range{"fleaux::runtime: argument index out of range"};
    }
    return values_[index];
  }

  // The argument tuple or lone value the arguments came as, if they did not come as values on the VM stack.
  Value held_;
  std::span<Value> values_;
};

// Calls `int_case`, `uint_case`, or `float_case` (or one overload set) with a number's value.
template <typename... Cases>
auto visit_number(const Value& value, Cases&&... cases) -> decltype(auto) {
  const common::overloaded visitor{std::forward<Cases>(cases)...};
  switch (value.kind()) {
    case ValueKind::kInt:
      return visitor(value.int_value());
    case ValueKind::kUInt:
      return visitor(value.uint_value());
    case ValueKind::kFloat:
      return visitor(value.float_value());
    default:
      throw kind_error("Int64, UInt64, or Float64");
  }
}

// Numbers

[[nodiscard]] inline auto to_double(const Value& value) -> double {
  return visit_number(value, [](const auto number) -> double { return static_cast<double>(number); });
}

[[nodiscard]] inline auto is_uint_number(const Value& value) -> bool { return value.is_uint(); }
[[nodiscard]] inline auto is_int_number(const Value& value) -> bool { return value.is_int(); }
[[nodiscard]] inline auto is_float_number(const Value& value) -> bool { return value.is_float(); }

[[nodiscard]] inline auto compare_numbers(const Value& lhs, const Value& rhs) -> int {
  const auto order = [](const auto left, const auto right) -> int {
    return (left < right) ? -1 : ((left > right) ? 1 : 0);
  };
  if (lhs.is_float() || rhs.is_float()) {
    return order(to_double(lhs), to_double(rhs));
  }
  if (lhs.is_int() && rhs.is_int()) {
    return order(lhs.int_value(), rhs.int_value());
  }
  if (lhs.is_uint() && rhs.is_uint()) {
    return order(lhs.uint_value(), rhs.uint_value());
  }
  if (lhs.is_int() && rhs.is_uint()) {
    return lhs.int_value() < 0 ? -1 : order(static_cast<UInt>(lhs.int_value()), rhs.uint_value());
  }
  if (lhs.is_uint() && rhs.is_int()) {
    return rhs.int_value() < 0 ? 1 : order(lhs.uint_value(), static_cast<UInt>(rhs.int_value()));
  }
  throw kind_error("Int64, UInt64, or Float64");
}

// The integer index embedded in a number, converted as by a C++ cast.
[[nodiscard]] inline auto as_index(const Value& value) -> std::size_t {
  return visit_number(value, [](const auto number) -> std::size_t { return static_cast<std::size_t>(number); });
}

[[nodiscard]] inline auto as_int_value(const Value& value) -> Int {
  return visit_number(value, [](const auto number) -> Int { return static_cast<Int>(number); });
}

// Reads of a parameter whose numeric type the Std signature fixes. The type checker, or the host entry check for
// host calls, guarantees the value's kind, so these do not validate it. A C++ caller that bypasses both and passes
// another numeric kind gets the value converted as by a C++ cast.
[[nodiscard]] inline auto int64_value(const Value& value) -> Int {
  return value.is_int() ? value.int_value() : as_int_value(value);
}

[[nodiscard]] inline auto uint64_value(const Value& value) -> UInt {
  if (value.is_uint()) {
    return value.uint_value();
  }
  return visit_number(value, [](const auto number) -> UInt { return static_cast<UInt>(number); });
}

// A UInt as a container index. On targets with a 32-bit std::size_t, such as wasm32, a value too large for it becomes
// the largest index, which no container reaches, so bounds checks still reject it instead of wrapping around.
[[nodiscard]] inline auto to_index(const UInt value) -> std::size_t {
  if constexpr (sizeof(std::size_t) < sizeof(UInt)) {
    if (value > std::numeric_limits<std::size_t>::max()) {
      return std::numeric_limits<std::size_t>::max();
    }
  }
  return static_cast<std::size_t>(value);
}

[[nodiscard]] inline auto uint64_index(const Value& value) -> std::size_t { return to_index(uint64_value(value)); }

// An Int64 position a builtin reads or writes at (Std.ElementAt's index), as a container index. A negative one is past
// the end of every container, so the builtin's bounds check rejects it, and its message shows the index as written.
[[nodiscard]] inline auto position_index(const Value& value) -> std::size_t {
  if (value.is_int()) {
    const Int position = value.int_value();
    return position < 0 ? std::numeric_limits<std::size_t>::max() : to_index(static_cast<UInt>(position));
  }
  return uint64_index(value);
}

// An Int64 count, length, bound, or duration a builtin takes, named `what` as its parameter is: a negative one is an
// error.
[[nodiscard]] inline auto count_index(const Value& value, const std::string_view what) -> std::size_t {
  if (value.is_int()) {
    const Int count = value.int_value();
    if (count < 0) {
      throw std::out_of_range(std::format("{} {} is negative", what, count));
    }
    return to_index(static_cast<UInt>(count));
  }
  return uint64_index(value);
}

[[nodiscard]] inline auto as_int_value_strict(const Value& value) -> Int {
  return visit_number(
      value, [&](const Int signed_value) -> Int { return signed_value; },
      [&](const UInt unsigned_value) -> Int {
        if (unsigned_value > static_cast<UInt>(std::numeric_limits<Int>::max())) {
          throw std::out_of_range(std::format("the UInt64 {} is out of Int64 range", unsigned_value));
        }
        return static_cast<Int>(unsigned_value);
      },
      [&](const Float float_value) -> Int {
        if (!std::isfinite(float_value) || std::floor(float_value) != float_value) {
          throw std::invalid_argument(std::format("the Float64 {} is not an integer", float_value));
        }
        if (float_value < static_cast<double>(std::numeric_limits<Int>::min()) ||
            float_value > static_cast<double>(std::numeric_limits<Int>::max())) {
          throw std::out_of_range(std::format("the Float64 {} is out of Int64 range", float_value));
        }
        return static_cast<Int>(float_value);
      });
}

// A call with one argument passes that argument itself rather than a one-item tuple.
// Function values

[[nodiscard]] inline auto invoke_callable(const Value& function, Value argument) -> Value {
  return as_callable(function).call(std::move(argument));
}

[[nodiscard]] inline auto invoke_binary_callable(const Value& function, Value lhs, Value rhs) -> Value {
  return as_callable(function).call_binary(std::move(lhs), std::move(rhs));
}

// A builtin node: anything that can be called with a Value and returns one.
template <typename Node>
concept NodeLike = requires(Node&& node, Value arg) {
  { std::invoke(std::forward<Node>(node), std::move(arg)) } -> std::same_as<Value>;
};

// The pipeline `value | node` calls the node with the value.
template <NodeLike Node>
auto operator|(Value arg, Node&& node) -> Value {
  return std::invoke(std::forward<Node>(node), std::move(arg));
}

inline auto operator|(Value arg, const Value& function) -> Value { return invoke_callable(function, std::move(arg)); }

// File handles

// Opens `path` with a mode made of the letters r, w, a, and b, or gives nothing when the file cannot be opened. With
// none of r, w, or a it reads.
[[nodiscard]] inline auto open_file_handle(const std::string& path, const std::string& mode) -> std::optional<Value> {
  auto* handle = new FileHandleObject;
  Value value = Value::adopt(ValueKind::kFileHandle, handle);
  handle->path = path;
  handle->mode = mode;
  std::ios::openmode flags{};
  const bool is_read = mode.find('r') != std::string::npos;
  const bool is_write = mode.find('w') != std::string::npos;
  const bool is_append = mode.find('a') != std::string::npos;
  if (is_read) {
    flags |= std::ios::in;
  }
  if (is_write) {
    flags |= std::ios::out | std::ios::trunc;
  }
  if (is_append) {
    flags |= std::ios::out | std::ios::app;
  }
  if (mode.find('b') != std::string::npos) {
    flags |= std::ios::binary;
  }
  if (!is_read && !is_write && !is_append) {
    flags |= std::ios::in;
  }
  handle->stream.open(path, flags);
  if (!handle->stream.is_open()) {
    return std::nullopt;
  }
  return value;
}

// The open file a handle refers to. The value is a FileHandle by type; what can still fail is that it was closed.
[[nodiscard]] inline auto require_handle(const Value& handle) -> FileHandleObject& {
  if (!handle.is_file_handle() || handle.file_handle().closed) {
    throw std::runtime_error{"the file handle is closed"};
  }
  return handle.file_handle();
}

// Printing

// Appends a number's text to `out`.
inline void append_number(std::string& out, const Value& number) {
  visit_number(number, [&out](const auto value) { std::format_to(std::back_inserter(out), "{}", value); });
}

[[nodiscard]] inline auto format_number(const Value& number) -> std::string {
  std::string text;
  append_number(text, number);
  return text;
}

// Appends the text of `value` to `out`: what Std.Println shows and Std.ToString returns. Values are written into a
// string rather than a stream, because a stream costs more to set up than most values cost to write.
inline void append_value_repr(std::string& out, const Value& value) {
  switch (value.kind()) {
    case ValueKind::kNull:
      out += "null";
      return;
    case ValueKind::kBool:
      out += value.bool_value() ? "True" : "False";
      return;
    case ValueKind::kInt:
    case ValueKind::kUInt:
    case ValueKind::kFloat:
      append_number(out, value);
      return;
    case ValueKind::kString:
      out += value.string_value();
      return;
    case ValueKind::kTuple: {
      const TupleView items = value.tuple();
      out += '(';
      for (std::size_t index = 0; index < items.size(); ++index) {
        if (index > 0) {
          out += ", ";
        }
        append_value_repr(out, items[index]);
      }
      if (items.size() == 1) {
        out += ',';
      }
      out += ')';
      return;
    }
    case ValueKind::kDict: {
      out += '{';
      bool first = true;
      for (const auto& [key, item] : value.dict()) {
        if (!first) {
          out += ", ";
        }
        first = false;
        append_value_repr(out, key);
        out += ": ";
        append_value_repr(out, item);
      }
      out += '}';
      return;
    }
    case ValueKind::kFunction:
      out += "<function>";
      return;
    case ValueKind::kFileHandle:
      out += "<file ";
      out += value.file_handle().path;
      out += '>';
      return;
    case ValueKind::kTask:
      out += "<task>";
      return;
    case ValueKind::kRandomGenerator:
      out += "<random generator>";
      return;
  }
}

[[nodiscard]] inline auto to_string(const Value& value) -> std::string {
  std::string text;
  append_value_repr(text, value);
  return text;
}

// Std.Println's text: a tuple argument prints as its items, separated by spaces.
inline void append_value_varargs(std::string& out, const Value& value) {
  if (!value.is_tuple()) {
    append_value_repr(out, value);
    return;
  }
  const TupleView items = value.tuple();
  for (std::size_t index = 0; index < items.size(); ++index) {
    if (index > 0) {
      out += ' ';
    }
    append_value_repr(out, items[index]);
  }
}

[[nodiscard]] inline auto type_name(const Value& value) -> std::string {
  switch (value.kind()) {
    case ValueKind::kNull:
      return "Null";
    case ValueKind::kBool:
      return "Bool";
    case ValueKind::kInt:
      return "Int64";
    case ValueKind::kUInt:
      return "UInt64";
    case ValueKind::kFloat:
      return "Float64";
    case ValueKind::kString:
      return "String";
    case ValueKind::kTuple:
      return "Tuple";
    case ValueKind::kDict:
      return "Dict";
    case ValueKind::kFunction:
      return "Callable";
    case ValueKind::kFileHandle:
      return "Handle";
    case ValueKind::kTask:
      return "TaskHandle";
    case ValueKind::kRandomGenerator:
      return "RandomGenerator";
  }
  return "Unknown";
}

// Ordering

[[nodiscard]] inline auto compare_values_for_sort(const Value& lhs, const Value& rhs) -> int;

[[nodiscard]] inline auto compare_tuples_for_sort(const TupleView lhs, const TupleView rhs) -> int {
  const std::size_t shared = std::min(lhs.size(), rhs.size());
  for (std::size_t index = 0; index < shared; ++index) {
    if (const int cmp = compare_values_for_sort(lhs[index], rhs[index]); cmp != 0) {
      return cmp;
    }
  }
  return (lhs.size() < rhs.size()) ? -1 : ((lhs.size() > rhs.size()) ? 1 : 0);
}

// Orders two values of one Comparable type (Std.Tuple.Sort, Min, Max): the <T: Comparable> bound on Tuple<T...>
// guarantees both values have the same kind, so the kinds are not compared here.
[[nodiscard]] inline auto compare_values_for_sort(const Value& lhs, const Value& rhs) -> int {
  switch (lhs.kind()) {
    case ValueKind::kNull:
      return 0;
    case ValueKind::kBool:
      return static_cast<int>(as_bool(lhs)) - static_cast<int>(as_bool(rhs));
    case ValueKind::kInt:
    case ValueKind::kUInt:
    case ValueKind::kFloat:
      return compare_numbers(lhs, rhs);
    case ValueKind::kString: {
      const int cmp = as_string(lhs).compare(as_string(rhs));
      return (cmp < 0) ? -1 : ((cmp > 0) ? 1 : 0);
    }
    case ValueKind::kTuple:
      return compare_tuples_for_sort(lhs.tuple(), as_tuple(rhs));
    default:
      break;
  }
  throw std::logic_error{"invariant violated: expected Comparable values"};
}

// Text helpers

[[nodiscard]] inline auto random_suffix(const std::size_t size = 12) -> std::string {
  thread_local std::mt19937_64 rng{std::random_device{}()};
  static constexpr auto alphabet = std::to_array("abcdefghijklmnopqrstuvwxyz0123456789");
  std::uniform_int_distribution<std::size_t> dist(0, sizeof(alphabet) - 2);
  std::string out;
  out.reserve(size);
  for (std::size_t char_index = 0; char_index < size; ++char_index) {
    out.push_back(alphabet[dist(rng)]);
  }
  return out;
}


[[nodiscard]] inline auto trim_left(std::string text) -> std::string {
  const auto it = std::ranges::find_if_not(text, [](const unsigned char ch) -> bool { return std::isspace(ch) != 0; });
  text.erase(text.begin(), it);
  return text;
}

[[nodiscard]] inline auto trim_right(std::string text) -> std::string {
  const auto it = std::ranges::find_if_not(std::views::reverse(text),
                                           [](const unsigned char ch) -> bool { return std::isspace(ch) != 0; });
  text.erase(it.base(), text.end());
  return text;
}

// Formatting

// A value formatted with `spec`, as by "{:spec}". Scalars format natively; other values format as their printed text.
[[nodiscard]] inline auto format_value_with_spec(const Value& value, const std::string& spec) -> std::string {
  const std::string single_fmt = std::format("{{0:{}}}", spec);
  switch (value.kind()) {
    case ValueKind::kBool: {
      const Bool flag = value.bool_value();
      return std::vformat(single_fmt, std::make_format_args(flag));
    }
    case ValueKind::kInt: {
      const Int number = value.int_value();
      return std::vformat(single_fmt, std::make_format_args(number));
    }
    case ValueKind::kUInt: {
      const UInt number = value.uint_value();
      return std::vformat(single_fmt, std::make_format_args(number));
    }
    case ValueKind::kFloat: {
      const Float number = value.float_value();
      return std::vformat(single_fmt, std::make_format_args(number));
    }
    case ValueKind::kString:
      return std::vformat(single_fmt, std::make_format_args(value.string_value()));
    default: {
      const std::string repr = to_string(value);
      return std::vformat(single_fmt, std::make_format_args(repr));
    }
  }
}

[[nodiscard]] inline auto format_value_plain(const Value& value) -> std::string {
  switch (value.kind()) {
    case ValueKind::kBool:
    case ValueKind::kInt:
    case ValueKind::kUInt:
    case ValueKind::kFloat:
    case ValueKind::kString:
      return format_value_with_spec(value, "");
    default:
      return to_string(value);
  }
}

[[nodiscard]] inline auto format_values(const std::string& fmt, const TupleView values) -> std::string {
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
      if (close == std::string::npos) {
        throw std::invalid_argument{"the format string has an unmatched '{'"};
      }

      const std::string field = fmt.substr(cursor + 1, close - (cursor + 1));
      std::size_t index = 0;
      std::string spec;

      const std::size_t colon = field.find(':');
      const std::string index_part = (colon == std::string::npos) ? field : field.substr(0, colon);
      if (colon != std::string::npos) {
        spec = field.substr(colon + 1);
      }

      if (index_part.empty()) {
        if (saw_manual_index) {
          throw std::invalid_argument{"the format string mixes automatic and manual field numbers"};
        }
        saw_auto_index = true;
        index = next_auto_index++;
      } else {
        if (saw_auto_index) {
          throw std::invalid_argument{"the format string mixes automatic and manual field numbers"};
        }
        saw_manual_index = true;
        for (const char c : index_part) {
          if (!std::isdigit(static_cast<unsigned char>(c))) {
            throw std::invalid_argument{"a format field is '{}' or '{N}', with an optional ':spec'"};
          }
        }
        index = to_index(std::stoull(index_part));
      }

      if (index >= values.size()) {
        throw std::invalid_argument{"the format string refers to a missing argument"};
      }

      out += spec.empty() ? format_value_plain(values[index]) : format_value_with_spec(values[index], spec);
      cursor = close + 1;
      continue;
    }

    if (ch == '}') {
      if (cursor + 1 < fmt.size() && fmt[cursor + 1] == '}') {
        out.push_back('}');
        cursor += 2;
        continue;
      }
      throw std::invalid_argument{"the format string has an unmatched '}'"};
    }
  }
  return out;
}

[[nodiscard]] inline auto format_values(const std::string& fmt, const std::vector<std::string>& values) -> std::string {
  std::vector<Value> wrapped;
  wrapped.reserve(values.size());
  for (const auto& text : values) {
    wrapped.push_back(make_string(text));
  }
  return format_values(fmt, wrapped);
}

}  // namespace fleaux::runtime
