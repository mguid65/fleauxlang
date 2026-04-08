#include "fleaux/vm/runtime.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <variant>
#include <vector>

#include "builtin_map.hpp"
#include "fleaux/vm/builtin_catalog.hpp"
#include "fleaux_runtime.hpp"

namespace fleaux::vm {
namespace {

using fleaux::runtime::Array;
using fleaux::runtime::RuntimeCallable;
using fleaux::runtime::Value;

// ── Value helpers ─────────────────────────────────────────────────────────────

Value const_to_value(const bytecode::ConstValue& c) {
  using namespace fleaux::runtime;
  return std::visit(
      []<typename ValueType>(const ValueType& v) -> Value {
        using T = std::decay_t<ValueType>;
        if constexpr (std::is_same_v<ValueType, std::int64_t>) return make_int(v);
        if constexpr (std::is_same_v<ValueType, double>) return make_float(v);
        if constexpr (std::is_same_v<ValueType, bool>) return make_bool(v);
        if constexpr (std::is_same_v<ValueType, std::string>) return make_string(v);
        return make_null();
      },
      c.data);
}

// ── Stack helpers ─────────────────────────────────────────────────────────────

tl::expected<Value, RuntimeError> pop_stack(std::vector<Value>& stack, const char* context) {
  if (stack.empty()) { return tl::unexpected(RuntimeError{std::string("stack underflow on ") + context}); }
  Value v = std::move(stack.back());
  stack.pop_back();
  return v;
}

template <typename Fn>
tl::expected<Value, RuntimeError> run_native_op(const char* opname, Fn&& fn) {
  try {
    return fn();
  } catch (const std::exception& ex) {
    return tl::unexpected(RuntimeError{std::string("native '") + opname + "' threw: " + ex.what()});
  }
}

// ── Call frame ────────────────────────────────────────────────────────────────

struct CallFrame {
  const std::vector<bytecode::Instruction>* instructions = nullptr;
  std::size_t ip = 0;
  std::vector<Value> locals;
};

// ── Loop exit type ────────────────────────────────────────────────────────────
// std::monostate → kHalt was hit (top-level completion).
// Value          → kReturn emptied the frame stack (standalone function result).

using LoopExit = std::variant<std::monostate, Value>;
using LoopResult = tl::expected<LoopExit, RuntimeError>;

// Forward declarations.
LoopResult run_loop(const bytecode::Module& bytecode_module, std::vector<Value>& stack, std::vector<CallFrame>& frames,
                    const std::unordered_map<std::string, RuntimeCallable>& builtins, bool allow_runtime_fallback,
                    std::ostream& output);

tl::expected<Value, RuntimeError> dispatch_builtin(const std::string& name, Value arg,
                                                   const std::unordered_map<std::string, RuntimeCallable>& builtins,
                                                   bool allow_runtime_fallback);

// Try VM-native builtin execution; returns nullopt when builtin is not ported yet.
tl::expected<std::optional<Value>, RuntimeError> try_run_vm_native_builtin(const std::string& name, const Value& arg);

tl::expected<Value, RuntimeError> run_user_function(const bytecode::Module& bytecode_module, std::size_t fn_idx,
                                                    Value arg,
                                                    const std::unordered_map<std::string, RuntimeCallable>& builtins,
                                                    bool allow_runtime_fallback, std::ostream& output);

tl::expected<Value, RuntimeError> run_loop_intrinsic(Value state, const Value& continue_func, const Value& step_func,
                                                     const std::optional<std::size_t> max_iters) {
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

// ── run_user_function ─────────────────────────────────────────────────────────

tl::expected<Value, RuntimeError> run_user_function(const bytecode::Module& bytecode_module, const std::size_t fn_idx,
                                                    Value arg,
                                                    const std::unordered_map<std::string, RuntimeCallable>& builtins,
                                                    const bool allow_runtime_fallback, std::ostream& output) {
  if (fn_idx >= bytecode_module.functions.size()) {
    return tl::unexpected(RuntimeError{"function index out of range"});
  }
  const auto& [name, arity, instructions] = bytecode_module.functions[fn_idx];

  std::vector<Value> inner_stack;
  std::vector<CallFrame> inner_frames;

  CallFrame frame;
  frame.instructions = &instructions;
  frame.ip = 0;

  if (arity == 0) {
    // no locals
  } else if (arity == 1) {
    frame.locals.push_back(fleaux::runtime::unwrap_singleton_arg(std::move(arg)));
  } else {
    try {
      const auto& arr = fleaux::runtime::as_array(arg);
      frame.locals.reserve(arity);
      for (std::uint32_t i = 0; i < arity; ++i) {
        auto elem = arr.TryGet(i);
        if (!elem) { return tl::unexpected(RuntimeError{"too few arguments for '" + name + "'"}); }
        frame.locals.push_back(*elem);
      }
    } catch (const std::exception& ex) {
      return tl::unexpected(RuntimeError{std::string("argument unpacking for '") + name + "': " + ex.what()});
    }
  }

  inner_frames.push_back(std::move(frame));

  auto loop_result = run_loop(bytecode_module, inner_stack, inner_frames, builtins, allow_runtime_fallback, output);
  if (!loop_result) return tl::unexpected(loop_result.error());

  if (std::get_if<std::monostate>(&*loop_result) != nullptr) {
    return tl::unexpected(RuntimeError{"halt inside function '" + name + "'"});
  }
  if (auto* value = std::get_if<Value>(&*loop_result); value != nullptr) { return std::move(*value); }
  return tl::unexpected(RuntimeError{"invalid loop result variant"});
}

// ── run_loop ──────────────────────────────────────────────────────────────────

LoopResult run_loop(const bytecode::Module& bytecode_module, std::vector<Value>& stack, std::vector<CallFrame>& frames,
                    const std::unordered_map<std::string, RuntimeCallable>& builtins, const bool allow_runtime_fallback,
                    std::ostream& output) {
  while (!frames.empty()) {
    const std::size_t curr_ip = frames.back().ip;
    frames.back().ip++;

    const auto& instr_list = *frames.back().instructions;
    if (curr_ip >= instr_list.size()) { return tl::unexpected(RuntimeError{"program terminated without halt"}); }

    const auto opcode = instr_list[curr_ip].opcode;
    const auto operand = instr_list[curr_ip].operand;

    switch (opcode) {
      // ── No-op ───────────────────────────────────────────────────────────
      case bytecode::Opcode::kNoOp:
        break;

      // ── Push ────────────────────────────────────────────────────────────
      case bytecode::Opcode::kPushConst: {
        const auto idx = static_cast<std::size_t>(operand);
        if (idx >= bytecode_module.constants.size()) {
          return tl::unexpected(RuntimeError{"constant pool index out of range"});
        }
        stack.push_back(const_to_value(bytecode_module.constants[idx]));
        break;
      }

      // ── Stack manipulation ────────────────────────────────────────────────
      case bytecode::Opcode::kPop: {
        if (auto v = pop_stack(stack, "pop"); !v) return tl::unexpected(v.error());
        break;
      }

      case bytecode::Opcode::kDup: {
        if (stack.empty()) { return tl::unexpected(RuntimeError{"stack underflow on dup"}); }
        stack.push_back(stack.back());
        break;
      }

      // ── Tuple construction ────────────────────────────────────────────────
      case bytecode::Opcode::kBuildTuple: {
        const auto n = static_cast<std::size_t>(operand);
        if (stack.size() < n) { return tl::unexpected(RuntimeError{"stack underflow on build_tuple"}); }
        Array arr;
        arr.Reserve(n);
        const auto base = stack.size() - n;
        for (std::size_t i = 0; i < n; ++i) { arr.PushBack(stack[base + i]); }
        stack.resize(base);
        stack.emplace_back(std::move(arr));
        break;
      }

      // ── Builtin call ──────────────────────────────────────────────────────
      case bytecode::Opcode::kCallBuiltin: {
        const auto idx = static_cast<std::size_t>(operand);
        if (idx >= bytecode_module.builtin_names.size()) {
          return tl::unexpected(RuntimeError{"builtin index out of range"});
        }
        const auto& name = bytecode_module.builtin_names[idx];

        auto arg = pop_stack(stack, "call_builtin");
        if (!arg) return tl::unexpected(arg.error());

        auto result = dispatch_builtin(name, std::move(*arg), builtins, allow_runtime_fallback);
        if (!result) return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      // ── User function call (frame-based) ──────────────────────────────────
      case bytecode::Opcode::kCallUserFunc: {
        const auto fn_idx = static_cast<std::size_t>(operand);
        if (fn_idx >= bytecode_module.functions.size()) {
          return tl::unexpected(RuntimeError{"function index out of range"});
        }
        auto arg = pop_stack(stack, "call_user_func");
        if (!arg) return tl::unexpected(arg.error());

        const auto& [name, arity, instructions] = bytecode_module.functions[fn_idx];
        CallFrame new_frame;
        new_frame.instructions = &instructions;
        new_frame.ip = 0;

        if (arity == 0) {
          // no locals
        } else if (arity == 1) {
          new_frame.locals.push_back(fleaux::runtime::unwrap_singleton_arg(std::move(*arg)));
        } else {
          try {
            const auto& arr = fleaux::runtime::as_array(*arg);
            new_frame.locals.reserve(arity);
            for (std::uint32_t i = 0; i < arity; ++i) {
              auto elem = arr.TryGet(i);
              if (!elem) { return tl::unexpected(RuntimeError{"too few arguments for '" + name + "'"}); }
              new_frame.locals.push_back(*elem);
            }
          } catch (const std::exception& ex) {
            return tl::unexpected(RuntimeError{std::string("argument unpacking for '") + name + "': " + ex.what()});
          }
        }

        frames.push_back(std::move(new_frame));
        break;
      }

      // ── User function → callable-ref (for higher-order use) ───────────────
      case bytecode::Opcode::kMakeUserFuncRef: {
        const auto fn_idx = static_cast<std::size_t>(operand);
        if (fn_idx >= bytecode_module.functions.size()) {
          return tl::unexpected(RuntimeError{"function index out of range"});
        }
        // Create a RuntimeCallable that re-enters the VM for this function.
        // Captures by reference — valid because callables are always invoked
        // synchronously (inside kCallBuiltin) within the same run_loop call.
        auto callable = [&bytecode_module, fn_idx, &builtins, allow_runtime_fallback,
                         &output](Value call_arg) -> Value {
          auto r =
              run_user_function(bytecode_module, fn_idx, std::move(call_arg), builtins, allow_runtime_fallback, output);
          if (!r) throw std::runtime_error(r.error().message);
          return std::move(*r);
        };
        stack.push_back(fleaux::runtime::make_callable_ref(std::move(callable)));
        break;
      }

      // ── Builtin → callable-ref (for higher-order use) ─────────────────────
      case bytecode::Opcode::kMakeBuiltinFuncRef: {
        const auto idx = static_cast<std::size_t>(operand);
        if (idx >= bytecode_module.builtin_names.size()) {
          return tl::unexpected(RuntimeError{"builtin index out of range"});
        }
        const auto& name = bytecode_module.builtin_names[idx];
        auto callable = [name, &builtins, allow_runtime_fallback](Value arg) -> Value {
          auto result = dispatch_builtin(name, std::move(arg), builtins, allow_runtime_fallback);
          if (!result) { throw std::runtime_error(result.error().message); }
          return std::move(*result);
        };
        stack.push_back(fleaux::runtime::make_callable_ref(std::move(callable)));
        break;
      }

      // ── Return from current frame ─────────────────────────────────────────
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

      // ── Local load ────────────────────────────────────────────────────────
      case bytecode::Opcode::kLoadLocal: {
        const auto slot = static_cast<std::size_t>(operand);
        const auto& locals = frames.back().locals;
        if (slot >= locals.size()) { return tl::unexpected(RuntimeError{"local slot index out of range"}); }
        stack.push_back(locals[slot]);
        break;
      }

      // ── Legacy print (uses injected output stream) ─────────────────────────
      case bytecode::Opcode::kPrint: {
        auto val = pop_stack(stack, "print");
        if (!val) return tl::unexpected(val.error());
        fleaux::runtime::print_value_varargs(output, *val);
        output << '\n';
        break;
      }

      // ── Unconditional jump ────────────────────────────────────────────────
      case bytecode::Opcode::kJump: {
        const auto target = static_cast<std::size_t>(operand);
        if (target > instr_list.size()) { return tl::unexpected(RuntimeError{"jump target out of range"}); }
        frames.back().ip = target;
        break;
      }

      // ── Conditional jump (jump if TOS is truthy) ──────────────────────────
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
          return fleaux::runtime::num_result(fleaux::runtime::to_double(*lhs) + fleaux::runtime::to_double(*rhs));
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
          return fleaux::runtime::num_result(fleaux::runtime::to_double(*lhs) - fleaux::runtime::to_double(*rhs));
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
          return fleaux::runtime::num_result(fleaux::runtime::to_double(*lhs) * fleaux::runtime::to_double(*rhs));
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
          return fleaux::runtime::num_result(fleaux::runtime::to_double(*lhs) / fleaux::runtime::to_double(*rhs));
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
          return fleaux::runtime::num_result(
              std::fmod(fleaux::runtime::to_double(*lhs), fleaux::runtime::to_double(*rhs)));
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
          return fleaux::runtime::make_bool(fleaux::runtime::to_double(*lhs) < fleaux::runtime::to_double(*rhs));
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
          return fleaux::runtime::make_bool(fleaux::runtime::to_double(*lhs) > fleaux::runtime::to_double(*rhs));
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
          return fleaux::runtime::make_bool(fleaux::runtime::to_double(*lhs) <= fleaux::runtime::to_double(*rhs));
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
          return fleaux::runtime::make_bool(fleaux::runtime::to_double(*lhs) >= fleaux::runtime::to_double(*rhs));
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
          const auto as_int = fleaux::runtime::as_int_value(*max_iters);
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

      // ── Halt ──────────────────────────────────────────────────────────────
      case bytecode::Opcode::kHalt:
        return LoopExit{std::monostate{}};
    }
  }

  return tl::unexpected(RuntimeError{"program terminated without halt"});
}

tl::expected<std::optional<Value>, RuntimeError> try_run_vm_native_builtin(const std::string& name, const Value& arg) {
  auto native_error = [&](const std::string& builtin_name,
                          const std::exception& ex) -> tl::expected<std::optional<Value>, RuntimeError> {
    return tl::unexpected(RuntimeError{std::string("native builtin '") + builtin_name + "' threw: " + ex.what()});
  };

  auto expect_n = [&](const char* builtin_name, const std::size_t n) -> tl::expected<const Array*, RuntimeError> {
    try {
      const auto& args = fleaux::runtime::as_array(arg);
      if (args.Size() != n) {
        return tl::unexpected(RuntimeError{std::string("native builtin '") + builtin_name + "' expects exactly " +
                                           std::to_string(n) + " arguments"});
      }
      return &args;
    } catch (const std::exception& ex) {
      return tl::unexpected(
          RuntimeError{std::string("native builtin '") + builtin_name + "' argument handling failed: " + ex.what()});
    }
  };

  auto expect_unary = [&](const char* builtin_name) -> tl::expected<const Value*, RuntimeError> {
    if (!arg.HasArray()) return &arg;
    if (const auto& args = fleaux::runtime::as_array(arg); args.Size() == 1) {
      const auto v0 = args.TryGet(0);
      if (!v0) {
        return tl::unexpected(
            RuntimeError{std::string("native builtin '") + builtin_name + "' argument unpack failed"});
      }
      return &*v0;
    }
    return &arg;
  };

  auto expect_binary =
      [&](const char* builtin_name) -> tl::expected<std::pair<const Value*, const Value*>, RuntimeError> {
    auto args = expect_n(builtin_name, 2);
    if (!args) return tl::unexpected(args.error());
    const auto lhs = (*args)->TryGet(0);
    const auto rhs = (*args)->TryGet(1);
    if (!lhs || !rhs) {
      return tl::unexpected(RuntimeError{std::string("native builtin '") + builtin_name + "' argument unpack failed"});
    }
    return std::make_pair(&*lhs, &*rhs);
  };

  auto numeric_bin = [&](const char* builtin_name, const auto& fn) -> tl::expected<std::optional<Value>, RuntimeError> {
    auto args = expect_binary(builtin_name);
    if (!args) return tl::unexpected(args.error());
    try {
      const double lhs = fleaux::runtime::to_double(*args->first);
      const double rhs = fleaux::runtime::to_double(*args->second);
      return std::optional<Value>{fleaux::runtime::num_result(fn(lhs, rhs))};
    } catch (const std::exception& ex) {
      return tl::unexpected(RuntimeError{std::string("native builtin '") + builtin_name + "' threw: " + ex.what()});
    }
  };

  auto numeric_cmp = [&](const char* builtin_name, const auto& fn) -> tl::expected<std::optional<Value>, RuntimeError> {
    auto args = expect_binary(builtin_name);
    if (!args) return tl::unexpected(args.error());
    try {
      const double lhs = fleaux::runtime::to_double(*args->first);
      const double rhs = fleaux::runtime::to_double(*args->second);
      return std::optional<Value>{fleaux::runtime::make_bool(fn(lhs, rhs))};
    } catch (const std::exception& ex) {
      return tl::unexpected(RuntimeError{std::string("native builtin '") + builtin_name + "' threw: " + ex.what()});
    }
  };

  auto numeric_unary = [&](const std::string& builtin_name,
                           const auto& fn) -> tl::expected<std::optional<Value>, RuntimeError> {
    auto v = expect_unary(builtin_name.c_str());
    if (!v) return tl::unexpected(v.error());
    try {
      return std::optional<Value>{fleaux::runtime::num_result(fn(fleaux::runtime::to_double(*v.value())))};
    } catch (const std::exception& ex) { return native_error(builtin_name, ex); }
  };

  auto trim_left_copy = [](std::string s) -> std::string {
    const auto it = std::ranges::find_if(s, [](const unsigned char ch) -> bool { return !std::isspace(ch); });
    s.erase(s.begin(), it);
    return s;
  };

  auto trim_right_copy = [](std::string s) -> std::string {
    const auto rit =
        std::ranges::find_if(std::views::reverse(s), [](const unsigned char ch) { return !std::isspace(ch); });
    s.erase(rit.base(), s.end());
    return s;
  };

  enum class BuiltinDispatchKey {
    kStd_Add,
    kStd_Subtract,
    kStd_Multiply,
    kStd_Divide,
    kStd_Mod,
    kStd_Pow,
    kStd_GreaterThan,
    kStd_LessThan,
    kStd_GreaterOrEqual,
    kStd_LessOrEqual,
    kStd_Equal,
    kStd_NotEqual,
    kStd_Not,
    kStd_And,
    kStd_Or,
    kStd_Select,
    kStd_UnaryMinus,
    kStd_UnaryPlus,
    kStd_Math_Floor,
    kStd_Math_Ceil,
    kStd_Math_Abs,
    kStd_Math_Log,
    kStd_Math_Clamp,
    kStd_Apply,
    kStd_Loop,
    kStd_LoopN,
    kStd_Wrap,
    kStd_Unwrap,
    kStd_ElementAt,
    kStd_Length,
    kStd_Take,
    kStd_Drop,
    kStd_Slice,
    kStd_ToNum,
    kStd_ToString,
    kStd_String_Upper,
    kStd_String_Lower,
    kStd_String_Trim,
    kStd_String_TrimStart,
    kStd_String_TrimEnd,
    kStd_String_Split,
    kStd_String_Join,
    kStd_String_Replace,
    kStd_String_Contains,
    kStd_String_StartsWith,
    kStd_String_EndsWith,
    kStd_String_Length,
    kStd_OS_Cwd,
    kStd_OS_Home,
    kStd_OS_TempDir,
    kStd_OS_Env,
    kStd_OS_HasEnv,
    kStd_OS_IsWindows,
    kStd_OS_IsLinux,
    kStd_OS_IsMacOS,
    kStd_OS_SetEnv,
    kStd_OS_UnsetEnv,
    kStd_OS_MakeTempFile,
    kStd_OS_MakeTempDir,
    kStd_Path_Join,
    kStd_Path_Normalize,
    kStd_Path_Basename,
    kStd_Path_Dirname,
    kStd_Path_Exists,
    kStd_Path_IsFile,
    kStd_Path_IsDir,
    kStd_Path_Absolute,
    kStd_Path_Extension,
    kStd_Path_Stem,
    kStd_Path_WithExtension,
    kStd_Path_WithBasename,
    kStd_File_ReadText,
    kStd_File_WriteText,
    kStd_File_AppendText,
    kStd_File_ReadLines,
    kStd_File_Delete,
    kStd_File_Size,
    kStd_File_Open,
    kStd_File_ReadLine,
    kStd_File_ReadChunk,
    kStd_File_WriteChunk,
    kStd_File_Flush,
    kStd_File_Close,
    kStd_File_WithOpen,
    kStd_Dir_Create,
    kStd_Dir_Delete,
    kStd_Dir_List,
    kStd_Dir_ListFull,
    kStd_Tuple_Append,
    kStd_Tuple_Prepend,
    kStd_Tuple_Reverse,
    kStd_Tuple_Contains,
    kStd_Tuple_Zip,
    kStd_Dict_Create,
    kStd_Dict_Set,
    kStd_Dict_Get,
    kStd_Dict_GetDefault,
    kStd_Dict_Keys,
    kStd_Dict_Values,
    kStd_Dict_Length,
    kStd_Println,
    kStd_Printf,
    kStd_GetArgs,
    kStd_Input,
    kStd_Exit,
    kStd_Tuple_Map,
    kStd_Tuple_Filter,
    kStd_Tuple_Sort,
    kStd_Tuple_Unique,
    kStd_Tuple_Min,
    kStd_Tuple_Max,
    kStd_Tuple_Reduce,
    kStd_Tuple_FindIndex,
    kStd_Tuple_Any,
    kStd_Tuple_All,
    kStd_Tuple_Range,
    kStd_Dict_Contains,
    kStd_Dict_Delete,
    kStd_Dict_Entries,
    kStd_Dict_Clear,
    kStd_Branch,
    kStd_Sqrt,
    kStd_Math_Sqrt,
    kStd_Sin,
    kStd_Math_Sin,
    kStd_Cos,
    kStd_Math_Cos,
    kStd_Tan,
    kStd_Math_Tan,
  };

  static const std::unordered_map<std::string, BuiltinDispatchKey> kBuiltinDispatchTable = {
      {"Std.Add", BuiltinDispatchKey::kStd_Add},
      {"Std.Subtract", BuiltinDispatchKey::kStd_Subtract},
      {"Std.Multiply", BuiltinDispatchKey::kStd_Multiply},
      {"Std.Divide", BuiltinDispatchKey::kStd_Divide},
      {"Std.Mod", BuiltinDispatchKey::kStd_Mod},
      {"Std.Pow", BuiltinDispatchKey::kStd_Pow},
      {"Std.GreaterThan", BuiltinDispatchKey::kStd_GreaterThan},
      {"Std.LessThan", BuiltinDispatchKey::kStd_LessThan},
      {"Std.GreaterOrEqual", BuiltinDispatchKey::kStd_GreaterOrEqual},
      {"Std.LessOrEqual", BuiltinDispatchKey::kStd_LessOrEqual},
      {"Std.Equal", BuiltinDispatchKey::kStd_Equal},
      {"Std.NotEqual", BuiltinDispatchKey::kStd_NotEqual},
      {"Std.Not", BuiltinDispatchKey::kStd_Not},
      {"Std.And", BuiltinDispatchKey::kStd_And},
      {"Std.Or", BuiltinDispatchKey::kStd_Or},
      {"Std.Select", BuiltinDispatchKey::kStd_Select},
      {"Std.UnaryMinus", BuiltinDispatchKey::kStd_UnaryMinus},
      {"Std.UnaryPlus", BuiltinDispatchKey::kStd_UnaryPlus},
      {"Std.Math.Floor", BuiltinDispatchKey::kStd_Math_Floor},
      {"Std.Math.Ceil", BuiltinDispatchKey::kStd_Math_Ceil},
      {"Std.Math.Abs", BuiltinDispatchKey::kStd_Math_Abs},
      {"Std.Math.Log", BuiltinDispatchKey::kStd_Math_Log},
      {"Std.Math.Clamp", BuiltinDispatchKey::kStd_Math_Clamp},
      {"Std.Apply", BuiltinDispatchKey::kStd_Apply},
      {"Std.Loop", BuiltinDispatchKey::kStd_Loop},
      {"Std.LoopN", BuiltinDispatchKey::kStd_LoopN},
      {"Std.Wrap", BuiltinDispatchKey::kStd_Wrap},
      {"Std.Unwrap", BuiltinDispatchKey::kStd_Unwrap},
      {"Std.ElementAt", BuiltinDispatchKey::kStd_ElementAt},
      {"Std.Length", BuiltinDispatchKey::kStd_Length},
      {"Std.Take", BuiltinDispatchKey::kStd_Take},
      {"Std.Drop", BuiltinDispatchKey::kStd_Drop},
      {"Std.Slice", BuiltinDispatchKey::kStd_Slice},
      {"Std.ToNum", BuiltinDispatchKey::kStd_ToNum},
      {"Std.ToString", BuiltinDispatchKey::kStd_ToString},
      {"Std.String.Upper", BuiltinDispatchKey::kStd_String_Upper},
      {"Std.String.Lower", BuiltinDispatchKey::kStd_String_Lower},
      {"Std.String.Trim", BuiltinDispatchKey::kStd_String_Trim},
      {"Std.String.TrimStart", BuiltinDispatchKey::kStd_String_TrimStart},
      {"Std.String.TrimEnd", BuiltinDispatchKey::kStd_String_TrimEnd},
      {"Std.String.Split", BuiltinDispatchKey::kStd_String_Split},
      {"Std.String.Join", BuiltinDispatchKey::kStd_String_Join},
      {"Std.String.Replace", BuiltinDispatchKey::kStd_String_Replace},
      {"Std.String.Contains", BuiltinDispatchKey::kStd_String_Contains},
      {"Std.String.StartsWith", BuiltinDispatchKey::kStd_String_StartsWith},
      {"Std.String.EndsWith", BuiltinDispatchKey::kStd_String_EndsWith},
      {"Std.String.Length", BuiltinDispatchKey::kStd_String_Length},
      {"Std.OS.Cwd", BuiltinDispatchKey::kStd_OS_Cwd},
      {"Std.OS.Home", BuiltinDispatchKey::kStd_OS_Home},
      {"Std.OS.TempDir", BuiltinDispatchKey::kStd_OS_TempDir},
      {"Std.OS.Env", BuiltinDispatchKey::kStd_OS_Env},
      {"Std.OS.HasEnv", BuiltinDispatchKey::kStd_OS_HasEnv},
      {"Std.OS.IsWindows", BuiltinDispatchKey::kStd_OS_IsWindows},
      {"Std.OS.IsLinux", BuiltinDispatchKey::kStd_OS_IsLinux},
      {"Std.OS.IsMacOS", BuiltinDispatchKey::kStd_OS_IsMacOS},
      {"Std.OS.SetEnv", BuiltinDispatchKey::kStd_OS_SetEnv},
      {"Std.OS.UnsetEnv", BuiltinDispatchKey::kStd_OS_UnsetEnv},
      {"Std.OS.MakeTempFile", BuiltinDispatchKey::kStd_OS_MakeTempFile},
      {"Std.OS.MakeTempDir", BuiltinDispatchKey::kStd_OS_MakeTempDir},
      {"Std.Path.Join", BuiltinDispatchKey::kStd_Path_Join},
      {"Std.Path.Normalize", BuiltinDispatchKey::kStd_Path_Normalize},
      {"Std.Path.Basename", BuiltinDispatchKey::kStd_Path_Basename},
      {"Std.Path.Dirname", BuiltinDispatchKey::kStd_Path_Dirname},
      {"Std.Path.Exists", BuiltinDispatchKey::kStd_Path_Exists},
      {"Std.Path.IsFile", BuiltinDispatchKey::kStd_Path_IsFile},
      {"Std.Path.IsDir", BuiltinDispatchKey::kStd_Path_IsDir},
      {"Std.Path.Absolute", BuiltinDispatchKey::kStd_Path_Absolute},
      {"Std.Path.Extension", BuiltinDispatchKey::kStd_Path_Extension},
      {"Std.Path.Stem", BuiltinDispatchKey::kStd_Path_Stem},
      {"Std.Path.WithExtension", BuiltinDispatchKey::kStd_Path_WithExtension},
      {"Std.Path.WithBasename", BuiltinDispatchKey::kStd_Path_WithBasename},
      {"Std.File.ReadText", BuiltinDispatchKey::kStd_File_ReadText},
      {"Std.File.WriteText", BuiltinDispatchKey::kStd_File_WriteText},
      {"Std.File.AppendText", BuiltinDispatchKey::kStd_File_AppendText},
      {"Std.File.ReadLines", BuiltinDispatchKey::kStd_File_ReadLines},
      {"Std.File.Delete", BuiltinDispatchKey::kStd_File_Delete},
      {"Std.File.Size", BuiltinDispatchKey::kStd_File_Size},
      {"Std.File.Open", BuiltinDispatchKey::kStd_File_Open},
      {"Std.File.ReadLine", BuiltinDispatchKey::kStd_File_ReadLine},
      {"Std.File.ReadChunk", BuiltinDispatchKey::kStd_File_ReadChunk},
      {"Std.File.WriteChunk", BuiltinDispatchKey::kStd_File_WriteChunk},
      {"Std.File.Flush", BuiltinDispatchKey::kStd_File_Flush},
      {"Std.File.Close", BuiltinDispatchKey::kStd_File_Close},
      {"Std.File.WithOpen", BuiltinDispatchKey::kStd_File_WithOpen},
      {"Std.Dir.Create", BuiltinDispatchKey::kStd_Dir_Create},
      {"Std.Dir.Delete", BuiltinDispatchKey::kStd_Dir_Delete},
      {"Std.Dir.List", BuiltinDispatchKey::kStd_Dir_List},
      {"Std.Dir.ListFull", BuiltinDispatchKey::kStd_Dir_ListFull},
      {"Std.Tuple.Append", BuiltinDispatchKey::kStd_Tuple_Append},
      {"Std.Tuple.Prepend", BuiltinDispatchKey::kStd_Tuple_Prepend},
      {"Std.Tuple.Reverse", BuiltinDispatchKey::kStd_Tuple_Reverse},
      {"Std.Tuple.Contains", BuiltinDispatchKey::kStd_Tuple_Contains},
      {"Std.Tuple.Zip", BuiltinDispatchKey::kStd_Tuple_Zip},
      {"Std.Dict.Create", BuiltinDispatchKey::kStd_Dict_Create},
      {"Std.Dict.Set", BuiltinDispatchKey::kStd_Dict_Set},
      {"Std.Dict.Get", BuiltinDispatchKey::kStd_Dict_Get},
      {"Std.Dict.GetDefault", BuiltinDispatchKey::kStd_Dict_GetDefault},
      {"Std.Dict.Keys", BuiltinDispatchKey::kStd_Dict_Keys},
      {"Std.Dict.Values", BuiltinDispatchKey::kStd_Dict_Values},
      {"Std.Dict.Length", BuiltinDispatchKey::kStd_Dict_Length},
      {"Std.Println", BuiltinDispatchKey::kStd_Println},
      {"Std.Printf", BuiltinDispatchKey::kStd_Printf},
      {"Std.GetArgs", BuiltinDispatchKey::kStd_GetArgs},
      {"Std.Input", BuiltinDispatchKey::kStd_Input},
      {"Std.Exit", BuiltinDispatchKey::kStd_Exit},
      {"Std.Tuple.Map", BuiltinDispatchKey::kStd_Tuple_Map},
      {"Std.Tuple.Filter", BuiltinDispatchKey::kStd_Tuple_Filter},
      {"Std.Tuple.Sort", BuiltinDispatchKey::kStd_Tuple_Sort},
      {"Std.Tuple.Unique", BuiltinDispatchKey::kStd_Tuple_Unique},
      {"Std.Tuple.Min", BuiltinDispatchKey::kStd_Tuple_Min},
      {"Std.Tuple.Max", BuiltinDispatchKey::kStd_Tuple_Max},
      {"Std.Tuple.Reduce", BuiltinDispatchKey::kStd_Tuple_Reduce},
      {"Std.Tuple.FindIndex", BuiltinDispatchKey::kStd_Tuple_FindIndex},
      {"Std.Tuple.Any", BuiltinDispatchKey::kStd_Tuple_Any},
      {"Std.Tuple.All", BuiltinDispatchKey::kStd_Tuple_All},
      {"Std.Tuple.Range", BuiltinDispatchKey::kStd_Tuple_Range},
      {"Std.Dict.Contains", BuiltinDispatchKey::kStd_Dict_Contains},
      {"Std.Dict.Delete", BuiltinDispatchKey::kStd_Dict_Delete},
      {"Std.Dict.Entries", BuiltinDispatchKey::kStd_Dict_Entries},
      {"Std.Dict.Clear", BuiltinDispatchKey::kStd_Dict_Clear},
      {"Std.Branch", BuiltinDispatchKey::kStd_Branch},
      {"Std.Sqrt", BuiltinDispatchKey::kStd_Sqrt},
      {"Std.Math.Sqrt", BuiltinDispatchKey::kStd_Math_Sqrt},
      {"Std.Sin", BuiltinDispatchKey::kStd_Sin},
      {"Std.Math.Sin", BuiltinDispatchKey::kStd_Math_Sin},
      {"Std.Cos", BuiltinDispatchKey::kStd_Cos},
      {"Std.Math.Cos", BuiltinDispatchKey::kStd_Math_Cos},
      {"Std.Tan", BuiltinDispatchKey::kStd_Tan},
      {"Std.Math.Tan", BuiltinDispatchKey::kStd_Math_Tan},
  };

  if (const auto dispatch_it = kBuiltinDispatchTable.find(name); dispatch_it != kBuiltinDispatchTable.end()) {
    switch (const auto dispatch = dispatch_it->second) {
      case BuiltinDispatchKey::kStd_Add: {
        auto args = expect_binary("Std.Add");
        if (!args) return tl::unexpected(args.error());
        try {
          if (args->first->HasString() && args->second->HasString()) {
            return std::optional<Value>{fleaux::runtime::make_string(fleaux::runtime::as_string(*args->first) +
                                                                     fleaux::runtime::as_string(*args->second))};
          }
          return std::optional<Value>{fleaux::runtime::num_result(fleaux::runtime::to_double(*args->first) +
                                                                  fleaux::runtime::to_double(*args->second))};
        } catch (const std::exception& ex) {
          return tl::unexpected(RuntimeError{std::string("native builtin 'Std.Add' threw: ") + ex.what()});
        }
      }
      case BuiltinDispatchKey::kStd_Subtract:
        return numeric_bin("Std.Subtract", [](const double l, const double r) -> double { return l - r; });
      case BuiltinDispatchKey::kStd_Multiply:
        return numeric_bin("Std.Multiply", [](const double l, const double r) -> double { return l * r; });
      case BuiltinDispatchKey::kStd_Divide:
        return numeric_bin("Std.Divide", [](const double l, const double r) -> double { return l / r; });
      case BuiltinDispatchKey::kStd_Mod:
        return numeric_bin("Std.Mod", [](const double l, const double r) -> double { return std::fmod(l, r); });
      case BuiltinDispatchKey::kStd_Pow:
        return numeric_bin("Std.Pow", [](const double l, const double r) -> double { return std::pow(l, r); });
      case BuiltinDispatchKey::kStd_GreaterThan:
        return numeric_cmp("Std.GreaterThan", [](const double l, const double r) -> bool { return l > r; });
      case BuiltinDispatchKey::kStd_LessThan:
        return numeric_cmp("Std.LessThan", [](const double l, const double r) -> bool { return l < r; });
      case BuiltinDispatchKey::kStd_GreaterOrEqual:
        return numeric_cmp("Std.GreaterOrEqual", [](const double l, const double r) -> bool { return l >= r; });
      case BuiltinDispatchKey::kStd_LessOrEqual:
        return numeric_cmp("Std.LessOrEqual", [](const double l, const double r) -> bool { return l <= r; });
      case BuiltinDispatchKey::kStd_Equal:
      case BuiltinDispatchKey::kStd_NotEqual: {
        auto args = expect_binary(name.c_str());
        if (!args) return tl::unexpected(args.error());
        const bool eq = (*args->first == *args->second);
        return std::optional<Value>{fleaux::runtime::make_bool(dispatch == BuiltinDispatchKey::kStd_Equal ? eq : !eq)};
      }
      case BuiltinDispatchKey::kStd_Not: {
        auto v = expect_unary("Std.Not");
        if (!v) return tl::unexpected(v.error());
        try {
          return std::optional<Value>{fleaux::runtime::make_bool(!fleaux::runtime::as_bool(*v.value()))};
        } catch (const std::exception& ex) {
          return tl::unexpected(RuntimeError{std::string("native builtin 'Std.Not' threw: ") + ex.what()});
        }
      }
      case BuiltinDispatchKey::kStd_And: {
        auto args = expect_binary("Std.And");
        if (!args) return tl::unexpected(args.error());
        try {
          return std::optional<Value>{fleaux::runtime::make_bool(fleaux::runtime::as_bool(*args->first) &&
                                                                 fleaux::runtime::as_bool(*args->second))};
        } catch (const std::exception& ex) {
          return tl::unexpected(RuntimeError{std::string("native builtin 'Std.And' threw: ") + ex.what()});
        }
      }
      case BuiltinDispatchKey::kStd_Or: {
        auto args = expect_binary("Std.Or");
        if (!args) return tl::unexpected(args.error());
        try {
          return std::optional<Value>{fleaux::runtime::make_bool(fleaux::runtime::as_bool(*args->first) ||
                                                                 fleaux::runtime::as_bool(*args->second))};
        } catch (const std::exception& ex) {
          return tl::unexpected(RuntimeError{std::string("native builtin 'Std.Or' threw: ") + ex.what()});
        }
      }
      case BuiltinDispatchKey::kStd_Select: {
        auto args = expect_n("Std.Select", 3);
        if (!args) return tl::unexpected(args.error());
        const auto cond = (*args)->TryGet(0);
        const auto tv = (*args)->TryGet(1);
        const auto fv = (*args)->TryGet(2);
        if (!cond || !tv || !fv) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.Select' argument unpack failed"});
        }
        try {
          return std::optional<Value>{fleaux::runtime::as_bool(*cond) ? *tv : *fv};
        } catch (const std::exception& ex) {
          return tl::unexpected(RuntimeError{std::string("native builtin 'Std.Select' threw: ") + ex.what()});
        }
      }
      case BuiltinDispatchKey::kStd_UnaryMinus: {
        auto v = expect_unary("Std.UnaryMinus");
        if (!v) return tl::unexpected(v.error());
        try {
          return std::optional<Value>{fleaux::runtime::num_result(-fleaux::runtime::to_double(*v.value()))};
        } catch (const std::exception& ex) {
          return tl::unexpected(RuntimeError{std::string("native builtin 'Std.UnaryMinus' threw: ") + ex.what()});
        }
      }
      case BuiltinDispatchKey::kStd_UnaryPlus: {
        auto v = expect_unary("Std.UnaryPlus");
        if (!v) return tl::unexpected(v.error());
        try {
          return std::optional<Value>{fleaux::runtime::num_result(+fleaux::runtime::to_double(*v.value()))};
        } catch (const std::exception& ex) {
          return tl::unexpected(RuntimeError{std::string("native builtin 'Std.UnaryPlus' threw: ") + ex.what()});
        }
      }
      case BuiltinDispatchKey::kStd_Sqrt:
      case BuiltinDispatchKey::kStd_Math_Sqrt: {
        return numeric_unary(name, [](const double v) -> double { return std::sqrt(v); });
        break;
      }
      case BuiltinDispatchKey::kStd_Sin:
      case BuiltinDispatchKey::kStd_Math_Sin: {
        return numeric_unary(name, [](const double v) -> double { return std::sin(v); });
        break;
      }
      case BuiltinDispatchKey::kStd_Cos:
      case BuiltinDispatchKey::kStd_Math_Cos: {
        return numeric_unary(name, [](const double v) -> double { return std::cos(v); });
        break;
      }
      case BuiltinDispatchKey::kStd_Tan:
      case BuiltinDispatchKey::kStd_Math_Tan: {
        return numeric_unary(name, [](const double v) -> double { return std::tan(v); });
        break;
      }
      case BuiltinDispatchKey::kStd_Math_Floor: {
        return numeric_unary(name, [](const double v) -> double { return std::floor(v); });
        break;
      }
      case BuiltinDispatchKey::kStd_Math_Ceil: {
        return numeric_unary(name, [](const double v) -> double { return std::ceil(v); });
        break;
      }
      case BuiltinDispatchKey::kStd_Math_Abs: {
        return numeric_unary(name, [](const double v) -> double { return std::fabs(v); });
        break;
      }
      case BuiltinDispatchKey::kStd_Math_Log: {
        return numeric_unary(name, [](const double v) -> double { return std::log(v); });
        break;
      }
      case BuiltinDispatchKey::kStd_Math_Clamp: {
        auto args = expect_n("Std.Math.Clamp", 3);
        if (!args) return tl::unexpected(args.error());
        const auto x = (*args)->TryGet(0);
        const auto lo = (*args)->TryGet(1);
        const auto hi = (*args)->TryGet(2);
        if (!x || !lo || !hi) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.Math.Clamp' argument unpack failed"});
        }
        try {
          return std::optional<Value>{fleaux::runtime::num_result(std::clamp(
              fleaux::runtime::to_double(*x), fleaux::runtime::to_double(*lo), fleaux::runtime::to_double(*hi)))};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Apply: {
        auto args = expect_n("Std.Apply", 2);
        if (!args) return tl::unexpected(args.error());
        const auto value = (*args)->TryGet(0);
        const auto func = (*args)->TryGet(1);
        if (!value || !func) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.Apply' argument unpack failed"});
        }
        try {
          return std::optional<Value>{fleaux::runtime::invoke_callable_ref(*func, *value)};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Loop: {
        auto args = expect_n("Std.Loop", 3);
        if (!args) return tl::unexpected(args.error());
        const auto state = (*args)->TryGet(0);
        const auto continue_func = (*args)->TryGet(1);
        const auto step_func = (*args)->TryGet(2);
        if (!state || !continue_func || !step_func) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.Loop' argument unpack failed"});
        }
        auto result = run_loop_intrinsic(*state, *continue_func, *step_func, std::nullopt);
        if (!result) {
          const std::string prefix = "native 'loop' threw: ";
          std::string msg = result.error().message;
          if (msg.starts_with(prefix)) { msg.replace(0, prefix.size(), "native builtin 'Std.Loop' threw: "); }
          return tl::unexpected(RuntimeError{std::move(msg)});
        }
        return std::optional<Value>{std::move(*result)};
        break;
      }
      case BuiltinDispatchKey::kStd_LoopN: {
        auto args = expect_n("Std.LoopN", 4);
        if (!args) return tl::unexpected(args.error());
        const auto state = (*args)->TryGet(0);
        const auto continue_func = (*args)->TryGet(1);
        const auto step_func = (*args)->TryGet(2);
        const auto max_iters = (*args)->TryGet(3);
        if (!state || !continue_func || !step_func || !max_iters) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.LoopN' argument unpack failed"});
        }
        std::size_t limit = 0;
        try {
          const auto as_int = fleaux::runtime::as_int_value(*max_iters);
          if (as_int < 0) {
            return tl::unexpected(
                RuntimeError{"native builtin 'Std.LoopN' threw: LoopN: max_iters must be non-negative"});
          }
          limit = static_cast<std::size_t>(as_int);
        } catch (const std::exception& ex) { return native_error(name, ex); }
        auto result = run_loop_intrinsic(*state, *continue_func, *step_func, limit);
        if (!result) {
          const std::string prefix = "native 'loop' threw: ";
          std::string msg = result.error().message;
          if (msg.starts_with(prefix)) { msg.replace(0, prefix.size(), "native builtin 'Std.LoopN' threw: "); }
          return tl::unexpected(RuntimeError{std::move(msg)});
        }
        return std::optional<Value>{std::move(*result)};
        break;
      }
      case BuiltinDispatchKey::kStd_Wrap: {
        try {
          return std::optional<Value>{fleaux::runtime::make_tuple(arg)};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Unwrap: {
        auto args = expect_n("Std.Unwrap", 1);
        if (!args) return tl::unexpected(args.error());
        const auto value = (*args)->TryGet(0);
        if (!value) { return tl::unexpected(RuntimeError{"native builtin 'Std.Unwrap' argument unpack failed"}); }
        return std::optional<Value>{*value};
        break;
      }
      case BuiltinDispatchKey::kStd_ElementAt: {
        auto args = expect_n("Std.ElementAt", 2);
        if (!args) return tl::unexpected(args.error());
        const auto seq = (*args)->TryGet(0);
        const auto idx = (*args)->TryGet(1);
        if (!seq || !idx) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.ElementAt' argument unpack failed"});
        }
        try {
          const auto& arr = fleaux::runtime::as_array(*seq);
          const auto i = fleaux::runtime::as_index(*idx);
          const auto elem = arr.TryGet(i);
          if (!elem) {
            return tl::unexpected(RuntimeError{"native builtin 'Std.ElementAt' threw: ElementAt: index out of range"});
          }
          return std::optional<Value>{*elem};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Length: {
        auto args = expect_n("Std.Length", 1);
        if (!args) return tl::unexpected(args.error());
        const auto seq = (*args)->TryGet(0);
        if (!seq) { return tl::unexpected(RuntimeError{"native builtin 'Std.Length' argument unpack failed"}); }
        try {
          return std::optional<Value>{
              fleaux::runtime::make_int(static_cast<fleaux::runtime::Int>(fleaux::runtime::as_array(*seq).Size()))};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Take: {
        auto args = expect_n("Std.Take", 2);
        if (!args) return tl::unexpected(args.error());
        const auto seq = (*args)->TryGet(0);
        const auto count = (*args)->TryGet(1);
        if (!seq || !count) { return tl::unexpected(RuntimeError{"native builtin 'Std.Take' argument unpack failed"}); }
        try {
          const auto& arr = fleaux::runtime::as_array(*seq);
          const auto n = std::min(fleaux::runtime::as_index(*count), arr.Size());
          Array out;
          out.Reserve(n);
          for (std::size_t i = 0; i < n; ++i) { out.PushBack(*arr.TryGet(i)); }
          return std::optional<Value>{Value{std::move(out)}};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Drop: {
        auto args = expect_n("Std.Drop", 2);
        if (!args) return tl::unexpected(args.error());
        const auto seq = (*args)->TryGet(0);
        const auto count = (*args)->TryGet(1);
        if (!seq || !count) { return tl::unexpected(RuntimeError{"native builtin 'Std.Drop' argument unpack failed"}); }
        try {
          const auto& arr = fleaux::runtime::as_array(*seq);
          const auto start = fleaux::runtime::as_index(*count);
          Array out;
          for (std::size_t i = start; i < arr.Size(); ++i) { out.PushBack(*arr.TryGet(i)); }
          return std::optional<Value>{Value{std::move(out)}};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Slice: {
        auto args = expect_n("Std.Slice", 3);
        if (!args) return tl::unexpected(args.error());
        const auto seq = (*args)->TryGet(0);
        const auto start = (*args)->TryGet(1);
        const auto stop = (*args)->TryGet(2);
        if (!seq || !start || !stop) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.Slice' argument unpack failed"});
        }
        try {
          const auto& arr = fleaux::runtime::as_array(*seq);
          const auto real_start = fleaux::runtime::as_index(*start);
          const auto real_stop = std::min(fleaux::runtime::as_index(*stop), arr.Size());
          Array out;
          for (std::size_t i = real_start; i < real_stop; ++i) { out.PushBack(*arr.TryGet(i)); }
          return std::optional<Value>{Value{std::move(out)}};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_ToNum: {
        auto v = expect_unary("Std.ToNum");
        if (!v) return tl::unexpected(v.error());
        try {
          const std::string& s = fleaux::runtime::as_string(*v.value());
          std::size_t consumed = 0;
          const double d = std::stod(s, &consumed);
          if (consumed != s.size()) {
            return tl::unexpected(
                RuntimeError{"native builtin 'Std.ToNum' threw: ToNum: trailing characters in input"});
          }
          return std::optional<Value>{fleaux::runtime::num_result(d)};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_ToString: {
        auto v = expect_unary("Std.ToString");
        if (!v) return tl::unexpected(v.error());
        try {
          return std::optional<Value>{fleaux::runtime::make_string(fleaux::runtime::to_string(*v.value()))};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_String_Upper: {
        auto v = expect_unary("Std.String.Upper");
        if (!v) return tl::unexpected(v.error());
        try {
          std::string s = fleaux::runtime::to_string(*v.value());
          std::ranges::transform(s, s.begin(),
                                 [](const unsigned char ch) -> char { return static_cast<char>(std::toupper(ch)); });
          return std::optional<Value>{fleaux::runtime::make_string(std::move(s))};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_String_Lower: {
        auto v = expect_unary("Std.String.Lower");
        if (!v) return tl::unexpected(v.error());
        try {
          std::string s = fleaux::runtime::to_string(*v.value());
          std::ranges::transform(s, s.begin(),
                                 [](const unsigned char ch) -> char { return static_cast<char>(std::tolower(ch)); });
          return std::optional<Value>{fleaux::runtime::make_string(std::move(s))};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_String_Trim: {
        auto v = expect_unary("Std.String.Trim");
        if (!v) return tl::unexpected(v.error());
        try {
          auto s = fleaux::runtime::to_string(*v.value());
          return std::optional<Value>{fleaux::runtime::make_string(trim_right_copy(trim_left_copy(std::move(s))))};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_String_TrimStart: {
        auto v = expect_unary("Std.String.TrimStart");
        if (!v) return tl::unexpected(v.error());
        try {
          auto s = fleaux::runtime::to_string(*v.value());
          return std::optional<Value>{fleaux::runtime::make_string(trim_left_copy(std::move(s)))};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_String_TrimEnd: {
        auto v = expect_unary("Std.String.TrimEnd");
        if (!v) return tl::unexpected(v.error());
        try {
          auto s = fleaux::runtime::to_string(*v.value());
          return std::optional<Value>{fleaux::runtime::make_string(trim_right_copy(std::move(s)))};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_String_Split: {
        auto args = expect_n("Std.String.Split", 2);
        if (!args) return tl::unexpected(args.error());
        const auto input = (*args)->TryGet(0);
        const auto sep = (*args)->TryGet(1);
        if (!input || !sep) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.String.Split' argument unpack failed"});
        }
        try {
          const std::string s = fleaux::runtime::to_string(*input);
          const std::string delim = fleaux::runtime::to_string(*sep);
          if (delim.empty()) {
            return tl::unexpected(
                RuntimeError{"native builtin 'Std.String.Split' threw: StringSplit separator cannot be empty"});
          }
          Array out;
          std::size_t pos = 0;
          while (true) {
            const std::size_t found = s.find(delim, pos);
            if (found == std::string::npos) {
              out.PushBack(fleaux::runtime::make_string(s.substr(pos)));
              break;
            }
            out.PushBack(fleaux::runtime::make_string(s.substr(pos, found - pos)));
            pos = found + delim.size();
          }
          return std::optional<Value>{Value{std::move(out)}};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_String_Join: {
        auto args = expect_n("Std.String.Join", 2);
        if (!args) return tl::unexpected(args.error());
        const auto sep = (*args)->TryGet(0);
        const auto parts = (*args)->TryGet(1);
        if (!sep || !parts) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.String.Join' argument unpack failed"});
        }
        try {
          const std::string delim = fleaux::runtime::to_string(*sep);
          std::ostringstream oss;
          if (parts->HasArray()) {
            const auto& arr = fleaux::runtime::as_array(*parts);
            for (std::size_t i = 0; i < arr.Size(); ++i) {
              if (i > 0) oss << delim;
              oss << fleaux::runtime::to_string(*arr.TryGet(i));
            }
          } else {
            const std::string s = fleaux::runtime::to_string(*parts);
            for (std::size_t i = 0; i < s.size(); ++i) {
              if (i > 0) oss << delim;
              oss << s[i];
            }
          }
          return std::optional<Value>{fleaux::runtime::make_string(oss.str())};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_String_Replace: {
        auto args = expect_n("Std.String.Replace", 3);
        if (!args) return tl::unexpected(args.error());
        const auto input = (*args)->TryGet(0);
        const auto old_sub = (*args)->TryGet(1);
        const auto new_sub = (*args)->TryGet(2);
        if (!input || !old_sub || !new_sub) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.String.Replace' argument unpack failed"});
        }
        try {
          std::string s = fleaux::runtime::to_string(*input);
          const std::string old_s = fleaux::runtime::to_string(*old_sub);
          const std::string new_s = fleaux::runtime::to_string(*new_sub);
          if (!old_s.empty()) {
            std::size_t pos = 0;
            while ((pos = s.find(old_s, pos)) != std::string::npos) {
              s.replace(pos, old_s.size(), new_s);
              pos += new_s.size();
            }
          }
          return std::optional<Value>{fleaux::runtime::make_string(std::move(s))};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_String_Contains: {
        auto args = expect_n("Std.String.Contains", 2);
        if (!args) return tl::unexpected(args.error());
        const auto input = (*args)->TryGet(0);
        const auto sub = (*args)->TryGet(1);
        if (!input || !sub) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.String.Contains' argument unpack failed"});
        }
        try {
          const std::string s = fleaux::runtime::to_string(*input);
          const std::string needle = fleaux::runtime::to_string(*sub);
          return std::optional<Value>{fleaux::runtime::make_bool(s.find(needle) != std::string::npos)};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_String_StartsWith: {
        auto args = expect_n("Std.String.StartsWith", 2);
        if (!args) return tl::unexpected(args.error());
        const auto input = (*args)->TryGet(0);
        const auto prefix = (*args)->TryGet(1);
        if (!input || !prefix) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.String.StartsWith' argument unpack failed"});
        }
        try {
          const std::string s = fleaux::runtime::to_string(*input);
          const std::string p = fleaux::runtime::to_string(*prefix);
          return std::optional<Value>{fleaux::runtime::make_bool(s.starts_with(p))};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_String_EndsWith: {
        auto args = expect_n("Std.String.EndsWith", 2);
        if (!args) return tl::unexpected(args.error());
        const auto input = (*args)->TryGet(0);
        const auto suffix = (*args)->TryGet(1);
        if (!input || !suffix) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.String.EndsWith' argument unpack failed"});
        }
        try {
          const std::string s = fleaux::runtime::to_string(*input);
          const std::string suf = fleaux::runtime::to_string(*suffix);
          if (suf.size() > s.size()) { return std::optional<Value>{fleaux::runtime::make_bool(false)}; }
          return std::optional<Value>{fleaux::runtime::make_bool(s.ends_with(suf))};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_String_Length: {
        auto v = expect_unary("Std.String.Length");
        if (!v) return tl::unexpected(v.error());
        try {
          return std::optional<Value>{fleaux::runtime::make_int(
              static_cast<fleaux::runtime::Int>(fleaux::runtime::to_string(*v.value()).size()))};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_OS_Cwd: {
        if (auto args = expect_n("Std.OS.Cwd", 0); !args) return tl::unexpected(args.error());
        try {
          return std::optional<Value>{fleaux::runtime::make_string(std::filesystem::current_path().string())};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_OS_Home: {
        if (auto args = expect_n("Std.OS.Home", 0); !args) return tl::unexpected(args.error());
        try {
          if (const char* home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
            return std::optional<Value>{fleaux::runtime::make_string(home)};
          }
          if (const char* userprofile = std::getenv("USERPROFILE"); userprofile != nullptr && userprofile[0] != '\0') {
            return std::optional<Value>{fleaux::runtime::make_string(userprofile)};
          }
          const char* homedrive = std::getenv("HOMEDRIVE");
          if (const char* homepath = std::getenv("HOMEPATH");
              homedrive != nullptr && homepath != nullptr && homedrive[0] != '\0' && homepath[0] != '\0') {
            return std::optional<Value>{fleaux::runtime::make_string(std::string(homedrive) + std::string(homepath))};
          }
          std::error_code ec;
          const auto cwd = std::filesystem::current_path(ec);
          if (!ec) { return std::optional<Value>{fleaux::runtime::make_string(cwd.string())}; }
          return std::optional<Value>{fleaux::runtime::make_string(".")};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_OS_TempDir: {
        if (auto args = expect_n("Std.OS.TempDir", 0); !args) return tl::unexpected(args.error());
        try {
          return std::optional<Value>{fleaux::runtime::make_string(std::filesystem::temp_directory_path().string())};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_OS_Env: {
        auto v = expect_unary("Std.OS.Env");
        if (!v) return tl::unexpected(v.error());
        try {
          const std::string key = fleaux::runtime::to_string(*v.value());
          if (const char* val = std::getenv(key.c_str())) {
            return std::optional<Value>{fleaux::runtime::make_string(val)};
          }
          return std::optional<Value>{fleaux::runtime::make_null()};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_OS_HasEnv: {
        auto v = expect_unary("Std.OS.HasEnv");
        if (!v) return tl::unexpected(v.error());
        try {
          const std::string key = fleaux::runtime::to_string(*v.value());
          return std::optional<Value>{fleaux::runtime::make_bool(std::getenv(key.c_str()) != nullptr)};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_OS_IsWindows: {
        if (auto args = expect_n("Std.OS.IsWindows", 0); !args) return tl::unexpected(args.error());
#if defined(_WIN32)
        return std::optional<Value>{fleaux::runtime::make_bool(true)};
#else
        return std::optional<Value>{fleaux::runtime::make_bool(false)};
#endif
        break;
      }
      case BuiltinDispatchKey::kStd_OS_IsLinux: {
        if (auto args = expect_n("Std.OS.IsLinux", 0); !args) return tl::unexpected(args.error());
#if defined(__linux__)
        return std::optional<Value>{fleaux::runtime::make_bool(true)};
#else
        return std::optional<Value>{fleaux::runtime::make_bool(false)};
#endif
        break;
      }
      case BuiltinDispatchKey::kStd_OS_IsMacOS: {
        if (auto args = expect_n("Std.OS.IsMacOS", 0); !args) return tl::unexpected(args.error());
#if defined(__APPLE__)
        return std::optional<Value>{fleaux::runtime::make_bool(true)};
#else
        return std::optional<Value>{fleaux::runtime::make_bool(false)};
#endif
        break;
      }
      case BuiltinDispatchKey::kStd_OS_SetEnv: {
        auto args = expect_n("Std.OS.SetEnv", 2);
        if (!args) return tl::unexpected(args.error());
        const auto key = (*args)->TryGet(0);
        const auto value = (*args)->TryGet(1);
        if (!key || !value) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.OS.SetEnv' argument unpack failed"});
        }
        try {
          const std::string k = fleaux::runtime::to_string(*key);
          const std::string v = fleaux::runtime::to_string(*value);
#if defined(_WIN32)
          if (_putenv_s(k.c_str(), v.c_str()) != 0) {
            return tl::unexpected(RuntimeError{"native builtin 'Std.OS.SetEnv' threw: OSSetEnv failed"});
          }
#else
          if (setenv(k.c_str(), v.c_str(), 1) != 0) {
            return tl::unexpected(RuntimeError{"native builtin 'Std.OS.SetEnv' threw: OSSetEnv failed"});
          }
#endif
          return std::optional<Value>{fleaux::runtime::make_string(v)};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_OS_UnsetEnv: {
        auto v = expect_unary("Std.OS.UnsetEnv");
        if (!v) return tl::unexpected(v.error());
        try {
          const std::string key = fleaux::runtime::to_string(*v.value());
          const bool existed = std::getenv(key.c_str()) != nullptr;
#if defined(_WIN32)
          if (_putenv_s(key.c_str(), "") != 0) {
            return tl::unexpected(RuntimeError{"native builtin 'Std.OS.UnsetEnv' threw: OSUnsetEnv failed"});
          }
#else
          if (unsetenv(key.c_str()) != 0) {
            return tl::unexpected(RuntimeError{"native builtin 'Std.OS.UnsetEnv' threw: OSUnsetEnv failed"});
          }
#endif
          return std::optional<Value>{fleaux::runtime::make_bool(existed)};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_OS_MakeTempFile: {
        if (auto args = expect_n("Std.OS.MakeTempFile", 0); !args) return tl::unexpected(args.error());
        try {
          std::error_code ec;
          const auto dir = std::filesystem::temp_directory_path(ec);
          if (ec) return std::optional<Value>{fleaux::runtime::make_null()};
          for (int i = 0; i < 100; ++i) {
            if (const auto candidate = dir / ("fleaux_" + fleaux::runtime::random_suffix() + ".tmp");
                !std::filesystem::exists(candidate, ec) && !ec) {
              if (std::ofstream out(candidate); out.good()) {
                return std::optional<Value>{fleaux::runtime::make_string(candidate.string())};
              }
            }
          }
          return std::optional<Value>{fleaux::runtime::make_null()};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_OS_MakeTempDir: {
        if (auto args = expect_n("Std.OS.MakeTempDir", 0); !args) return tl::unexpected(args.error());
        try {
          std::error_code ec;
          const auto dir = std::filesystem::temp_directory_path(ec);
          if (ec) return std::optional<Value>{fleaux::runtime::make_null()};
          for (int i = 0; i < 100; ++i) {
            if (const auto candidate = dir / ("fleaux_" + fleaux::runtime::random_suffix());
                std::filesystem::create_directory(candidate, ec) && !ec) {
              return std::optional<Value>{fleaux::runtime::make_string(candidate.string())};
            }
          }
          return std::optional<Value>{fleaux::runtime::make_null()};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Path_Join: {
        try {
          const auto& args = fleaux::runtime::as_array(arg);
          if (args.Size() < 2) {
            return tl::unexpected(
                RuntimeError{"native builtin 'Std.Path.Join' threw: PathJoin expects at least 2 arguments"});
          }
          std::filesystem::path out = fleaux::runtime::to_string(*args.TryGet(0));
          for (std::size_t i = 1; i < args.Size(); ++i) { out /= fleaux::runtime::to_string(*args.TryGet(i)); }
          return std::optional<Value>{fleaux::runtime::make_string(out.string())};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Path_Normalize: {
        auto v = expect_unary("Std.Path.Normalize");
        if (!v) return tl::unexpected(v.error());
        try {
          return std::optional<Value>{fleaux::runtime::make_string(
              std::filesystem::path(fleaux::runtime::to_string(*v.value())).lexically_normal().string())};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Path_Basename: {
        auto v = expect_unary("Std.Path.Basename");
        if (!v) return tl::unexpected(v.error());
        try {
          return std::optional<Value>{fleaux::runtime::make_string(
              std::filesystem::path(fleaux::runtime::to_string(*v.value())).filename().string())};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Path_Dirname: {
        auto v = expect_unary("Std.Path.Dirname");
        if (!v) return tl::unexpected(v.error());
        try {
          return std::optional<Value>{fleaux::runtime::make_string(
              std::filesystem::path(fleaux::runtime::to_string(*v.value())).parent_path().string())};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Path_Exists: {
        auto v = expect_unary("Std.Path.Exists");
        if (!v) return tl::unexpected(v.error());
        try {
          return std::optional<Value>{fleaux::runtime::make_bool(
              std::filesystem::exists(std::filesystem::path(fleaux::runtime::to_string(*v.value()))))};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Path_IsFile: {
        auto v = expect_unary("Std.Path.IsFile");
        if (!v) return tl::unexpected(v.error());
        try {
          return std::optional<Value>{fleaux::runtime::make_bool(
              std::filesystem::is_regular_file(std::filesystem::path(fleaux::runtime::to_string(*v.value()))))};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Path_IsDir: {
        auto v = expect_unary("Std.Path.IsDir");
        if (!v) return tl::unexpected(v.error());
        try {
          return std::optional<Value>{fleaux::runtime::make_bool(
              std::filesystem::is_directory(std::filesystem::path(fleaux::runtime::to_string(*v.value()))))};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Path_Absolute: {
        auto v = expect_unary("Std.Path.Absolute");
        if (!v) return tl::unexpected(v.error());
        try {
          return std::optional<Value>{fleaux::runtime::make_string(
              std::filesystem::absolute(std::filesystem::path(fleaux::runtime::to_string(*v.value()))).string())};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Path_Extension: {
        auto v = expect_unary("Std.Path.Extension");
        if (!v) return tl::unexpected(v.error());
        try {
          return std::optional<Value>{fleaux::runtime::make_string(
              std::filesystem::path(fleaux::runtime::to_string(*v.value())).extension().string())};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Path_Stem: {
        auto v = expect_unary("Std.Path.Stem");
        if (!v) return tl::unexpected(v.error());
        try {
          return std::optional<Value>{fleaux::runtime::make_string(
              std::filesystem::path(fleaux::runtime::to_string(*v.value())).stem().string())};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Path_WithExtension: {
        auto args = expect_n("Std.Path.WithExtension", 2);
        if (!args) return tl::unexpected(args.error());
        const auto input = (*args)->TryGet(0);
        const auto ext = (*args)->TryGet(1);
        if (!input || !ext) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.Path.WithExtension' argument unpack failed"});
        }
        try {
          std::filesystem::path p = fleaux::runtime::to_string(*input);
          std::string extension = fleaux::runtime::to_string(*ext);
          if (!extension.empty() && extension[0] != '.') { extension.insert(extension.begin(), '.'); }
          p.replace_extension(extension);
          return std::optional<Value>{fleaux::runtime::make_string(p.string())};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Path_WithBasename: {
        auto args = expect_n("Std.Path.WithBasename", 2);
        if (!args) return tl::unexpected(args.error());
        const auto input = (*args)->TryGet(0);
        const auto basename = (*args)->TryGet(1);
        if (!input || !basename) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.Path.WithBasename' argument unpack failed"});
        }
        try {
          std::filesystem::path p = fleaux::runtime::to_string(*input);
          p.replace_filename(fleaux::runtime::to_string(*basename));
          return std::optional<Value>{fleaux::runtime::make_string(p.string())};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_File_ReadText: {
        auto v = expect_unary("Std.File.ReadText");
        if (!v) return tl::unexpected(v.error());
        try {
          std::ifstream in(fleaux::runtime::to_string(*v.value()));
          if (!in) {
            return tl::unexpected(RuntimeError{"native builtin 'Std.File.ReadText' threw: FileReadText failed"});
          }
          std::ostringstream ss;
          ss << in.rdbuf();
          return std::optional<Value>{fleaux::runtime::make_string(ss.str())};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_File_WriteText: {
        auto args = expect_n("Std.File.WriteText", 2);
        if (!args) return tl::unexpected(args.error());
        const auto path = (*args)->TryGet(0);
        const auto text = (*args)->TryGet(1);
        if (!path || !text) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.File.WriteText' argument unpack failed"});
        }
        try {
          const std::string p = fleaux::runtime::to_string(*path);
          std::ofstream out(p, std::ios::trunc);
          if (!out) {
            return tl::unexpected(RuntimeError{"native builtin 'Std.File.WriteText' threw: FileWriteText failed"});
          }
          out << fleaux::runtime::to_string(*text);
          return std::optional<Value>{fleaux::runtime::make_string(p)};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_File_AppendText: {
        auto args = expect_n("Std.File.AppendText", 2);
        if (!args) return tl::unexpected(args.error());
        const auto path = (*args)->TryGet(0);
        const auto text = (*args)->TryGet(1);
        if (!path || !text) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.File.AppendText' argument unpack failed"});
        }
        try {
          const std::string p = fleaux::runtime::to_string(*path);
          std::ofstream out(p, std::ios::app);
          if (!out) {
            return tl::unexpected(RuntimeError{"native builtin 'Std.File.AppendText' threw: FileAppendText failed"});
          }
          out << fleaux::runtime::to_string(*text);
          return std::optional<Value>{fleaux::runtime::make_string(p)};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_File_ReadLines: {
        auto v = expect_unary("Std.File.ReadLines");
        if (!v) return tl::unexpected(v.error());
        try {
          std::ifstream in(fleaux::runtime::to_string(*v.value()));
          if (!in) {
            return tl::unexpected(RuntimeError{"native builtin 'Std.File.ReadLines' threw: FileReadLines failed"});
          }
          Array out;
          std::string line;
          while (std::getline(in, line)) { out.PushBack(fleaux::runtime::make_string(line)); }
          return std::optional<Value>{Value{std::move(out)}};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_File_Delete: {
        auto v = expect_unary("Std.File.Delete");
        if (!v) return tl::unexpected(v.error());
        try {
          std::error_code ec;
          const bool removed =
              std::filesystem::remove(std::filesystem::path(fleaux::runtime::to_string(*v.value())), ec);
          if (ec) { return tl::unexpected(RuntimeError{"native builtin 'Std.File.Delete' threw: " + ec.message()}); }
          return std::optional<Value>{fleaux::runtime::make_bool(removed)};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_File_Size: {
        auto v = expect_unary("Std.File.Size");
        if (!v) return tl::unexpected(v.error());
        try {
          return std::optional<Value>{fleaux::runtime::make_int(static_cast<fleaux::runtime::Int>(
              std::filesystem::file_size(std::filesystem::path(fleaux::runtime::to_string(*v.value())))))};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_File_Open: {
        try {
          const auto& arr = arg.TryGetArray();
          std::string path;
          std::string mode = "r";
          if (arr && arr->Size() == 2) {
            path = fleaux::runtime::as_string(*arr->TryGet(0));
            mode = fleaux::runtime::as_string(*arr->TryGet(1));
          } else if (arr && arr->Size() == 1) {
            path = fleaux::runtime::as_string(*arr->TryGet(0));
          } else if (arr) {
            return tl::unexpected(
                RuntimeError{"native builtin 'Std.File.Open' threw: FileOpen: expected (path,) or (path, mode)"});
          } else {
            path = fleaux::runtime::as_string(arg);
          }
          const auto slot = fleaux::runtime::handle_registry().open(path, mode);
          const auto gen = fleaux::runtime::handle_registry().entries[slot].generation;
          return std::optional<Value>{fleaux::runtime::make_handle_token(slot, gen)};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_File_ReadLine: {
        auto v = expect_unary("Std.File.ReadLine");
        if (!v) return tl::unexpected(v.error());
        try {
          const Value token = *v.value();
          auto& e = fleaux::runtime::require_handle(token, "FileReadLine");
          if (e.mode.find('r') == std::string::npos) {
            return tl::unexpected(RuntimeError{"native builtin 'Std.File.ReadLine' threw: FileReadLine: read failed"});
          }
          std::string line;
          bool eof = false;
          if (!std::getline(e.stream, line)) {
            if (e.stream.eof()) {
              eof = true;
              line.clear();
            } else {
              return tl::unexpected(
                  RuntimeError{"native builtin 'Std.File.ReadLine' threw: FileReadLine: read failed"});
            }
          }
          const auto id = fleaux::runtime::handle_id_from_value(token);
          if (!id) {
            return tl::unexpected(RuntimeError{"native builtin 'Std.File.ReadLine' threw: invalid handle token"});
          }
          return std::optional<Value>{fleaux::runtime::make_tuple(fleaux::runtime::make_handle_token(id->slot, id->gen),
                                                                  fleaux::runtime::make_string(std::move(line)),
                                                                  fleaux::runtime::make_bool(eof))};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_File_ReadChunk: {
        auto args = expect_n("Std.File.ReadChunk", 2);
        if (!args) return tl::unexpected(args.error());
        const auto token = (*args)->TryGet(0);
        const auto nbytes = (*args)->TryGet(1);
        if (!token || !nbytes) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.File.ReadChunk' argument unpack failed"});
        }
        try {
          auto& e = fleaux::runtime::require_handle(*token, "FileReadChunk");
          const auto n = fleaux::runtime::as_index(*nbytes);
          std::string buf(n, '\0');
          e.stream.read(buf.data(), static_cast<std::streamsize>(n));
          const std::streamsize got = e.stream.gcount();
          buf.resize(static_cast<std::size_t>(got));
          const bool eof = (got == 0 || e.stream.eof());
          const auto id = fleaux::runtime::handle_id_from_value(*token);
          if (!id) {
            return tl::unexpected(RuntimeError{"native builtin 'Std.File.ReadChunk' threw: invalid handle token"});
          }
          return std::optional<Value>{fleaux::runtime::make_tuple(fleaux::runtime::make_handle_token(id->slot, id->gen),
                                                                  fleaux::runtime::make_string(std::move(buf)),
                                                                  fleaux::runtime::make_bool(eof))};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_File_WriteChunk: {
        auto args = expect_n("Std.File.WriteChunk", 2);
        if (!args) return tl::unexpected(args.error());
        const auto token = (*args)->TryGet(0);
        const auto data = (*args)->TryGet(1);
        if (!token || !data) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.File.WriteChunk' argument unpack failed"});
        }
        try {
          auto& e = fleaux::runtime::require_handle(*token, "FileWriteChunk");
          const std::string& s = fleaux::runtime::as_string(*data);
          e.stream.write(s.data(), static_cast<std::streamsize>(s.size()));
          if (!e.stream) {
            return tl::unexpected(
                RuntimeError{"native builtin 'Std.File.WriteChunk' threw: FileWriteChunk: write failed"});
          }
          const auto id = fleaux::runtime::handle_id_from_value(*token);
          if (!id) {
            return tl::unexpected(RuntimeError{"native builtin 'Std.File.WriteChunk' threw: invalid handle token"});
          }
          return std::optional<Value>{fleaux::runtime::make_handle_token(id->slot, id->gen)};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_File_Flush: {
        auto v = expect_unary("Std.File.Flush");
        if (!v) return tl::unexpected(v.error());
        try {
          const Value token = *v.value();
          auto& e = fleaux::runtime::require_handle(token, "FileFlush");
          e.stream.flush();
          if (!e.stream) {
            return tl::unexpected(RuntimeError{"native builtin 'Std.File.Flush' threw: FileFlush: flush failed"});
          }
          const auto id = fleaux::runtime::handle_id_from_value(token);
          if (!id) {
            return tl::unexpected(RuntimeError{"native builtin 'Std.File.Flush' threw: invalid handle token"});
          }
          return std::optional<Value>{fleaux::runtime::make_handle_token(id->slot, id->gen)};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_File_Close: {
        auto v = expect_unary("Std.File.Close");
        if (!v) return tl::unexpected(v.error());
        try {
          const auto id = fleaux::runtime::handle_id_from_value(*v.value());
          if (!id) return std::optional<Value>{fleaux::runtime::make_bool(false)};
          return std::optional<Value>{
              fleaux::runtime::make_bool(fleaux::runtime::handle_registry().close(id->slot, id->gen))};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_File_WithOpen: {
        auto args = expect_n("Std.File.WithOpen", 3);
        if (!args) return tl::unexpected(args.error());
        const auto path = (*args)->TryGet(0);
        const auto mode = (*args)->TryGet(1);
        const auto fn = (*args)->TryGet(2);
        if (!path || !mode || !fn) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.File.WithOpen' argument unpack failed"});
        }
        try {
          const auto slot = fleaux::runtime::handle_registry().open(fleaux::runtime::as_string(*path),
                                                                    fleaux::runtime::as_string(*mode));
          const auto gen = fleaux::runtime::handle_registry().entries[slot].generation;
          const Value token = fleaux::runtime::make_handle_token(slot, gen);
          try {
            Value result = fleaux::runtime::invoke_callable_ref(*fn, token);
            fleaux::runtime::handle_registry().close(slot, gen);
            return std::optional<Value>{std::move(result)};
          } catch (...) {
            fleaux::runtime::handle_registry().close(slot, gen);
            throw;
          }
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Dir_Create: {
        auto v = expect_unary("Std.Dir.Create");
        if (!v) return tl::unexpected(v.error());
        try {
          const std::string path = fleaux::runtime::to_string(*v.value());
          std::filesystem::create_directories(path);
          return std::optional<Value>{fleaux::runtime::make_string(path)};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Dir_Delete: {
        auto v = expect_unary("Std.Dir.Delete");
        if (!v) return tl::unexpected(v.error());
        try {
          std::error_code ec;
          const auto removed =
              std::filesystem::remove_all(std::filesystem::path(fleaux::runtime::to_string(*v.value())), ec);
          if (ec) { return tl::unexpected(RuntimeError{"native builtin 'Std.Dir.Delete' threw: " + ec.message()}); }
          return std::optional<Value>{fleaux::runtime::make_bool(removed > 0)};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Dir_List: {
        auto v = expect_unary("Std.Dir.List");
        if (!v) return tl::unexpected(v.error());
        try {
          std::vector<std::string> names;
          for (const auto& entry :
               std::filesystem::directory_iterator(std::filesystem::path(fleaux::runtime::to_string(*v.value())))) {
            names.push_back(entry.path().filename().string());
          }
          Array out;
          out.Reserve(names.size());
          for (const auto& n : names) { out.PushBack(fleaux::runtime::make_string(n)); }
          return std::optional<Value>{Value{std::move(out)}};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Dir_ListFull: {
        auto v = expect_unary("Std.Dir.ListFull");
        if (!v) return tl::unexpected(v.error());
        try {
          std::vector<std::string> names;
          for (const auto& entry :
               std::filesystem::directory_iterator(std::filesystem::path(fleaux::runtime::to_string(*v.value())))) {
            names.push_back(entry.path().string());
          }
          Array out;
          out.Reserve(names.size());
          for (const auto& n : names) { out.PushBack(fleaux::runtime::make_string(n)); }
          return std::optional<Value>{Value{std::move(out)}};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Tuple_Append: {
        auto args = expect_n("Std.Tuple.Append", 2);
        if (!args) return tl::unexpected(args.error());
        const auto seq = (*args)->TryGet(0);
        const auto item = (*args)->TryGet(1);
        if (!seq || !item) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.Tuple.Append' argument unpack failed"});
        }
        try {
          const auto& src = fleaux::runtime::as_array(*seq);
          Array out;
          out.Reserve(src.Size() + 1);
          for (std::size_t i = 0; i < src.Size(); ++i) { out.PushBack(*src.TryGet(i)); }
          out.PushBack(*item);
          return std::optional<Value>{Value{std::move(out)}};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Tuple_Prepend: {
        auto args = expect_n("Std.Tuple.Prepend", 2);
        if (!args) return tl::unexpected(args.error());
        const auto seq = (*args)->TryGet(0);
        const auto item = (*args)->TryGet(1);
        if (!seq || !item) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.Tuple.Prepend' argument unpack failed"});
        }
        try {
          const auto& src = fleaux::runtime::as_array(*seq);
          Array out;
          out.Reserve(src.Size() + 1);
          out.PushBack(*item);
          for (std::size_t i = 0; i < src.Size(); ++i) { out.PushBack(*src.TryGet(i)); }
          return std::optional<Value>{Value{std::move(out)}};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Tuple_Reverse: {
        auto args = expect_n("Std.Tuple.Reverse", 1);
        if (!args) return tl::unexpected(args.error());
        const auto seq = (*args)->TryGet(0);
        if (!seq) { return tl::unexpected(RuntimeError{"native builtin 'Std.Tuple.Reverse' argument unpack failed"}); }
        try {
          const auto& src = fleaux::runtime::as_array(*seq);
          Array out;
          out.Reserve(src.Size());
          for (std::size_t i = src.Size(); i > 0; --i) { out.PushBack(*src.TryGet(i - 1)); }
          return std::optional<Value>{Value{std::move(out)}};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Tuple_Contains: {
        auto args = expect_n("Std.Tuple.Contains", 2);
        if (!args) return tl::unexpected(args.error());
        const auto seq = (*args)->TryGet(0);
        const auto item = (*args)->TryGet(1);
        if (!seq || !item) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.Tuple.Contains' argument unpack failed"});
        }
        try {
          const auto& src = fleaux::runtime::as_array(*seq);
          for (std::size_t i = 0; i < src.Size(); ++i) {
            if (*src.TryGet(i) == *item) { return std::optional<Value>{fleaux::runtime::make_bool(true)}; }
          }
          return std::optional<Value>{fleaux::runtime::make_bool(false)};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Tuple_Zip: {
        auto args = expect_n("Std.Tuple.Zip", 2);
        if (!args) return tl::unexpected(args.error());
        const auto a = (*args)->TryGet(0);
        const auto b = (*args)->TryGet(1);
        if (!a || !b) { return tl::unexpected(RuntimeError{"native builtin 'Std.Tuple.Zip' argument unpack failed"}); }
        try {
          const auto& left = fleaux::runtime::as_array(*a);
          const auto& right = fleaux::runtime::as_array(*b);
          const std::size_t n = std::min(left.Size(), right.Size());
          Array out;
          out.Reserve(n);
          for (std::size_t i = 0; i < n; ++i) {
            out.PushBack(fleaux::runtime::make_tuple(*left.TryGet(i), *right.TryGet(i)));
          }
          return std::optional<Value>{Value{std::move(out)}};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Dict_Create: {
        try {
          const auto& arr = arg.TryGetArray();
          if (arr && arr->Size() == 0) { return std::optional<Value>{Value{fleaux::runtime::Object{}}}; }
          if (arr) {
            if (arr->Size() != 1) {
              return tl::unexpected(
                  RuntimeError{"native builtin 'Std.Dict.Create' threw: DictCreate expects 0 or 1 arguments"});
            }
            return std::optional<Value>{Value{fleaux::runtime::as_object(*arr->TryGet(0))}};
          }
          return std::optional<Value>{Value{fleaux::runtime::as_object(arg)}};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Dict_Set: {
        auto args = expect_n("Std.Dict.Set", 3);
        if (!args) return tl::unexpected(args.error());
        const auto dict = (*args)->TryGet(0);
        const auto key = (*args)->TryGet(1);
        const auto value = (*args)->TryGet(2);
        if (!dict || !key || !value) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.Dict.Set' argument unpack failed"});
        }
        try {
          auto out = fleaux::runtime::as_object(*dict);
          out[fleaux::runtime::dict_key_from_value(*key)] = *value;
          return std::optional<Value>{Value{std::move(out)}};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Dict_Get: {
        auto args = expect_n("Std.Dict.Get", 2);
        if (!args) return tl::unexpected(args.error());
        const auto dict = (*args)->TryGet(0);
        const auto key = (*args)->TryGet(1);
        if (!dict || !key) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.Dict.Get' argument unpack failed"});
        }
        try {
          const auto& obj = fleaux::runtime::as_object(*dict);
          const auto got = obj.TryGet(fleaux::runtime::dict_key_from_value(*key));
          if (!got) {
            return tl::unexpected(RuntimeError{"native builtin 'Std.Dict.Get' threw: DictGet: key not found"});
          }
          return std::optional<Value>{*got};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Dict_GetDefault: {
        auto args = expect_n("Std.Dict.GetDefault", 3);
        if (!args) return tl::unexpected(args.error());
        const auto dict = (*args)->TryGet(0);
        const auto key = (*args)->TryGet(1);
        const auto default_value = (*args)->TryGet(2);
        if (!dict || !key || !default_value) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.Dict.GetDefault' argument unpack failed"});
        }
        try {
          const auto& obj = fleaux::runtime::as_object(*dict);
          const auto got = obj.TryGet(fleaux::runtime::dict_key_from_value(*key));
          if (!got) { return std::optional<Value>{*default_value}; }
          return std::optional<Value>{*got};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Dict_Keys: {
        auto v = expect_unary("Std.Dict.Keys");
        if (!v) return tl::unexpected(v.error());
        try {
          const auto& obj = fleaux::runtime::as_object(*v.value());
          const auto keys = fleaux::runtime::sorted_dict_keys(obj);
          Array out;
          out.Reserve(keys.size());
          for (const auto& key : keys) { out.PushBack(fleaux::runtime::dict_key_to_value(key)); }
          return std::optional<Value>{Value{std::move(out)}};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Dict_Values: {
        auto v = expect_unary("Std.Dict.Values");
        if (!v) return tl::unexpected(v.error());
        try {
          const auto& obj = fleaux::runtime::as_object(*v.value());
          const auto keys = fleaux::runtime::sorted_dict_keys(obj);
          Array out;
          out.Reserve(keys.size());
          for (const auto& key : keys) {
            if (const auto got = obj.TryGet(key)) out.PushBack(*got);
          }
          return std::optional<Value>{Value{std::move(out)}};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Dict_Length: {
        auto v = expect_unary("Std.Dict.Length");
        if (!v) return tl::unexpected(v.error());
        try {
          return std::optional<Value>{fleaux::runtime::make_int(
              static_cast<fleaux::runtime::Int>(fleaux::runtime::as_object(*v.value()).Size()))};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Println: {
        try {
          fleaux::runtime::print_value_varargs(std::cout, arg);
          std::cout << '\n';
          return std::optional<Value>{arg};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Printf: {
        try {
          const auto& args = fleaux::runtime::as_array(arg);
          if (args.Size() < 1) {
            return tl::unexpected(
                RuntimeError{"native builtin 'Std.Printf' threw: Printf expects at least 1 argument"});
          }
          const std::string fmt = fleaux::runtime::to_string(*args.TryGet(0));
          std::vector<std::string> values;
          values.reserve(args.Size() > 0 ? args.Size() - 1 : 0);
          Array returned_args;
          returned_args.Reserve(args.Size() > 0 ? args.Size() - 1 : 0);
          for (std::size_t i = 1; i < args.Size(); ++i) {
            values.push_back(fleaux::runtime::to_string(*args.TryGet(i)));
            returned_args.PushBack(*args.TryGet(i));
          }
          std::cout << fleaux::runtime::format_values(fmt, values) << '\n';
          return std::optional<Value>{
              fleaux::runtime::make_tuple(fleaux::runtime::make_string(fmt), Value{std::move(returned_args)})};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_GetArgs: {
        if (auto args = expect_n("Std.GetArgs", 0); !args) return tl::unexpected(args.error());
        try {
          Array out;
          const auto& process_args = fleaux::runtime::get_process_args();
          out.Reserve(process_args.size());
          for (const auto& s : process_args) { out.PushBack(fleaux::runtime::make_string(s)); }
          return std::optional<Value>{Value{std::move(out)}};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Input: {
        try {
          auto read_line = []() -> Value {
            std::string line;
            if (!std::getline(std::cin, line)) { return fleaux::runtime::make_string(""); }
            return fleaux::runtime::make_string(line);
          };
          if (!arg.HasArray()) {
            std::cout << fleaux::runtime::to_string(arg);
            std::cout.flush();
            return std::optional<Value>{read_line()};
          }
          const auto& args = fleaux::runtime::as_array(arg);
          if (args.Size() == 0) { return std::optional<Value>{read_line()}; }
          if (args.Size() == 1) {
            std::cout << fleaux::runtime::to_string(*args.TryGet(0));
            std::cout.flush();
            return std::optional<Value>{read_line()};
          }
          return tl::unexpected(RuntimeError{"native builtin 'Std.Input' threw: Input expects 0 or 1 argument"});
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Exit: {
        try {
          if (arg.HasArray()) {
            const auto& args = fleaux::runtime::as_array(arg);
            if (args.Size() == 0) { std::exit(0); }
            if (args.Size() == 1) { std::exit(static_cast<int>(fleaux::runtime::to_double(*args.TryGet(0)))); }
            return tl::unexpected(RuntimeError{"native builtin 'Std.Exit' threw: Exit expects 0 or 1 argument"});
          }
          std::exit(static_cast<int>(fleaux::runtime::to_double(arg)));
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Tuple_Map: {
        auto args = expect_n("Std.Tuple.Map", 2);
        if (!args) return tl::unexpected(args.error());
        const auto seq = (*args)->TryGet(0);
        const auto fn = (*args)->TryGet(1);
        if (!seq || !fn) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.Tuple.Map' argument unpack failed"});
        }
        try {
          const auto& src = fleaux::runtime::as_array(*seq);
          Array out;
          out.Reserve(src.Size());
          for (std::size_t i = 0; i < src.Size(); ++i) {
            out.PushBack(fleaux::runtime::invoke_callable_ref(*fn, *src.TryGet(i)));
          }
          return std::optional<Value>{Value{std::move(out)}};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Tuple_Filter: {
        auto args = expect_n("Std.Tuple.Filter", 2);
        if (!args) return tl::unexpected(args.error());
        const auto seq = (*args)->TryGet(0);
        const auto pred = (*args)->TryGet(1);
        if (!seq || !pred) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.Tuple.Filter' argument unpack failed"});
        }
        try {
          const auto& src = fleaux::runtime::as_array(*seq);
          Array out;
          for (std::size_t i = 0; i < src.Size(); ++i) {
            if (const Value& item = *src.TryGet(i);
                fleaux::runtime::as_bool(fleaux::runtime::invoke_callable_ref(*pred, item))) {
              out.PushBack(item);
            }
          }
          return std::optional<Value>{Value{std::move(out)}};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Tuple_Sort: {
        auto args = expect_n("Std.Tuple.Sort", 1);
        if (!args) return tl::unexpected(args.error());
        const auto seq = (*args)->TryGet(0);
        if (!seq) { return tl::unexpected(RuntimeError{"native builtin 'Std.Tuple.Sort' argument unpack failed"}); }
        try {
          const auto& src = fleaux::runtime::as_array(*seq);
          std::vector<Value> items;
          items.reserve(src.Size());
          for (std::size_t i = 0; i < src.Size(); ++i) { items.push_back(*src.TryGet(i)); }
          std::ranges::stable_sort(items, [](const Value& lhs, const Value& rhs) -> bool {
            return fleaux::runtime::compare_values_for_sort(lhs, rhs) < 0;
          });
          Array out;
          out.Reserve(items.size());
          for (const auto& item : items) { out.PushBack(item); }
          return std::optional<Value>{Value{std::move(out)}};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Tuple_Unique: {
        auto args = expect_n("Std.Tuple.Unique", 1);
        if (!args) return tl::unexpected(args.error());
        const auto seq = (*args)->TryGet(0);
        if (!seq) { return tl::unexpected(RuntimeError{"native builtin 'Std.Tuple.Unique' argument unpack failed"}); }
        try {
          const auto& src = fleaux::runtime::as_array(*seq);
          Array out;
          out.Reserve(src.Size());
          for (std::size_t i = 0; i < src.Size(); ++i) {
            const Value& item = *src.TryGet(i);
            bool seen = false;
            for (std::size_t j = 0; j < out.Size(); ++j) {
              if (*out.TryGet(j) == item) {
                seen = true;
                break;
              }
            }
            if (!seen) out.PushBack(item);
          }
          return std::optional<Value>{Value{std::move(out)}};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Tuple_Min: {
        auto args = expect_n("Std.Tuple.Min", 1);
        if (!args) return tl::unexpected(args.error());
        const auto seq = (*args)->TryGet(0);
        if (!seq) { return tl::unexpected(RuntimeError{"native builtin 'Std.Tuple.Min' argument unpack failed"}); }
        try {
          const auto& src = fleaux::runtime::as_array(*seq);
          if (src.Size() == 0) {
            return tl::unexpected(
                RuntimeError{"native builtin 'Std.Tuple.Min' threw: TupleMin expects non-empty tuple"});
          }
          const Value* best = &*src.TryGet(0);
          for (std::size_t i = 1; i < src.Size(); ++i) {
            const Value& item = *src.TryGet(i);
            if (fleaux::runtime::compare_values_for_sort(item, *best) < 0) { best = &item; }
          }
          return std::optional<Value>{*best};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Tuple_Max: {
        auto args = expect_n("Std.Tuple.Max", 1);
        if (!args) return tl::unexpected(args.error());
        const auto seq = (*args)->TryGet(0);
        if (!seq) { return tl::unexpected(RuntimeError{"native builtin 'Std.Tuple.Max' argument unpack failed"}); }
        try {
          const auto& src = fleaux::runtime::as_array(*seq);
          if (src.Size() == 0) {
            return tl::unexpected(
                RuntimeError{"native builtin 'Std.Tuple.Max' threw: TupleMax expects non-empty tuple"});
          }
          const Value* best = &*src.TryGet(0);
          for (std::size_t i = 1; i < src.Size(); ++i) {
            const Value& item = *src.TryGet(i);
            if (fleaux::runtime::compare_values_for_sort(item, *best) > 0) { best = &item; }
          }
          return std::optional<Value>{*best};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Tuple_Reduce: {
        auto args = expect_n("Std.Tuple.Reduce", 3);
        if (!args) return tl::unexpected(args.error());
        const auto seq = (*args)->TryGet(0);
        const auto initial = (*args)->TryGet(1);
        const auto fn = (*args)->TryGet(2);
        if (!seq || !initial || !fn) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.Tuple.Reduce' argument unpack failed"});
        }
        try {
          const auto& src = fleaux::runtime::as_array(*seq);
          Value acc = *initial;
          for (std::size_t i = 0; i < src.Size(); ++i) {
            acc =
                fleaux::runtime::invoke_callable_ref(*fn, fleaux::runtime::make_tuple(std::move(acc), *src.TryGet(i)));
          }
          return std::optional<Value>{std::move(acc)};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Tuple_FindIndex: {
        auto args = expect_n("Std.Tuple.FindIndex", 2);
        if (!args) return tl::unexpected(args.error());
        const auto seq = (*args)->TryGet(0);
        const auto pred = (*args)->TryGet(1);
        if (!seq || !pred) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.Tuple.FindIndex' argument unpack failed"});
        }
        try {
          const auto& src = fleaux::runtime::as_array(*seq);
          for (std::size_t i = 0; i < src.Size(); ++i) {
            if (fleaux::runtime::as_bool(fleaux::runtime::invoke_callable_ref(*pred, *src.TryGet(i)))) {
              return std::optional<Value>{fleaux::runtime::make_int(static_cast<fleaux::runtime::Int>(i))};
            }
          }
          return std::optional<Value>{fleaux::runtime::make_int(-1)};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Tuple_Any: {
        auto args = expect_n("Std.Tuple.Any", 2);
        if (!args) return tl::unexpected(args.error());
        const auto seq = (*args)->TryGet(0);
        const auto pred = (*args)->TryGet(1);
        if (!seq || !pred) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.Tuple.Any' argument unpack failed"});
        }
        try {
          const auto& src = fleaux::runtime::as_array(*seq);
          for (std::size_t i = 0; i < src.Size(); ++i) {
            if (fleaux::runtime::as_bool(fleaux::runtime::invoke_callable_ref(*pred, *src.TryGet(i)))) {
              return std::optional<Value>{fleaux::runtime::make_bool(true)};
            }
          }
          return std::optional<Value>{fleaux::runtime::make_bool(false)};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Tuple_All: {
        auto args = expect_n("Std.Tuple.All", 2);
        if (!args) return tl::unexpected(args.error());
        const auto seq = (*args)->TryGet(0);
        const auto pred = (*args)->TryGet(1);
        if (!seq || !pred) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.Tuple.All' argument unpack failed"});
        }
        try {
          const auto& src = fleaux::runtime::as_array(*seq);
          for (std::size_t i = 0; i < src.Size(); ++i) {
            if (!fleaux::runtime::as_bool(fleaux::runtime::invoke_callable_ref(*pred, *src.TryGet(i)))) {
              return std::optional<Value>{fleaux::runtime::make_bool(false)};
            }
          }
          return std::optional<Value>{fleaux::runtime::make_bool(true)};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Tuple_Range: {
        try {
          const auto& args = fleaux::runtime::as_array(arg);
          fleaux::runtime::Int start = 0;
          fleaux::runtime::Int stop = 0;
          fleaux::runtime::Int step = 1;
          if (args.Size() == 1) {
            stop = fleaux::runtime::as_int_value(*args.TryGet(0));
          } else if (args.Size() == 2) {
            start = fleaux::runtime::as_int_value(*args.TryGet(0));
            stop = fleaux::runtime::as_int_value(*args.TryGet(1));
          } else if (args.Size() == 3) {
            start = fleaux::runtime::as_int_value(*args.TryGet(0));
            stop = fleaux::runtime::as_int_value(*args.TryGet(1));
            step = fleaux::runtime::as_int_value(*args.TryGet(2));
          } else {
            return tl::unexpected(
                RuntimeError{"native builtin 'Std.Tuple.Range' threw: TupleRange expects 1, 2, or 3 arguments"});
          }
          if (step == 0) {
            return tl::unexpected(RuntimeError{"native builtin 'Std.Tuple.Range' threw: TupleRange step cannot be 0"});
          }
          Array out;
          if (step > 0) {
            for (fleaux::runtime::Int i = start; i < stop; i += step) { out.PushBack(fleaux::runtime::make_int(i)); }
          } else {
            for (fleaux::runtime::Int i = start; i > stop; i += step) { out.PushBack(fleaux::runtime::make_int(i)); }
          }
          return std::optional<Value>{Value{std::move(out)}};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Dict_Contains: {
        auto args = expect_n("Std.Dict.Contains", 2);
        if (!args) return tl::unexpected(args.error());
        const auto dict = (*args)->TryGet(0);
        const auto key = (*args)->TryGet(1);
        if (!dict || !key) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.Dict.Contains' argument unpack failed"});
        }
        try {
          const auto& obj = fleaux::runtime::as_object(*dict);
          return std::optional<Value>{
              fleaux::runtime::make_bool(obj.Contains(fleaux::runtime::dict_key_from_value(*key)))};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Dict_Delete: {
        auto args = expect_n("Std.Dict.Delete", 2);
        if (!args) return tl::unexpected(args.error());
        const auto dict = (*args)->TryGet(0);
        const auto key = (*args)->TryGet(1);
        if (!dict || !key) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.Dict.Delete' argument unpack failed"});
        }
        try {
          auto out = fleaux::runtime::as_object(*dict);
          out.Erase(fleaux::runtime::dict_key_from_value(*key));
          return std::optional<Value>{Value{std::move(out)}};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Dict_Entries: {
        auto v = expect_unary("Std.Dict.Entries");
        if (!v) return tl::unexpected(v.error());
        try {
          const auto& obj = fleaux::runtime::as_object(*v.value());
          const auto keys = fleaux::runtime::sorted_dict_keys(obj);
          Array out;
          out.Reserve(keys.size());
          for (const auto& key : keys) {
            if (const auto got = obj.TryGet(key)) {
              out.PushBack(fleaux::runtime::make_tuple(fleaux::runtime::dict_key_to_value(key), *got));
            }
          }
          return std::optional<Value>{Value{std::move(out)}};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Dict_Clear: {
        auto v = expect_unary("Std.Dict.Clear");
        if (!v) return tl::unexpected(v.error());
        try {
          (void)fleaux::runtime::as_object(*v.value());
          return std::optional<Value>{Value{fleaux::runtime::Object{}}};
        } catch (const std::exception& ex) { return native_error(name, ex); }
        break;
      }
      case BuiltinDispatchKey::kStd_Branch: {
        auto args = expect_n("Std.Branch", 4);
        if (!args) return tl::unexpected(args.error());
        const auto cond = (*args)->TryGet(0);
        const auto val = (*args)->TryGet(1);
        const auto tf = (*args)->TryGet(2);
        const auto ff = (*args)->TryGet(3);
        if (!cond || !val || !tf || !ff) {
          return tl::unexpected(RuntimeError{"native builtin 'Std.Branch' argument unpack failed"});
        }
        try {
          const Value& chosen = fleaux::runtime::as_bool(*cond) ? *tf : *ff;
          return std::optional<Value>{fleaux::runtime::invoke_callable_ref(chosen, *val)};
        } catch (const std::exception& ex) {
          return tl::unexpected(RuntimeError{std::string("native builtin 'Std.Branch' threw: ") + ex.what()});
        }
        break;
      }
      default:
        break;
    }
  }

  static const std::unordered_map<std::string, double> kConstantDispatchTable = [] {
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

tl::expected<Value, RuntimeError> dispatch_builtin(const std::string& name, Value arg,
                                                   const std::unordered_map<std::string, RuntimeCallable>& builtins,
                                                   const bool allow_runtime_fallback) {
  auto native = try_run_vm_native_builtin(name, arg);
  if (!native) return tl::unexpected(native.error());
  if (native->has_value()) { return std::move(**native); }

  if (!allow_runtime_fallback) {
    return tl::unexpected(RuntimeError{"builtin not implemented natively in bytecode VM: '" + name + "'"});
  }

  const auto it = builtins.find(name);
  if (it == builtins.end()) { return tl::unexpected(RuntimeError{"unknown builtin: '" + name + "'"}); }
  try {
    return it->second(std::move(arg));
  } catch (const std::exception& ex) {
    return tl::unexpected(RuntimeError{std::string("builtin '") + name + "' threw: " + ex.what()});
  }
}

}  // namespace

// ── Runtime::execute ──────────────────────────────────────────────────────────

RuntimeResult Runtime::execute(const bytecode::Module& bytecode_module) const {
  return execute(bytecode_module, std::cout);
}

RuntimeResult Runtime::execute(const bytecode::Module& bytecode_module, std::ostream& output) const {
  const auto& builtins = vm_builtin_callables();

  std::vector<Value> stack;
  std::vector<CallFrame> frames;
  frames.push_back(CallFrame{.instructions=&bytecode_module.instructions, .ip=0, .locals={}});

  auto loop_result = run_loop(bytecode_module, stack, frames, builtins, options_.allow_runtime_fallback, output);
  if (!loop_result) return tl::unexpected(loop_result.error());

  if (std::get_if<Value>(&*loop_result) != nullptr) {
    // A top-level kReturn would be a compiler bug.
    return tl::unexpected(RuntimeError{"top-level code returned a value instead of halting"});
  }
  return ExecutionResult{0};
}

Runtime::Runtime(const RuntimeOptions options) : options_(options) {}

}  // namespace fleaux::vm
