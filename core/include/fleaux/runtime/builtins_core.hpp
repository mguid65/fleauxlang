#pragma once
// Core builtins: sequence access, arithmetic, comparison, logical, output, control flow.
// Part of the split runtime support layer; included by fleaux/runtime/runtime_support.hpp.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <thread>
#include <vector>

#include "fleaux/runtime/value.hpp"

namespace fleaux::runtime {

inline constexpr std::string_view k_match_wildcard_sentinel = "__fleaux_match_wildcard__";
inline constexpr std::string_view k_task_handle_tag = "__fleaux_task__";
inline constexpr std::string_view k_task_cancelled_message = "Task cancelled";
inline constexpr std::string_view k_task_timeout_message = "Task.WithTimeout: timeout exceeded";

inline auto require_result_tuple(const Value& value, const char* op_name) -> const Array& {
  const auto& result = as_array(value);
  if (result.Size() != 2) {
    throw std::invalid_argument{std::string(op_name) + ": expected Result tuple (tag, payload)"};
  }

  if (const Value& tag = *result.TryGet(0); !tag.HasBool()) {
    throw std::invalid_argument{std::string(op_name) + ": Result tag must be a Bool (true for Ok, false for Err)"};
  }
  return result;
}

inline auto normalize_runtime_error_message(std::string message) -> std::string {
  // Unwrap nested runtime wrappers like:
  // "native 'branch_call' threw: native builtin 'Std.X' threw: actual message"
  constexpr std::string_view k_threw = "threw: ";
  while (message.starts_with("native ")) {
    const std::size_t split = message.find(k_threw);
    if (split == std::string::npos) { break; }
    message = message.substr(split + k_threw.size());
  }
  return message;
}

class RuntimePayloadError final : public std::exception {
public:
  RuntimePayloadError(Value payload, std::string message) : payload_(std::move(payload)), message_(std::move(message)) {}

  [[nodiscard]] auto payload() const -> const Value& { return payload_; }

