#pragma once

#include <array>
#include <cstddef>
#include <string_view>

namespace fleaux::bytecode {

enum class Opcode {
  // No-op
  kNoOp,

  // Push:
  // Push a typed constant from the module's constant pool (operand = pool index).
  kPushConst,

  // Stack manipulation:
  kPop,  // discard top of stack
  kDup,  // duplicate top of stack

  // Tuple construction:
  // Pop N items off the stack (bottom-first) and push a single Array value.
  // operand = N (number of items).
  kBuildTuple,

  // Stdlib builtin call:
  // Pop the top-of-stack argument, call the builtin identified by operand,
  // push result.
  // operand = BuiltinId value.
  kCallBuiltin,

  // User-defined function call:
  // Pop the top-of-stack argument, enter the user function, push result when done.
  // operand = index into Module::functions.
  kCallUserFunc,

  // Return:
  // Return the top-of-stack value from the current call frame.
  kReturn,

  // Local variables:
  // Push the value of a local parameter slot (operand = slot index).
  kLoadLocal,

  // First-class function references:
  // Wrap a user-defined function as a callable-ref Value so it can be passed
  // to higher-order builtins (Apply, Branch, Loop, Map, Filter, …).
  // operand = index into Module::functions.
  kMakeUserFuncRef,
  // Wrap a stdlib builtin as a callable-ref Value for higher-order use.
  // operand = BuiltinId value.
  kMakeBuiltinFuncRef,
  // Materialize an inline closure as a callable-ref, capturing the current
  // lexical values from the stack-built capture tuple.
  // operand = index into Module::closures.
  kMakeClosureRef,

  // Unconditional jump:
  // Set ip to the absolute instruction index given by operand (within the
  // current instruction list: top-level or function body).
  kJump,

  // Conditional jump (on truthy top-of-stack):
  // Pop TOS. If it is truthy, set ip to the absolute instruction index given
  // by operand. If falsy, continue normally.
  kJumpIf,

  // Conditional jump (on falsy top-of-stack):
  // Pop TOS. If it is falsy, set ip to operand. If truthy, continue normally.
  kJumpIfNot,

  // Native binary arithmetic:
  // Pop rhs then lhs; push result. Works on Value (numeric or string for kAdd).
  kAdd,  // numeric add; string concat when both sides are strings
  kSub,  // numeric subtract
  kMul,  // numeric multiply
  kDiv,  // numeric divide
  kMod,  // numeric modulo (fmod)
  kPow,  // numeric power

  // Native unary arithmetic:
  // Pop TOS; push result.
  kNeg,  // unary numeric negate  (-x)

  // Native binary comparison:
  // Pop rhs then lhs; push bool result.
  kCmpEq,  // structural equality  (==)
  kCmpNe,  // structural inequality (!=)
  kCmpLt,  // numeric less-than    (<)
  kCmpGt,  // numeric greater-than (>)
  kCmpLe,  // numeric less-or-equal    (<=)
  kCmpGe,  // numeric greater-or-equal (>=)

  // Native logical:
  // Binary: pop rhs then lhs; push bool.
  kAnd,  // logical and  (&&)
  kOr,   // logical or   (||)
  // Unary: pop TOS; push bool.
  kNot,  // logical not  (!)

  // Native control-flow intrinsics:
  // Select: pop false_value, true_value, condition; push chosen value.
  kSelect,
  // Branch: pop false_func_ref, true_func_ref, value, condition;
  // invoke chosen function with value; push result.
  kBranchCall,
  // Loop: pop step_func_ref, continue_func_ref, state; push final state.
  kLoopCall,
  // LoopN: pop max_iters, step_func_ref, continue_func_ref, state;
  // push final state or error if max_iters is exceeded.
  kLoopNCall,

  // Control
  kHalt,

  // Local variable store:
  // Pop TOS and write it into the local slot identified by operand, growing the
  // current frame's local storage if needed.
  kStoreLocal,

  // User-defined function call with its arguments on the stack:
  // Enter the non-variadic user function identified by operand, which takes two or more parameters, and bind the top
  // values of the stack to them, the first argument deepest, without building an argument tuple.
  // operand = index into Module::functions.
  kCallUserFuncArgs,

  // Runtime type checks for casts from Any (operand = index into Module::type_checks):
  // kCheckType leaves TOS in place when it matches the check and is a runtime error otherwise.
  kCheckType,
  // kTryCheckType pops TOS and pushes Ok(value) when it matches the check, otherwise Err(message).
  kTryCheckType,

  // Concatenation of argument pieces for a parameter list with spreads: pops N tuples (bottom-first) and pushes one
  // tuple of all their items in order. operand = N.
  kConcatTuples,

  // Native function call: pops the argument tuple, calls the extension function bound to the native function record,
  // checks its result against the record's return check, and pushes the result.
  // operand = index into Module::native_functions.
  kCallNative,

  // Std.ElementAt: pops index, pops tuple; pushes the tuple's item at index.
  kElementAt,

