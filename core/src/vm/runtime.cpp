#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <variant>
#include <vector>

// The order here is important
#include "fleaux/runtime/runtime_support.hpp"
#include "repl_support.hpp"
#include "std_help_metadata.hpp"

#include "fleaux/bytecode/compiler.hpp"
#include "fleaux/common/overloaded.hpp"
#include "fleaux/vm/builtin_catalog.hpp"
#include "fleaux/vm/runtime.hpp"

namespace fleaux::vm {
namespace {

using runtime::Array;
using runtime::RuntimeCallable;
using runtime::Value;

auto make_runtime_error(const std::string& message, const std::optional<std::string>& hint = std::nullopt,
                        const std::optional<frontend::diag::SourceSpan>& span = std::nullopt) -> RuntimeError {
  return RuntimeError{
      .message = message,
      .hint = hint,
      .span = span,
  };
}

// Value helpers

auto const_to_value(const bytecode::ConstValue& const_value) -> Value {
  using namespace fleaux::runtime;
  return std::visit(common::overloaded{[](const std::int64_t& val) -> Value { return make_int(val); },
                                       [](const std::uint64_t& val) -> Value { return make_uint(val); },
                                       [](const double& val) -> Value { return make_float(val); },
                                       [](const bool& val) -> Value { return make_bool(val); },
                                       [](const std::string& val) -> Value { return make_string(val); },
                                       [](const std::monostate&) -> Value { return make_null(); }},
                    const_value.data);
}

// Stack helpers

auto pop_stack(std::vector<Value>& stack, const char* context) -> tl::expected<Value, RuntimeError> {
  if (stack.empty()) {
    return tl::unexpected(make_runtime_error(std::string("stack underflow on ") + context));
  }
  Value value = std::move(stack.back());
  stack.pop_back();
  return value;
}

template <typename Fn>
auto run_native_op(const char* opname, Fn&& fn) -> tl::expected<Value, RuntimeError> {
  try {
    return fn();
  } catch (const std::exception& ex) {
    return tl::unexpected(make_runtime_error(std::string("native '") + opname + "' threw: " + ex.what()));
  }
}

void collapse_stack_tail_into_tuple(std::vector<Value>& stack, const std::size_t tuple_size) {
  if (tuple_size == 0U) {
    stack.emplace_back(Array{});
    return;
  }

  const auto base = stack.size() - tuple_size;
  Value first_element = std::move(stack[base]);
  stack[base] = Value{Array{}};
  auto& out = runtime::as_array(stack[base]);
  out.Reserve(tuple_size);
  out.EmplaceBack(std::move(first_element));
  for (std::size_t element_index = 1; element_index < tuple_size; ++element_index) {
    out.EmplaceBack(std::move(stack[base + element_index]));
  }
  stack.resize(base + 1U);
}

auto make_array_tail_value(Array& source, const std::size_t first_index) -> Value {
  Value tail{Array{}};
  auto& out = runtime::as_array(tail);
  out.Reserve(source.Size() - first_index);
  for (std::size_t element_index = first_index; element_index < source.Size(); ++element_index) {
    out.EmplaceBack(std::move(source[element_index]));
  }
  return tail;
}

void append_array_copy(Array& out, const Array& source) {
  for (std::size_t element_index = 0; element_index < source.Size(); ++element_index) {
    out.EmplaceBack(*source.TryGet(element_index));
  }
}

auto inline_closure_too_few_args_error() -> tl::unexpected<RuntimeError> {
  return tl::unexpected(make_runtime_error("too few arguments for inline closure"));
}

auto inline_closure_unpack_error(const std::exception& ex) -> tl::unexpected<RuntimeError> {
  return tl::unexpected(make_runtime_error(std::string("argument unpacking for inline closure: ") + ex.what()));
}

void append_array_prefix(Array& out_array, Array& arr, const std::size_t count) {
  for (std::size_t arg_index = 0; arg_index < count; ++arg_index) {
    out_array.EmplaceBack(std::move(arr[arg_index]));
  }
}

struct CallFrame {
  const std::vector<bytecode::Instruction>* instructions = nullptr;
  std::size_t ip = 0;
  std::vector<Value> locals;
};

struct CallFrameStack {
  std::vector<CallFrame> storage;
  std::size_t active_size = 0U;

  [[nodiscard]] auto empty() const -> bool { return active_size == 0U; }

  [[nodiscard]] auto back() -> CallFrame& { return storage[active_size - 1U]; }

  [[nodiscard]] auto back() const -> const CallFrame& { return storage[active_size - 1U]; }

  auto push(const std::vector<bytecode::Instruction>* instructions) -> CallFrame& {
    if (active_size == storage.size()) {
      storage.emplace_back();
    }
    auto& frame = storage[active_size++];
    frame.instructions = instructions;
    frame.ip = 0;
    frame.locals.clear();
    return frame;
  }

  void pop() {
    auto& [instructions, ip, locals] = storage[active_size - 1U];
    instructions = nullptr;
    ip = 0;
    locals.clear();
    --active_size;
  }

  void clear_active() {
    while (!empty()) {
      pop();
    }
  }

  void reserve(const std::size_t capacity) { storage.reserve(capacity); }
};

struct InvocationScratch {
  std::vector<Value> stack;
  CallFrameStack frames;
};

thread_local std::deque<InvocationScratch> g_invocation_scratch_pool;
thread_local std::size_t g_invocation_scratch_depth = 0U;

class ScopedInvocationScratch {
public:
  ScopedInvocationScratch(const std::size_t operand_stack_capacity, const std::size_t frame_stack_capacity)
      : depth_index_(g_invocation_scratch_depth++) {
    if (depth_index_ == g_invocation_scratch_pool.size()) {
      g_invocation_scratch_pool.emplace_back();
    }
    scratch().stack.clear();
    scratch().frames.clear_active();
    scratch().stack.reserve(operand_stack_capacity);
    scratch().frames.reserve(frame_stack_capacity);
  }

  ScopedInvocationScratch(const ScopedInvocationScratch&) = delete;
  auto operator=(const ScopedInvocationScratch&) -> ScopedInvocationScratch& = delete;

  ~ScopedInvocationScratch() {
    scratch().stack.clear();
    scratch().frames.clear_active();
    g_invocation_scratch_depth = depth_index_;
  }

  [[nodiscard]] auto stack() const -> std::vector<Value>& { return scratch().stack; }

  [[nodiscard]] auto frames() const -> CallFrameStack& { return scratch().frames; }

private:
  [[nodiscard]] auto scratch() const -> InvocationScratch& { return g_invocation_scratch_pool[depth_index_]; }

