
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

auto constant_builtin_value(const fleaux::vm::BuiltinId builtin_id) -> std::optional<double> {
  for (const auto& spec : fleaux::vm::all_constant_builtin_specs()) {
    if (spec.id == builtin_id) { return spec.value; }
  }
  return std::nullopt;
}

// Forward declarations.
auto run_loop(const bytecode::Module& bytecode_module, std::vector<Value>& stack, std::vector<CallFrame>& frames,
              std::ostream& output) -> LoopResult;

auto dispatch_builtin(fleaux::vm::BuiltinId builtin_id, Value arg) -> tl::expected<Value, RuntimeError>;

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

auto run_user_function(const bytecode::Module& bytecode_module, std::size_t fn_idx, Value arg,
                       std::ostream& output) -> tl::expected<Value, RuntimeError>;

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
                       std::ostream& output) -> tl::expected<Value, RuntimeError> {
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

  auto loop_result = run_loop(bytecode_module, inner_stack, inner_frames, output);
  if (!loop_result) return tl::unexpected(loop_result.error());

  if (std::get_if<std::monostate>(&*loop_result) != nullptr) {
    return tl::unexpected(RuntimeError{"halt inside function '" + name + "'"});
  }
  if (auto* value = std::get_if<Value>(&*loop_result); value != nullptr) { return std::move(*value); }
  return tl::unexpected(RuntimeError{"invalid loop result variant"});
}

// run_loop

