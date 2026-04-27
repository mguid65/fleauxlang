#pragma once

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
  // current instruction list — top-level or function body).
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

  // Value-reference intrinsics (appended to preserve existing opcode values
  // for serialized modules):
  // kMakeValueRef pops TOS and stores it in the value registry, pushing a
  // generation-safe value-ref token.
  kMakeValueRef,
  // kDerefValueRef pops a value-ref token and pushes the referenced Value.
  kDerefValueRef,
};

inline std::string_view stringify_opcode(Opcode opcode) {
  constexpr std::array opcode_names{
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
      std::string_view{"MakeValueRef"},
      std::string_view{"DerefValueRef"}
  };

  return opcode_names.at(static_cast<std::size_t>(opcode));
}

}  // namespace fleaux::bytecode