  [[nodiscard]] auto what() const noexcept -> const char* override { return message_.c_str(); }

private:
  Value payload_;
  std::string message_;
};

struct Wrap {
  // arg = any Value  ->  [arg]
  auto operator()(Value value) const -> Value { return make_tuple(std::move(value)); }
};

struct Unwrap {
  // arg = [v]  ->  v
  auto operator()(Value value) const -> Value { return array_at(value, 0); }
};

struct ElementAt {
  // arg = [sequence, index]
  auto operator()(Value arg) const -> Value {
    const auto& seq = as_array(array_at(arg, 0));
    const std::size_t idx = as_index_strict(array_at(arg, 1), "ElementAt index");
    auto result = seq.TryGet(idx);
    if (!result) throw std::out_of_range{"ElementAt: index out of range"};
    return *result;
  }
};

struct Length {
  // arg = sequence  (Option B: arg IS the array, no 1-element wrapper)
  auto operator()(Value arg) const -> Value { return make_int(static_cast<Int>(as_array(arg).Size())); }
};

struct Take {
  // arg = [sequence, count]
  auto operator()(Value arg) const -> Value {
    const auto& seq = as_array(array_at(arg, 0));
    const std::size_t take_count = std::min(as_index_strict(array_at(arg, 1), "Take count"), seq.Size());
    Array out;
    out.Reserve(take_count);
    for (std::size_t index = 0; index < take_count; ++index) { out.PushBack(*seq.TryGet(index)); }
    return Value{std::move(out)};
  }
};

struct Drop {
  // arg = [sequence, count]
  auto operator()(Value arg) const -> Value {
    const auto& seq = as_array(array_at(arg, 0));
    const std::size_t start = as_index_strict(array_at(arg, 1), "Drop count");
    Array out;
    for (std::size_t index = start; index < seq.Size(); ++index) { out.PushBack(*seq.TryGet(index)); }
    return Value{std::move(out)};
  }
};

struct Slice {
  // arg = [sequence, stop]
  //    | [sequence, start, stop]
  //    | [sequence, start, stop, step]
  auto operator()(Value arg) const -> Value {
    const auto& arr = as_array(arg);
    if (arr.Size() < 2) throw std::invalid_argument{"Slice: need at least 2 arguments"};
    const auto& seq = as_array(*arr.TryGet(0));

    std::size_t real_start{0};
    std::size_t real_stop{0};
    std::size_t real_step{1};
    if (arr.Size() == 2) {
      real_stop = as_index_strict(*arr.TryGet(1), "Slice stop");
    } else if (arr.Size() == 3) {
      real_start = as_index_strict(*arr.TryGet(1), "Slice start");
      real_stop = as_index_strict(*arr.TryGet(2), "Slice stop");
    } else {
      real_start = as_index_strict(*arr.TryGet(1), "Slice start");
      real_stop = as_index_strict(*arr.TryGet(2), "Slice stop");
      real_step = as_index_strict(*arr.TryGet(3), "Slice step");
      if (real_step == 0) throw std::invalid_argument{"Slice: step cannot be 0"};
    }

    Array out;
    const std::size_t end = std::min(real_stop, seq.Size());
    for (std::size_t index = real_start; index < end; index += real_step) { out.PushBack(*seq.TryGet(index)); }
    return Value{std::move(out)};
  }
};

// Arithmetic

inline auto require_same_integer_kind(const Value& lhs, const Value& rhs, const char* op_name) -> void {
  if (is_mixed_signed_unsigned_integer_pair(lhs, rhs)) {
    throw std::invalid_argument{std::string(op_name) + ": cannot mix Int64 and UInt64 operands without explicit cast"};
  }
}

struct Add {
  // arg = [lhs, rhs]
  auto operator()(Value arg) const -> Value {
    const Value& lhs = array_at(arg, 0);
    const Value& rhs = array_at(arg, 1);
    // String concatenation
    if (lhs.HasString() && rhs.HasString()) { return make_string(as_string(lhs) + as_string(rhs)); }
    require_same_integer_kind(lhs, rhs, "Add");
    return num_result(to_double(lhs) + to_double(rhs), is_uint_number(lhs) && is_uint_number(rhs));
  }
};

struct Subtract {
  auto operator()(Value arg) const -> Value {
    const Value& lhs = array_at(arg, 0);
    const Value& rhs = array_at(arg, 1);
    require_same_integer_kind(lhs, rhs, "Subtract");
    return num_result(to_double(lhs) - to_double(rhs), is_uint_number(lhs) && is_uint_number(rhs));
  }
};

struct Multiply {
  auto operator()(Value arg) const -> Value {
    const Value& lhs = array_at(arg, 0);
    const Value& rhs = array_at(arg, 1);
    require_same_integer_kind(lhs, rhs, "Multiply");
    return num_result(to_double(lhs) * to_double(rhs), is_uint_number(lhs) && is_uint_number(rhs));
  }
};

struct Divide {
  auto operator()(Value arg) const -> Value {
    const Value& lhs = array_at(arg, 0);
    const Value& rhs = array_at(arg, 1);
    require_same_integer_kind(lhs, rhs, "Divide");
    return num_result(to_double(lhs) / to_double(rhs), is_uint_number(lhs) && is_uint_number(rhs));
  }
};

struct Mod {
  auto operator()(Value arg) const -> Value {
    const Value& lhs = array_at(arg, 0);
    const Value& rhs = array_at(arg, 1);
    require_same_integer_kind(lhs, rhs, "Mod");
    return num_result(std::fmod(to_double(lhs), to_double(rhs)), is_uint_number(lhs) && is_uint_number(rhs));
  }
};

struct Pow {
  auto operator()(Value arg) const -> Value {
    return num_result(std::pow(to_double(array_at(arg, 0)), to_double(array_at(arg, 1))));
  }
};

struct BitAnd {
  auto operator()(Value arg) const -> Value {
    return make_int(as_int_value_strict(array_at(arg, 0), "BitAnd lhs") &
                    as_int_value_strict(array_at(arg, 1), "BitAnd rhs"));
  }
};

struct BitOr {
  auto operator()(Value arg) const -> Value {
    return make_int(as_int_value_strict(array_at(arg, 0), "BitOr lhs") |
                    as_int_value_strict(array_at(arg, 1), "BitOr rhs"));
  }
};

struct BitXor {
  auto operator()(Value arg) const -> Value {
    return make_int(as_int_value_strict(array_at(arg, 0), "BitXor lhs") ^
                    as_int_value_strict(array_at(arg, 1), "BitXor rhs"));
  }
};

struct BitNot {
  auto operator()(Value arg) const -> Value {
    return make_int(~as_int_value_strict(unwrap_singleton_arg(std::move(arg)), "BitNot value"));
  }
};

struct BitShiftLeft {
  auto operator()(Value arg) const -> Value {
    const Int value = as_int_value_strict(array_at(arg, 0), "BitShiftLeft value");
    const Int shift = as_int_value_strict(array_at(arg, 1), "BitShiftLeft shift");
    if (shift < 0) { throw std::invalid_argument{"BitShiftLeft: shift must be non-negative"}; }
    return make_int(value << shift);
  }
};

struct BitShiftRight {
  auto operator()(Value arg) const -> Value {
    const Int value = as_int_value_strict(array_at(arg, 0), "BitShiftRight value");
    const Int shift = as_int_value_strict(array_at(arg, 1), "BitShiftRight shift");
    if (shift < 0) { throw std::invalid_argument{"BitShiftRight: shift must be non-negative"}; }
    return make_int(value >> shift);
  }
};

struct Sqrt {
  auto operator()(Value arg) const -> Value {
    return num_result(std::sqrt(to_double(unwrap_singleton_arg(std::move(arg)))));
  }
};

struct UnaryMinus {
  auto operator()(Value arg) const -> Value { return num_result(-to_double(unwrap_singleton_arg(std::move(arg)))); }
};

struct UnaryPlus {
  auto operator()(Value arg) const -> Value { return num_result(+to_double(unwrap_singleton_arg(std::move(arg)))); }
};

struct Sin {
  auto operator()(Value arg) const -> Value {
    return num_result(std::sin(to_double(unwrap_singleton_arg(std::move(arg)))));
  }
};
struct Cos {
  auto operator()(Value arg) const -> Value {
    return num_result(std::cos(to_double(unwrap_singleton_arg(std::move(arg)))));
  }
};
struct Tan {
  auto operator()(Value arg) const -> Value {
    return num_result(std::tan(to_double(unwrap_singleton_arg(std::move(arg)))));
  }
};

// Comparison & logical

struct GreaterThan {
  auto operator()(Value arg) const -> Value {
    return make_bool(compare_numbers(array_at(arg, 0), array_at(arg, 1)) > 0);
  }
};
struct LessThan {
  auto operator()(Value arg) const -> Value {
    return make_bool(compare_numbers(array_at(arg, 0), array_at(arg, 1)) < 0);
  }
};
struct GreaterOrEqual {
  auto operator()(Value arg) const -> Value {
    return make_bool(compare_numbers(array_at(arg, 0), array_at(arg, 1)) >= 0);
  }
};
struct LessOrEqual {
  auto operator()(Value arg) const -> Value {
    return make_bool(compare_numbers(array_at(arg, 0), array_at(arg, 1)) <= 0);
  }
};

struct Equal {
  auto operator()(Value arg) const -> Value { return make_bool(array_at(arg, 0) == array_at(arg, 1)); }
};
struct NotEqual {
  auto operator()(Value arg) const -> Value { return make_bool(array_at(arg, 0) != array_at(arg, 1)); }
};

struct Not {
  auto operator()(Value arg) const -> Value { return make_bool(!as_bool(unwrap_singleton_arg(std::move(arg)))); }
};
struct And {
  auto operator()(Value arg) const -> Value {
    return make_bool(as_bool(array_at(arg, 0)) && as_bool(array_at(arg, 1)));
  }
};
struct Or {
  auto operator()(Value arg) const -> Value {
    return make_bool(as_bool(array_at(arg, 0)) || as_bool(array_at(arg, 1)));
  }
};

// Output

struct Println {
  // Prints the value, returns it unchanged.
  auto operator()(Value arg) const -> Value {
    print_value_varargs(std::cout, arg);
    std::cout << '\n';
    return arg;
  }
};

struct Printf {
  // arg = [format, arg0, arg1, ...]
  // Prints formatted text and returns the original argument tuple unchanged.
  auto operator()(Value arg) const -> Value {
    const auto& args = as_array(arg);
    if (args.Size() < 1) { throw std::invalid_argument{"Printf expects at least 1 argument"}; }
    const std::string fmt = to_string(*args.TryGet(0));
    std::vector<Value> values;
    values.reserve(args.Size() > 0 ? args.Size() - 1 : 0);
    for (std::size_t arg_index = 1; arg_index < args.Size(); ++arg_index) {
      values.push_back(*args.TryGet(arg_index));
    }

    std::cout << format_values(fmt, values);
    return arg;
  }
};

struct Input {
  // arg = [] | [prompt] | prompt
  auto operator()(Value arg) const -> Value {
    auto read_line = []() -> Value {
      std::string line;
      if (!std::getline(std::cin, line)) { return make_string(""); }
      return make_string(line);
    };

    if (!arg.HasArray()) {
      std::cout << to_string(arg);
      std::cout.flush();
      return read_line();
    }

    const auto& args = as_array(arg);
    if (args.Size() == 0) { return read_line(); }
    if (args.Size() == 1) {
      std::cout << to_string(*args.TryGet(0));
      std::cout.flush();
      return read_line();
    }
    throw std::invalid_argument{"Input expects 0 or 1 argument"};
  }
};

struct GetArgs {
  auto operator()(Value arg) const -> Value {
    (void)require_args(arg, 0, "GetArgs");
    Array out;
    const auto& args = get_process_args();
    out.Reserve(args.size());
    for (const auto& process_arg : args) { out.PushBack(make_string(process_arg)); }
    return Value{std::move(out)};
  }
};

struct Type {
  // arg = [value] | value -> String runtime type name
  auto operator()(Value arg) const -> Value { return make_string(type_name(unwrap_singleton_arg(std::move(arg)))); }
};


struct ToInt64 {
  auto operator()(Value arg) const -> Value {
    return make_int(as_int_value_strict(unwrap_singleton_arg(std::move(arg)), "ToInt64"));
  }
};

struct ToUInt64 {
  auto operator()(Value arg) const -> Value {
    const Value value = unwrap_singleton_arg(std::move(arg));
    return as_number(value).Visit(
        [](const Int signed_value) -> Value {
          if (signed_value < 0) { throw std::invalid_argument{"ToUInt64: cannot cast negative Int64 to UInt64"}; }
          return make_uint(static_cast<UInt>(signed_value));
        },
        [](const UInt unsigned_value) -> Value { return make_uint(unsigned_value); },
        [](const Float float_value) -> Value {
          if (!std::isfinite(float_value) || std::floor(float_value) != float_value || float_value < 0.0) {
            throw std::invalid_argument{"ToUInt64: cannot cast Float64 value to UInt64"};
          }
          if (float_value > static_cast<double>(std::numeric_limits<UInt>::max())) {
            throw std::out_of_range{"ToUInt64: Float64 value out of UInt64 range"};
          }
          return make_uint(static_cast<UInt>(float_value));
        });
  }
};

struct ToFloat64 {
  auto operator()(Value arg) const -> Value { return make_float(to_double(unwrap_singleton_arg(std::move(arg)))); }
};

struct Exit {
  auto operator()(Value arg) const -> Value {
    if (arg.HasArray()) {
      const auto& args = as_array(arg);
      if (args.Size() == 0) { std::exit(0); }
      if (args.Size() == 1) { std::exit(static_cast<int>(to_double(*args.TryGet(0)))); }
      throw std::invalid_argument{"Exit expects 0 or 1 argument"};
    }
    std::exit(static_cast<int>(to_double(arg)));
  }
};

// Control flow (templated: functions remain concrete C++ callables)

struct Select {
  // arg = [condition, true_val, false_val] — all Values
  auto operator()(Value arg) const -> Value { return as_bool(array_at(arg, 0)) ? array_at(arg, 1) : array_at(arg, 2); }
};

struct Match {
  // arg = [value, [pattern, handler], [pattern, handler], ...]
  // Pattern wildcard is encoded as the lowering sentinel string.
  // Callable patterns are predicates: pattern(subject) -> Bool.
  auto operator()(Value arg) const -> Value {
    const auto& args = as_array(arg);
    if (args.Size() < 2) { throw std::invalid_argument{"Match expects a value and at least one case"}; }

    const Value subject = *args.TryGet(0);
    for (std::size_t case_index = 1; case_index < args.Size(); ++case_index) {
      const auto& case_tuple = as_array(*args.TryGet(case_index));
      if (case_tuple.Size() != 2) { throw std::invalid_argument{"Match case must be a (pattern, handler) tuple"}; }

      const Value pattern = *case_tuple.TryGet(0);
      const Value handler = *case_tuple.TryGet(1);

      const bool wildcard_match = pattern.HasString() && as_string(pattern) == k_match_wildcard_sentinel;
      bool predicate_match = false;
      if (!wildcard_match && callable_id_from_value(pattern).has_value()) {
        predicate_match = as_bool(invoke_callable_ref(pattern, subject));
      }

      if (wildcard_match || predicate_match || pattern == subject) { return invoke_callable_ref(handler, subject); }
    }

    throw std::runtime_error{"Match: no case matched and no wildcard case provided"};
  }
};

struct ResultOk {
  auto operator()(Value arg) const -> Value {
    return make_tuple(make_bool(true), unwrap_singleton_arg(std::move(arg)));
  }
};

struct ResultErr {
  auto operator()(Value arg) const -> Value {
    return make_tuple(make_bool(false), unwrap_singleton_arg(std::move(arg)));
  }
};

struct ResultTag {
  auto operator()(Value arg) const -> Value {
    const Value result = unwrap_singleton_arg(std::move(arg));
    const auto& tuple = require_result_tuple(result, "Result.Tag");
    return *tuple.TryGet(0);
  }
};

struct ResultPayload {
  auto operator()(Value arg) const -> Value {
    const Value result = unwrap_singleton_arg(std::move(arg));
    const auto& tuple = require_result_tuple(result, "Result.Payload");
    return *tuple.TryGet(1);
  }
};

struct ResultIsOk {
  auto operator()(Value arg) const -> Value {
    const Value result = unwrap_singleton_arg(std::move(arg));
    const auto& tuple = require_result_tuple(result, "Result.IsOk");
    return *tuple.TryGet(0);  // true is Ok, false is Err
  }
};

struct ResultIsErr {
  auto operator()(Value arg) const -> Value {
    const Value result = unwrap_singleton_arg(std::move(arg));
    const auto& tuple = require_result_tuple(result, "Result.IsErr");
    return make_bool(!as_bool(*tuple.TryGet(0)));  // negation of Ok
  }
};

struct ResultUnwrap {
  auto operator()(Value arg) const -> Value {
    const Value result = unwrap_singleton_arg(std::move(arg));
    const auto& tuple = require_result_tuple(result, "Result.Unwrap");
    if (!as_bool(*tuple.TryGet(0))) { throw std::runtime_error{"Result.Unwrap expected Ok (true), got Err (false)"}; }
    return *tuple.TryGet(1);
  }
};

struct ResultUnwrapErr {
  auto operator()(Value arg) const -> Value {
    const Value result = unwrap_singleton_arg(std::move(arg));
    const auto& tuple = require_result_tuple(result, "Result.UnwrapErr");
    if (as_bool(*tuple.TryGet(0))) { throw std::runtime_error{"Result.UnwrapErr expected Err (false), got Ok (true)"}; }
    return *tuple.TryGet(1);
  }
};

struct Try {
  // arg = [value, func_ref]
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 2, "Try");
    const Value& value = *args.TryGet(0);
    const Value& function_ref = *args.TryGet(1);
    try {
      return ResultOk{}(make_tuple(invoke_callable_ref(function_ref, value)));
    } catch (const std::exception& ex) {
      return ResultErr{}(make_tuple(make_string(normalize_runtime_error_message(ex.what()))));
    }
  }
};

