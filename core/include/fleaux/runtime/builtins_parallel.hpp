#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <thread>
#include <vector>

#include "fleaux/runtime/builtins_core.hpp"
#include "fleaux/runtime/value.hpp"

namespace fleaux::runtime {

struct TaskControl {
  std::mutex mutex;
  std::condition_variable cv;
  bool started{false};
  bool completed{false};
  bool cancel_requested{false};
  Value result = ResultErr(make_tuple(make_string("Task: result not ready")));
};

inline auto task_registry() -> ScopedRegistry<TaskControlPtr>& {
  return runtime_execution_state().task_registry;
}

inline auto task_registry_mutex() -> std::mutex& {
  return runtime_execution_state().task_registry_mutex;
}

[[nodiscard]] inline auto task_registry_size() -> std::size_t {
  std::scoped_lock lock(task_registry_mutex());
  return task_registry().active_count;
}

[[nodiscard]] inline auto collect_registered_tasks_since(const std::size_t checkpoint)
    -> std::vector<std::pair<RegistryId, TaskControlPtr>> {
  std::vector<std::pair<RegistryId, TaskControlPtr>> tasks;

  std::scoped_lock lock(task_registry_mutex());
  const auto& registry = task_registry();
  const auto log_size = registry.registration_log.size();
  for (std::size_t index = checkpoint; index < log_size; ++index) {
    const auto slot = registry.registration_log[index];
    const auto entry_index = static_cast<std::size_t>(slot);
    if (entry_index >= registry.entries.size()) {
      continue;
    }

    const auto& [value, generation, occupied] = registry.entries[entry_index];
    if (!occupied || !value) {
      continue;
    }

    tasks.emplace_back(RegistryId{.slot = slot, .generation = generation}, value);
  }

  return tasks;
}

inline void retire_registered_tasks(const std::vector<std::pair<RegistryId, TaskControlPtr>>& tasks,
                                    const std::size_t checkpoint) {
  std::scoped_lock lock(task_registry_mutex());
  auto& registry = task_registry();
  for (const auto& [id, task] : tasks) {
    (void)task;
    if (registry.get(id) != nullptr) {
      registry.retire(id.slot);
    }
  }
  registry.registration_log.resize(checkpoint);
}

inline void reset_task_registry_for_tests() {
  const auto tasks = collect_registered_tasks_since(0);
  {
    std::scoped_lock lock(task_registry_mutex());
    task_registry().clear();
  }
  for (const auto& [id, task] : tasks) {
    (void)id;
    {
      std::scoped_lock task_lock(task->mutex);
      task->cancel_requested = true;
    }
    task->cv.notify_all();
  }
}

inline auto make_task_handle_from_id(const RegistryId& id) -> Value {
  return make_tagged_registry_token(k_task_handle_tag, id.slot, id.generation);
}

[[nodiscard]] inline auto task_id_from_handle(const Value& task_handle) -> std::optional<RegistryId> {
  return parse_tagged_registry_token(task_handle, k_task_handle_tag);
}

[[nodiscard]] inline auto task_control_from_handle(const Value& task_handle) -> TaskControlPtr {
  if (const auto token_state_id = parse_tagged_registry_token_state_id(task_handle, k_task_handle_tag);
      !token_state_id || *token_state_id != runtime_execution_state().token_state_id) {
    return {};
  }
  const auto id = task_id_from_handle(task_handle);
  if (!id) {
    return {};
  }
  std::scoped_lock lock(task_registry_mutex());
  const TaskControlPtr* control = task_registry().get(*id);
  return control != nullptr ? *control : TaskControlPtr{};
}

inline auto request_task_cancel(const TaskControlPtr& task) -> bool {
  if (!task) {
    return false;
  }
  std::scoped_lock lock(task->mutex);
  if (task->started || task->completed || task->cancel_requested) {
    return false;
  }
  task->cancel_requested = true;
  return true;
}

inline void request_task_cancel_and_notify(const TaskControlPtr& task) {
  if (request_task_cancel(task)) {
    task->cv.notify_all();
  }
}

[[nodiscard]] inline auto wait_task_result(const TaskControlPtr& task) -> Value {
  if (!task) {
    return ResultErr(make_tuple(make_string("Task.Await: invalid task handle")));
  }

  std::unique_lock lock(task->mutex);
  task->cv.wait(lock, [&task]() -> bool { return task->completed; });
  return task->result;
}

[[nodiscard]] inline auto wait_task_result_for(const TaskControlPtr& task, const std::chrono::milliseconds timeout)
    -> std::optional<Value> {
  if (!task) {
    return std::nullopt;
  }

  std::unique_lock lock(task->mutex);
  if (!task->cv.wait_for(lock, timeout, [&task]() -> bool { return task->completed; })) {
    return std::nullopt;
  }
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
    if (worker_count == 0) {
      worker_count = static_cast<std::size_t>(std::thread::hardware_concurrency());
    }
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
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  TaskRuntime(const TaskRuntime&) = delete;
  auto operator=(const TaskRuntime&) -> TaskRuntime& = delete;

  // Submit a tracked user-visible task. Callable is resolved eagerly so
  // workers do not race on callable-registry cleanup.
  [[nodiscard]] auto submit(const Value& function_ref, Value arg) -> TaskControlPtr {
    auto task = std::make_shared<TaskControl>();
    auto process_args = get_process_args();

    RuntimeCallable callable;
    try {
      callable = resolve_callable_ref(function_ref);
    } catch (const std::exception& ex) {
      std::scoped_lock task_lock(task->mutex);
      task->started = true;
      task->completed = true;
      task->result = ResultErr(make_tuple(make_string(normalize_runtime_error_message(ex.what()))));
      task->cv.notify_all();
      return task;
    }

    enqueue(Job{.callable = std::move(callable),
                .arg = std::move(arg),
                .task = task,
                .execution_state = std::ref(runtime_execution_state()),
                .output_stream = std::ref(runtime_output_stream()),
                .input_stream = std::ref(runtime_input_stream()),
                .process_args = std::move(process_args)});
    return task;
  }

  // Submit an untracked parallel item (not registered in the task registry).
  [[nodiscard]] auto submit_untracked(RuntimeCallable callable, Value arg) -> TaskControlPtr {
    auto task = std::make_shared<TaskControl>();
    auto process_args = get_process_args();
    enqueue(Job{.callable = std::move(callable),
                .arg = std::move(arg),
                .task = task,
                .execution_state = std::ref(runtime_execution_state()),
                .output_stream = std::ref(runtime_output_stream()),
                .input_stream = std::ref(runtime_input_stream()),
                .process_args = std::move(process_args)});
    return task;
  }

  [[nodiscard]] auto worker_count() const -> std::size_t { return workers_.size(); }

private:
  struct Job {
    RuntimeCallable callable;
    Value arg;
    TaskControlPtr task;
    std::reference_wrapper<RuntimeExecutionState> execution_state{default_runtime_execution_state()};
    std::reference_wrapper<std::ostream> output_stream{std::cout};
    std::reference_wrapper<std::istream> input_stream{std::cin};
    std::vector<std::string> process_args{};
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
        if (stopping_ && queue_.empty()) {
          return;
        }
        job = std::move(queue_.front());
        queue_.pop_front();
      }

      ActiveRuntimeExecutionStateScope execution_state_scope(job.execution_state.get());
      RuntimeOutputStreamScope output_scope(job.output_stream.get());
      RuntimeInputStreamScope input_scope(job.input_stream.get());
      const auto submitted_process_args = std::move(job.process_args);
      RuntimeProcessArgsOverrideScope process_args_scope(submitted_process_args);

      {
        std::scoped_lock task_lock(job.task->mutex);
        if (job.task->cancel_requested) {
          job.task->completed = true;
          job.task->result = ResultErr(make_tuple(make_string(String{k_task_cancelled_message})));
          job.task->cv.notify_all();
          continue;
        }
        job.task->started = true;
      }

      Value result;
      try {
        result = ResultOk(make_tuple(job.callable(std::move(job.arg))));
      } catch (const RuntimePayloadError& ex) {
        result = ResultErr(ex.payload());
      } catch (const std::exception& ex) {
        result = ResultErr(make_tuple(make_string(normalize_runtime_error_message(ex.what()))));
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
    const auto tasks = collect_registered_tasks_since(checkpoint_);

    for (const auto& [id, task] : tasks) {
      (void)id;
      request_task_cancel_and_notify(task);
      (void)wait_task_result(task);
    }

    retire_registered_tasks(tasks, checkpoint_);
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
  if (value == nullptr) {
    return std::nullopt;
  }
  const auto parsed = as_index_strict(*value, key);
  if (parsed == 0) {
    throw std::invalid_argument{std::string{"Parallel.WithOptions: "} + std::string{key} + " must be > 0"};
  }
  return parsed;
}

inline auto strip_object_key_prefix(const std::string& internal_key) -> std::string {
  if (internal_key.size() >= 2U && internal_key[1] == ':') {
    return internal_key.substr(2);
  }
  return internal_key;
}

inline auto validate_parallel_with_options_keys(const Object& options) -> void {
  for (const auto& internal_key : options | std::views::keys) {
    if (internal_key == "s:max_workers") {
      continue;
    }
    throw std::invalid_argument{"Parallel.WithOptions: unsupported option '" + strip_object_key_prefix(internal_key) +
                                "'"};
  }
}

// Run a parallel map over items using the shared task pool. max_in_flight caps how
// many items are queued to the pool at once (back-pressure). Output order matches
// input order. Returns a Result value.
inline auto run_parallel_map_pooled(const Array& items, const Value& function_ref, const std::size_t max_in_flight)
    -> Value {
  if (items.Size() == 0) {
    return ResultOk(make_tuple(Value{Array{}}));
  }

  // Resolve callable once before submitting.
  RuntimeCallable callable;
  try {
    callable = resolve_callable_ref(function_ref);
  } catch (const std::exception& ex) {
    return ResultErr(
        make_tuple(make_tuple(make_int(static_cast<Int>(0)), make_string(normalize_runtime_error_message(ex.what())))));
  }

  const std::size_t n = items.Size();
  const std::size_t window = std::min(max_in_flight, n);

  // Pre-collect items so we can index them.
  std::vector<Value> item_vec;
  item_vec.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    item_vec.push_back(*items.TryGet(i));
  }

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
  while (in_flight.size() < window && next_submit < n) {
    submit_next();
  }

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
      return ResultErr(make_tuple(
          make_tuple(make_int(static_cast<Int>(item_index)), make_string("Parallel.Map: unexpected result shape"))));
    }
    if (const auto& ok_tag = result_arr->TryGet(0)->TryGetBool(); !ok_tag || !*ok_tag) {
      const auto payload = result_arr->Size() >= 2 ? *result_arr->TryGet(1) : make_string("unknown error");
      const auto msg = payload.HasArray() && as_array(payload).Size() == 1 ? to_string(*as_array(payload).TryGet(0))
                                                                           : to_string(payload);
      return ResultErr(make_tuple(make_tuple(make_int(static_cast<Int>(item_index)), make_string(msg))));
    }
    const auto& payload_arr = result_arr->TryGet(1)->TryGetArray();
    out.PushBack(payload_arr && payload_arr->Size() == 1 ? *payload_arr->TryGet(0) : *result_arr->TryGet(1));
  }

  return ResultOk(make_tuple(Value{std::move(out)}));
}

