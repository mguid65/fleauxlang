#pragma once
// Task and parallel builtins (Std.Task.*, Std.Parallel.*), run on one shared thread pool.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "fleaux/runtime/builtins_core.hpp"
#include "fleaux/runtime/value.hpp"

namespace fleaux::runtime {

// A task's state. Task values share it; the pool's worker that runs the task fills in its result.
class TaskControl final : public TaskState {
public:
  std::mutex mutex;
  std::condition_variable cv;
  bool started{false};
  bool completed{false};
  bool cancel_requested{false};
  Value result = ResultErr(make_string("Task: result not ready"));
};

[[nodiscard]] inline auto make_task() -> Value { return Value::adopt(ValueKind::kTask, new TaskControl); }

// The task a handle refers to, or nullptr when the value is not a task.
[[nodiscard]] inline auto task_control(const Value& handle) -> TaskControl* {
  return handle.is_task() ? &static_cast<TaskControl&>(handle.task()) : nullptr;
}

// Cancels a task that has not started; false when it already started, completed, or was cancelled.
inline auto request_task_cancel(TaskControl* task) -> bool {
  if (task == nullptr) {
    return false;
  }
  const std::scoped_lock lock(task->mutex);
  if (task->started || task->completed || task->cancel_requested) {
    return false;
  }
  task->cancel_requested = true;
  return true;
}

inline void request_task_cancel_and_notify(TaskControl* task) {
  if (request_task_cancel(task)) {
    task->cv.notify_all();
  }
}

[[nodiscard]] inline auto wait_task_result(TaskControl* task) -> Value {
  if (task == nullptr) {
    return ResultErr(make_string("Task.Await: invalid task handle"));
  }
  std::unique_lock lock(task->mutex);
  task->cv.wait(lock, [task]() -> bool { return task->completed; });
  return task->result;
}

[[nodiscard]] inline auto wait_task_result_for(TaskControl* task, const std::chrono::milliseconds timeout)
    -> std::optional<Value> {
  if (task == nullptr) {
    return std::nullopt;
  }
  std::unique_lock lock(task->mutex);
  if (!task->cv.wait_for(lock, timeout, [task]() -> bool { return task->completed; })) {
    return std::nullopt;
  }
  return task->result;
}

// Bounded thread pool shared by Std.Task.* and Std.Parallel.*.
//
// Worker count defaults to hardware_concurrency() clamped to [1, 16]. A job carries the function value it calls and
// the stream and process-argument context of the code that submitted it; the jobs of one batch share their process
// arguments.
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
    for (std::size_t index = 0; index < worker_count; ++index) {
      workers_.emplace_back([this]() -> void { worker_loop(); });
    }
  }

  ~TaskRuntime() {
    {
      const std::scoped_lock lock(mutex_);
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
  TaskRuntime(TaskRuntime&&) = delete;
  auto operator=(TaskRuntime&&) -> TaskRuntime& = delete;

  // Runs `function` with `argument` on the pool. The task value holds an Ok or Err result once it completes.
  [[nodiscard]] auto submit(Value function, Value argument) -> Value {
    Value task = make_task();
    enqueue(Job{.function = std::move(function),
                .argument = std::move(argument),
                .task = task,
                .execution_state = std::ref(runtime_execution_state()),
                .output_stream = std::ref(runtime_output_stream()),
                .input_stream = std::ref(runtime_input_stream()),
                .process_args = std::make_shared<const std::vector<std::string>>(get_process_args())});
    return task;
  }

  // Runs `function` with each of `arguments` on the pool, one task each, in order. The jobs are queued under one lock,
  // and as many workers are woken as there are jobs, up to all of them. Submitted one at a time, items that take less
  // time than a submission left the workers asleep between them, and every submission paid to wake one: about 4.7 us
  // per item, however little the items did.
  [[nodiscard]] auto submit_all(const Value& function, const TupleView arguments) -> std::vector<Value> {
    std::vector<Value> tasks;
    std::vector<Job> jobs;
    tasks.reserve(arguments.size());
    jobs.reserve(arguments.size());
    const auto process_args = std::make_shared<const std::vector<std::string>>(get_process_args());
    for (const Value& argument : arguments) {
      Value task = make_task();
      tasks.push_back(task);
      jobs.push_back(Job{.function = function,
                         .argument = argument,
                         .task = std::move(task),
                         .execution_state = std::ref(runtime_execution_state()),
                         .output_stream = std::ref(runtime_output_stream()),
                         .input_stream = std::ref(runtime_input_stream()),
                         .process_args = process_args});
    }
    {
      const std::scoped_lock lock(mutex_);
      for (Job& job : jobs) {
        queue_.push_back(std::move(job));
      }
    }
    // Waking every worker for a few jobs made the samples with a few tasks up to 2.3 times slower.
    for (std::size_t woken = 0; woken < std::min(jobs.size(), workers_.size()); ++woken) {
      cv_.notify_one();
    }
    return tasks;
  }

  [[nodiscard]] auto worker_count() const -> std::size_t { return workers_.size(); }

private:
  struct Job {
    Value function;
    Value argument;
    Value task;
    std::reference_wrapper<RuntimeExecutionState> execution_state{default_runtime_execution_state()};
    std::reference_wrapper<std::ostream> output_stream{std::cout};
    std::reference_wrapper<std::istream> input_stream{std::cin};
    std::shared_ptr<const std::vector<std::string>> process_args{};
  };

  void enqueue(Job job) {
    {
      const std::scoped_lock lock(mutex_);
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

      const ActiveRuntimeExecutionStateScope execution_state_scope(job.execution_state.get());
      const RuntimeOutputStreamScope output_scope(job.output_stream.get());
      const RuntimeInputStreamScope input_scope(job.input_stream.get());
      const auto submitted_process_args = std::move(job.process_args);
      const RuntimeProcessArgsOverrideScope process_args_scope(*submitted_process_args);
      TaskControl& task = *task_control(job.task);

      {
        const std::scoped_lock task_lock(task.mutex);
        if (task.cancel_requested) {
          task.completed = true;
          task.result = ResultErr(make_string(String{k_task_cancelled_message}));
          task.cv.notify_all();
          continue;
        }
        task.started = true;
      }

      // A failure of the VM's function values arrives as a value (call_or_fail); one of a function made from C++, such
      // as the parallel builtins' chunks, as an exception.
      Value result;
      try {
        std::string error;
        if (auto value = as_callable(job.function).call_or_fail(std::move(job.argument), error); value.has_value()) {
          result = ResultOk(std::move(*value));
        } else {
          result = ResultErr(make_string(normalize_runtime_error_message(std::move(error))));
        }
      } catch (const RuntimePayloadError& ex) {
        result = ResultErr(ex.payload());
      } catch (const std::exception& ex) {
        result = ResultErr(make_string(normalize_runtime_error_message(ex.what())));
      }

      {
        const std::scoped_lock task_lock(task.mutex);
        task.result = std::move(result);
        task.completed = true;
      }
      task.cv.notify_all();
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

// The number of tasks the active run has spawned and not yet settled.
[[nodiscard]] inline auto spawned_task_count() -> std::size_t {
  RuntimeExecutionState& state = runtime_execution_state();
  const std::scoped_lock lock(state.task_mutex);
  return state.spawned_tasks.size();
}

// Settles every task the active run spawned: cancels the ones not started, without waiting for the others.
inline void reset_spawned_tasks_for_tests() {
  RuntimeExecutionState& state = runtime_execution_state();
  std::vector<Value> tasks;
  {
    const std::scoped_lock lock(state.task_mutex);
    tasks.swap(state.spawned_tasks);
  }
  for (const Value& task : tasks) {
    request_task_cancel_and_notify(task_control(task));
  }
}

// When a run ends, cancels the tasks it spawned that have not started, and waits for all of them, so that no task
// outlives the run that spawned it.
class TaskScope {
public:
  TaskScope() : state_(runtime_execution_state()) {
    const std::scoped_lock lock(state_.task_mutex);
    checkpoint_ = state_.spawned_tasks.size();
  }

  TaskScope(const TaskScope&) = delete;
  auto operator=(const TaskScope&) -> TaskScope& = delete;
  TaskScope(TaskScope&&) = delete;
  auto operator=(TaskScope&&) -> TaskScope& = delete;

  ~TaskScope() {
    std::vector<Value> tasks;
    {
      const std::scoped_lock lock(state_.task_mutex);
      auto& spawned = state_.spawned_tasks;
      tasks.reserve(spawned.size() - checkpoint_);
      for (auto index = checkpoint_; index < spawned.size(); ++index) {
        tasks.push_back(std::move(spawned[index]));
      }
      spawned.resize(checkpoint_);
    }
    for (const Value& task : tasks) {
      request_task_cancel_and_notify(task_control(task));
      (void)wait_task_result(task_control(task));
    }
  }

private:
  RuntimeExecutionState& state_;
  std::size_t checkpoint_{0};
};

// The Err value of a parallel item that failed: (index, message).
[[nodiscard]] inline auto parallel_item_error(const std::size_t index, String message) -> Value {
  return ResultErr(make_tuple(make_int(static_cast<Int>(index)), make_string(std::move(message))));
}

// The text of an Err result's payload: a one-item tuple prints as its item.
[[nodiscard]] inline auto result_error_text(const TupleView result) -> String {
  const Value payload = result.size() >= 2 ? result[1] : make_string("unknown error");
  return payload.is_tuple() && payload.tuple().size() == 1 ? to_string(payload.tuple()[0]) : to_string(payload);
}

// Runs `function` on each item on the shared pool, keeping at most `max_in_flight` items queued at once, and calls
// `on_result` with each item's index and Ok payload in input order. Returns the Err of the first item that fails. With
// room for every item, they are all queued in one batch (TaskRuntime::submit_all); otherwise each is submitted when
// the one `max_in_flight` before it has finished.
template <typename OnResult>
[[nodiscard]] auto run_parallel_pooled(const TupleView items, const Value& function, const std::size_t max_in_flight,
                                       const std::string_view builtin_name, OnResult&& on_result)
    -> std::optional<Value> {
  // Waits for an item's task: its Err, or nothing after passing its payload to on_result.
  const auto settle = [&](const std::size_t item_index, const Value& task) -> std::optional<Value> {
    const Value result = wait_task_result(task_control(task));
    if (!result.is_tuple() || result.tuple().empty()) {
      return parallel_item_error(item_index, std::format("{}: unexpected result shape", builtin_name));
    }
    const TupleView outcome = result.tuple();
    if (!outcome[0].is_bool() || !outcome[0].bool_value()) {
      return parallel_item_error(item_index, result_error_text(outcome));
    }
    const Value& payload = outcome[1];
    on_result(item_index, payload.is_tuple() && payload.tuple().size() == 1 ? payload.tuple()[0] : payload);
    return std::nullopt;
  };

  const std::size_t count = items.size();
  if (max_in_flight >= count) {
    const std::vector<Value> tasks = task_runtime().submit_all(function, items);
    for (std::size_t item_index = 0; item_index < count; ++item_index) {
      if (auto error = settle(item_index, tasks[item_index])) {
        return error;
      }
    }
    return std::nullopt;
  }

  const std::size_t window = std::min(max_in_flight, count);
  std::deque<std::pair<std::size_t, Value>> in_flight;
  std::size_t next_submit = 0;
  const auto submit_next = [&]() -> void {
    if (next_submit < count) {
      in_flight.emplace_back(next_submit, task_runtime().submit(function, items[next_submit]));
      ++next_submit;
    }
  };
  while (in_flight.size() < window && next_submit < count) {
    submit_next();
  }
  while (!in_flight.empty()) {
    auto [item_index, task] = std::move(in_flight.front());
    in_flight.pop_front();
    submit_next();
    if (auto error = settle(item_index, task)) {
      return error;
    }
  }
  return std::nullopt;
}

// Std.Parallel.Map: the results in input order, or the Err of the first item that fails.
[[nodiscard]] inline auto run_parallel_map_pooled(const TupleView items, const Value& function,
                                                  const std::size_t max_in_flight) -> Value {
  if (items.empty()) {
    return ResultOk(make_tuple());
  }
  TupleBuilder out(items.size());
  if (auto error = run_parallel_pooled(items, function, max_in_flight, "Parallel.Map",
                                       [&out](std::size_t, const Value& value) -> void { out.push(value); })) {
    return std::move(*error);
  }
  return ResultOk(std::move(out).finish());
}

[[nodiscard]] inline auto TaskSpawn(const Arguments& args) -> Value {
  Value task = task_runtime().submit(args[0], args[1]);
  RuntimeExecutionState& state = runtime_execution_state();
  const std::scoped_lock lock(state.task_mutex);
  state.spawned_tasks.push_back(task);
  return task;
}

[[nodiscard]] inline auto TaskAwait(Value arg) -> Value {
  const Value task_handle = std::move(arg);
  return wait_task_result(task_control(task_handle));
}

[[nodiscard]] inline auto TaskAwaitAll(Value arg) -> Value {
  const Value tasks_arg = std::move(arg);
  const TupleView tasks = as_tuple(tasks_arg);

  TupleBuilder out(tasks.size());
  for (std::size_t task_index = 0; task_index < tasks.size(); ++task_index) {
    TaskControl* task = task_control(tasks[task_index]);
    if (task == nullptr) {
      return parallel_item_error(task_index, "Task.AwaitAll: invalid task handle");
    }
    const Value result = wait_task_result(task);
    const TupleView outcome = as_tuple(result);
    if (!as_bool(outcome[0])) {
      return ResultErr(make_tuple(make_int(static_cast<Int>(task_index)), Value{outcome[1]}));
    }
    out.push(outcome[1]);
  }
  return ResultOk(std::move(out).finish());
}

[[nodiscard]] inline auto TaskCancel(Value arg) -> Value {
  const Value task_handle = std::move(arg);
  TaskControl* task = task_control(task_handle);
  const bool cancelled = request_task_cancel(task);
  if (cancelled) {
    task->cv.notify_all();
  }
  return make_bool(cancelled);
}

[[nodiscard]] inline auto TaskWithTimeout(const Arguments& args) -> Value {
  const std::size_t timeout_ms = uint64_index(args[1]);
  TaskControl* task = task_control(args[0]);
  if (task == nullptr) {
    return ResultErr(make_string("Task.WithTimeout: invalid task handle"));
  }
  const auto result = wait_task_result_for(task, std::chrono::milliseconds{static_cast<Int>(timeout_ms)});
  if (!result) {
    return ResultErr(make_string(String{k_task_timeout_message}));
  }
  return *result;
}

[[nodiscard]] inline auto ParallelMap(const Arguments& args) -> Value {
  const TupleView items = as_tuple(args[0]);
  const std::size_t max_in_flight = std::max<std::size_t>(1, items.size());
  return run_parallel_map_pooled(items, args[1], max_in_flight);
}

// The options Std.Parallel.WithOptions accepts. The only option is "max_workers", a positive integer.
[[nodiscard]] inline auto parallel_max_workers_option(const Dict& options) -> std::optional<std::size_t> {
  static const Value max_workers_key = make_string("max_workers");
  for (const auto& [key, item] : options) {
    if (!(key == max_workers_key)) {
      throw std::invalid_argument{
          "Parallel.WithOptions: unsupported option '" + (key.is_string() ? key.string_value() : to_string(key)) + "'"};
    }
  }
  const Value* value = options.find(max_workers_key);
  if (value == nullptr) {
    return std::nullopt;
  }
  const auto parsed = as_index_strict(*value, "max_workers");
  if (parsed == 0) {
    throw std::invalid_argument{"Parallel.WithOptions: max_workers must be > 0"};
  }
  return parsed;
}

[[nodiscard]] inline auto ParallelWithOptions(const Arguments& args) -> Value {
  if (!args[2].is_dict()) {
    return ResultErr(
        make_tuple(make_int(static_cast<Int>(0)), make_string("Parallel.WithOptions: options must be a Dict")));
  }
  const TupleView items = as_tuple(args[0]);
  try {
    const std::size_t max_in_flight =
        parallel_max_workers_option(args[2].dict()).value_or(task_runtime().worker_count());
    return run_parallel_map_pooled(items, args[1], max_in_flight);
  } catch (const std::exception& ex) {
    return ResultErr(
        make_tuple(make_int(static_cast<Int>(0)), make_string(normalize_runtime_error_message(ex.what()))));
  }
}

// Std.Parallel.ForEach: runs `function` on each item for its effects. Returns Ok(()) or the Err of the first item
// that fails.
[[nodiscard]] inline auto run_parallel_foreach_pooled(const TupleView items, const Value& function,
                                                      const std::size_t max_in_flight) -> Value {
  if (items.empty()) {
    return ResultOk(make_tuple());
  }
  if (auto error = run_parallel_pooled(items, function, max_in_flight, "Parallel.ForEach",
                                       [](std::size_t, const Value&) -> void {})) {
    return std::move(*error);
  }
  return ResultOk(make_tuple());
}

// Std.Parallel.Reduce: a left fold in input order. The items are split into chunks, and each chunk is folded by one
// pool task seeded with the previous chunk's result, so the fold matches a sequential one exactly; `function` need
// not be associative.
[[nodiscard]] inline auto run_parallel_reduce_chunked(const TupleView items, const Value& init, const Value& function,
                                                      const std::size_t chunk_size) -> Value {
  const std::size_t count = items.size();
  if (count == 0) {
    return ResultOk(Value{init});
  }
  const std::size_t size = std::max<std::size_t>(1, chunk_size);
  Value accumulator = init;
  for (std::size_t start = 0; start < count; start += size) {
    const std::size_t end = std::min(start + size, count);
    std::vector<Value> chunk(items.begin() + static_cast<std::ptrdiff_t>(start),
                             items.begin() + static_cast<std::ptrdiff_t>(end));
    Value fold_chunk = make_function([function, chunk = std::move(chunk), seed = accumulator,
                                      first_index = start](const Value&) -> Value {
      Value local = seed;
      std::size_t index = first_index;
      for (const Value& item : chunk) {
        try {
          local = invoke_binary_callable(function, std::move(local), item);
        } catch (const std::exception& ex) {
          const auto message = normalize_runtime_error_message(ex.what());
          throw RuntimePayloadError{make_tuple(make_int(static_cast<Int>(index)), make_string(message)), message};
        }
        ++index;
      }
      return local;
    });

    const Value result = wait_task_result(task_control(task_runtime().submit(std::move(fold_chunk), make_tuple())));
    if (!result.is_tuple() || result.tuple().empty()) {
      return parallel_item_error(start, "Parallel.Reduce: unexpected result shape");
    }
    const TupleView outcome = result.tuple();
    if (!outcome[0].is_bool() || !outcome[0].bool_value()) {
      return result;
    }
    accumulator = outcome.size() >= 2 ? outcome[1] : make_tuple();
    if (accumulator.is_tuple() && accumulator.tuple().size() == 1) {
      accumulator = Value{accumulator.tuple()[0]};
    }
  }
  return ResultOk(std::move(accumulator));
}

[[nodiscard]] inline auto ParallelForEach(const Arguments& args) -> Value {
  const TupleView items = as_tuple(args[0]);
  const std::size_t max_in_flight = std::max<std::size_t>(1, items.size());
  return run_parallel_foreach_pooled(items, args[1], max_in_flight);
}

[[nodiscard]] inline auto ParallelReduce(const Arguments& args) -> Value {
  const TupleView items = as_tuple(args[0]);
  const std::size_t chunk_size = std::max<std::size_t>(1, task_runtime().worker_count());
  return run_parallel_reduce_chunked(items, args[1], args[2], chunk_size);
}

}  // namespace fleaux::runtime