struct TaskControl {
  std::mutex mutex;
  std::condition_variable cv;
  bool started{false};
  bool completed{false};
  bool cancel_requested{false};
  Value result = ResultErr{}(make_tuple(make_string("Task: result not ready")));
};

using TaskControlPtr = std::shared_ptr<TaskControl>;

inline auto task_registry() -> ScopedRegistry<TaskControlPtr>& {
  static ScopedRegistry<TaskControlPtr> registry;
  return registry;
}

inline auto task_registry_mutex() -> std::mutex& {
  static std::mutex mutex;
  return mutex;
}

[[nodiscard]] inline auto task_registry_size() -> std::size_t {
  std::scoped_lock lock(task_registry_mutex());
  return task_registry().active_count;
}

inline void reset_task_registry_for_tests() {
  std::vector<TaskControlPtr> tasks;
  {
    std::scoped_lock lock(task_registry_mutex());
    auto& registry = task_registry();
    for (const auto& entry : registry.entries) {
      if (entry.occupied && entry.value) { tasks.push_back(entry.value); }
    }
    registry.clear();
  }
  for (const auto& task : tasks) {
    {
      std::scoped_lock task_lock(task->mutex);
      task->cancel_requested = true;
    }
    task->cv.notify_all();
  }
}

inline auto make_task_handle_from_id(const RegistryId& id) -> Value {
  Array token;
  token.Reserve(3);
  token.PushBack(make_string(String{k_task_handle_tag}));
  token.PushBack(make_uint(id.slot));
  token.PushBack(make_uint(id.generation));
  return Value{std::move(token)};
}