[[nodiscard]] inline auto TaskSpawn(Value arg) -> Value {
  const auto& args = require_args(arg, 2, "Task.Spawn");
  const Value& function_ref = *args.TryGet(0);
  const Value& value = *args.TryGet(1);

  const TaskControlPtr task = task_runtime().submit(function_ref, value);
  const RegistryId id = [&]() {
    std::scoped_lock lock(task_registry_mutex());
    return task_registry().insert(task, /*logged=*/true);
  }();
  return make_task_handle_from_id(id);
}

[[nodiscard]] inline auto TaskAwait(Value arg) -> Value {
  const Value task_handle = unwrap_singleton_arg(std::move(arg));
  return wait_task_result(task_control_from_handle(task_handle));
}

[[nodiscard]] inline auto TaskAwaitAll(Value arg) -> Value {
  const Value tasks_arg = unwrap_singleton_arg(std::move(arg));
  if (!tasks_arg.HasArray()) {
    return ResultErr(make_tuple(make_int(static_cast<Int>(0)), make_string("Task.AwaitAll: tasks must be a Tuple")));
  }
  const auto& tasks = as_array(tasks_arg);

  Array out;
  out.Reserve(tasks.Size());
  for (std::size_t task_index = 0; task_index < tasks.Size(); ++task_index) {
    const TaskControlPtr task = task_control_from_handle(*tasks.TryGet(task_index));
    if (!task) {
      return ResultErr(make_tuple(
          make_tuple(make_int(static_cast<Int>(task_index)), make_string("Task.AwaitAll: invalid task handle"))));
    }

    const Value result = wait_task_result(task);

    const auto& tuple = require_result_tuple(result, "Task.AwaitAll");
    if (!as_bool(*tuple.TryGet(0))) {
      return ResultErr(make_tuple(make_tuple(make_int(static_cast<Int>(task_index)), *tuple.TryGet(1))));
    }
    out.PushBack(*tuple.TryGet(1));
  }
  return ResultOk(make_tuple(Value{std::move(out)}));
}

