#include "fleaux/vm/runtime.hpp"

#include <iostream>
#include <stdexcept>
#include <variant>
#include <vector>

#include "fleaux_runtime.hpp"
#include "builtin_map.hpp"

namespace fleaux::vm {
namespace {

using fleaux::runtime::Array;
using fleaux::runtime::Value;
using fleaux::runtime::RuntimeCallable;

// ── Value helpers ─────────────────────────────────────────────────────────────

Value const_to_value(const bytecode::ConstValue& c) {
  using namespace fleaux::runtime;
  return std::visit(
      [](const auto& v) -> Value {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::int64_t>) return make_int(v);
        if constexpr (std::is_same_v<T, double>)       return make_float(v);
        if constexpr (std::is_same_v<T, bool>)         return make_bool(v);
        if constexpr (std::is_same_v<T, std::string>)  return make_string(v);
        return make_null();
      },
      c.data);
}

// ── Stack helpers ─────────────────────────────────────────────────────────────

tl::expected<Value, RuntimeError> pop_stack(std::vector<Value>& stack,
                                            const char* context) {
  if (stack.empty()) {
    return tl::unexpected(
        RuntimeError{std::string("stack underflow on ") + context});
  }
  Value v = std::move(stack.back());
  stack.pop_back();
  return v;
}

// Extract an int64 from a Value for legacy integer opcodes.
tl::expected<std::int64_t, RuntimeError> extract_int64(const Value& v,
                                                        const char* context) {
  const auto r = v.TryGetNumber();
  if (!r) {
    return tl::unexpected(
        RuntimeError{std::string("expected integer for ") + context});
  }
  return r->Visit(
      [](const fleaux::runtime::Int  i) -> std::int64_t { return i; },
      [](const fleaux::runtime::UInt u) -> std::int64_t {
        return static_cast<std::int64_t>(u);
      },
      [](const fleaux::runtime::Float d) -> std::int64_t {
        return static_cast<std::int64_t>(d);
      });
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

using LoopExit   = std::variant<std::monostate, Value>;
using LoopResult = tl::expected<LoopExit, RuntimeError>;

// Forward declarations.
LoopResult run_loop(const bytecode::Module& module,
                    std::vector<Value>& stack,
                    std::vector<CallFrame>& frames,
                    const std::unordered_map<std::string, RuntimeCallable>& builtins,
                    std::ostream& output);

tl::expected<Value, RuntimeError>
run_user_function(const bytecode::Module& module,
                  std::size_t fn_idx,
                  Value arg,
                  const std::unordered_map<std::string, RuntimeCallable>& builtins,
                  std::ostream& output);

// ── run_user_function ─────────────────────────────────────────────────────────

tl::expected<Value, RuntimeError>
run_user_function(const bytecode::Module& module,
                  const std::size_t fn_idx,
                  Value arg,
                  const std::unordered_map<std::string, RuntimeCallable>& builtins,
                  std::ostream& output) {
  if (fn_idx >= module.functions.size()) {
    return tl::unexpected(RuntimeError{"function index out of range"});
  }
  const auto& fn_def = module.functions[fn_idx];

  std::vector<Value> inner_stack;
  std::vector<CallFrame> inner_frames;

  CallFrame frame;
  frame.instructions = &fn_def.instructions;
  frame.ip = 0;

  if (fn_def.arity == 0) {
    // no locals
  } else if (fn_def.arity == 1) {
    frame.locals.push_back(
        fleaux::runtime::unwrap_singleton_arg(std::move(arg)));
  } else {
    try {
      const auto& arr = fleaux::runtime::as_array(arg);
      frame.locals.reserve(fn_def.arity);
      for (std::uint32_t i = 0; i < fn_def.arity; ++i) {
        auto elem = arr.TryGet(i);
        if (!elem) {
          return tl::unexpected(RuntimeError{
              "too few arguments for '" + fn_def.name + "'"});
        }
        frame.locals.push_back(*elem);
      }
    } catch (const std::exception& ex) {
      return tl::unexpected(RuntimeError{
          std::string("argument unpacking for '") + fn_def.name +
          "': " + ex.what()});
    }
  }

  inner_frames.push_back(std::move(frame));

  auto loop_result = run_loop(module, inner_stack, inner_frames, builtins, output);
  if (!loop_result) return tl::unexpected(loop_result.error());

  if (std::holds_alternative<std::monostate>(*loop_result)) {
    return tl::unexpected(
        RuntimeError{"halt inside function '" + fn_def.name + "'"});
  }
  return std::get<Value>(std::move(*loop_result));
}

// ── run_loop ──────────────────────────────────────────────────────────────────

LoopResult run_loop(const bytecode::Module& module,
                    std::vector<Value>& stack,
                    std::vector<CallFrame>& frames,
                    const std::unordered_map<std::string, RuntimeCallable>& builtins,
                    std::ostream& output) {
  while (!frames.empty()) {
    const std::size_t curr_ip = frames.back().ip;
    frames.back().ip++;

    const auto& instr_list = *frames.back().instructions;
    if (curr_ip >= instr_list.size()) {
      return tl::unexpected(RuntimeError{"program terminated without halt"});
    }

    const auto opcode  = instr_list[curr_ip].opcode;
    const auto operand = instr_list[curr_ip].operand;

    switch (opcode) {

      // ── No-op ───────────────────────────────────────────────────────────
      case bytecode::Opcode::kNoOp:
        break;

      // ── Push ────────────────────────────────────────────────────────────
      case bytecode::Opcode::kPushConstI64:
        stack.push_back(fleaux::runtime::make_int(operand));
        break;

      case bytecode::Opcode::kPushConst: {
        const auto idx = static_cast<std::size_t>(operand);
        if (idx >= module.constants.size()) {
          return tl::unexpected(
              RuntimeError{"constant pool index out of range"});
        }
        stack.push_back(const_to_value(module.constants[idx]));
        break;
      }

      // ── Stack manipulation ────────────────────────────────────────────────
      case bytecode::Opcode::kPop: {
        auto v = pop_stack(stack, "pop");
        if (!v) return tl::unexpected(v.error());
        break;
      }

      case bytecode::Opcode::kDup: {
        if (stack.empty()) {
          return tl::unexpected(RuntimeError{"stack underflow on dup"});
        }
        stack.push_back(stack.back());
        break;
      }

      // ── Tuple construction ────────────────────────────────────────────────
      case bytecode::Opcode::kBuildTuple: {
        const auto n = static_cast<std::size_t>(operand);
        if (stack.size() < n) {
          return tl::unexpected(
              RuntimeError{"stack underflow on build_tuple"});
        }
        Array arr;
        arr.Reserve(n);
        const auto base = stack.size() - n;
        for (std::size_t i = 0; i < n; ++i) {
          arr.PushBack(std::move(stack[base + i]));
        }
        stack.resize(base);
        stack.push_back(Value{std::move(arr)});
        break;
      }

      // ── Legacy integer arithmetic ─────────────────────────────────────────
      case bytecode::Opcode::kAddI64: {
        auto rhs = pop_stack(stack, "add");
        if (!rhs) return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "add");
        if (!lhs) return tl::unexpected(lhs.error());
        auto r = extract_int64(*rhs, "add");
        if (!r) return tl::unexpected(r.error());
        auto l = extract_int64(*lhs, "add");
        if (!l) return tl::unexpected(l.error());
        stack.push_back(fleaux::runtime::make_int(*l + *r));
        break;
      }

      case bytecode::Opcode::kSubI64: {
        auto rhs = pop_stack(stack, "sub");
        if (!rhs) return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "sub");
        if (!lhs) return tl::unexpected(lhs.error());
        auto r = extract_int64(*rhs, "sub");
        if (!r) return tl::unexpected(r.error());
        auto l = extract_int64(*lhs, "sub");
        if (!l) return tl::unexpected(l.error());
        stack.push_back(fleaux::runtime::make_int(*l - *r));
        break;
      }

      case bytecode::Opcode::kMulI64: {
        auto rhs = pop_stack(stack, "mul");
        if (!rhs) return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "mul");
        if (!lhs) return tl::unexpected(lhs.error());
        auto r = extract_int64(*rhs, "mul");
        if (!r) return tl::unexpected(r.error());
        auto l = extract_int64(*lhs, "mul");
        if (!l) return tl::unexpected(l.error());
        stack.push_back(fleaux::runtime::make_int(*l * *r));
        break;
      }

      case bytecode::Opcode::kDivI64: {
        auto rhs = pop_stack(stack, "div");
        if (!rhs) return tl::unexpected(rhs.error());
        auto lhs = pop_stack(stack, "div");
        if (!lhs) return tl::unexpected(lhs.error());
        auto r = extract_int64(*rhs, "div");
        if (!r) return tl::unexpected(r.error());
        auto l = extract_int64(*lhs, "div");
        if (!l) return tl::unexpected(l.error());
        if (*r == 0) return tl::unexpected(RuntimeError{"division by zero"});
        stack.push_back(fleaux::runtime::make_int(*l / *r));
        break;
      }

      // ── Builtin call ──────────────────────────────────────────────────────
      case bytecode::Opcode::kCallBuiltin: {
        const auto idx = static_cast<std::size_t>(operand);
        if (idx >= module.builtin_names.size()) {
          return tl::unexpected(RuntimeError{"builtin index out of range"});
        }
        const auto& name = module.builtin_names[idx];
        const auto it = builtins.find(name);
        if (it == builtins.end()) {
          return tl::unexpected(
              RuntimeError{"unknown builtin: '" + name + "'"});
        }
        auto arg = pop_stack(stack, "call_builtin");
        if (!arg) return tl::unexpected(arg.error());
        try {
          stack.push_back(it->second(std::move(*arg)));
        } catch (const std::exception& ex) {
          return tl::unexpected(RuntimeError{
              std::string("builtin '") + name + "' threw: " + ex.what()});
        }
        break;
      }

      // ── User function call (frame-based) ──────────────────────────────────
      case bytecode::Opcode::kCallUserFunc: {
        const auto fn_idx = static_cast<std::size_t>(operand);
        if (fn_idx >= module.functions.size()) {
          return tl::unexpected(RuntimeError{"function index out of range"});
        }
        auto arg = pop_stack(stack, "call_user_func");
        if (!arg) return tl::unexpected(arg.error());

        const auto& fn_def = module.functions[fn_idx];
        CallFrame new_frame;
        new_frame.instructions = &fn_def.instructions;
        new_frame.ip = 0;

        if (fn_def.arity == 0) {
          // no locals
        } else if (fn_def.arity == 1) {
          new_frame.locals.push_back(
              fleaux::runtime::unwrap_singleton_arg(std::move(*arg)));
        } else {
          try {
            const auto& arr = fleaux::runtime::as_array(*arg);
            new_frame.locals.reserve(fn_def.arity);
            for (std::uint32_t i = 0; i < fn_def.arity; ++i) {
              auto elem = arr.TryGet(i);
              if (!elem) {
                return tl::unexpected(RuntimeError{
                    "too few arguments for '" + fn_def.name + "'"});
              }
              new_frame.locals.push_back(*elem);
            }
          } catch (const std::exception& ex) {
            return tl::unexpected(RuntimeError{
                std::string("argument unpacking for '") + fn_def.name +
                "': " + ex.what()});
          }
        }

        frames.push_back(std::move(new_frame));
        break;
      }

      // ── User function → callable-ref (for higher-order use) ───────────────
      case bytecode::Opcode::kMakeUserFuncRef: {
        const auto fn_idx = static_cast<std::size_t>(operand);
        if (fn_idx >= module.functions.size()) {
          return tl::unexpected(RuntimeError{"function index out of range"});
        }
        // Create a RuntimeCallable that re-enters the VM for this function.
        // Captures by reference — valid because callables are always invoked
        // synchronously (inside kCallBuiltin) within the same run_loop call.
        auto callable = [&module, fn_idx, &builtins, &output](Value call_arg) -> Value {
          auto r = run_user_function(module, fn_idx, std::move(call_arg),
                                     builtins, output);
          if (!r) throw std::runtime_error(r.error().message);
          return std::move(*r);
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
        if (slot >= locals.size()) {
          return tl::unexpected(
              RuntimeError{"local slot index out of range"});
        }
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

      // ── Halt ──────────────────────────────────────────────────────────────
      case bytecode::Opcode::kHalt:
        return LoopExit{std::monostate{}};
    }
  }

  return tl::unexpected(RuntimeError{"program terminated without halt"});
}

}  // namespace

// ── Runtime::execute ──────────────────────────────────────────────────────────

RuntimeResult Runtime::execute(const bytecode::Module& module) const {
  return execute(module, std::cout);
}

RuntimeResult Runtime::execute(const bytecode::Module& module,
                               std::ostream& output) const {
  const auto& builtins = vm_builtin_callables();

  std::vector<Value> stack;
  std::vector<CallFrame> frames;
  frames.push_back(CallFrame{&module.instructions, 0, {}});

  auto loop_result = run_loop(module, stack, frames, builtins, output);
  if (!loop_result) return tl::unexpected(loop_result.error());

  if (std::holds_alternative<Value>(*loop_result)) {
    // A top-level kReturn would be a compiler bug.
    return tl::unexpected(
        RuntimeError{"top-level code returned a value instead of halting"});
  }
  return ExecutionResult{0};
}

}  // namespace fleaux::vm