[[nodiscard]] inline auto task_id_from_handle(const Value& task_handle) -> std::optional<RegistryId> {
  const auto& arr = task_handle.TryGetArray();
  if (!arr || arr->Size() != 3) { return std::nullopt; }
  const auto& tag = arr->TryGet(0)->TryGetString();
  if (!tag || *tag != k_task_handle_tag) { return std::nullopt; }

  const auto as_uint = [](const Number& n) -> std::optional<UInt> {
    return n.Visit(
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

  const auto& slot_number = arr->TryGet(1)->TryGetNumber();
  const auto& gen_number = arr->TryGet(2)->TryGetNumber();
  if (!slot_number || !gen_number) { return std::nullopt; }
  const auto slot = as_uint(*slot_number);
  const auto generation = as_uint(*gen_number);
  if (!slot || !generation) { return std::nullopt; }
  return RegistryId{.slot = *slot, .generation = *generation};
}

[[nodiscard]] inline auto task_control_from_handle(const Value& task_handle) -> TaskControlPtr {
  const auto id = task_id_from_handle(task_handle);
  if (!id) { return {}; }
  std::scoped_lock lock(task_registry_mutex());
  const TaskControlPtr* control = task_registry().get(*id);
  return control != nullptr ? *control : TaskControlPtr{};
}

inline auto request_task_cancel(const TaskControlPtr& task) -> bool {
  if (!task) { return false; }
  std::scoped_lock lock(task->mutex);
  if (task->started || task->completed || task->cancel_requested) { return false; }
  task->cancel_requested = true;
  return true;
}

[[nodiscard]] inline auto wait_task_result(const TaskControlPtr& task) -> Value {
  if (!task) { return ResultErr{}(make_tuple(make_string("Task.Await: invalid task handle"))); }

  std::unique_lock lock(task->mutex);
  task->cv.wait(lock, [&task]() -> bool { return task->completed; });
  return task->result;
}

[[nodiscard]] inline auto wait_task_result_for(const TaskControlPtr& task, const std::chrono::milliseconds timeout)
    -> std::optional<Value> {
  if (!task) { return std::nullopt; }

  std::unique_lock lock(task->mutex);
  if (!task->cv.wait_for(lock, timeout, [&task]() -> bool { return task->completed; })) { return std::nullopt; }
  return task->result;
}

// Bounded thread pool shared by Std.Task.* and Std.Parallel.*.
//
// Worker count defaults to hardware_concurrency() clamped to [1, 16].
// Tasks submitted via submit() are tracked in the task registry and exposed
// as user-visible handles. Tasks submitted via submit_untracked() are used
// by Parallel combinators and are not registered.
class TaskRuntime {
public:
  static constexpr std::size_t k_min_workers = 1;
  static constexpr std::size_t k_max_workers = 16;

  explicit TaskRuntime(std::size_t worker_count = 0) {
    if (worker_count == 0) { worker_count = static_cast<std::size_t>(std::thread::hardware_concurrency()); }
    worker_count = std::clamp(worker_count, k_min_workers, k_max_workers);
    workers_.reserve(worker_count);
    for (std::size_t i = 0; i < worker_count; ++i) {
      workers_.emplace_back([this]() -> void { worker_loop(); });
    }
  }

  ~TaskRuntime() {
    {
      std::scoped_lock lock(mutex_);
      stopping_ = true;
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
      if (worker.joinable()) { worker.join(); }
    }
  }

  TaskRuntime(const TaskRuntime&) = delete;
  auto operator=(const TaskRuntime&) -> TaskRuntime& = delete;

  // Submit a tracked user-visible task. Callable is resolved eagerly so
  // workers do not race on callable-registry cleanup.
  [[nodiscard]] auto submit(const Value& function_ref, Value arg) -> TaskControlPtr {
    auto task = std::make_shared<TaskControl>();

    RuntimeCallable callable;
    try {
      const auto callable_id = callable_id_from_value(function_ref);
      if (!callable_id) { throw std::runtime_error{"Expected callable reference"}; }
      std::scoped_lock callable_lock(callable_registry_mutex());
      const RuntimeCallable* resolved = callable_registry().get(*callable_id);
      if (!resolved) { throw std::runtime_error{"Unknown callable reference"}; }
      callable = *resolved;
    } catch (const std::exception& ex) {
      std::scoped_lock task_lock(task->mutex);
      task->started = true;
      task->completed = true;
      task->result = ResultErr{}(make_tuple(make_string(normalize_runtime_error_message(ex.what()))));
      task->cv.notify_all();
      return task;
    }

    enqueue(Job{.callable = std::move(callable), .arg = std::move(arg), .task = task});
    return task;
  }

  // Submit an untracked parallel item (not registered in the task registry).
  [[nodiscard]] auto submit_untracked(RuntimeCallable callable, Value arg) -> TaskControlPtr {
    auto task = std::make_shared<TaskControl>();
    enqueue(Job{.callable = std::move(callable), .arg = std::move(arg), .task = task});
    return task;
  }

  [[nodiscard]] auto worker_count() const -> std::size_t { return workers_.size(); }

private:
  struct Job {
    RuntimeCallable callable;
    Value arg;
    TaskControlPtr task;
  };

  void enqueue(Job job) {
    {
      std::scoped_lock lock(mutex_);
      queue_.push_back(std::move(job));
    }
    cv_.notify_one();
  }

  void worker_loop() {
    for (;;) {
      Job job;
      {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this]() -> bool { return stopping_ || !queue_.empty(); });
        if (stopping_ && queue_.empty()) { return; }
        job = std::move(queue_.front());
        queue_.pop_front();
      }

      {
        std::scoped_lock task_lock(job.task->mutex);
        if (job.task->cancel_requested) {
          job.task->completed = true;
          job.task->result = ResultErr{}(make_tuple(make_string(String{k_task_cancelled_message})));
          job.task->cv.notify_all();
          continue;
        }
        job.task->started = true;
      }

      Value result;
      try {
        result = ResultOk{}(make_tuple(job.callable(std::move(job.arg))));
      } catch (const RuntimePayloadError& ex) {
        result = ResultErr{}(ex.payload());
      } catch (const std::exception& ex) {
        result = ResultErr{}(make_tuple(make_string(normalize_runtime_error_message(ex.what()))));
      }

      {
        std::scoped_lock task_lock(job.task->mutex);
        job.task->result = std::move(result);
        job.task->completed = true;
      }
      job.task->cv.notify_all();
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Job> queue_;
  bool stopping_{false};
  std::vector<std::thread> workers_;
};

inline auto task_runtime() -> TaskRuntime& {
  static TaskRuntime runtime;
  return runtime;
}

class TaskRegistryScope {
public:
  TaskRegistryScope() {
    std::scoped_lock lock(task_registry_mutex());
    checkpoint_ = task_registry().registration_log.size();
  }

  TaskRegistryScope(const TaskRegistryScope&) = delete;
  auto operator=(const TaskRegistryScope&) -> TaskRegistryScope& = delete;
  TaskRegistryScope(TaskRegistryScope&&) = delete;
  auto operator=(TaskRegistryScope&&) -> TaskRegistryScope& = delete;

  ~TaskRegistryScope() {
    std::vector<std::pair<RegistryId, TaskControlPtr>> tasks;
    {
      std::scoped_lock lock(task_registry_mutex());
      auto& registry = task_registry();
      const auto log_size = registry.registration_log.size();
      for (std::size_t index = checkpoint_; index < log_size; ++index) {
        const auto slot = registry.registration_log[index];
        const auto entry_index = static_cast<std::size_t>(slot);
        if (entry_index >= registry.entries.size()) { continue; }
        const auto& entry = registry.entries[entry_index];
        if (!entry.occupied || !entry.value) { continue; }
        tasks.emplace_back(RegistryId{.slot = slot, .generation = entry.generation}, entry.value);
      }
    }

    for (const auto& [id, task] : tasks) {
      (void)id;
      if (request_task_cancel(task)) { task->cv.notify_all(); }
      (void)wait_task_result(task);
    }

    std::scoped_lock lock(task_registry_mutex());
    auto& registry = task_registry();
    for (const auto& [id, task] : tasks) {
      (void)task;
      if (registry.get(id) != nullptr) { registry.retire(id.slot); }
    }
    registry.registration_log.resize(checkpoint_);
  }

private:
  std::size_t checkpoint_{0};
};

inline auto try_get_string_dict_option(const Object& options, const std::string_view key) -> const Value* {
  const auto result = options.TryGet(std::string{"s:"} + std::string{key});
  return result ? &*result : nullptr;
}

inline auto parse_positive_size_option(const Object& options, const std::string_view key)
    -> std::optional<std::size_t> {
  const auto* value = try_get_string_dict_option(options, key);
  if (value == nullptr) { return std::nullopt; }
  const auto parsed = as_index_strict(*value, key);
  if (parsed == 0) {
    throw std::invalid_argument{std::string{"Parallel.WithOptions: "} + std::string{key} + " must be > 0"};
  }
  return parsed;
}

inline auto strip_object_key_prefix(const std::string& internal_key) -> std::string {
  if (internal_key.size() >= 2U && internal_key[1] == ':') { return internal_key.substr(2); }
  return internal_key;
}

inline auto validate_parallel_with_options_keys(const Object& options) -> void {
  for (const auto& internal_key : options | std::views::keys) {
    if (internal_key == "s:max_workers") { continue; }
    throw std::invalid_argument{"Parallel.WithOptions: unsupported option '" +
                                strip_object_key_prefix(internal_key) + "'"};
  }
}

// Run a parallel map over items using the shared task pool. max_in_flight caps how
// many items are queued to the pool at once (back-pressure). Output order matches
// input order. Returns a Result value.
inline auto run_parallel_map_pooled(const Array& items, const Value& function_ref, const std::size_t max_in_flight)
    -> Value {
  if (items.Size() == 0) { return ResultOk{}(make_tuple(Value{Array{}})); }

  // Resolve callable once before submitting.
  RuntimeCallable callable;
  try {
    const auto callable_id = callable_id_from_value(function_ref);
    if (!callable_id) { throw std::runtime_error{"Expected callable reference"}; }
    std::scoped_lock callable_lock(callable_registry_mutex());
    const RuntimeCallable* resolved = callable_registry().get(*callable_id);
    if (!resolved) { throw std::runtime_error{"Unknown callable reference"}; }
    callable = *resolved;
  } catch (const std::exception& ex) {
    return ResultErr{}(
        make_tuple(make_tuple(make_int(static_cast<Int>(0)), make_string(normalize_runtime_error_message(ex.what())))));
  }

  const std::size_t n = items.Size();
  const std::size_t window = std::min(max_in_flight, n);

  // Pre-collect items so we can index them.
  std::vector<Value> item_vec;
  item_vec.reserve(n);
  for (std::size_t i = 0; i < n; ++i) { item_vec.push_back(*items.TryGet(i)); }

  // Sliding window: submit up to `window` items, then await front before submitting next.
  std::deque<std::pair<std::size_t, TaskControlPtr>> in_flight;
  std::size_t next_submit = 0;

  auto submit_next = [&]() -> void {
    if (next_submit < n) {
      auto task = task_runtime().submit_untracked(callable, item_vec[next_submit]);
      in_flight.emplace_back(next_submit, std::move(task));
      ++next_submit;
    }
  };

  // Fill initial window.
  while (in_flight.size() < window && next_submit < n) { submit_next(); }

  Array out;
  out.Reserve(n);

  while (!in_flight.empty()) {
    auto [item_index, task] = std::move(in_flight.front());
    in_flight.pop_front();

    // Submit next item to keep window full.
    submit_next();

    const Value result = wait_task_result(task);
    const auto& result_arr = result.TryGetArray();
    if (!result_arr || result_arr->Size() < 1) {
      return ResultErr{}(make_tuple(
          make_tuple(make_int(static_cast<Int>(item_index)), make_string("Parallel.Map: unexpected result shape"))));
    }
    const auto& ok_tag = result_arr->TryGet(0)->TryGetBool();
    if (!ok_tag || !*ok_tag) {
      const auto payload = result_arr->Size() >= 2 ? *result_arr->TryGet(1) : make_string("unknown error");
      const auto msg = payload.HasArray() && as_array(payload).Size() == 1 ? to_string(*as_array(payload).TryGet(0))
                                                                           : to_string(payload);
      return ResultErr{}(make_tuple(make_tuple(make_int(static_cast<Int>(item_index)), make_string(msg))));
    }
    const auto& payload_arr = result_arr->TryGet(1)->TryGetArray();
    out.PushBack(payload_arr && payload_arr->Size() == 1 ? *payload_arr->TryGet(0) : *result_arr->TryGet(1));
  }

  return ResultOk{}(make_tuple(Value{std::move(out)}));
}

struct TaskSpawn {
  // arg = [func_ref, value]
  // Phase B foundation: queue work onto the task runtime and return a registry-backed handle.
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 2, "Task.Spawn");
    const Value& function_ref = *args.TryGet(0);
    const Value& value = *args.TryGet(1);

    const TaskControlPtr task = task_runtime().submit(function_ref, value);
    RegistryId id{};
    {
      std::scoped_lock lock(task_registry_mutex());
      id = task_registry().insert(task, /*logged=*/true);
    }
    return make_task_handle_from_id(id);
  }
};