[[nodiscard]] inline auto TaskCancel(Value arg) -> Value {
  const Value task_handle = unwrap_singleton_arg(std::move(arg));
  const TaskControlPtr task = task_control_from_handle(task_handle);
  if (!task) {
    return make_bool(false);
  }
  const bool cancelled = request_task_cancel(task);
  if (cancelled) {
    task->cv.notify_all();
  }
  return make_bool(cancelled);
}

[[nodiscard]] inline auto TaskWithTimeout(Value arg) -> Value {
  const auto& args = require_args(arg, 2, "Task.WithTimeout");
  const Int timeout_ms = as_int_value_strict(*args.TryGet(1), "Task.WithTimeout timeout_ms");
  if (timeout_ms < 0) {
    return ResultErr(make_tuple(make_string("Task.WithTimeout: timeout_ms must be non-negative")));
  }
  const TaskControlPtr task = task_control_from_handle(*args.TryGet(0));
  if (!task) {
    return ResultErr(make_tuple(make_string("Task.WithTimeout: invalid task handle")));
  }
  const auto result = wait_task_result_for(task, std::chrono::milliseconds{timeout_ms});
  if (!result) {
    return ResultErr(make_tuple(make_string(String{k_task_timeout_message})));
  }
  return *result;
}

[[nodiscard]] inline auto ParallelMap(Value arg) -> Value {
  const auto& args = require_args(arg, 2, "Parallel.Map");
  const auto& items = as_array(*args.TryGet(0));
  const Value function_ref = *args.TryGet(1);
  const std::size_t max_in_flight = std::max<std::size_t>(1, items.Size());
  return run_parallel_map_pooled(items, function_ref, max_in_flight);
}