  std::size_t depth_index_ = 0U;
};

// Loop exit type
// std::monostate -> kHalt was hit (top-level completion).
// Value          -> kReturn emptied the frame stack (standalone function result).

using LoopExit = std::variant<std::monostate, Value>;
using LoopResult = tl::expected<LoopExit, RuntimeError>;

constexpr std::size_t k_min_stack_capacity = 16U;
constexpr std::size_t k_min_frame_capacity = 4U;

[[nodiscard]] auto suggested_operand_stack_capacity(const std::size_t instruction_count) -> std::size_t {
  return std::max(k_min_stack_capacity, instruction_count);
}

[[nodiscard]] auto suggested_frame_stack_capacity(const std::size_t function_count) -> std::size_t {
  return std::max(k_min_frame_capacity, function_count + 1U);
}

auto constant_builtin_value(const BuiltinId builtin_id) -> std::optional<double> {
  for (const auto& spec : all_constant_builtin_specs()) {
    if (spec.id == builtin_id) {
      return spec.value;
    }
  }
  return std::nullopt;
}

// Forward declarations.
auto run_loop(const bytecode::Module& bytecode_module, std::vector<Value>& stack, CallFrameStack& frames,
              std::ostream& output) -> LoopResult;

auto dispatch_builtin(BuiltinId builtin_id, Value arg) -> tl::expected<Value, RuntimeError>;

auto bind_user_function_locals(std::vector<Value>& locals, const std::string& fn_name, const std::uint32_t arity,
                               const bool has_variadic_tail, Value arg) -> tl::expected<void, RuntimeError> {
  locals.clear();
  locals.reserve(arity);

  if (arity == 0U) {
    return {};
  }

  if (!has_variadic_tail) {
    if (arity == 1U) {
      locals.push_back(runtime::unwrap_singleton_arg(std::move(arg)));
      return {};
    }

    try {
      auto& arr = runtime::as_array(arg);
      if (arr.Size() < arity) {
        return tl::unexpected(make_runtime_error("too few arguments for '" + fn_name + "'"));
      }
      for (std::uint32_t arg_index = 0; arg_index < arity; ++arg_index) {
        locals.emplace_back(std::move(arr[static_cast<std::size_t>(arg_index)]));
      }
      return {};
    } catch (const std::exception& ex) {
      return tl::unexpected(make_runtime_error(std::string("argument unpacking for '") + fn_name + "': " + ex.what()));
    }
  }

  // Variadic: final parameter captures remaining args as a tuple.
  if (arity == 1U) {
    if (arg.HasArray()) {
      locals.push_back(std::move(arg));
    } else {
      locals.push_back(runtime::make_tuple(std::move(arg)));
    }
    return {};
  }

  const auto fixed_count = static_cast<std::size_t>(arity - 1U);
  try {
    auto& arr = runtime::as_array(arg);
    if (arr.Size() < fixed_count) {
      return tl::unexpected(make_runtime_error("too few arguments for '" + fn_name + "'"));
    }
    for (std::size_t arg_index = 0; arg_index < fixed_count; ++arg_index) {
      locals.emplace_back(std::move(arr[arg_index]));
    }

    locals.emplace_back(make_array_tail_value(arr, fixed_count));
    return {};
  } catch (const std::exception& ex) {
    return tl::unexpected(make_runtime_error(std::string("argument unpacking for '") + fn_name + "': " + ex.what()));
  }
}

auto pack_declared_call_args(Value arg, const std::uint32_t declared_arity, const bool declared_has_variadic_tail)
    -> tl::expected<Value, RuntimeError> {
  if (declared_arity == 0U) {
    return Value{Array{}};
  }

  if (!declared_has_variadic_tail) {
    if (declared_arity == 1U) {
      return runtime::unwrap_singleton_arg(std::move(arg));
    }

    try {
      auto& arr = runtime::as_array(arg);
      if (arr.Size() < declared_arity) {
        return inline_closure_too_few_args_error();
      }
      if (arr.Size() == declared_arity) {
        return arg;
      }

      Value out{Array{}};
      auto& out_array = runtime::as_array(out);
      out_array.Reserve(declared_arity);
      append_array_prefix(out_array, arr, declared_arity);
      return out;
    } catch (const std::exception& ex) {
      return inline_closure_unpack_error(ex);
    }
  }

  if (declared_arity == 1U) {
    return arg.HasArray() ? std::move(arg) : runtime::make_tuple(std::move(arg));
  }

  const auto fixed_count = static_cast<std::size_t>(declared_arity - 1U);
  try {
    auto& arr = runtime::as_array(arg);
    if (arr.Size() < fixed_count) {
      return inline_closure_too_few_args_error();
    }

    Value out{Array{}};
    auto& out_array = runtime::as_array(out);
    out_array.Reserve(declared_arity);
    append_array_prefix(out_array, arr, fixed_count);
    out_array.EmplaceBack(make_array_tail_value(arr, fixed_count));
    return out;
  } catch (const std::exception& ex) {
    return inline_closure_unpack_error(ex);
  }
}

auto pack_prefixed_call_args(const Value& prefix_args, Value arg, const std::uint32_t declared_arity,
                             const bool declared_has_variadic_tail) -> tl::expected<Value, RuntimeError> {
  const auto& prefix = runtime::as_array(prefix_args);
  if (prefix.Size() == 0U) {
    return pack_declared_call_args(std::move(arg), declared_arity, declared_has_variadic_tail);
  }

  if (declared_arity == 0U) {
    if (prefix.Size() == 1U) {
      return *prefix.TryGet(0);
    }
    return prefix_args;
  }

  if (!declared_has_variadic_tail) {
    try {
      Value out{Array{}};
      auto& out_array = runtime::as_array(out);

      if (declared_arity == 1U) {
        out_array.Reserve(prefix.Size() + 1U);
        append_array_copy(out_array, prefix);
        out_array.EmplaceBack(runtime::unwrap_singleton_arg(std::move(arg)));
        return out;
      }

      auto& arr = runtime::as_array(arg);
      if (arr.Size() < declared_arity) {
        return inline_closure_too_few_args_error();
      }

      out_array.Reserve(prefix.Size() + declared_arity);
      append_array_copy(out_array, prefix);
      append_array_prefix(out_array, arr, declared_arity);
      return out;
    } catch (const std::exception& ex) {
      return inline_closure_unpack_error(ex);
    }
  }

  Value out{Array{}};
  auto& out_array = runtime::as_array(out);
  out_array.Reserve(prefix.Size() + declared_arity);
  append_array_copy(out_array, prefix);

  if (declared_arity == 1U) {
    out_array.EmplaceBack(arg.HasArray() ? std::move(arg) : runtime::make_tuple(std::move(arg)));
    return out;
  }

  const auto fixed_count = static_cast<std::size_t>(declared_arity - 1U);
  try {
    auto& arr = runtime::as_array(arg);
    if (arr.Size() < fixed_count) {
      return inline_closure_too_few_args_error();
    }

    append_array_prefix(out_array, arr, fixed_count);
    out_array.EmplaceBack(make_array_tail_value(arr, fixed_count));
    return out;
  } catch (const std::exception& ex) {
    return inline_closure_unpack_error(ex);
  }
}

auto run_user_function(const bytecode::Module& bytecode_module, std::size_t fn_idx, Value arg, std::ostream& output)
    -> tl::expected<Value, RuntimeError>;

auto run_loop_intrinsic(Value state, const Value& continue_func, const Value& step_func,
                        const std::optional<std::size_t> max_iters) -> tl::expected<Value, RuntimeError> {
  try {
    std::size_t iterations = 0;
    while (runtime::as_bool(runtime::invoke_callable_ref(continue_func, state))) {
      if (max_iters.has_value() && iterations >= *max_iters) {
        throw std::runtime_error("LoopN: exceeded max_iters");
      }
      state = runtime::invoke_callable_ref(step_func, std::move(state));
      ++iterations;
    }
    return state;
  } catch (const std::exception& ex) {
    return tl::unexpected(make_runtime_error(std::string("native 'loop' threw: ") + ex.what()));
  }
}

// run_user_function

auto run_user_function(const bytecode::Module& bytecode_module, const std::size_t fn_idx, Value arg,
                       std::ostream& output) -> tl::expected<Value, RuntimeError> {
  if (fn_idx >= bytecode_module.functions.size()) {
    return tl::unexpected(make_runtime_error("function index out of range"));
  }
  const auto& function = bytecode_module.functions[fn_idx];
  const auto& name = function.name;
  const auto arity = function.arity;
  const auto has_variadic_tail = function.has_variadic_tail;
  const auto& instructions = function.instructions;

  const ScopedInvocationScratch scratch{suggested_operand_stack_capacity(instructions.size()),
                                        suggested_frame_stack_capacity(bytecode_module.functions.size())};

  auto& frame = scratch.frames().push(&instructions);

  if (auto bound_locals = bind_user_function_locals(frame.locals, name, arity, has_variadic_tail, std::move(arg));
      !bound_locals) {
    return tl::unexpected(bound_locals.error());
  }

  auto loop_result = run_loop(bytecode_module, scratch.stack(), scratch.frames(), output);
  if (!loop_result)
    return tl::unexpected(loop_result.error());

  if (std::get_if<std::monostate>(&*loop_result) != nullptr) {
    return tl::unexpected(make_runtime_error("halt inside function '" + name + "'"));
  }
  if (auto* value = std::get_if<Value>(&*loop_result); value != nullptr) {
    return std::move(*value);
  }
  return tl::unexpected(make_runtime_error("invalid loop result variant"));
}

// run_loop

auto run_loop(const bytecode::Module& bytecode_module, std::vector<Value>& stack, CallFrameStack& frames,
              std::ostream& output) -> LoopResult {
  while (!frames.empty()) {
    const std::size_t curr_ip = frames.back().ip;
    frames.back().ip++;

    const auto& instr_list = *frames.back().instructions;
    if (curr_ip >= instr_list.size()) {
      return tl::unexpected(make_runtime_error("program terminated without halt"));
    }

    const auto opcode = instr_list[curr_ip].opcode;
    const auto operand = instr_list[curr_ip].operand;

    switch (opcode) {
      case bytecode::Opcode::kNoOp:
        break;

      case bytecode::Opcode::kPushConst: {
        const auto idx = static_cast<std::size_t>(operand);
        if (idx >= bytecode_module.constants.size()) {
          return tl::unexpected(make_runtime_error("constant pool index out of range"));
        }
        stack.push_back(const_to_value(bytecode_module.constants[idx]));
        break;
      }

      case bytecode::Opcode::kPop: {
        if (auto popped_value = pop_stack(stack, "pop"); !popped_value)
          return tl::unexpected(popped_value.error());
        break;
      }

      case bytecode::Opcode::kDup: {
        if (stack.empty()) {
          return tl::unexpected(make_runtime_error("stack underflow on dup"));
        }
        stack.push_back(stack.back());
        break;
      }

      case bytecode::Opcode::kBuildTuple: {
        const auto tuple_size = static_cast<std::size_t>(operand);
        if (stack.size() < tuple_size) {
          return tl::unexpected(make_runtime_error("stack underflow on build_tuple"));
        }
        collapse_stack_tail_into_tuple(stack, tuple_size);
        break;
      }

      case bytecode::Opcode::kMakeValueRef: {
        auto value = pop_stack(stack, "make_value_ref");
        if (!value)
          return tl::unexpected(value.error());
        stack.push_back(runtime::make_value_ref(std::move(*value)));
        break;
      }

      case bytecode::Opcode::kDerefValueRef: {
        auto token = pop_stack(stack, "deref_value_ref");
        if (!token)
          return tl::unexpected(token.error());
        auto result = run_native_op("deref_value_ref", [&]() -> Value { return runtime::deref_value_ref(*token); });
        if (!result)
          return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kCallBuiltin: {
        const auto builtin_id = builtin_id_from_operand(operand);
        if (!builtin_id.has_value()) {
          return tl::unexpected(make_runtime_error("builtin index out of range"));
        }

        auto arg = pop_stack(stack, "call_builtin");
        if (!arg)
          return tl::unexpected(arg.error());

        auto result = dispatch_builtin(*builtin_id, std::move(*arg));
        if (!result)
          return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kCallUserFunc: {
        const auto fn_idx = static_cast<std::size_t>(operand);
        if (fn_idx >= bytecode_module.functions.size()) {
          return tl::unexpected(make_runtime_error("function index out of range"));
        }
        auto arg = pop_stack(stack, "call_user_func");
        if (!arg)
          return tl::unexpected(arg.error());

        const auto& function = bytecode_module.functions[fn_idx];
        const auto& name = function.name;
        const auto arity = function.arity;
        const auto has_variadic_tail = function.has_variadic_tail;
        const auto& instructions = function.instructions;
        auto& new_frame = frames.push(&instructions);
        auto bound_locals =
            bind_user_function_locals(new_frame.locals, name, arity, has_variadic_tail, std::move(*arg));
        if (!bound_locals) {
          frames.pop();
          return tl::unexpected(bound_locals.error());
        }
        break;
      }

      case bytecode::Opcode::kMakeUserFuncRef: {
        const auto fn_idx = static_cast<std::size_t>(operand);
        if (fn_idx >= bytecode_module.functions.size()) {
          return tl::unexpected(make_runtime_error("function index out of range"));
        }
        // Callable captures bytecode_module, builtins, and output by reference.
        // Safety contract: these refs remain valid for the entire duration of the
        // enclosing Runtime::execute() call. The callable is registered globally
        // but the callable-ref Value only lives on the execution stack, so it
        // cannot outlive execute(). For async builtins (e.g. Std.Parallel.Map),
        // all futures complete via .get() before run_loop continues, so the
        // captured refs are still live during any async invocation.
        auto callable = [&bytecode_module, fn_idx, &output](Value call_arg) -> Value {
          auto run_result = run_user_function(bytecode_module, fn_idx, std::move(call_arg), output);
          if (!run_result)
            throw std::runtime_error(run_result.error().message);
          return std::move(*run_result);
        };
        stack.push_back(runtime::make_callable_ref(std::move(callable)));
        break;
      }

      case bytecode::Opcode::kMakeBuiltinFuncRef: {
        const auto builtin_id = builtin_id_from_operand(operand);
        if (!builtin_id.has_value()) {
          return tl::unexpected(make_runtime_error("builtin index out of range"));
        }
        auto callable = [builtin_id = *builtin_id](Value arg) -> Value {
          auto result = dispatch_builtin(builtin_id, std::move(arg));
          if (!result) {
            throw std::runtime_error(result.error().message);
          }
          return std::move(*result);
        };
        stack.push_back(runtime::make_callable_ref(std::move(callable)));
        break;
      }

      case bytecode::Opcode::kMakeClosureRef: {
        const auto closure_idx = static_cast<std::size_t>(operand);
        if (closure_idx >= bytecode_module.closures.size()) {
          return tl::unexpected(make_runtime_error("closure index out of range"));
        }

        auto captured_tuple = pop_stack(stack, "make_closure_ref");
        if (!captured_tuple) {
          return tl::unexpected(captured_tuple.error());
        }

        const auto& [function_index, capture_count, declared_arity, declared_has_variadic_tail] =
            bytecode_module.closures[closure_idx];
        if (const auto& capture_array = runtime::as_array(*captured_tuple); capture_array.Size() != capture_count) {
          return tl::unexpected(make_runtime_error("closure capture tuple size mismatch"));
        }

        auto captured_args = std::move(*captured_tuple);

        // Callable captures bytecode_module, builtins, and output by reference.
        // Same lifetime contract as kMakeUserFuncRef: valid for the duration of
        // the enclosing Runtime::execute() call. Callable-ref Values stay on the
        // execution stack and cannot outlive execute().
        auto closure_callable = [&bytecode_module, function_index, declared_arity, declared_has_variadic_tail,
                                 captured_args = std::move(captured_args), &output](Value call_arg) -> Value {
          auto packed_args =
              pack_prefixed_call_args(captured_args, std::move(call_arg), declared_arity, declared_has_variadic_tail);
          if (!packed_args) {
            throw std::runtime_error(packed_args.error().message);
          }

          auto result = run_user_function(bytecode_module, function_index, std::move(*packed_args), output);
          if (!result) {
            throw std::runtime_error(result.error().message);
          }
          return std::move(*result);
        };

        stack.push_back(runtime::make_callable_ref(std::move(closure_callable)));
        break;
      }

      case bytecode::Opcode::kReturn: {
        auto ret_val = pop_stack(stack, "return");
        if (!ret_val)
          return tl::unexpected(ret_val.error());
        frames.pop();
        if (frames.empty()) {
          // Standalone function execution: the result IS the return value.
          return LoopExit{std::move(*ret_val)};
        }
        stack.push_back(std::move(*ret_val));
        break;
      }

      case bytecode::Opcode::kLoadLocal: {
        const auto slot = static_cast<std::size_t>(operand);
        const auto& locals = frames.back().locals;
        if (slot >= locals.size()) {
          return tl::unexpected(make_runtime_error("local slot index out of range"));
        }
        stack.push_back(locals[slot]);
        break;
      }

      case bytecode::Opcode::kJump: {
        const auto target = static_cast<std::size_t>(operand);
        if (target > instr_list.size()) {
          return tl::unexpected(make_runtime_error("jump target out of range"));
        }
        frames.back().ip = target;
        break;
      }

      case bytecode::Opcode::kJumpIf: {
        auto cond = pop_stack(stack, "jump_if");
        if (!cond)
          return tl::unexpected(cond.error());
        const auto target = static_cast<std::size_t>(operand);
        if (target > instr_list.size()) {
          return tl::unexpected(make_runtime_error("jump_if target out of range"));
        }
        if (runtime::as_bool(*cond)) {
          frames.back().ip = target;
        }
        break;
      }

      case bytecode::Opcode::kJumpIfNot: {
        auto cond = pop_stack(stack, "jump_if_not");
        if (!cond)
          return tl::unexpected(cond.error());
        const auto target = static_cast<std::size_t>(operand);
        if (target > instr_list.size()) {
          return tl::unexpected(make_runtime_error("jump_if_not target out of range"));
        }
        if (!runtime::as_bool(*cond)) {
          frames.back().ip = target;
        }
        break;
      }

      case bytecode::Opcode::kAdd: {
        auto rhs = pop_stack(stack, "add");
        if (!rhs)
          return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "add");
        if (!lhs)
          return tl::unexpected(lhs.error());
        auto result = run_native_op("add", [&]() -> Value {
          if (lhs->HasString() && rhs->HasString()) {
            return runtime::make_string(runtime::as_string(*lhs) + runtime::as_string(*rhs));
          }
          runtime::require_no_implicit_float_promotion(*lhs, *rhs, "add");
          runtime::require_same_integer_kind(*lhs, *rhs, "add");
          return runtime::num_result(runtime::to_double(*lhs) + runtime::to_double(*rhs),
                                     runtime::is_uint_number(*lhs) && runtime::is_uint_number(*rhs),
                                     runtime::prefer_float_numeric_result(*lhs, *rhs));
        });
        if (!result)
          return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kSub: {
        auto rhs = pop_stack(stack, "sub");
        if (!rhs)
          return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "sub");
        if (!lhs)
          return tl::unexpected(lhs.error());
        auto result = run_native_op("sub", [&]() -> Value {
          runtime::require_no_implicit_float_promotion(*lhs, *rhs, "sub");
          runtime::require_same_integer_kind(*lhs, *rhs, "sub");
          return runtime::num_result(runtime::to_double(*lhs) - runtime::to_double(*rhs),
                                     runtime::is_uint_number(*lhs) && runtime::is_uint_number(*rhs),
                                     runtime::prefer_float_numeric_result(*lhs, *rhs));
        });
        if (!result)
          return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kMul: {
        auto rhs = pop_stack(stack, "mul");
        if (!rhs)
          return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "mul");
        if (!lhs)
          return tl::unexpected(lhs.error());
        auto result = run_native_op("mul", [&]() -> Value {
          runtime::require_no_implicit_float_promotion(*lhs, *rhs, "mul");
          runtime::require_same_integer_kind(*lhs, *rhs, "mul");
          return runtime::num_result(runtime::to_double(*lhs) * runtime::to_double(*rhs),
                                     runtime::is_uint_number(*lhs) && runtime::is_uint_number(*rhs),
                                     runtime::prefer_float_numeric_result(*lhs, *rhs));
        });
        if (!result)
          return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kDiv: {
        auto rhs = pop_stack(stack, "div");
        if (!rhs)
          return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "div");
        if (!lhs)
          return tl::unexpected(lhs.error());
        auto result = run_native_op("div", [&]() -> Value {
          // Native division follows floating-point semantics (e.g. x/0 -> inf).
          runtime::require_no_implicit_float_promotion(*lhs, *rhs, "div");
          runtime::require_same_integer_kind(*lhs, *rhs, "div");
          return runtime::num_result(runtime::to_double(*lhs) / runtime::to_double(*rhs),
                                     runtime::is_uint_number(*lhs) && runtime::is_uint_number(*rhs),
                                     runtime::prefer_float_numeric_result(*lhs, *rhs));
        });
        if (!result)
          return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kMod: {
        auto rhs = pop_stack(stack, "mod");
        if (!rhs)
          return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "mod");
        if (!lhs)
          return tl::unexpected(lhs.error());
        auto result = run_native_op("mod", [&]() -> Value {
          runtime::require_no_implicit_float_promotion(*lhs, *rhs, "mod");
          runtime::require_same_integer_kind(*lhs, *rhs, "mod");
          return runtime::num_result(std::fmod(runtime::to_double(*lhs), runtime::to_double(*rhs)),
                                     runtime::is_uint_number(*lhs) && runtime::is_uint_number(*rhs),
                                     runtime::prefer_float_numeric_result(*lhs, *rhs));
        });
        if (!result)
          return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kPow: {
        auto rhs = pop_stack(stack, "pow");
        if (!rhs)
          return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "pow");
        if (!lhs)
          return tl::unexpected(lhs.error());
        auto result = run_native_op("pow", [&]() -> Value {
          runtime::require_no_implicit_float_promotion(*lhs, *rhs, "pow");
          return runtime::num_result(std::pow(runtime::to_double(*lhs), runtime::to_double(*rhs)), false,
                                     runtime::prefer_float_numeric_result(*lhs, *rhs));
        });
        if (!result)
          return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kNeg: {
        auto value = pop_stack(stack, "neg");
        if (!value)
          return tl::unexpected(value.error());
        auto result = run_native_op("neg", [&]() -> Value {
          return runtime::num_result(-runtime::to_double(*value), false, runtime::is_float_number(*value));
        });
        if (!result)
          return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kCmpEq: {
        auto rhs = pop_stack(stack, "cmp_eq");
        if (!rhs)
          return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "cmp_eq");
        if (!lhs)
          return tl::unexpected(lhs.error());
        stack.push_back(runtime::make_bool(*lhs == *rhs));
        break;
      }

      case bytecode::Opcode::kCmpNe: {
        auto rhs = pop_stack(stack, "cmp_ne");
        if (!rhs)
          return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "cmp_ne");
        if (!lhs)
          return tl::unexpected(lhs.error());
        stack.push_back(runtime::make_bool(*lhs != *rhs));
        break;
      }

      case bytecode::Opcode::kCmpLt: {
        auto rhs = pop_stack(stack, "cmp_lt");
        if (!rhs)
          return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "cmp_lt");
        if (!lhs)
          return tl::unexpected(lhs.error());
        auto result = run_native_op("cmp_lt", [&]() -> Value {
          runtime::require_no_implicit_float_promotion(*lhs, *rhs, "cmp_lt");
          return runtime::make_bool(runtime::compare_numbers(*lhs, *rhs) < 0);
        });
        if (!result)
          return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kCmpGt: {
        auto rhs = pop_stack(stack, "cmp_gt");
        if (!rhs)
          return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "cmp_gt");
        if (!lhs)
          return tl::unexpected(lhs.error());
        auto result = run_native_op("cmp_gt", [&]() -> Value {
          runtime::require_no_implicit_float_promotion(*lhs, *rhs, "cmp_gt");
          return runtime::make_bool(runtime::compare_numbers(*lhs, *rhs) > 0);
        });
        if (!result)
          return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kCmpLe: {
        auto rhs = pop_stack(stack, "cmp_le");
        if (!rhs)
          return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "cmp_le");
        if (!lhs)
          return tl::unexpected(lhs.error());
        auto result = run_native_op("cmp_le", [&]() -> Value {
          runtime::require_no_implicit_float_promotion(*lhs, *rhs, "cmp_le");
          return runtime::make_bool(runtime::compare_numbers(*lhs, *rhs) <= 0);
        });
        if (!result)
          return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kCmpGe: {
        auto rhs = pop_stack(stack, "cmp_ge");
        if (!rhs)
          return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "cmp_ge");
        if (!lhs)
          return tl::unexpected(lhs.error());
        auto result = run_native_op("cmp_ge", [&]() -> Value {
          runtime::require_no_implicit_float_promotion(*lhs, *rhs, "cmp_ge");
          return runtime::make_bool(runtime::compare_numbers(*lhs, *rhs) >= 0);
        });
        if (!result)
          return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kAnd: {
        auto rhs = pop_stack(stack, "and");
        if (!rhs)
          return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "and");
        if (!lhs)
          return tl::unexpected(lhs.error());
        auto result = run_native_op(
            "and", [&]() -> Value { return runtime::make_bool(runtime::as_bool(*lhs) && runtime::as_bool(*rhs)); });
        if (!result)
          return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kOr: {
        auto rhs = pop_stack(stack, "or");
        if (!rhs)
          return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "or");
        if (!lhs)
          return tl::unexpected(lhs.error());
        auto result = run_native_op(
            "or", [&]() -> Value { return runtime::make_bool(runtime::as_bool(*lhs) || runtime::as_bool(*rhs)); });
        if (!result)
          return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kNot: {
        auto value = pop_stack(stack, "not");
        if (!value)
          return tl::unexpected(value.error());
        auto result = run_native_op("not", [&]() -> Value { return runtime::make_bool(!runtime::as_bool(*value)); });
        if (!result)
          return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kSelect: {
        auto false_val = pop_stack(stack, "select");
        if (!false_val)
          return tl::unexpected(false_val.error());
        auto true_val = pop_stack(stack, "select");
        if (!true_val)
          return tl::unexpected(true_val.error());
        auto cond = pop_stack(stack, "select");
        if (!cond)
          return tl::unexpected(cond.error());
        stack.push_back(runtime::as_bool(*cond) ? std::move(*true_val) : std::move(*false_val));
        break;
      }

      case bytecode::Opcode::kBranchCall: {
        auto false_func = pop_stack(stack, "branch_call");
        if (!false_func)
          return tl::unexpected(false_func.error());
        auto true_func = pop_stack(stack, "branch_call");
        if (!true_func)
          return tl::unexpected(true_func.error());
        auto value = pop_stack(stack, "branch_call");
        if (!value)
          return tl::unexpected(value.error());
        auto cond = pop_stack(stack, "branch_call");
        if (!cond)
          return tl::unexpected(cond.error());
        auto result = run_native_op("branch_call", [&]() -> Value {
          const Value& chosen = runtime::as_bool(*cond) ? *true_func : *false_func;
          return runtime::invoke_callable_ref(chosen, std::move(*value));
        });
        if (!result)
          return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kLoopCall: {
        auto step_func = pop_stack(stack, "loop_call");
        if (!step_func)
          return tl::unexpected(step_func.error());
        auto continue_func = pop_stack(stack, "loop_call");
        if (!continue_func)
          return tl::unexpected(continue_func.error());
        auto state = pop_stack(stack, "loop_call");
        if (!state)
          return tl::unexpected(state.error());

        auto result = run_loop_intrinsic(std::move(*state), *continue_func, *step_func, std::nullopt);
        if (!result) {
          const std::string prefix = "native 'loop' threw: ";
          std::string msg = result.error().message;
          if (msg.starts_with(prefix)) {
            msg.replace(0, prefix.size(), "native 'loop_call' threw: ");
          }
          return tl::unexpected(make_runtime_error(msg));
        }
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kLoopNCall: {
        auto max_iters = pop_stack(stack, "loop_n_call");
        if (!max_iters)
          return tl::unexpected(max_iters.error());
        auto step_func = pop_stack(stack, "loop_n_call");
        if (!step_func)
          return tl::unexpected(step_func.error());
        auto continue_func = pop_stack(stack, "loop_n_call");
        if (!continue_func)
          return tl::unexpected(continue_func.error());
        auto state = pop_stack(stack, "loop_n_call");
        if (!state)
          return tl::unexpected(state.error());

        std::size_t limit = 0;
        try {
          const auto as_int = runtime::as_int_value_strict(*max_iters, "LoopN max_iters");
          if (as_int < 0) {
            return tl::unexpected(
                make_runtime_error("native 'loop_n_call' threw: LoopN: max_iters must be non-negative"));
          }
          limit = static_cast<std::size_t>(as_int);
        } catch (const std::exception& ex) {
          return tl::unexpected(make_runtime_error(std::string("native 'loop_n_call' threw: ") + ex.what()));
        }

        auto result = run_loop_intrinsic(std::move(*state), *continue_func, *step_func, limit);
        if (!result) {
          const std::string prefix = "native 'loop' threw: ";
          std::string msg = result.error().message;
          if (msg.starts_with(prefix)) {
            msg.replace(0, prefix.size(), "native 'loop_n_call' threw: ");
          }
          return tl::unexpected(make_runtime_error(msg));
        }
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kHalt:
        return LoopExit{std::monostate{}};
      default:
        return tl::unexpected(make_runtime_error(std::format("invalid opcode {}", static_cast<int>(opcode))));
    }
  }

  return tl::unexpected(make_runtime_error("program terminated without halt"));
}

auto dispatch_builtin(const BuiltinId builtin_id, Value arg) -> tl::expected<Value, RuntimeError> {
  try {
    switch (builtin_id) {
      case BuiltinId::UnaryPlus:
        return runtime::UnaryPlus(std::move(arg));
      case BuiltinId::UnaryMinus:
        return runtime::UnaryMinus(std::move(arg));
      case BuiltinId::Add:
        return runtime::Add(std::move(arg));
      case BuiltinId::Subtract:
        return runtime::Subtract(std::move(arg));
      case BuiltinId::Multiply:
        return runtime::Multiply(std::move(arg));
      case BuiltinId::Divide:
        return runtime::Divide(std::move(arg));
      case BuiltinId::Mod:
        return runtime::Mod(std::move(arg));
      case BuiltinId::Pow:
        return runtime::Pow(std::move(arg));
      case BuiltinId::BitAnd:
        return runtime::BitAnd(std::move(arg));
      case BuiltinId::BitOr:
        return runtime::BitOr(std::move(arg));
      case BuiltinId::BitXor:
        return runtime::BitXor(std::move(arg));
      case BuiltinId::BitNot:
        return runtime::BitNot(std::move(arg));
      case BuiltinId::BitShiftLeft:
        return runtime::BitShiftLeft(std::move(arg));
      case BuiltinId::BitShiftRight:
        return runtime::BitShiftRight(std::move(arg));
      case BuiltinId::Equal:
        return runtime::Equal(std::move(arg));
      case BuiltinId::NotEqual:
        return runtime::NotEqual(std::move(arg));
      case BuiltinId::LessThan:
        return runtime::LessThan(std::move(arg));
      case BuiltinId::GreaterThan:
        return runtime::GreaterThan(std::move(arg));
      case BuiltinId::GreaterOrEqual:
        return runtime::GreaterOrEqual(std::move(arg));
      case BuiltinId::LessOrEqual:
        return runtime::LessOrEqual(std::move(arg));
      case BuiltinId::Not:
        return runtime::Not(std::move(arg));
      case BuiltinId::And:
        return runtime::And(std::move(arg));
      case BuiltinId::Or:
        return runtime::Or(std::move(arg));
      case BuiltinId::Select:
        return runtime::Select(std::move(arg));
      case BuiltinId::Match:
        return runtime::Match(std::move(arg));
      case BuiltinId::Apply:
        return runtime::Apply(std::move(arg));
      case BuiltinId::Branch:
        return runtime::Branch(std::move(arg));
      case BuiltinId::Loop:
        return runtime::Loop(std::move(arg));
      case BuiltinId::LoopN:
        return runtime::LoopN(std::move(arg));
      case BuiltinId::Printf:
        return runtime::Printf(std::move(arg));
      case BuiltinId::Println:
        return runtime::Println(std::move(arg));
      case BuiltinId::GetArgs:
        return runtime::GetArgs(std::move(arg));
      case BuiltinId::Type:
        return runtime::Type(std::move(arg));
      case BuiltinId::Input:
        return runtime::Input(std::move(arg));
      case BuiltinId::Help:
        return runtime::Help(std::move(arg));
      case BuiltinId::ExitVoid:
        return runtime::Exit_Void(std::move(arg));
      case BuiltinId::ExitInt64:
        return runtime::Exit_Int64(std::move(arg));
      case BuiltinId::Cwd:
        return runtime::Cwd(std::move(arg));
      case BuiltinId::OSEnv:
        return runtime::OSEnv(std::move(arg));
      case BuiltinId::OSHasEnv:
        return runtime::OSHasEnv(std::move(arg));
      case BuiltinId::OSSetEnv:
        return runtime::OSSetEnv(std::move(arg));
      case BuiltinId::OSUnsetEnv:
        return runtime::OSUnsetEnv(std::move(arg));
      case BuiltinId::OSIsWindows:
        return runtime::OSIsWindows(std::move(arg));
      case BuiltinId::OSIsLinux:
        return runtime::OSIsLinux(std::move(arg));
      case BuiltinId::OSIsMacOS:
        return runtime::OSIsMacOS(std::move(arg));
      case BuiltinId::OSHome:
        return runtime::OSHome(std::move(arg));
      case BuiltinId::OSTempDir:
        return runtime::OSTempDir(std::move(arg));
      case BuiltinId::OSExec:
        return runtime::OSExec(std::move(arg));
      case BuiltinId::OSMakeTempFile:
        return runtime::OSMakeTempFile(std::move(arg));
      case BuiltinId::OSMakeTempDir:
        return runtime::OSMakeTempDir(std::move(arg));
      case BuiltinId::PathJoin:
        return runtime::PathJoin(std::move(arg));
      case BuiltinId::PathNormalize:
        return runtime::PathNormalize(std::move(arg));
      case BuiltinId::PathBasename:
        return runtime::PathBasename(std::move(arg));
      case BuiltinId::PathDirname:
        return runtime::PathDirname(std::move(arg));
      case BuiltinId::PathExists:
        return runtime::PathExists(std::move(arg));
      case BuiltinId::PathIsFile:
        return runtime::PathIsFile(std::move(arg));
      case BuiltinId::PathIsDir:
        return runtime::PathIsDir(std::move(arg));
      case BuiltinId::PathAbsolute:
        return runtime::PathAbsolute(std::move(arg));
      case BuiltinId::PathExtension:
        return runtime::PathExtension(std::move(arg));
      case BuiltinId::PathStem:
        return runtime::PathStem(std::move(arg));
      case BuiltinId::PathWithExtension:
        return runtime::PathWithExtension(std::move(arg));
      case BuiltinId::PathWithBasename:
        return runtime::PathWithBasename(std::move(arg));
      case BuiltinId::FileReadText:
        return runtime::FileReadText(std::move(arg));
      case BuiltinId::FileWriteText:
        return runtime::FileWriteText(std::move(arg));
      case BuiltinId::FileAppendText:
        return runtime::FileAppendText(std::move(arg));
      case BuiltinId::FileReadLines:
        return runtime::FileReadLines(std::move(arg));
      case BuiltinId::FileDelete:
        return runtime::FileDelete(std::move(arg));
      case BuiltinId::FileSize:
        return runtime::FileSize(std::move(arg));
      case BuiltinId::FileOpen:
        return runtime::FileOpen(std::move(arg));
      case BuiltinId::FileReadLine:
        return runtime::FileReadLine(std::move(arg));
      case BuiltinId::FileReadChunk:
        return runtime::FileReadChunk(std::move(arg));
      case BuiltinId::FileWriteChunk:
        return runtime::FileWriteChunk(std::move(arg));
      case BuiltinId::FileFlush:
        return runtime::FileFlush(std::move(arg));
      case BuiltinId::FileClose:
        return runtime::FileClose(std::move(arg));
      case BuiltinId::FileWithOpen:
        return runtime::FileWithOpen(std::move(arg));
      case BuiltinId::DirCreate:
        return runtime::DirCreate(std::move(arg));
      case BuiltinId::DirDelete:
        return runtime::DirDelete(std::move(arg));
      case BuiltinId::DirList:
        return runtime::DirList(std::move(arg));
      case BuiltinId::DirListFull:
        return runtime::DirListFull(std::move(arg));
      case BuiltinId::TupleAppend:
        return runtime::TupleAppend(std::move(arg));
      case BuiltinId::TuplePrepend:
        return runtime::TuplePrepend(std::move(arg));
      case BuiltinId::TupleReverse:
        return runtime::TupleReverse(std::move(arg));
      case BuiltinId::TupleContains:
        return runtime::TupleContains(std::move(arg));
      case BuiltinId::TupleZip:
        return runtime::TupleZip(std::move(arg));
      case BuiltinId::TupleMap:
        return runtime::TupleMap(std::move(arg));
      case BuiltinId::TupleFilter:
        return runtime::TupleFilter(std::move(arg));
      case BuiltinId::TupleSort:
        return runtime::TupleSort(std::move(arg));
      case BuiltinId::TupleUnique:
        return runtime::TupleUnique(std::move(arg));
      case BuiltinId::TupleMin:
        return runtime::TupleMin(std::move(arg));
      case BuiltinId::TupleMax:
        return runtime::TupleMax(std::move(arg));
      case BuiltinId::TupleReduce:
        return runtime::TupleReduce(std::move(arg));
      case BuiltinId::TupleFindIndex:
        return runtime::TupleFindIndex(std::move(arg));
      case BuiltinId::TupleAny:
        return runtime::TupleAny(std::move(arg));
      case BuiltinId::TupleAll:
        return runtime::TupleAll(std::move(arg));
      case BuiltinId::TupleRange:
        return runtime::TupleRange(std::move(arg));
      case BuiltinId::ArrayGetAt:
        return runtime::ArrayGetAt(std::move(arg));
      case BuiltinId::ArraySetAt:
        return runtime::ArraySetAt(std::move(arg));
      case BuiltinId::ArrayInsertAt:
        return runtime::ArrayInsertAt(std::move(arg));
      case BuiltinId::ArrayRemoveAt:
        return runtime::ArrayRemoveAt(std::move(arg));
      case BuiltinId::ArraySlice:
        return runtime::ArraySlice(std::move(arg));
      case BuiltinId::ArrayConcat:
        return runtime::ArrayConcat(std::move(arg));
      case BuiltinId::ArraySetAt2D:
        return runtime::ArraySetAt2D(std::move(arg));
      case BuiltinId::ArrayFill:
        return runtime::ArrayFill(std::move(arg));
      case BuiltinId::ArrayTranspose2D:
        return runtime::ArrayTranspose2D(std::move(arg));
      case BuiltinId::ArraySlice2D:
        return runtime::ArraySlice2D(std::move(arg));
      case BuiltinId::ArrayReshape:
        return runtime::ArrayReshape(std::move(arg));
      case BuiltinId::ArrayRank:
        return runtime::ArrayRank(std::move(arg));
      case BuiltinId::ArrayShape:
        return runtime::ArrayShape(std::move(arg));
      case BuiltinId::ArrayFlatten:
        return runtime::ArrayFlatten(std::move(arg));
      case BuiltinId::ArrayGetAtND:
        return runtime::ArrayGetAtND(std::move(arg));
      case BuiltinId::ArraySetAtND:
        return runtime::ArraySetAtND(std::move(arg));
      case BuiltinId::ArrayReshapeND:
        return runtime::ArrayReshapeND(std::move(arg));
      case BuiltinId::DictCreateVoid:
        return runtime::DictCreate_Void(std::move(arg));
      case BuiltinId::DictCreateDict:
        return runtime::DictCreate_Dict(std::move(arg));
      case BuiltinId::DictSet:
        return runtime::DictSet(std::move(arg));
      case BuiltinId::DictGet:
        return runtime::DictGet(std::move(arg));
      case BuiltinId::DictGetDefault:
        return runtime::DictGetDefault(std::move(arg));
      case BuiltinId::DictContains:
        return runtime::DictContains(std::move(arg));
      case BuiltinId::DictDelete:
        return runtime::DictDelete(std::move(arg));
      case BuiltinId::DictMerge:
        return runtime::DictMerge(std::move(arg));
      case BuiltinId::DictKeys:
        return runtime::DictKeys(std::move(arg));
      case BuiltinId::DictValues:
        return runtime::DictValues(std::move(arg));
      case BuiltinId::DictEntries:
        return runtime::DictEntries(std::move(arg));
      case BuiltinId::DictClear:
        return runtime::DictClear(std::move(arg));
      case BuiltinId::DictLength:
        return runtime::DictLength(std::move(arg));
      case BuiltinId::Cast:
        return runtime::Cast(std::move(arg));
      case BuiltinId::ToInt64:
        return runtime::ToInt64(std::move(arg));
      case BuiltinId::ToUInt64:
        return runtime::ToUInt64(std::move(arg));
      case BuiltinId::ToFloat64:
        return runtime::ToFloat64(std::move(arg));
      case BuiltinId::MathFloor:
        return runtime::MathFloor(std::move(arg));
      case BuiltinId::MathCeil:
        return runtime::MathCeil(std::move(arg));
      case BuiltinId::MathAbs:
        return runtime::MathAbs(std::move(arg));
      case BuiltinId::MathLog:
        return runtime::MathLog(std::move(arg));
      case BuiltinId::MathClamp:
        return runtime::MathClamp(std::move(arg));
      case BuiltinId::Sqrt:
        return runtime::Sqrt(std::move(arg));
      case BuiltinId::Sin:
        return runtime::Sin(std::move(arg));
      case BuiltinId::Cos:
        return runtime::Cos(std::move(arg));
      case BuiltinId::Tan:
        return runtime::Tan(std::move(arg));
      case BuiltinId::ResultOk:
        return runtime::ResultOk(std::move(arg));
      case BuiltinId::ResultErr:
        return runtime::ResultErr(std::move(arg));
      case BuiltinId::ResultTag:
        return runtime::ResultTag(std::move(arg));
      case BuiltinId::ResultPayload:
        return runtime::ResultPayload(std::move(arg));
      case BuiltinId::ResultIsOk:
        return runtime::ResultIsOk(std::move(arg));
      case BuiltinId::ResultIsErr:
        return runtime::ResultIsErr(std::move(arg));
      case BuiltinId::ResultUnwrap:
        return runtime::ResultUnwrap(std::move(arg));
      case BuiltinId::ResultUnwrapErr:
        return runtime::ResultUnwrapErr(std::move(arg));
      case BuiltinId::Try:
        return runtime::Try(std::move(arg));
      case BuiltinId::ParallelMap:
        return runtime::ParallelMap(std::move(arg));
      case BuiltinId::ParallelWithOptions:
        return runtime::ParallelWithOptions(std::move(arg));
      case BuiltinId::ParallelForEach:
        return runtime::ParallelForEach(std::move(arg));
      case BuiltinId::ParallelReduce:
        return runtime::ParallelReduce(std::move(arg));
      case BuiltinId::TaskSpawn:
        return runtime::TaskSpawn(std::move(arg));
      case BuiltinId::TaskAwait:
        return runtime::TaskAwait(std::move(arg));
      case BuiltinId::TaskAwaitAll:
        return runtime::TaskAwaitAll(std::move(arg));
      case BuiltinId::TaskCancel:
        return runtime::TaskCancel(std::move(arg));
      case BuiltinId::TaskWithTimeout:
        return runtime::TaskWithTimeout(std::move(arg));
      case BuiltinId::Wrap:
        return runtime::Wrap(std::move(arg));
      case BuiltinId::Unwrap:
        return runtime::Unwrap(std::move(arg));
      case BuiltinId::ElementAt:
        return runtime::ElementAt(std::move(arg));
      case BuiltinId::Length:
        return runtime::Length(std::move(arg));
      case BuiltinId::Take:
        return runtime::Take(std::move(arg));
      case BuiltinId::Drop:
        return runtime::Drop(std::move(arg));
      case BuiltinId::Slice:
        return runtime::Slice(std::move(arg));
      case BuiltinId::ToString:
        return runtime::ToString(std::move(arg));
      case BuiltinId::ToNum:
        return runtime::ToNum(std::move(arg));
      case BuiltinId::StringParseInt64:
        return runtime::StringParseInt64(std::move(arg));
      case BuiltinId::StringParseUInt64:
        return runtime::StringParseUInt64(std::move(arg));
      case BuiltinId::StringParseFloat64:
        return runtime::StringParseFloat64(std::move(arg));
      case BuiltinId::StringUpper:
        return runtime::StringUpper(std::move(arg));
      case BuiltinId::StringLower:
        return runtime::StringLower(std::move(arg));
      case BuiltinId::StringTrim:
        return runtime::StringTrim(std::move(arg));
      case BuiltinId::StringTrimStart:
        return runtime::StringTrimStart(std::move(arg));
      case BuiltinId::StringTrimEnd:
        return runtime::StringTrimEnd(std::move(arg));
      case BuiltinId::StringSplit:
        return runtime::StringSplit(std::move(arg));
      case BuiltinId::StringJoin:
        return runtime::StringJoin(std::move(arg));
      case BuiltinId::StringReplace:
        return runtime::StringReplace(std::move(arg));
      case BuiltinId::StringContains:
        return runtime::StringContains(std::move(arg));
      case BuiltinId::StringStartsWith:
        return runtime::StringStartsWith(std::move(arg));
      case BuiltinId::StringEndsWith:
        return runtime::StringEndsWith(std::move(arg));
      case BuiltinId::StringLength:
        return runtime::StringLength(std::move(arg));
      case BuiltinId::StringCharAt:
        return runtime::StringCharAt(std::move(arg));
      case BuiltinId::StringSlice:
        return runtime::StringSlice(std::move(arg));
      case BuiltinId::StringFind:
        return runtime::StringFind(std::move(arg));
      case BuiltinId::StringFormat:
        return runtime::StringFormat(std::move(arg));
      case BuiltinId::StringRegexIsMatch:
        return runtime::StringRegexIsMatch(arg);
      case BuiltinId::StringRegexFind:
        return runtime::StringRegexFind(arg);
      case BuiltinId::StringRegexReplace:
        return runtime::StringRegexReplace(arg);
      case BuiltinId::StringRegexSplit:
        return runtime::StringRegexSplit(arg);
      default:
        if (const auto constant_value = constant_builtin_value(builtin_id); constant_value.has_value()) {
          return runtime::make_float(*constant_value);
        }
        return tl::unexpected(make_runtime_error("unknown builtin id"));
    }
  } catch (const std::exception& ex) {
    return tl::unexpected(
        make_runtime_error(std::string("builtin '") + std::string(builtin_name(builtin_id)) + "' threw: " + ex.what()));
  }
}

}  // namespace

struct RuntimeSession::Impl {
  explicit Impl(const std::vector<std::string>& process_args, const RuntimeCompileOptions& session_compile_options)
      : compile_options(session_compile_options),
        source_path((std::filesystem::current_path() / "__repl__.fleaux").lexically_normal()) {
    std::vector<std::string> args_storage;
    args_storage.reserve(process_args.size() + 1U);
    args_storage.emplace_back("<repl>");
    args_storage.insert(args_storage.end(), process_args.begin(), process_args.end());

    std::vector<char*> argv_ptrs;
    argv_ptrs.reserve(args_storage.size());
    for (auto& arg : args_storage) {
      argv_ptrs.push_back(arg.data());
    }
    runtime::set_process_args(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());
  }

  RuntimeCompileOptions compile_options;
  std::filesystem::path source_path;
  std::vector<frontend::ir::IRLet> lets;
  std::vector<frontend::ir::IRTypeDecl> type_decls;
  std::vector<frontend::ir::IRAliasDecl> alias_decls;
};

RuntimeSession::RuntimeSession(const std::vector<std::string>& process_args,
                               const RuntimeCompileOptions& compile_options)
    : impl_(frontend::make_box<Impl>(process_args, compile_options)) {}

RuntimeSession::RuntimeSession(const RuntimeSession& other) = default;

RuntimeSession::RuntimeSession(RuntimeSession&& other) noexcept = default;

auto RuntimeSession::operator=(const RuntimeSession& other) -> RuntimeSession& = default;

auto RuntimeSession::operator=(RuntimeSession&& other) noexcept -> RuntimeSession& = default;

RuntimeSession::~RuntimeSession() = default;

auto RuntimeSession::run_snippet(const std::string& snippet_text, std::ostream& output) const -> RuntimeResult {
  auto analyzed = detail::parse_and_analyze_repl_text(snippet_text, impl_->source_path, impl_->lets, impl_->type_decls,
                                                      impl_->alias_decls);
  if (!analyzed) {
    return tl::unexpected(make_runtime_error(analyzed.error().message, analyzed.error().hint, analyzed.error().span));
  }

  auto merged_lets = detail::merge_repl_session_lets(impl_->lets, analyzed->lets, impl_->source_path);
  auto merged_type_decls =
      detail::merge_repl_session_type_decls(impl_->type_decls, analyzed->type_decls, impl_->source_path);
  auto merged_alias_decls =
      detail::merge_repl_session_alias_decls(impl_->alias_decls, analyzed->alias_decls, impl_->source_path);
  auto program_to_execute = analyzed.value();
  program_to_execute.lets = merged_lets;
  program_to_execute.type_decls = merged_type_decls;
  program_to_execute.alias_decls = merged_alias_decls;

  constexpr bytecode::BytecodeCompiler compiler;
  auto compiled =
      compiler.compile(program_to_execute, bytecode::CompileOptions{
                                               .source_path = impl_->source_path,
                                               .source_text = snippet_text,
                                               .module_name = std::string{"repl"},
                                               .imported_modules = {},
                                               .enable_value_ref_gate = impl_->compile_options.enable_value_ref_gate ||
                                                                        impl_->compile_options.enable_auto_value_ref,
                                               .enable_auto_value_ref = impl_->compile_options.enable_auto_value_ref,
                                               .value_ref_byte_cutoff = impl_->compile_options.value_ref_byte_cutoff,
                                           });
  if (!compiled) {
    return tl::unexpected(
        make_runtime_error(compiled.error().message, "This REPL snippet is not yet supported by the VM compiler."));
  }

  impl_->lets = std::move(merged_lets);
  impl_->type_decls = std::move(merged_type_decls);
  impl_->alias_decls = std::move(merged_alias_decls);

  constexpr Runtime runtime;
  return runtime.execute(*compiled, output);
}

// Runtime::execute

auto Runtime::create_session(const std::vector<std::string>& process_args,
                             const RuntimeCompileOptions& compile_options) const -> RuntimeSession {
  return RuntimeSession(process_args, compile_options);
}

auto Runtime::execute(const bytecode::Module& bytecode_module) const -> RuntimeResult {
  return execute(bytecode_module, std::cout);
}

auto Runtime::execute(const bytecode::Module& bytecode_module, std::ostream& output) const -> RuntimeResult {
  runtime::CallableRegistryScope callable_scope;
  runtime::ValueRegistryScope value_scope;
  runtime::HandleRegistryScope handle_scope;
  runtime::TaskRegistryScope task_scope;
  detail::StdHelpMetadataScope help_scope;
  runtime::RuntimeOutputStreamScope output_scope(output);

  std::vector<Value> stack;
  CallFrameStack frames;
  stack.reserve(suggested_operand_stack_capacity(bytecode_module.instructions.size()));
  frames.reserve(suggested_frame_stack_capacity(bytecode_module.functions.size()));
  (void)frames.push(&bytecode_module.instructions);

  auto loop_result = run_loop(bytecode_module, stack, frames, output);
  if (!loop_result)
    return tl::unexpected(loop_result.error());

  if (std::get_if<Value>(&*loop_result) != nullptr) {
    // A top-level kReturn would be a compiler bug.
    return tl::unexpected(make_runtime_error("top-level code returned a value instead of halting"));
  }
  return ExecutionResult{0};
}

}  // namespace fleaux::vm