auto run_loop(const bytecode::Module& bytecode_module, std::vector<Value>& stack, std::vector<CallFrame>& frames,
              std::ostream& output) -> LoopResult {
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
        const auto builtin_id = fleaux::vm::builtin_id_from_operand(operand);
        if (!builtin_id.has_value()) {
          return tl::unexpected(RuntimeError{"builtin index out of range"});
        }

        auto arg = pop_stack(stack, "call_builtin");
        if (!arg) return tl::unexpected(arg.error());

        auto result = dispatch_builtin(*builtin_id, std::move(*arg));
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
        auto callable = [&bytecode_module, fn_idx, &output](Value call_arg) -> Value {
          auto run_result = run_user_function(bytecode_module, fn_idx, std::move(call_arg), output);
          if (!run_result) throw std::runtime_error(run_result.error().message);
          return std::move(*run_result);
        };
        stack.push_back(fleaux::runtime::make_callable_ref(std::move(callable)));
        break;
      }

      case bytecode::Opcode::kMakeBuiltinFuncRef: {
        const auto builtin_id = fleaux::vm::builtin_id_from_operand(operand);
        if (!builtin_id.has_value()) {
          return tl::unexpected(RuntimeError{"builtin index out of range"});
        }
        auto callable = [builtin_id = *builtin_id](Value arg) -> Value {
          auto result = dispatch_builtin(builtin_id, std::move(arg));
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
                                 declared_has_variadic_tail, captured_values = std::move(captured_values),
                                 &output](Value call_arg) mutable -> Value {
          auto declared_args =
              unpack_declared_call_args(std::move(call_arg), declared_arity, declared_has_variadic_tail);
          if (!declared_args) { throw std::runtime_error(declared_args.error().message); }

          std::vector<Value> full_args;
          full_args.reserve(capture_count + declared_args->size());
          for (const auto& captured : captured_values) { full_args.push_back(captured); }
          for (auto& arg : *declared_args) { full_args.push_back(std::move(arg)); }

          auto result = run_user_function(bytecode_module, function_index, pack_call_args(std::move(full_args)), output);
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

auto dispatch_builtin(const fleaux::vm::BuiltinId builtin_id, Value arg) -> tl::expected<Value, RuntimeError> {
  try {
    switch (builtin_id) {
      case BuiltinId::UnaryPlus: return fleaux::runtime::UnaryPlus(std::move(arg));
      case BuiltinId::UnaryMinus: return fleaux::runtime::UnaryMinus(std::move(arg));
      case BuiltinId::Add: return fleaux::runtime::Add(std::move(arg));
      case BuiltinId::Subtract: return fleaux::runtime::Subtract(std::move(arg));
      case BuiltinId::Multiply: return fleaux::runtime::Multiply(std::move(arg));
      case BuiltinId::Divide: return fleaux::runtime::Divide(std::move(arg));
      case BuiltinId::Mod: return fleaux::runtime::Mod(std::move(arg));
      case BuiltinId::Pow: return fleaux::runtime::Pow(std::move(arg));
      case BuiltinId::BitAnd: return fleaux::runtime::BitAnd(std::move(arg));
      case BuiltinId::BitOr: return fleaux::runtime::BitOr(std::move(arg));
      case BuiltinId::BitXor: return fleaux::runtime::BitXor(std::move(arg));
      case BuiltinId::BitNot: return fleaux::runtime::BitNot(std::move(arg));
      case BuiltinId::BitShiftLeft: return fleaux::runtime::BitShiftLeft(std::move(arg));
      case BuiltinId::BitShiftRight: return fleaux::runtime::BitShiftRight(std::move(arg));
      case BuiltinId::Equal: return fleaux::runtime::Equal(std::move(arg));
      case BuiltinId::NotEqual: return fleaux::runtime::NotEqual(std::move(arg));
      case BuiltinId::LessThan: return fleaux::runtime::LessThan(std::move(arg));
      case BuiltinId::GreaterThan: return fleaux::runtime::GreaterThan(std::move(arg));
      case BuiltinId::GreaterOrEqual: return fleaux::runtime::GreaterOrEqual(std::move(arg));
      case BuiltinId::LessOrEqual: return fleaux::runtime::LessOrEqual(std::move(arg));
      case BuiltinId::Not: return fleaux::runtime::Not(std::move(arg));
      case BuiltinId::And: return fleaux::runtime::And(std::move(arg));
      case BuiltinId::Or: return fleaux::runtime::Or(std::move(arg));
      case BuiltinId::Select: return fleaux::runtime::Select(std::move(arg));
      case BuiltinId::Match: return fleaux::runtime::Match(std::move(arg));
      case BuiltinId::Apply: return fleaux::runtime::Apply(std::move(arg));
      case BuiltinId::Branch: return fleaux::runtime::Branch(std::move(arg));
      case BuiltinId::Loop: return fleaux::runtime::Loop(std::move(arg));
      case BuiltinId::LoopN: return fleaux::runtime::LoopN(std::move(arg));
      case BuiltinId::Printf: return fleaux::runtime::Printf(std::move(arg));
      case BuiltinId::Println: return fleaux::runtime::Println(std::move(arg));
      case BuiltinId::GetArgs: return fleaux::runtime::GetArgs(std::move(arg));
      case BuiltinId::Type: return fleaux::runtime::Type(std::move(arg));
      case BuiltinId::Input: return fleaux::runtime::Input(std::move(arg));
      case BuiltinId::Help: return fleaux::runtime::Help(std::move(arg));
      case BuiltinId::ExitVoid: return fleaux::runtime::Exit_Void(std::move(arg));
      case BuiltinId::ExitInt64: return fleaux::runtime::Exit_Int64(std::move(arg));
      case BuiltinId::Cwd: return fleaux::runtime::Cwd(std::move(arg));
      case BuiltinId::OSEnv: return fleaux::runtime::OSEnv(std::move(arg));
      case BuiltinId::OSHasEnv: return fleaux::runtime::OSHasEnv(std::move(arg));
      case BuiltinId::OSSetEnv: return fleaux::runtime::OSSetEnv(std::move(arg));
      case BuiltinId::OSUnsetEnv: return fleaux::runtime::OSUnsetEnv(std::move(arg));
      case BuiltinId::OSIsWindows: return fleaux::runtime::OSIsWindows(std::move(arg));
      case BuiltinId::OSIsLinux: return fleaux::runtime::OSIsLinux(std::move(arg));
      case BuiltinId::OSIsMacOS: return fleaux::runtime::OSIsMacOS(std::move(arg));
      case BuiltinId::OSHome: return fleaux::runtime::OSHome(std::move(arg));
      case BuiltinId::OSTempDir: return fleaux::runtime::OSTempDir(std::move(arg));
      case BuiltinId::OSExec: return fleaux::runtime::OSExec(std::move(arg));
      case BuiltinId::OSMakeTempFile: return fleaux::runtime::OSMakeTempFile(std::move(arg));
      case BuiltinId::OSMakeTempDir: return fleaux::runtime::OSMakeTempDir(std::move(arg));
      case BuiltinId::PathJoin: return fleaux::runtime::PathJoin(std::move(arg));
      case BuiltinId::PathNormalize: return fleaux::runtime::PathNormalize(std::move(arg));
      case BuiltinId::PathBasename: return fleaux::runtime::PathBasename(std::move(arg));
      case BuiltinId::PathDirname: return fleaux::runtime::PathDirname(std::move(arg));
      case BuiltinId::PathExists: return fleaux::runtime::PathExists(std::move(arg));
      case BuiltinId::PathIsFile: return fleaux::runtime::PathIsFile(std::move(arg));
      case BuiltinId::PathIsDir: return fleaux::runtime::PathIsDir(std::move(arg));
      case BuiltinId::PathAbsolute: return fleaux::runtime::PathAbsolute(std::move(arg));
      case BuiltinId::PathExtension: return fleaux::runtime::PathExtension(std::move(arg));
      case BuiltinId::PathStem: return fleaux::runtime::PathStem(std::move(arg));
      case BuiltinId::PathWithExtension: return fleaux::runtime::PathWithExtension(std::move(arg));
      case BuiltinId::PathWithBasename: return fleaux::runtime::PathWithBasename(std::move(arg));
      case BuiltinId::FileReadText: return fleaux::runtime::FileReadText(std::move(arg));
      case BuiltinId::FileWriteText: return fleaux::runtime::FileWriteText(std::move(arg));
      case BuiltinId::FileAppendText: return fleaux::runtime::FileAppendText(std::move(arg));
      case BuiltinId::FileReadLines: return fleaux::runtime::FileReadLines(std::move(arg));
      case BuiltinId::FileDelete: return fleaux::runtime::FileDelete(std::move(arg));
      case BuiltinId::FileSize: return fleaux::runtime::FileSize(std::move(arg));
      case BuiltinId::FileOpen: return fleaux::runtime::FileOpen(std::move(arg));
      case BuiltinId::FileReadLine: return fleaux::runtime::FileReadLine(std::move(arg));
      case BuiltinId::FileReadChunk: return fleaux::runtime::FileReadChunk(std::move(arg));
      case BuiltinId::FileWriteChunk: return fleaux::runtime::FileWriteChunk(std::move(arg));
      case BuiltinId::FileFlush: return fleaux::runtime::FileFlush(std::move(arg));
      case BuiltinId::FileClose: return fleaux::runtime::FileClose(std::move(arg));
      case BuiltinId::FileWithOpen: return fleaux::runtime::FileWithOpen(std::move(arg));
      case BuiltinId::DirCreate: return fleaux::runtime::DirCreate(std::move(arg));
      case BuiltinId::DirDelete: return fleaux::runtime::DirDelete(std::move(arg));
      case BuiltinId::DirList: return fleaux::runtime::DirList(std::move(arg));
      case BuiltinId::DirListFull: return fleaux::runtime::DirListFull(std::move(arg));
      case BuiltinId::TupleAppend: return fleaux::runtime::TupleAppend(std::move(arg));
      case BuiltinId::TuplePrepend: return fleaux::runtime::TuplePrepend(std::move(arg));
      case BuiltinId::TupleReverse: return fleaux::runtime::TupleReverse(std::move(arg));
      case BuiltinId::TupleContains: return fleaux::runtime::TupleContains(std::move(arg));
      case BuiltinId::TupleZip: return fleaux::runtime::TupleZip(std::move(arg));
      case BuiltinId::TupleMap: return fleaux::runtime::TupleMap(std::move(arg));
      case BuiltinId::TupleFilter: return fleaux::runtime::TupleFilter(std::move(arg));
      case BuiltinId::TupleSort: return fleaux::runtime::TupleSort(std::move(arg));
      case BuiltinId::TupleUnique: return fleaux::runtime::TupleUnique(std::move(arg));
      case BuiltinId::TupleMin: return fleaux::runtime::TupleMin(std::move(arg));
      case BuiltinId::TupleMax: return fleaux::runtime::TupleMax(std::move(arg));
      case BuiltinId::TupleReduce: return fleaux::runtime::TupleReduce(std::move(arg));
      case BuiltinId::TupleFindIndex: return fleaux::runtime::TupleFindIndex(std::move(arg));
      case BuiltinId::TupleAny: return fleaux::runtime::TupleAny(std::move(arg));
      case BuiltinId::TupleAll: return fleaux::runtime::TupleAll(std::move(arg));
      case BuiltinId::TupleRange: return fleaux::runtime::TupleRange(std::move(arg));
      case BuiltinId::ArrayGetAt: return fleaux::runtime::ArrayGetAt(std::move(arg));
      case BuiltinId::ArraySetAt: return fleaux::runtime::ArraySetAt(std::move(arg));
      case BuiltinId::ArrayInsertAt: return fleaux::runtime::ArrayInsertAt(std::move(arg));
      case BuiltinId::ArrayRemoveAt: return fleaux::runtime::ArrayRemoveAt(std::move(arg));
      case BuiltinId::ArraySlice: return fleaux::runtime::ArraySlice(std::move(arg));
      case BuiltinId::ArrayConcat: return fleaux::runtime::ArrayConcat(std::move(arg));
      case BuiltinId::ArraySetAt2D: return fleaux::runtime::ArraySetAt2D(std::move(arg));
      case BuiltinId::ArrayFill: return fleaux::runtime::ArrayFill(std::move(arg));
      case BuiltinId::ArrayTranspose2D: return fleaux::runtime::ArrayTranspose2D(std::move(arg));
      case BuiltinId::ArraySlice2D: return fleaux::runtime::ArraySlice2D(std::move(arg));
      case BuiltinId::ArrayReshape: return fleaux::runtime::ArrayReshape(std::move(arg));
      case BuiltinId::ArrayRank: return fleaux::runtime::ArrayRank(std::move(arg));
      case BuiltinId::ArrayShape: return fleaux::runtime::ArrayShape(std::move(arg));
      case BuiltinId::ArrayFlatten: return fleaux::runtime::ArrayFlatten(std::move(arg));
      case BuiltinId::ArrayGetAtND: return fleaux::runtime::ArrayGetAtND(std::move(arg));
      case BuiltinId::ArraySetAtND: return fleaux::runtime::ArraySetAtND(std::move(arg));
      case BuiltinId::ArrayReshapeND: return fleaux::runtime::ArrayReshapeND(std::move(arg));
      case BuiltinId::DictCreateVoid: return fleaux::runtime::DictCreate_Void(std::move(arg));
      case BuiltinId::DictCreateDict: return fleaux::runtime::DictCreate_Dict(std::move(arg));
      case BuiltinId::DictSet: return fleaux::runtime::DictSet(std::move(arg));
      case BuiltinId::DictGet: return fleaux::runtime::DictGet(std::move(arg));
      case BuiltinId::DictGetDefault: return fleaux::runtime::DictGetDefault(std::move(arg));
      case BuiltinId::DictContains: return fleaux::runtime::DictContains(std::move(arg));
      case BuiltinId::DictDelete: return fleaux::runtime::DictDelete(std::move(arg));
      case BuiltinId::DictMerge: return fleaux::runtime::DictMerge(std::move(arg));
      case BuiltinId::DictKeys: return fleaux::runtime::DictKeys(std::move(arg));
      case BuiltinId::DictValues: return fleaux::runtime::DictValues(std::move(arg));
      case BuiltinId::DictEntries: return fleaux::runtime::DictEntries(std::move(arg));
      case BuiltinId::DictClear: return fleaux::runtime::DictClear(std::move(arg));
      case BuiltinId::DictLength: return fleaux::runtime::DictLength(std::move(arg));
      case BuiltinId::ToInt64: return fleaux::runtime::ToInt64(std::move(arg));
      case BuiltinId::ToUInt64: return fleaux::runtime::ToUInt64(std::move(arg));
      case BuiltinId::ToFloat64: return fleaux::runtime::ToFloat64(std::move(arg));
      case BuiltinId::MathFloor: return fleaux::runtime::MathFloor(std::move(arg));
      case BuiltinId::MathCeil: return fleaux::runtime::MathCeil(std::move(arg));
      case BuiltinId::MathAbs: return fleaux::runtime::MathAbs(std::move(arg));
      case BuiltinId::MathLog: return fleaux::runtime::MathLog(std::move(arg));
      case BuiltinId::MathClamp: return fleaux::runtime::MathClamp(std::move(arg));
      case BuiltinId::Sqrt: return fleaux::runtime::Sqrt(std::move(arg));
      case BuiltinId::Sin: return fleaux::runtime::Sin(std::move(arg));
      case BuiltinId::Cos: return fleaux::runtime::Cos(std::move(arg));
      case BuiltinId::Tan: return fleaux::runtime::Tan(std::move(arg));
      case BuiltinId::ResultOk: return fleaux::runtime::ResultOk(std::move(arg));
      case BuiltinId::ResultErr: return fleaux::runtime::ResultErr(std::move(arg));
      case BuiltinId::ResultTag: return fleaux::runtime::ResultTag(std::move(arg));
      case BuiltinId::ResultPayload: return fleaux::runtime::ResultPayload(std::move(arg));
      case BuiltinId::ResultIsOk: return fleaux::runtime::ResultIsOk(std::move(arg));
      case BuiltinId::ResultIsErr: return fleaux::runtime::ResultIsErr(std::move(arg));
      case BuiltinId::ResultUnwrap: return fleaux::runtime::ResultUnwrap(std::move(arg));
      case BuiltinId::ResultUnwrapErr: return fleaux::runtime::ResultUnwrapErr(std::move(arg));
      case BuiltinId::Try: return fleaux::runtime::Try(std::move(arg));
      case BuiltinId::ParallelMap: return fleaux::runtime::ParallelMap(std::move(arg));
      case BuiltinId::ParallelWithOptions: return fleaux::runtime::ParallelWithOptions(std::move(arg));
      case BuiltinId::ParallelForEach: return fleaux::runtime::ParallelForEach(std::move(arg));
      case BuiltinId::ParallelReduce: return fleaux::runtime::ParallelReduce(std::move(arg));
      case BuiltinId::TaskSpawn: return fleaux::runtime::TaskSpawn(std::move(arg));
      case BuiltinId::TaskAwait: return fleaux::runtime::TaskAwait(std::move(arg));
      case BuiltinId::TaskAwaitAll: return fleaux::runtime::TaskAwaitAll(std::move(arg));
      case BuiltinId::TaskCancel: return fleaux::runtime::TaskCancel(std::move(arg));
      case BuiltinId::TaskWithTimeout: return fleaux::runtime::TaskWithTimeout(std::move(arg));
      case BuiltinId::Wrap: return fleaux::runtime::Wrap(std::move(arg));
      case BuiltinId::Unwrap: return fleaux::runtime::Unwrap(std::move(arg));
      case BuiltinId::ElementAt: return fleaux::runtime::ElementAt(std::move(arg));
      case BuiltinId::Length: return fleaux::runtime::Length(std::move(arg));
      case BuiltinId::Take: return fleaux::runtime::Take(std::move(arg));
      case BuiltinId::Drop: return fleaux::runtime::Drop(std::move(arg));
      case BuiltinId::Slice: return fleaux::runtime::Slice(std::move(arg));
      case BuiltinId::ToString: return fleaux::runtime::ToString(std::move(arg));
      case BuiltinId::ToNum: return fleaux::runtime::ToNum(std::move(arg));
      case BuiltinId::StringUpper: return fleaux::runtime::StringUpper(std::move(arg));
      case BuiltinId::StringLower: return fleaux::runtime::StringLower(std::move(arg));
      case BuiltinId::StringTrim: return fleaux::runtime::StringTrim(std::move(arg));
      case BuiltinId::StringTrimStart: return fleaux::runtime::StringTrimStart(std::move(arg));
      case BuiltinId::StringTrimEnd: return fleaux::runtime::StringTrimEnd(std::move(arg));
      case BuiltinId::StringSplit: return fleaux::runtime::StringSplit(std::move(arg));
      case BuiltinId::StringJoin: return fleaux::runtime::StringJoin(std::move(arg));
      case BuiltinId::StringReplace: return fleaux::runtime::StringReplace(std::move(arg));
      case BuiltinId::StringContains: return fleaux::runtime::StringContains(std::move(arg));
      case BuiltinId::StringStartsWith: return fleaux::runtime::StringStartsWith(std::move(arg));
      case BuiltinId::StringEndsWith: return fleaux::runtime::StringEndsWith(std::move(arg));
      case BuiltinId::StringLength: return fleaux::runtime::StringLength(std::move(arg));
      case BuiltinId::StringCharAt: return fleaux::runtime::StringCharAt(std::move(arg));
      case BuiltinId::StringSlice: return fleaux::runtime::StringSlice(std::move(arg));
      case BuiltinId::StringFind: return fleaux::runtime::StringFind(std::move(arg));
      case BuiltinId::StringFormat: return fleaux::runtime::StringFormat(std::move(arg));
      case BuiltinId::StringRegexIsMatch: return fleaux::runtime::StringRegexIsMatch(std::move(arg));
      case BuiltinId::StringRegexFind: return fleaux::runtime::StringRegexFind(std::move(arg));
      case BuiltinId::StringRegexReplace: return fleaux::runtime::StringRegexReplace(std::move(arg));
      case BuiltinId::StringRegexSplit: return fleaux::runtime::StringRegexSplit(std::move(arg));
      default:
        if (const auto constant_value = constant_builtin_value(builtin_id); constant_value.has_value()) {
          return fleaux::runtime::make_float(*constant_value);
        }
        return tl::unexpected(RuntimeError{"unknown builtin id"});
    }
  } catch (const std::exception& ex) {
    return tl::unexpected(RuntimeError{std::string("builtin '") + std::string(fleaux::vm::builtin_name(builtin_id)) +
                                       "' threw: " + ex.what()});
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

  constexpr Runtime runtime;
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

  std::vector<Value> stack;
  std::vector<CallFrame> frames;
  frames.push_back(CallFrame{.instructions = &bytecode_module.instructions, .ip = 0, .locals = {}});

  auto loop_result = run_loop(bytecode_module, stack, frames, output);
  if (!loop_result) return tl::unexpected(loop_result.error());

  if (std::get_if<Value>(&*loop_result) != nullptr) {
    // A top-level kReturn would be a compiler bug.
    return tl::unexpected(RuntimeError{"top-level code returned a value instead of halting"});
  }
  return ExecutionResult{0};
}

}  // namespace fleaux::vm