[[nodiscard]] inline auto ParallelWithOptions(Value arg) -> Value {
  const auto& args = require_args(arg, 3, "Parallel.WithOptions");
  if (!args.TryGet(2)->HasObject()) {
    return ResultErr(
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
    return ResultErr(
        make_tuple(make_int(static_cast<Int>(0)), make_string(normalize_runtime_error_message(ex.what()))));
  }
}

// Run fn on each item in parallel for side effects. Output is discarded; returns Result(()).
// On first error returns Err((index, message)).
inline auto run_parallel_foreach_pooled(const Array& items, const Value& function_ref, const std::size_t max_in_flight)
    -> Value {
  if (items.Size() == 0) {
    return ResultOk(make_tuple(Value{Array{}}));
  }

  RuntimeCallable callable;
  try {
    callable = resolve_callable_ref(function_ref);
  } catch (const std::exception& ex) {
    return ResultErr(
        make_tuple(make_tuple(make_int(static_cast<Int>(0)), make_string(normalize_runtime_error_message(ex.what())))));
  }

  const std::size_t n = items.Size();
  const std::size_t window = std::min(max_in_flight, n);

  std::vector<Value> item_vec;
  item_vec.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    item_vec.push_back(*items.TryGet(i));
  }

  std::deque<std::pair<std::size_t, TaskControlPtr>> in_flight;
  std::size_t next_submit = 0;

  auto submit_next = [&]() -> void {
    if (next_submit < n) {
      auto task = task_runtime().submit_untracked(callable, item_vec[next_submit]);
      in_flight.emplace_back(next_submit, std::move(task));
      ++next_submit;
    }
  };

  while (in_flight.size() < window && next_submit < n) {
    submit_next();
  }

  while (!in_flight.empty()) {
    auto [item_index, task] = std::move(in_flight.front());
    in_flight.pop_front();
    submit_next();

    const Value result = wait_task_result(task);
    const auto& result_arr = result.TryGetArray();
    if (!result_arr || result_arr->Size() < 1) {
      return ResultErr(make_tuple(make_tuple(make_int(static_cast<Int>(item_index)),
                                             make_string("Parallel.ForEach: unexpected result shape"))));
    }
    if (const auto& ok_tag = result_arr->TryGet(0)->TryGetBool(); !ok_tag || !*ok_tag) {
      const auto payload = result_arr->Size() >= 2 ? *result_arr->TryGet(1) : make_string("unknown error");
      const auto msg = payload.HasArray() && as_array(payload).Size() == 1 ? to_string(*as_array(payload).TryGet(0))
                                                                           : to_string(payload);
      return ResultErr(make_tuple(make_tuple(make_int(static_cast<Int>(item_index)), make_string(msg))));
    }
  }

  return ResultOk(make_tuple(Value{Array{}}));
}