struct TaskAwait {
  // arg = [task_handle] | task_handle
  auto operator()(Value arg) const -> Value {
    const Value task_handle = unwrap_singleton_arg(std::move(arg));
    return wait_task_result(task_control_from_handle(task_handle));
  }
};

struct TaskAwaitAll {
  // arg = [tasks_tuple] | tasks_tuple
  auto operator()(Value arg) const -> Value {
    const Value tasks_arg = unwrap_singleton_arg(std::move(arg));
    if (!tasks_arg.HasArray()) {
      return ResultErr{}(
          make_tuple(make_int(static_cast<Int>(0)), make_string("Task.AwaitAll: tasks must be a Tuple")));
    }
    const auto& tasks = as_array(tasks_arg);

    Array out;
    out.Reserve(tasks.Size());
    for (std::size_t task_index = 0; task_index < tasks.Size(); ++task_index) {
      const TaskControlPtr task = task_control_from_handle(*tasks.TryGet(task_index));
      if (!task) {
        return ResultErr{}(make_tuple(
            make_tuple(make_int(static_cast<Int>(task_index)), make_string("Task.AwaitAll: invalid task handle"))));
      }

      const Value result = wait_task_result(task);

      const auto& tuple = require_result_tuple(result, "Task.AwaitAll");
      if (!as_bool(*tuple.TryGet(0))) {
        return ResultErr{}(make_tuple(make_tuple(make_int(static_cast<Int>(task_index)), *tuple.TryGet(1))));
      }
      out.PushBack(*tuple.TryGet(1));
    }
    return ResultOk{}(make_tuple(Value{std::move(out)}));
  }
};