  // Tuple construction with a count known only at run time: pops the count, then that many values (bottom-first),
  // and pushes one tuple of them.
  kBuildTupleCounted,

  // One step of a loop over a tuple. The tuple is in the local slot `operand` and the UInt64 index of the next item in
  // the slot after it. While items remain, pushes the next item and then true, and advances the index; after the last
  // item, pushes false.
  kIterate,

  // The end of a Std.Match compiled in place that has no wildcard case: no case matched, a runtime error.
  kMatchFailed,

  // Native bitwise operations on integers: pop rhs then lhs; push the result (Std.Bit.And, Or, Xor, ShiftLeft,
  // ShiftRight).
  kBitAnd,
  kBitOr,
  kBitXor,
  kShiftLeft,
  kShiftRight,

  // Calls of function values (Std.Apply):
  // kCallFunctionValue pops the function value, then its argument: the argument itself for a function of one
  // parameter, the tuple of the arguments otherwise. It pushes the result.
  kCallFunctionValue,
  // kCallFunctionValueArgs pops the function value, then operand (two or more) arguments, the first deepest. The call
  // is the same as with the tuple of the arguments, which is not built when the function value takes that many
  // parameters. It pushes the result.
  // A function value the VM made for the running module runs as a frame in the same loop; any other is called
  // through Callable::call.
  kCallFunctionValueArgs,
  // The last read of a local slot: pushes the value of the slot (operand = slot index) and leaves the slot null. The
  // compiler turns a kLoadLocal into one when nothing reads the slot again before writing it (local_liveness.hpp), so
  // that a value held only by a slot reaches the next operation as its only reference.
  kTakeLocal,
  // Stdlib builtin call with its arguments on the stack: calls the builtin on the top values of the stack, the first
  // argument deepest, pops them, and pushes the result. For the builtins that take runtime::Arguments: all but those of
  // one parameter (vm::builtin_takes_arguments).
  // operand = vm::builtin_operand_with_arguments(builtin, argument count).
  kCallBuiltinArgs,
};

inline constexpr std::array opcode_names{
  std::string_view{"NoOp"},
  std::string_view{"PushConst"},
  std::string_view{"Pop"},
  std::string_view{"Dup"},
  std::string_view{"BuildTuple"},
  std::string_view{"CallBuiltin"},
  std::string_view{"CallUserFunc"},
  std::string_view{"Return"},
  std::string_view{"LoadLocal"},
  std::string_view{"MakeUserFuncRef"},
  std::string_view{"MakeBuiltinFuncRef"},
  std::string_view{"MakeClosureRef"},
  std::string_view{"Jump"},
  std::string_view{"JumpIf"},
  std::string_view{"JumpIfNot"},
  std::string_view{"Add"},
  std::string_view{"Sub"},
  std::string_view{"Mul"},
  std::string_view{"Div"},
  std::string_view{"Mod"},
  std::string_view{"Pow"},
  std::string_view{"Neg"},
  std::string_view{"CmpEq"},
  std::string_view{"CmpNe"},
  std::string_view{"CmpLt"},
  std::string_view{"CmpGt"},
  std::string_view{"CmpLe"},
  std::string_view{"CmpGe"},
  std::string_view{"And"},
  std::string_view{"Or"},
  std::string_view{"Not"},
  std::string_view{"Select"},
  std::string_view{"BranchCall"},
  std::string_view{"LoopCall"},
  std::string_view{"LoopNCall"},
  std::string_view{"Halt"},
  std::string_view{"StoreLocal"},
  std::string_view{"CallUserFuncArgs"},
  std::string_view{"CheckType"},
  std::string_view{"TryCheckType"},
  std::string_view{"ConcatTuples"},
  std::string_view{"CallNative"},
  std::string_view{"ElementAt"},
  std::string_view{"BuildTupleCounted"},
  std::string_view{"Iterate"},
  std::string_view{"MatchFailed"},
  std::string_view{"BitAnd"},
  std::string_view{"BitOr"},
  std::string_view{"BitXor"},
  std::string_view{"ShiftLeft"},
  std::string_view{"ShiftRight"},
  std::string_view{"CallFunctionValue"},
  std::string_view{"CallFunctionValueArgs"},
  std::string_view{"TakeLocal"},
  std::string_view{"CallBuiltinArgs"}
};

[[nodiscard]] inline constexpr auto opcode_index_is_valid(const std::size_t opcode_index) -> bool {
  return opcode_index < opcode_names.size();
}

[[nodiscard]] inline constexpr auto is_valid_opcode(const Opcode opcode) -> bool {
  return opcode_index_is_valid(static_cast<std::size_t>(opcode));
}

inline auto stringify_opcode(Opcode opcode) -> std::string_view {
  if (!is_valid_opcode(opcode)) {
    return "<invalid-opcode>";
  }
  return opcode_names[static_cast<std::size_t>(opcode)];
}

}  // namespace fleaux::bytecode