// Deterministic left-fold helper. Items are partitioned into chunks, each chunk is reduced
// sequentially via the pool, and chunk results are chained in input order.
// fn signature: fn(acc, item) -> acc
inline auto run_parallel_reduce_chunked(const Array& items, const Value& init, const Value& function_ref,
                                        const std::size_t chunk_size) -> Value {
  const std::size_t n = items.Size();
  if (n == 0) {
    return ResultOk(make_tuple(init));
  }

  RegisteredCallable callable;
  try {
    callable = resolve_registered_callable_ref(function_ref);
  } catch (const std::exception& ex) {
    return ResultErr(
        make_tuple(make_tuple(make_int(static_cast<Int>(0)), make_string(normalize_runtime_error_message(ex.what())))));
  }

  // Collect items into vector for random access.
  std::vector<Value> item_vec;
  item_vec.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    item_vec.push_back(*items.TryGet(i));
  }

  // Build chunk descriptors: [start, end) pairs.
  const std::size_t cs = std::max<std::size_t>(1, chunk_size);
  struct Chunk {
    std::size_t start;
    std::size_t end;
  };
  std::vector<Chunk> chunks;
  for (std::size_t s = 0; s < n; s += cs) {
    chunks.push_back({.start = s, .end = std::min(s + cs, n)});
  }

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
    for (std::size_t i = start; i < end; ++i) {
      chunk_items.PushBack(item_vec[i]);
    }

    // Submit a task that folds this chunk.
    // The task receives (seed, items_tuple) and calls callable(acc, item) repeatedly.
    Value seed_copy = acc;
    Array chunk_copy = chunk_items;
    RegisteredCallable fn_copy = callable;
    const std::size_t first_idx = global_index;

    auto chunk_task = task_runtime().submit_untracked(
        RuntimeCallable{[fn = std::move(fn_copy), items_arr = std::move(chunk_copy), seed = std::move(seed_copy),
                         base_idx = first_idx](const Value& /*ignored*/) mutable -> Value {
          Value local_acc = std::move(seed);
          std::size_t idx = base_idx;
          for (std::size_t i = 0; i < items_arr.Size(); ++i, ++idx) {
            try {
              local_acc = invoke_binary_callable(fn, std::move(local_acc), std::move(items_arr[i]));
            } catch (const std::exception& ex) {
              const auto message = normalize_runtime_error_message(ex.what());
              throw RuntimePayloadError{make_tuple(make_int(static_cast<Int>(idx)), make_string(message)), message};
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
      return ResultErr(make_tuple(
          make_tuple(make_int(static_cast<Int>(start)), make_string("Parallel.Reduce: unexpected result shape"))));
    }
    if (const auto& ok_tag = chunk_arr->TryGet(0)->TryGetBool(); !ok_tag || !*ok_tag) {
      return chunk_result;  // propagate Err as-is
    }
    acc = chunk_arr->Size() >= 2 ? *chunk_arr->TryGet(1) : Value{Array{}};
    if (acc.HasArray() && as_array(acc).Size() == 1) {
      acc = *as_array(acc).TryGet(0);
    }
  }

  return ResultOk(make_tuple(acc));
}

[[nodiscard]] inline auto ParallelForEach(Value arg) -> Value {
  const auto& args = require_args(arg, 2, "Parallel.ForEach");
  const auto& items = as_array(*args.TryGet(0));
  const Value function_ref = *args.TryGet(1);
  const std::size_t max_in_flight = std::max<std::size_t>(1, items.Size());
  return run_parallel_foreach_pooled(items, function_ref, max_in_flight);
}

[[nodiscard]] inline auto ParallelReduce(Value arg) -> Value {
  const auto& args = require_args(arg, 3, "Parallel.Reduce");
  const auto& items = as_array(*args.TryGet(0));
  const Value init = *args.TryGet(1);
  const Value function_ref = *args.TryGet(2);
  const std::size_t chunk_size = std::max<std::size_t>(1, task_runtime().worker_count());
  return run_parallel_reduce_chunked(items, init, function_ref, chunk_size);
}

}  // namespace fleaux::runtime