struct TaskCancel {
  // arg = [task_handle] | task_handle
  auto operator()(Value arg) const -> Value {
    const Value task_handle = unwrap_singleton_arg(std::move(arg));
    const TaskControlPtr task = task_control_from_handle(task_handle);
    if (!task) { return make_bool(false); }
    const bool cancelled = request_task_cancel(task);
    if (cancelled) { task->cv.notify_all(); }
    return make_bool(cancelled);
  }
};

struct TaskWithTimeout {
  // arg = [task_handle, timeout_ms]
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 2, "Task.WithTimeout");
    const Int timeout_ms = as_int_value_strict(*args.TryGet(1), "Task.WithTimeout timeout_ms");
    if (timeout_ms < 0) {
      return ResultErr{}(make_tuple(make_string("Task.WithTimeout: timeout_ms must be non-negative")));
    }
    const TaskControlPtr task = task_control_from_handle(*args.TryGet(0));
    if (!task) { return ResultErr{}(make_tuple(make_string("Task.WithTimeout: invalid task handle"))); }
    const auto result = wait_task_result_for(task, std::chrono::milliseconds{timeout_ms});
    if (!result) { return ResultErr{}(make_tuple(make_string(String{k_task_timeout_message}))); }
    return *result;
  }
};

struct ParallelMap {
  // arg = [items_tuple, func_ref]
  // Delegates to the shared bounded pool backend with full in-flight window.
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 2, "Parallel.Map");
    const auto& items = as_array(*args.TryGet(0));
    const Value function_ref = *args.TryGet(1);
    const std::size_t max_in_flight = std::max<std::size_t>(1, items.Size());
    return run_parallel_map_pooled(items, function_ref, max_in_flight);
  }
};

