#pragma once

namespace fleaux::bytecode {

enum class Opcode {
  // ── No-op ─────────────────────────────────────────────────────────────────
  kNoOp,

  // ── Push ──────────────────────────────────────────────────────────────────
  // Push an inlined int64 onto the value stack (backward compat).
  kPushConstI64,
  // Push a typed constant from the module's constant pool (operand = pool index).
  kPushConst,

  // ── Stack manipulation ────────────────────────────────────────────────────
  kPop,  // discard top of stack
  kDup,  // duplicate top of stack

  // ── Tuple construction ────────────────────────────────────────────────────
  // Pop N items off the stack (bottom-first) and push a single Array value.
  // operand = N (number of items).
  kBuildTuple,

  // ── Legacy integer arithmetic (backward compat; operate on Value-wrapped int64) ──
  kAddI64,
  kSubI64,
  kMulI64,
  kDivI64,

  // ── Stdlib builtin call ───────────────────────────────────────────────────
  // Pop the top-of-stack argument, call the named builtin, push result.
  // operand = index into Module::builtin_names.
  kCallBuiltin,

  // ── User-defined function call ────────────────────────────────────────────
  // Pop the top-of-stack argument, enter the user function, push result when done.
  // operand = index into Module::functions.
  kCallUserFunc,

  // ── Return ────────────────────────────────────────────────────────────────
  // Return the top-of-stack value from the current call frame.
  kReturn,

  // ── Local variables ───────────────────────────────────────────────────────
  // Push the value of a local parameter slot (operand = slot index).
  kLoadLocal,

  // ── First-class function references ───────────────────────────────────────
  // Wrap a user-defined function as a callable-ref Value so it can be passed
  // to higher-order builtins (Apply, Branch, Loop, Map, Filter, …).
  // operand = index into Module::functions.
  kMakeUserFuncRef,

  // ── I/O (legacy; uses the injected output stream) ─────────────────────────
  kPrint,

  // ── Control ───────────────────────────────────────────────────────────────
  kHalt,
};

}  // namespace fleaux::bytecode

