
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <variant>
#include <vector>

// The order here is important
#include "fleaux/runtime/runtime_support.hpp"
// builtin_map.hpp must be included after runtime_support.hpp
#include "builtin_map.hpp"
#include "repl_support.hpp"
#include "std_help_metadata.hpp"

#include "fleaux/bytecode/compiler.hpp"
#include "fleaux/common/overloaded.hpp"
#include "fleaux/vm/builtin_catalog.hpp"
#include "fleaux/vm/runtime.hpp"

namespace fleaux::vm {
namespace {

using fleaux::runtime::Array;
using fleaux::runtime::RuntimeCallable;
using fleaux::runtime::Value;

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
  if (stack.empty()) { return tl::unexpected(RuntimeError{std::string("stack underflow on ") + context}); }
  Value value = std::move(stack.back());
  stack.pop_back();
  return value;
}

template <typename Fn>
auto run_native_op(const char* opname, Fn&& fn) -> tl::expected<Value, RuntimeError> {
  try {
    return fn();
  } catch (const std::exception& ex) {
    return tl::unexpected(RuntimeError{std::string("native '") + opname + "' threw: " + ex.what()});
  }
}

struct CallFrame {
  const std::vector<bytecode::Instruction>* instructions = nullptr;
  std::size_t ip = 0;
  std::vector<Value> locals;
};

// Loop exit type
// std::monostate -> kHalt was hit (top-level completion).
// Value          -> kReturn emptied the frame stack (standalone function result).

using LoopExit = std::variant<std::monostate, Value>;
using LoopResult = tl::expected<LoopExit, RuntimeError>;

// Forward declarations.
auto run_loop(const bytecode::Module& bytecode_module, std::vector<Value>& stack, std::vector<CallFrame>& frames,
              const std::unordered_map<std::string, RuntimeCallable>& builtins, std::ostream& output) -> LoopResult;

auto dispatch_builtin(const std::string& name, Value arg,
                      const std::unordered_map<std::string, RuntimeCallable>& builtins, std::ostream& output)
    -> tl::expected<Value, RuntimeError>;

auto bind_user_function_locals(const std::string& fn_name, const std::uint32_t arity, const bool has_variadic_tail,
                               Value arg) -> tl::expected<std::vector<Value>, RuntimeError> {
  std::vector<Value> locals;
  locals.reserve(arity);

  if (arity == 0U) { return locals; }

  if (!has_variadic_tail) {
    if (arity == 1U) {
      locals.push_back(fleaux::runtime::unwrap_singleton_arg(std::move(arg)));
      return locals;
    }

    try {
      const auto& arr = fleaux::runtime::as_array(arg);
      for (std::uint32_t arg_index = 0; arg_index < arity; ++arg_index) {
        auto elem = arr.TryGet(arg_index);
        if (!elem) { return tl::unexpected(RuntimeError{"too few arguments for '" + fn_name + "'"}); }
        locals.push_back(*elem);
      }
      return locals;
    } catch (const std::exception& ex) {
      return tl::unexpected(RuntimeError{std::string("argument unpacking for '") + fn_name + "': " + ex.what()});
    }
  }

  // Variadic: final parameter captures remaining args as a tuple.
  if (arity == 1U) {
    if (arg.HasArray()) {
      locals.push_back(std::move(arg));
    } else {
      locals.push_back(fleaux::runtime::make_tuple(std::move(arg)));
    }
    return locals;
  }

  const auto fixed_count = static_cast<std::size_t>(arity - 1U);
  try {
    const auto& arr = fleaux::runtime::as_array(arg);
    if (arr.Size() < fixed_count) { return tl::unexpected(RuntimeError{"too few arguments for '" + fn_name + "'"}); }
    for (std::size_t arg_index = 0; arg_index < fixed_count; ++arg_index) { locals.push_back(*arr.TryGet(arg_index)); }

    Array tail;
    tail.Reserve(arr.Size() - fixed_count);
    for (std::size_t arg_index = fixed_count; arg_index < arr.Size(); ++arg_index) {
      tail.PushBack(*arr.TryGet(arg_index));
    }
    locals.emplace_back(std::move(tail));
    return locals;
  } catch (const std::exception& ex) {
    return tl::unexpected(RuntimeError{std::string("argument unpacking for '") + fn_name + "': " + ex.what()});
  }
}

auto unpack_declared_call_args(Value arg, const std::uint32_t declared_arity, const bool declared_has_variadic_tail)
    -> tl::expected<std::vector<Value>, RuntimeError> {
  std::vector<Value> out;
  out.reserve(declared_arity);

  if (declared_arity == 0U) { return out; }

  if (!declared_has_variadic_tail) {
    if (declared_arity == 1U) {
      out.push_back(fleaux::runtime::unwrap_singleton_arg(std::move(arg)));
      return out;
    }

    try {
      const auto& arr = fleaux::runtime::as_array(arg);
      for (std::uint32_t arg_index = 0; arg_index < declared_arity; ++arg_index) {
        auto elem = arr.TryGet(arg_index);
        if (!elem) { return tl::unexpected(RuntimeError{"too few arguments for inline closure"}); }
        out.push_back(*elem);
      }
      return out;
    } catch (const std::exception& ex) {
      return tl::unexpected(RuntimeError{std::string("argument unpacking for inline closure: ") + ex.what()});
    }
  }

  if (declared_arity == 1U) {
    out.push_back(arg.HasArray() ? std::move(arg) : fleaux::runtime::make_tuple(std::move(arg)));
    return out;
  }

  const auto fixed_count = static_cast<std::size_t>(declared_arity - 1U);
  try {
    const auto& arr = fleaux::runtime::as_array(arg);
    if (arr.Size() < fixed_count) { return tl::unexpected(RuntimeError{"too few arguments for inline closure"}); }
    for (std::size_t arg_index = 0; arg_index < fixed_count; ++arg_index) { out.push_back(*arr.TryGet(arg_index)); }

    Array tail;
    tail.Reserve(arr.Size() - fixed_count);
    for (std::size_t arg_index = fixed_count; arg_index < arr.Size(); ++arg_index) {
      tail.PushBack(*arr.TryGet(arg_index));
    }
    out.emplace_back(std::move(tail));
    return out;
  } catch (const std::exception& ex) {
    return tl::unexpected(RuntimeError{std::string("argument unpacking for inline closure: ") + ex.what()});
  }
}

auto pack_call_args(std::vector<Value> values) -> Value {
  if (values.empty()) { return Value{Array{}}; }
  if (values.size() == 1U) { return std::move(values[0]); }

  Array out;
  out.Reserve(values.size());
  for (auto& value : values) { out.PushBack(value); }
  return Value{std::move(out)};
}

// Try VM-native builtin execution; returns nullopt when builtin is not ported yet.
auto try_run_vm_native_builtin(const std::string& name, const Value& arg, std::ostream& output)
    -> tl::expected<std::optional<Value>, RuntimeError>;

auto run_user_function(const bytecode::Module& bytecode_module, std::size_t fn_idx, Value arg,
                       const std::unordered_map<std::string, RuntimeCallable>& builtins, std::ostream& output)
    -> tl::expected<Value, RuntimeError>;

auto run_loop_intrinsic(Value state, const Value& continue_func, const Value& step_func,
                        const std::optional<std::size_t> max_iters) -> tl::expected<Value, RuntimeError> {
  try {
    std::size_t iterations = 0;
    while (fleaux::runtime::as_bool(fleaux::runtime::invoke_callable_ref(continue_func, state))) {
      if (max_iters.has_value() && iterations >= *max_iters) { throw std::runtime_error("LoopN: exceeded max_iters"); }
      state = fleaux::runtime::invoke_callable_ref(step_func, std::move(state));
      ++iterations;
    }
    return state;
  } catch (const std::exception& ex) {
    return tl::unexpected(RuntimeError{std::string("native 'loop' threw: ") + ex.what()});
  }
}

// run_user_function

auto run_user_function(const bytecode::Module& bytecode_module, const std::size_t fn_idx, Value arg,
                       const std::unordered_map<std::string, RuntimeCallable>& builtins, std::ostream& output)
    -> tl::expected<Value, RuntimeError> {
  if (fn_idx >= bytecode_module.functions.size()) {
    return tl::unexpected(RuntimeError{"function index out of range"});
  }
  const auto& function = bytecode_module.functions[fn_idx];
  const auto& name = function.name;
  const auto arity = function.arity;
  const auto has_variadic_tail = function.has_variadic_tail;
  const auto& instructions = function.instructions;

  std::vector<Value> inner_stack;
  std::vector<CallFrame> inner_frames;

  CallFrame frame;
  frame.instructions = &instructions;
  frame.ip = 0;

  auto bound_locals = bind_user_function_locals(name, arity, has_variadic_tail, std::move(arg));
  if (!bound_locals) { return tl::unexpected(bound_locals.error()); }
  frame.locals = std::move(*bound_locals);

  inner_frames.push_back(std::move(frame));

  auto loop_result = run_loop(bytecode_module, inner_stack, inner_frames, builtins, output);
  if (!loop_result) return tl::unexpected(loop_result.error());

  if (std::get_if<std::monostate>(&*loop_result) != nullptr) {
    return tl::unexpected(RuntimeError{"halt inside function '" + name + "'"});
  }
  if (auto* value = std::get_if<Value>(&*loop_result); value != nullptr) { return std::move(*value); }
  return tl::unexpected(RuntimeError{"invalid loop result variant"});
}

// run_loop

auto run_loop(const bytecode::Module& bytecode_module, std::vector<Value>& stack, std::vector<CallFrame>& frames,
              const std::unordered_map<std::string, RuntimeCallable>& builtins, std::ostream& output) -> LoopResult {
  while (!frames.empty()) {
    const std::size_t curr_ip = frames.back().ip;
    frames.back().ip++;

    const auto& instr_list = *frames.back().instructions;
    if (curr_ip >= instr_list.size()) { return tl::unexpected(RuntimeError{"program terminated without halt"}); }

    const auto opcode = instr_list[curr_ip].opcode;
    const auto operand = instr_list[curr_ip].operand;

    switch (opcode) {
      case bytecode::Opcode::kNoOp:
        break;

      case bytecode::Opcode::kPushConst: {
        const auto idx = static_cast<std::size_t>(operand);
        if (idx >= bytecode_module.constants.size()) {
          return tl::unexpected(RuntimeError{"constant pool index out of range"});
        }
        stack.push_back(const_to_value(bytecode_module.constants[idx]));
        break;
      }

      case bytecode::Opcode::kPop: {
        if (auto popped_value = pop_stack(stack, "pop"); !popped_value) return tl::unexpected(popped_value.error());
        break;
      }

      case bytecode::Opcode::kDup: {
        if (stack.empty()) { return tl::unexpected(RuntimeError{"stack underflow on dup"}); }
        stack.push_back(stack.back());
        break;
      }

      case bytecode::Opcode::kBuildTuple: {
        const auto tuple_size = static_cast<std::size_t>(operand);
        if (stack.size() < tuple_size) { return tl::unexpected(RuntimeError{"stack underflow on build_tuple"}); }
        Array arr;
        arr.Reserve(tuple_size);
        const auto base = stack.size() - tuple_size;
        for (std::size_t element_index = 0; element_index < tuple_size; ++element_index) {
          arr.PushBack(stack[base + element_index]);
        }
        stack.resize(base);
        stack.emplace_back(std::move(arr));
        break;
      }

      case bytecode::Opcode::kMakeValueRef: {
        auto value = pop_stack(stack, "make_value_ref");
        if (!value) return tl::unexpected(value.error());
        stack.push_back(fleaux::runtime::make_value_ref(std::move(*value)));
        break;
      }

      case bytecode::Opcode::kDerefValueRef: {
        auto token = pop_stack(stack, "deref_value_ref");
        if (!token) return tl::unexpected(token.error());
        auto result =
            run_native_op("deref_value_ref", [&]() -> Value { return fleaux::runtime::deref_value_ref(*token); });
        if (!result) return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kCallBuiltin: {
        const auto idx = static_cast<std::size_t>(operand);
        if (idx >= bytecode_module.builtin_names.size()) {
          return tl::unexpected(RuntimeError{"builtin index out of range"});
        }
        const auto& name = bytecode_module.builtin_names[idx];

        auto arg = pop_stack(stack, "call_builtin");
        if (!arg) return tl::unexpected(arg.error());

        auto result = dispatch_builtin(name, std::move(*arg), builtins, output);
        if (!result) return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kCallUserFunc: {
        const auto fn_idx = static_cast<std::size_t>(operand);
        if (fn_idx >= bytecode_module.functions.size()) {
          return tl::unexpected(RuntimeError{"function index out of range"});
        }
        auto arg = pop_stack(stack, "call_user_func");
        if (!arg) return tl::unexpected(arg.error());

        const auto& function = bytecode_module.functions[fn_idx];
        const auto& name = function.name;
        const auto arity = function.arity;
        const auto has_variadic_tail = function.has_variadic_tail;
        const auto& instructions = function.instructions;
        CallFrame new_frame;
        new_frame.instructions = &instructions;
        new_frame.ip = 0;

        auto bound_locals = bind_user_function_locals(name, arity, has_variadic_tail, std::move(*arg));
        if (!bound_locals) { return tl::unexpected(bound_locals.error()); }
        new_frame.locals = std::move(*bound_locals);

        frames.push_back(std::move(new_frame));
        break;
      }

      case bytecode::Opcode::kMakeUserFuncRef: {
        const auto fn_idx = static_cast<std::size_t>(operand);
        if (fn_idx >= bytecode_module.functions.size()) {
          return tl::unexpected(RuntimeError{"function index out of range"});
        }
        // Callable captures bytecode_module, builtins, and output by reference.
        // Safety contract: these refs remain valid for the entire duration of the
        // enclosing Runtime::execute() call. The callable is registered globally
        // but the callable-ref Value only lives on the execution stack, so it
        // cannot outlive execute(). For async builtins (e.g. Std.Parallel.Map),
        // all futures complete via .get() before run_loop continues, so the
        // captured refs are still live during any async invocation.
        auto callable = [&bytecode_module, fn_idx, &builtins, &output](Value call_arg) -> Value {
          auto run_result = run_user_function(bytecode_module, fn_idx, std::move(call_arg), builtins, output);
          if (!run_result) throw std::runtime_error(run_result.error().message);
          return std::move(*run_result);
        };
        stack.push_back(fleaux::runtime::make_callable_ref(std::move(callable)));
        break;
      }

      case bytecode::Opcode::kMakeBuiltinFuncRef: {
        const auto idx = static_cast<std::size_t>(operand);
        if (idx >= bytecode_module.builtin_names.size()) {
          return tl::unexpected(RuntimeError{"builtin index out of range"});
        }
        const auto& name = bytecode_module.builtin_names[idx];
        auto callable = [name, &builtins, &output](Value arg) -> Value {
          auto result = dispatch_builtin(name, std::move(arg), builtins, output);
          if (!result) { throw std::runtime_error(result.error().message); }
          return std::move(*result);
        };
        stack.push_back(fleaux::runtime::make_callable_ref(std::move(callable)));
        break;
      }

      case bytecode::Opcode::kMakeClosureRef: {
        const auto closure_idx = static_cast<std::size_t>(operand);
        if (closure_idx >= bytecode_module.closures.size()) {
          return tl::unexpected(RuntimeError{"closure index out of range"});
        }

        auto captured_tuple = pop_stack(stack, "make_closure_ref");
        if (!captured_tuple) { return tl::unexpected(captured_tuple.error()); }

        const auto& [function_index, capture_count, declared_arity, declared_has_variadic_tail] =
            bytecode_module.closures[closure_idx];
        const auto& capture_array = fleaux::runtime::as_array(*captured_tuple);
        if (capture_array.Size() != capture_count) {
          return tl::unexpected(RuntimeError{"closure capture tuple size mismatch"});
        }

        std::vector<Value> captured_values;
        captured_values.reserve(capture_count);
        for (std::size_t capture_index = 0; capture_index < capture_count; ++capture_index) {
          captured_values.push_back(*capture_array.TryGet(capture_index));
        }

        // Callable captures bytecode_module, builtins, and output by reference.
        // Same lifetime contract as kMakeUserFuncRef: valid for the duration of
        // the enclosing Runtime::execute() call. Callable-ref Values stay on the
        // execution stack and cannot outlive execute().
        auto closure_callable = [&bytecode_module, function_index, capture_count, declared_arity,
                                 declared_has_variadic_tail, captured_values = std::move(captured_values), &builtins,
                                 &output](Value call_arg) mutable -> Value {
          auto declared_args =
              unpack_declared_call_args(std::move(call_arg), declared_arity, declared_has_variadic_tail);
          if (!declared_args) { throw std::runtime_error(declared_args.error().message); }

          std::vector<Value> full_args;
          full_args.reserve(capture_count + declared_args->size());
          for (const auto& captured : captured_values) { full_args.push_back(captured); }
          for (auto& arg : *declared_args) { full_args.push_back(std::move(arg)); }

          auto result = run_user_function(bytecode_module, function_index, pack_call_args(std::move(full_args)),
                                          builtins, output);
          if (!result) { throw std::runtime_error(result.error().message); }
          return std::move(*result);
        };

        stack.push_back(fleaux::runtime::make_callable_ref(std::move(closure_callable)));
        break;
      }

      case bytecode::Opcode::kReturn: {
        auto ret_val = pop_stack(stack, "return");
        if (!ret_val) return tl::unexpected(ret_val.error());
        frames.pop_back();
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
        if (slot >= locals.size()) { return tl::unexpected(RuntimeError{"local slot index out of range"}); }
        stack.push_back(locals[slot]);
        break;
      }

      case bytecode::Opcode::kJump: {
        const auto target = static_cast<std::size_t>(operand);
        if (target > instr_list.size()) { return tl::unexpected(RuntimeError{"jump target out of range"}); }
        frames.back().ip = target;
        break;
      }

      case bytecode::Opcode::kJumpIf: {
        auto cond = pop_stack(stack, "jump_if");
        if (!cond) return tl::unexpected(cond.error());
        const auto target = static_cast<std::size_t>(operand);
        if (target > instr_list.size()) { return tl::unexpected(RuntimeError{"jump_if target out of range"}); }
        if (fleaux::runtime::as_bool(*cond)) { frames.back().ip = target; }
        break;
      }

      case bytecode::Opcode::kJumpIfNot: {
        auto cond = pop_stack(stack, "jump_if_not");
        if (!cond) return tl::unexpected(cond.error());
        const auto target = static_cast<std::size_t>(operand);
        if (target > instr_list.size()) { return tl::unexpected(RuntimeError{"jump_if_not target out of range"}); }
        if (!fleaux::runtime::as_bool(*cond)) { frames.back().ip = target; }
        break;
      }

      case bytecode::Opcode::kAdd: {
        auto rhs = pop_stack(stack, "add");
        if (!rhs) return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "add");
        if (!lhs) return tl::unexpected(lhs.error());
        auto result = run_native_op("add", [&]() -> Value {
          if (lhs->HasString() && rhs->HasString()) {
            return fleaux::runtime::make_string(fleaux::runtime::as_string(*lhs) + fleaux::runtime::as_string(*rhs));
          }
          fleaux::runtime::require_same_integer_kind(*lhs, *rhs, "add");
          return fleaux::runtime::num_result(
              fleaux::runtime::to_double(*lhs) + fleaux::runtime::to_double(*rhs),
              fleaux::runtime::is_uint_number(*lhs) && fleaux::runtime::is_uint_number(*rhs));
        });
        if (!result) return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kSub: {
        auto rhs = pop_stack(stack, "sub");
        if (!rhs) return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "sub");
        if (!lhs) return tl::unexpected(lhs.error());
        auto result = run_native_op("sub", [&]() -> Value {
          fleaux::runtime::require_same_integer_kind(*lhs, *rhs, "sub");
          return fleaux::runtime::num_result(
              fleaux::runtime::to_double(*lhs) - fleaux::runtime::to_double(*rhs),
              fleaux::runtime::is_uint_number(*lhs) && fleaux::runtime::is_uint_number(*rhs));
        });
        if (!result) return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kMul: {
        auto rhs = pop_stack(stack, "mul");
        if (!rhs) return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "mul");
        if (!lhs) return tl::unexpected(lhs.error());
        auto result = run_native_op("mul", [&]() -> Value {
          fleaux::runtime::require_same_integer_kind(*lhs, *rhs, "mul");
          return fleaux::runtime::num_result(
              fleaux::runtime::to_double(*lhs) * fleaux::runtime::to_double(*rhs),
              fleaux::runtime::is_uint_number(*lhs) && fleaux::runtime::is_uint_number(*rhs));
        });
        if (!result) return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kDiv: {
        auto rhs = pop_stack(stack, "div");
        if (!rhs) return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "div");
        if (!lhs) return tl::unexpected(lhs.error());
        auto result = run_native_op("div", [&]() -> Value {
          // Native division follows floating-point semantics (e.g. x/0 -> inf).
          fleaux::runtime::require_same_integer_kind(*lhs, *rhs, "div");
          return fleaux::runtime::num_result(
              fleaux::runtime::to_double(*lhs) / fleaux::runtime::to_double(*rhs),
              fleaux::runtime::is_uint_number(*lhs) && fleaux::runtime::is_uint_number(*rhs));
        });
        if (!result) return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kMod: {
        auto rhs = pop_stack(stack, "mod");
        if (!rhs) return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "mod");
        if (!lhs) return tl::unexpected(lhs.error());
        auto result = run_native_op("mod", [&]() -> Value {
          fleaux::runtime::require_same_integer_kind(*lhs, *rhs, "mod");
          return fleaux::runtime::num_result(
              std::fmod(fleaux::runtime::to_double(*lhs), fleaux::runtime::to_double(*rhs)),
              fleaux::runtime::is_uint_number(*lhs) && fleaux::runtime::is_uint_number(*rhs));
        });
        if (!result) return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kPow: {
        auto rhs = pop_stack(stack, "pow");
        if (!rhs) return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "pow");
        if (!lhs) return tl::unexpected(lhs.error());
        auto result = run_native_op("pow", [&]() -> Value {
          return fleaux::runtime::num_result(
              std::pow(fleaux::runtime::to_double(*lhs), fleaux::runtime::to_double(*rhs)));
        });
        if (!result) return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kNeg: {
        auto value = pop_stack(stack, "neg");
        if (!value) return tl::unexpected(value.error());
        auto result = run_native_op(
            "neg", [&]() -> Value { return fleaux::runtime::num_result(-fleaux::runtime::to_double(*value)); });
        if (!result) return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kCmpEq: {
        auto rhs = pop_stack(stack, "cmp_eq");
        if (!rhs) return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "cmp_eq");
        if (!lhs) return tl::unexpected(lhs.error());
        stack.push_back(fleaux::runtime::make_bool(*lhs == *rhs));
        break;
      }

      case bytecode::Opcode::kCmpNe: {
        auto rhs = pop_stack(stack, "cmp_ne");
        if (!rhs) return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "cmp_ne");
        if (!lhs) return tl::unexpected(lhs.error());
        stack.push_back(fleaux::runtime::make_bool(*lhs != *rhs));
        break;
      }

      case bytecode::Opcode::kCmpLt: {
        auto rhs = pop_stack(stack, "cmp_lt");
        if (!rhs) return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "cmp_lt");
        if (!lhs) return tl::unexpected(lhs.error());
        auto result = run_native_op("cmp_lt", [&]() -> Value {
          return fleaux::runtime::make_bool(fleaux::runtime::compare_numbers(*lhs, *rhs) < 0);
        });
        if (!result) return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kCmpGt: {
        auto rhs = pop_stack(stack, "cmp_gt");
        if (!rhs) return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "cmp_gt");
        if (!lhs) return tl::unexpected(lhs.error());
        auto result = run_native_op("cmp_gt", [&]() -> Value {
          return fleaux::runtime::make_bool(fleaux::runtime::compare_numbers(*lhs, *rhs) > 0);
        });
        if (!result) return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kCmpLe: {
        auto rhs = pop_stack(stack, "cmp_le");
        if (!rhs) return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "cmp_le");
        if (!lhs) return tl::unexpected(lhs.error());
        auto result = run_native_op("cmp_le", [&]() -> Value {
          return fleaux::runtime::make_bool(fleaux::runtime::compare_numbers(*lhs, *rhs) <= 0);
        });
        if (!result) return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kCmpGe: {
        auto rhs = pop_stack(stack, "cmp_ge");
        if (!rhs) return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "cmp_ge");
        if (!lhs) return tl::unexpected(lhs.error());
        auto result = run_native_op("cmp_ge", [&]() -> Value {
          return fleaux::runtime::make_bool(fleaux::runtime::compare_numbers(*lhs, *rhs) >= 0);
        });
        if (!result) return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kAnd: {
        auto rhs = pop_stack(stack, "and");
        if (!rhs) return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "and");
        if (!lhs) return tl::unexpected(lhs.error());
        auto result = run_native_op("and", [&]() -> Value {
          return fleaux::runtime::make_bool(fleaux::runtime::as_bool(*lhs) && fleaux::runtime::as_bool(*rhs));
        });
        if (!result) return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kOr: {
        auto rhs = pop_stack(stack, "or");
        if (!rhs) return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "or");
        if (!lhs) return tl::unexpected(lhs.error());
        auto result = run_native_op("or", [&]() -> Value {
          return fleaux::runtime::make_bool(fleaux::runtime::as_bool(*lhs) || fleaux::runtime::as_bool(*rhs));
        });
        if (!result) return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kNot: {
        auto value = pop_stack(stack, "not");
        if (!value) return tl::unexpected(value.error());
        auto result = run_native_op(
            "not", [&]() -> Value { return fleaux::runtime::make_bool(!fleaux::runtime::as_bool(*value)); });
        if (!result) return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kSelect: {
        auto false_val = pop_stack(stack, "select");
        if (!false_val) return tl::unexpected(false_val.error());
        auto true_val = pop_stack(stack, "select");
        if (!true_val) return tl::unexpected(true_val.error());
        auto cond = pop_stack(stack, "select");
        if (!cond) return tl::unexpected(cond.error());
        stack.push_back(fleaux::runtime::as_bool(*cond) ? std::move(*true_val) : std::move(*false_val));
        break;
      }

      case bytecode::Opcode::kBranchCall: {
        auto false_func = pop_stack(stack, "branch_call");
        if (!false_func) return tl::unexpected(false_func.error());
        auto true_func = pop_stack(stack, "branch_call");
        if (!true_func) return tl::unexpected(true_func.error());
        auto value = pop_stack(stack, "branch_call");
        if (!value) return tl::unexpected(value.error());
        auto cond = pop_stack(stack, "branch_call");
        if (!cond) return tl::unexpected(cond.error());
        auto result = run_native_op("branch_call", [&]() -> Value {
          const Value& chosen = fleaux::runtime::as_bool(*cond) ? *true_func : *false_func;
          return fleaux::runtime::invoke_callable_ref(chosen, std::move(*value));
        });
        if (!result) return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kLoopCall: {
        auto step_func = pop_stack(stack, "loop_call");
        if (!step_func) return tl::unexpected(step_func.error());
        auto continue_func = pop_stack(stack, "loop_call");
        if (!continue_func) return tl::unexpected(continue_func.error());
        auto state = pop_stack(stack, "loop_call");
        if (!state) return tl::unexpected(state.error());

        auto result = run_loop_intrinsic(std::move(*state), *continue_func, *step_func, std::nullopt);
        if (!result) {
          const std::string prefix = "native 'loop' threw: ";
          std::string msg = result.error().message;
          if (msg.starts_with(prefix)) { msg.replace(0, prefix.size(), "native 'loop_call' threw: "); }
          return tl::unexpected(RuntimeError{std::move(msg)});
        }
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kLoopNCall: {
        auto max_iters = pop_stack(stack, "loop_n_call");
        if (!max_iters) return tl::unexpected(max_iters.error());
        auto step_func = pop_stack(stack, "loop_n_call");
        if (!step_func) return tl::unexpected(step_func.error());
        auto continue_func = pop_stack(stack, "loop_n_call");
        if (!continue_func) return tl::unexpected(continue_func.error());
        auto state = pop_stack(stack, "loop_n_call");
        if (!state) return tl::unexpected(state.error());

        std::size_t limit = 0;
        try {
          const auto as_int = fleaux::runtime::as_int_value_strict(*max_iters, "LoopN max_iters");
          if (as_int < 0) {
            return tl::unexpected(RuntimeError{"native 'loop_n_call' threw: LoopN: max_iters must be non-negative"});
          }
          limit = static_cast<std::size_t>(as_int);
        } catch (const std::exception& ex) {
          return tl::unexpected(RuntimeError{std::string("native 'loop_n_call' threw: ") + ex.what()});
        }

        auto result = run_loop_intrinsic(std::move(*state), *continue_func, *step_func, limit);
        if (!result) {
          const std::string prefix = "native 'loop' threw: ";
          std::string msg = result.error().message;
          if (msg.starts_with(prefix)) { msg.replace(0, prefix.size(), "native 'loop_n_call' threw: "); }
          return tl::unexpected(RuntimeError{std::move(msg)});
        }
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kHalt:
        return LoopExit{std::monostate{}};
    }
  }

  return tl::unexpected(RuntimeError{"program terminated without halt"});
}

auto try_run_vm_native_builtin(const std::string& name, const Value& arg, std::ostream& output)
    -> tl::expected<std::optional<Value>, RuntimeError> {
  (void)arg;
  (void)output;

  static const std::unordered_map<std::string, double> kConstantDispatchTable =
      []() -> std::unordered_map<std::string, double> {
    std::unordered_map<std::string, double> out;
#define FLEAUX_INSERT_NATIVE_CONST(name_literal, numeric_value) out.emplace(name_literal, numeric_value);
    FLEAUX_VM_CONSTANT_BUILTINS(FLEAUX_INSERT_NATIVE_CONST)
#undef FLEAUX_INSERT_NATIVE_CONST
    return out;
  }();

  if (const auto constant_it = kConstantDispatchTable.find(name); constant_it != kConstantDispatchTable.end()) {
    return std::optional<Value>{fleaux::runtime::make_float(constant_it->second)};
  }

  return std::optional<Value>{std::nullopt};
}

auto dispatch_builtin(const std::string& name, Value arg,
                      const std::unordered_map<std::string, RuntimeCallable>& builtins, std::ostream& output)
    -> tl::expected<Value, RuntimeError> {
  auto native = try_run_vm_native_builtin(name, arg, output);
  if (!native) return tl::unexpected(native.error());
  if (native->has_value()) { return std::move(**native); }

  const auto it = builtins.find(name);
  if (it == builtins.end()) { return tl::unexpected(RuntimeError{"unknown builtin: '" + name + "'"}); }
  try {
    return it->second(std::move(arg));
  } catch (const std::exception& ex) {
    return tl::unexpected(RuntimeError{std::string("builtin '") + name + "' threw: " + ex.what()});
  }
}

}  // namespace

struct RuntimeSession::Impl {
  explicit Impl(const std::vector<std::string>& process_args) {
    std::vector<std::string> args_storage;
    args_storage.reserve(process_args.size() + 1U);
    args_storage.emplace_back("<repl>");
    args_storage.insert(args_storage.end(), process_args.begin(), process_args.end());

    std::vector<char*> argv_ptrs;
    argv_ptrs.reserve(args_storage.size());
    for (auto& arg : args_storage) { argv_ptrs.push_back(arg.data()); }
    fleaux::runtime::set_process_args(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());
  }

  std::vector<frontend::ir::IRLet> lets;
};

RuntimeSession::RuntimeSession(const std::vector<std::string>& process_args) : impl_(std::make_shared<Impl>(process_args)) {}

auto RuntimeSession::run_snippet(const std::string& snippet_text, std::ostream& output) const -> RuntimeResult {
  auto analyzed = detail::parse_and_analyze_repl_text(snippet_text, "<repl>", impl_->lets);
  if (!analyzed) {
    return tl::unexpected(make_runtime_error(analyzed.error().message, analyzed.error().hint, analyzed.error().span));
  }

  auto merged_lets = detail::merge_repl_session_lets(impl_->lets, analyzed->lets);
  auto program_to_execute = analyzed.value();
  program_to_execute.lets = merged_lets;

  constexpr fleaux::bytecode::BytecodeCompiler compiler;
  auto compiled = compiler.compile(program_to_execute, fleaux::bytecode::CompileOptions{
                                                          .module_name = std::string{"repl"},
                                                      });
  if (!compiled) {
    return tl::unexpected(make_runtime_error(
        compiled.error().message,
        "This REPL snippet is not yet supported by the VM compiler."));
  }

  impl_->lets = std::move(merged_lets);

  const Runtime runtime;
  return runtime.execute(*compiled, output);
}

// Runtime::execute

auto Runtime::create_session(const std::vector<std::string>& process_args) const -> RuntimeSession {
  return RuntimeSession(process_args);
}

auto Runtime::execute(const bytecode::Module& bytecode_module) const -> RuntimeResult {
  return execute(bytecode_module, std::cout);
}

auto Runtime::execute(const bytecode::Module& bytecode_module, std::ostream& output) const -> RuntimeResult {
  fleaux::runtime::CallableRegistryScope callable_scope;
  fleaux::runtime::ValueRegistryScope value_scope;
  fleaux::runtime::HandleRegistryScope handle_scope;
  fleaux::runtime::TaskRegistryScope task_scope;
  detail::StdHelpMetadataScope help_scope;
  fleaux::runtime::RuntimeOutputStreamScope output_scope(output);
  const auto& builtins = vm_builtin_callables();

  std::vector<Value> stack;
  std::vector<CallFrame> frames;
  frames.push_back(CallFrame{.instructions = &bytecode_module.instructions, .ip = 0, .locals = {}});

  auto loop_result = run_loop(bytecode_module, stack, frames, builtins, output);
  if (!loop_result) return tl::unexpected(loop_result.error());

  if (std::get_if<Value>(&*loop_result) != nullptr) {
    // A top-level kReturn would be a compiler bug.
    return tl::unexpected(RuntimeError{"top-level code returned a value instead of halting"});
  }
  return ExecutionResult{0};
}

}  // namespace fleaux::vm