struct ParallelWithOptions {
  // arg = [items_tuple, func_ref, options_dict]
  // Bounded worker policy with deterministic output ordering.
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 3, "Parallel.WithOptions");
    if (!args.TryGet(2)->HasObject()) {
      return ResultErr{}(
          make_tuple(make_int(static_cast<Int>(0)), make_string("Parallel.WithOptions: options must be a Dict")));
    }

    const auto& items = as_array(*args.TryGet(0));
    const Value function_ref = *args.TryGet(1);
    const auto& options = as_object(*args.TryGet(2));

    try {
      validate_parallel_with_options_keys(options);
      const auto requested_workers = parse_positive_size_option(options, "max_workers");

      const std::size_t pool_workers = task_runtime().worker_count();
      const std::size_t max_in_flight = requested_workers.value_or(pool_workers);

      return run_parallel_map_pooled(items, function_ref, max_in_flight);
    } catch (const std::exception& ex) {
      return ResultErr{}(
          make_tuple(make_int(static_cast<Int>(0)), make_string(normalize_runtime_error_message(ex.what()))));
    }
  }
};

// Run fn on each item in parallel for side effects. Output is discarded; returns Result(()).
// On first error returns Err((index, message)).
inline auto run_parallel_foreach_pooled(const Array& items, const Value& function_ref, const std::size_t max_in_flight)
    -> Value {
  if (items.Size() == 0) { return ResultOk{}(make_tuple(Value{Array{}})); }

  RuntimeCallable callable;
  try {
    const auto callable_id = callable_id_from_value(function_ref);
    if (!callable_id) { throw std::runtime_error{"Expected callable reference"}; }
    std::scoped_lock callable_lock(callable_registry_mutex());
    const RuntimeCallable* resolved = callable_registry().get(*callable_id);
    if (!resolved) { throw std::runtime_error{"Unknown callable reference"}; }
    callable = *resolved;
  } catch (const std::exception& ex) {
    return ResultErr{}(
        make_tuple(make_tuple(make_int(static_cast<Int>(0)), make_string(normalize_runtime_error_message(ex.what())))));
  }

  const std::size_t n = items.Size();
  const std::size_t window = std::min(max_in_flight, n);

  std::vector<Value> item_vec;
  item_vec.reserve(n);
  for (std::size_t i = 0; i < n; ++i) { item_vec.push_back(*items.TryGet(i)); }

  std::deque<std::pair<std::size_t, TaskControlPtr>> in_flight;
  std::size_t next_submit = 0;

  auto submit_next = [&]() -> void {
    if (next_submit < n) {
      auto task = task_runtime().submit_untracked(callable, item_vec[next_submit]);
      in_flight.emplace_back(next_submit, std::move(task));
      ++next_submit;
    }
  };

  while (in_flight.size() < window && next_submit < n) { submit_next(); }

  while (!in_flight.empty()) {
    auto [item_index, task] = std::move(in_flight.front());
    in_flight.pop_front();
    submit_next();

    const Value result = wait_task_result(task);
    const auto& result_arr = result.TryGetArray();
    if (!result_arr || result_arr->Size() < 1) {
      return ResultErr{}(make_tuple(make_tuple(make_int(static_cast<Int>(item_index)),
                                               make_string("Parallel.ForEach: unexpected result shape"))));
    }
    if (const auto& ok_tag = result_arr->TryGet(0)->TryGetBool(); !ok_tag || !*ok_tag) {
      const auto payload = result_arr->Size() >= 2 ? *result_arr->TryGet(1) : make_string("unknown error");
      const auto msg = payload.HasArray() && as_array(payload).Size() == 1 ? to_string(*as_array(payload).TryGet(0))
                                                                           : to_string(payload);
      return ResultErr{}(make_tuple(make_tuple(make_int(static_cast<Int>(item_index)), make_string(msg))));
    }
  }

  return ResultOk{}(make_tuple(Value{Array{}}));
}

// Deterministic left-fold helper. Items are partitioned into chunks, each chunk is reduced
// sequentially via the pool, and chunk results are chained in input order.
// fn signature: fn(acc, item) -> acc
inline auto run_parallel_reduce_chunked(const Array& items, const Value& init, const Value& function_ref,
                                        const std::size_t chunk_size) -> Value {
  const std::size_t n = items.Size();
  if (n == 0) { return ResultOk{}(make_tuple(init)); }

  RuntimeCallable callable;
  try {
    const auto callable_id = callable_id_from_value(function_ref);
    if (!callable_id) { throw std::runtime_error{"Expected callable reference"}; }
    std::scoped_lock callable_lock(callable_registry_mutex());
    const RuntimeCallable* resolved = callable_registry().get(*callable_id);
    if (!resolved) { throw std::runtime_error{"Unknown callable reference"}; }
    callable = *resolved;
  } catch (const std::exception& ex) {
    return ResultErr{}(
        make_tuple(make_tuple(make_int(static_cast<Int>(0)), make_string(normalize_runtime_error_message(ex.what())))));
  }

  // Collect items into vector for random access.
  std::vector<Value> item_vec;
  item_vec.reserve(n);
  for (std::size_t i = 0; i < n; ++i) { item_vec.push_back(*items.TryGet(i)); }

  // Build chunk descriptors: [start, end) pairs.
  const std::size_t cs = std::max<std::size_t>(1, chunk_size);
  struct Chunk {
    std::size_t start;
    std::size_t end;
  };
  std::vector<Chunk> chunks;
  for (std::size_t s = 0; s < n; s += cs) { chunks.push_back({.start = s, .end = std::min(s + cs, n)}); }

  // A chunk-fold is a sequential left-fold over that chunk, starting from its provided seed.
  // We submit each chunk-fold as a single pool task. The "arg" is a (seed, chunk_items_tuple).
  // The callable receives (acc, item) pairs. To run a chunk fold inside one pool task we
  // need a trampoline - we just run it synchronously inside the submitted lambda.

  // Build chunk reduction tasks. Each task carries its own copy of items and seed.
  // For the first chunk the seed is `init`. For subsequent chunks we need the previous
  // chunk result - so chunks must execute sequentially with respect to seed propagation.
  // Strategy: submit all chunks with a placeholder seed, collect in order, then chain.
  // Actually the simplest correct approach: submit each chunk as an untracked task that
  // performs a sequential fold over its slice. The seed for chunk i is the result of chunk i-1.
  // Because seeds chain, we must wait for chunk i before submitting chunk i+1.
  // This limits parallelism but preserves exact left-fold semantics.
  // Within each chunk the fold is sequential; chunks themselves are sequential.
  // For a fully parallel reduction the function must be associative - we don't assume that.

  Value acc = init;
  std::size_t global_index = 0;
  for (const auto& [start, end] : chunks) {
    // Build a tuple of the chunk's items.
    Array chunk_items;
    chunk_items.Reserve(end - start);
    for (std::size_t i = start; i < end; ++i) { chunk_items.PushBack(item_vec[i]); }

    // Submit a task that folds this chunk.
    // The task receives (seed, items_tuple) and calls callable(acc, item) repeatedly.
    Value seed_copy = acc;
    Array chunk_copy = chunk_items;
    RuntimeCallable fn_copy = callable;
    const std::size_t first_idx = global_index;

    auto chunk_task = task_runtime().submit_untracked(
        RuntimeCallable{[fn = std::move(fn_copy), items_arr = std::move(chunk_copy), seed = std::move(seed_copy),
                         base_idx = first_idx](const Value& /*ignored*/) mutable -> Value {
          Value local_acc = std::move(seed);
          std::size_t idx = base_idx;
          for (std::size_t i = 0; i < items_arr.Size(); ++i, ++idx) {
            Value step_arg = make_tuple(local_acc, *items_arr.TryGet(i));
            try {
              local_acc = fn(std::move(step_arg));
            } catch (const std::exception& ex) {
              const auto message = normalize_runtime_error_message(ex.what());
              throw RuntimePayloadError{
                  make_tuple(make_int(static_cast<Int>(idx)), make_string(message)), std::move(message)};
            }
          }
          // Return plain local_acc; the worker wraps it in ResultOk.
          return local_acc;
        }},
        Value{Array{}}  // ignored arg
    );

    global_index += (end - start);

    Value chunk_result = wait_task_result(chunk_task);
    const auto& chunk_arr = chunk_result.TryGetArray();
    if (!chunk_arr || chunk_arr->Size() < 1) {
      return ResultErr{}(make_tuple(
          make_tuple(make_int(static_cast<Int>(start)), make_string("Parallel.Reduce: unexpected result shape"))));
    }
    if (const auto& ok_tag = chunk_arr->TryGet(0)->TryGetBool(); !ok_tag || !*ok_tag) {
      return chunk_result;  // propagate Err as-is
    }
    acc = chunk_arr->Size() >= 2 ? *chunk_arr->TryGet(1) : Value{Array{}};
    if (acc.HasArray() && as_array(acc).Size() == 1) { acc = *as_array(acc).TryGet(0); }
  }

  return ResultOk{}(make_tuple(acc));
}

struct ParallelForEach {
  // arg = [items_tuple, func_ref]
  // Executes fn on each item in parallel for side effects. Returns Result(()).
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 2, "Parallel.ForEach");
    const auto& items = as_array(*args.TryGet(0));
    const Value function_ref = *args.TryGet(1);
    const std::size_t max_in_flight = std::max<std::size_t>(1, items.Size());
    return run_parallel_foreach_pooled(items, function_ref, max_in_flight);
  }
};

struct ParallelReduce {
  // arg = [items_tuple, init, func_ref]
  // Performs a chunked left-fold: fn(acc, item) -> acc. Returns Result(final_value).
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 3, "Parallel.Reduce");
    const auto& items = as_array(*args.TryGet(0));
    const Value init = *args.TryGet(1);
    const Value function_ref = *args.TryGet(2);
    const std::size_t chunk_size = std::max<std::size_t>(1, task_runtime().worker_count());
    return run_parallel_reduce_chunked(items, init, function_ref, chunk_size);
  }
};

struct Apply {
  // arg = [value, func_ref]
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 2, "Apply");
    return invoke_callable_ref(*args.TryGet(1), *args.TryGet(0));
  }
};

struct Branch {
  // arg = [condition, value, true_func_ref, false_func_ref]
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 4, "Branch");
    const Value& condition = *args.TryGet(0);
    const Value& value = *args.TryGet(1);
    const Value& true_func = *args.TryGet(2);
    const Value& false_func = *args.TryGet(3);
    return as_bool(condition) ? invoke_callable_ref(true_func, value) : invoke_callable_ref(false_func, value);
  }
};

struct Loop {
  // arg = [state, continue_func_ref, step_func_ref]
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 3, "Loop");
    Value state = *args.TryGet(0);
    const Value& continue_func = *args.TryGet(1);
    const Value& step_func = *args.TryGet(2);
    while (as_bool(invoke_callable_ref(continue_func, state))) {
      state = invoke_callable_ref(step_func, std::move(state));
    }
    return state;
  }
};

struct LoopN {
  // arg = [state, continue_func_ref, step_func_ref, max_iters]
  auto operator()(Value arg) const -> Value {
    const auto& args = require_args(arg, 4, "LoopN");
    Value state = *args.TryGet(0);
    const Value& continue_func = *args.TryGet(1);
    const Value& step_func = *args.TryGet(2);
    const std::size_t max_iters = as_index_strict(*args.TryGet(3), "LoopN max_iters");

    std::size_t steps = 0;
    while (as_bool(invoke_callable_ref(continue_func, state))) {
      if (steps >= max_iters) { throw std::runtime_error{"LoopN: exceeded max_iters"}; }
      state = invoke_callable_ref(step_func, std::move(state));
      ++steps;
    }
    return state;
  }
};

}  // namespace fleaux::runtime
