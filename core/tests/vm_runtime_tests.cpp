#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/bytecode/module.hpp"
#include "fleaux/bytecode/opcode.hpp"
#include "fleaux/vm/runtime.hpp"

TEST_CASE("VM executes arithmetic bytecode and prints result", "[vm]") {
  fleaux::bytecode::Module module;
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConstI64, 9},
      {fleaux::bytecode::Opcode::kPushConstI64, 3},
      {fleaux::bytecode::Opcode::kDivI64, 0},
      {fleaux::bytecode::Opcode::kPushConstI64, 7},
      {fleaux::bytecode::Opcode::kAddI64, 0},
      {fleaux::bytecode::Opcode::kPrint, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "10\n");
}

TEST_CASE("VM reports division by zero", "[vm]") {
  fleaux::bytecode::Module module;
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConstI64, 10},
      {fleaux::bytecode::Opcode::kPushConstI64, 0},
      {fleaux::bytecode::Opcode::kDivI64, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message == "division by zero");
}

// ---------------------------------------------------------------------------
// kJump: unconditional jump skips instructions.
//
//  [0] kPushConstI64 10
//  [1] kJump 3          → jump to [3], skip [2]
//  [2] kPushConstI64 99 (never reached)
//  [3] kPrint
//  [4] kHalt
// Expected output: "10\n"
// ---------------------------------------------------------------------------
TEST_CASE("VM kJump skips instructions unconditionally", "[vm][jump]") {
  fleaux::bytecode::Module module;
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConstI64, 10},
      {fleaux::bytecode::Opcode::kJump,         3},
      {fleaux::bytecode::Opcode::kPushConstI64, 99},   // skipped
      {fleaux::bytecode::Opcode::kPrint,         0},
      {fleaux::bytecode::Opcode::kHalt,          0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "10\n");
}

// ---------------------------------------------------------------------------
// kJumpIf: jump is taken when TOS is true.
//
//  constants[0] = true
//  [0] kPushConst 0     -> push true
//  [1] kJumpIf 3        -> is true -> jump to [3]
//  [2] kNoOp            (skipped)
//  [3] kPushConstI64 77
//  [4] kPrint
//  [5] kHalt
// Expected output: "77\n"
// ---------------------------------------------------------------------------
TEST_CASE("VM kJumpIf jumps when condition is true", "[vm][jump]") {
  fleaux::bytecode::Module module;
  module.constants.push_back(fleaux::bytecode::ConstValue{true});
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst,     0},   // push true
      {fleaux::bytecode::Opcode::kJumpIf,        3},   // true -> jump to [3]
      {fleaux::bytecode::Opcode::kNoOp,           0},   // skipped
      {fleaux::bytecode::Opcode::kPushConstI64,  77},
      {fleaux::bytecode::Opcode::kPrint,          0},
      {fleaux::bytecode::Opcode::kHalt,           0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "77\n");
}

// ---------------------------------------------------------------------------
// kJumpIf: jump is NOT taken when TOS is false.
//
//  constants[0] = false
//  [0] kPushConstI64 55
//  [1] kPushConst 0      -> push false
//  [2] kJumpIf 5         -> is false -> no jump, continue to [3]
//  [3] kPrint            -> prints 55
//  [4] kHalt
// Expected output: "55\n"
// ---------------------------------------------------------------------------
TEST_CASE("VM kJumpIf falls through when condition is false", "[vm][jump]") {
  fleaux::bytecode::Module module;
  module.constants.push_back(fleaux::bytecode::ConstValue{false});
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConstI64,  55},
      {fleaux::bytecode::Opcode::kPushConst,      0},   // push false
      {fleaux::bytecode::Opcode::kJumpIf,         5},   // false -> no jump
      {fleaux::bytecode::Opcode::kPrint,           0},  // prints 55
      {fleaux::bytecode::Opcode::kHalt,            0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "55\n");
}

TEST_CASE("VM kJumpIfNot jumps when condition is false", "[vm][jump]") {
  fleaux::bytecode::Module module;
  module.constants.push_back(fleaux::bytecode::ConstValue{false});
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst,      0},
      {fleaux::bytecode::Opcode::kJumpIfNot,      3},
      {fleaux::bytecode::Opcode::kPushConstI64,  99},  // skipped
      {fleaux::bytecode::Opcode::kPushConstI64,  42},
      {fleaux::bytecode::Opcode::kPrint,          0},
      {fleaux::bytecode::Opcode::kHalt,           0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "42\n");
}

TEST_CASE("VM reports out-of-range jump target", "[vm][jump]") {
  fleaux::bytecode::Module module;
  module.instructions = {
      {fleaux::bytecode::Opcode::kJump, 3},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message == "jump target out of range");
}

TEST_CASE("VM reports out-of-range kJumpIf target", "[vm][jump]") {
  fleaux::bytecode::Module module;
  module.constants.push_back(fleaux::bytecode::ConstValue{true});
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kJumpIf, 4},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message == "jump_if target out of range");
}

TEST_CASE("VM reports out-of-range kJumpIfNot target", "[vm][jump]") {
  fleaux::bytecode::Module module;
  module.constants.push_back(fleaux::bytecode::ConstValue{false});
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst,   0},
      {fleaux::bytecode::Opcode::kJumpIfNot,   4},
      {fleaux::bytecode::Opcode::kHalt,        0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message == "jump_if_not target out of range");
}

TEST_CASE("VM executes native arithmetic and logical opcodes", "[vm]") {
  fleaux::bytecode::Module module;
  module.constants.push_back(fleaux::bytecode::ConstValue{true});
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"Hello, "}});
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"VM"}});
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConstI64, 10},
      {fleaux::bytecode::Opcode::kPushConstI64,  3},
      {fleaux::bytecode::Opcode::kAdd,           0},
      {fleaux::bytecode::Opcode::kPrint,         0},

      {fleaux::bytecode::Opcode::kPushConst,     1},
      {fleaux::bytecode::Opcode::kPushConst,     2},
      {fleaux::bytecode::Opcode::kAdd,           0},
      {fleaux::bytecode::Opcode::kPrint,         0},

      {fleaux::bytecode::Opcode::kPushConstI64,  2},
      {fleaux::bytecode::Opcode::kPushConstI64,  8},
      {fleaux::bytecode::Opcode::kPow,           0},
      {fleaux::bytecode::Opcode::kPrint,         0},

      {fleaux::bytecode::Opcode::kPushConstI64,  2},
      {fleaux::bytecode::Opcode::kPushConstI64,  8},
      {fleaux::bytecode::Opcode::kCmpLt,         0},
      {fleaux::bytecode::Opcode::kPrint,         0},

      {fleaux::bytecode::Opcode::kPushConst,     0},
      {fleaux::bytecode::Opcode::kNot,           0},
      {fleaux::bytecode::Opcode::kPrint,         0},

      {fleaux::bytecode::Opcode::kPushConst,     0},
      {fleaux::bytecode::Opcode::kPushConst,     0},
      {fleaux::bytecode::Opcode::kAnd,           0},
      {fleaux::bytecode::Opcode::kPrint,         0},
      {fleaux::bytecode::Opcode::kHalt,          0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "13\nHello, VM\n256\nTrue\nFalse\nTrue\n");
}

TEST_CASE("VM executes native kSelect opcode", "[vm]") {
  fleaux::bytecode::Module module;
  module.constants.push_back(fleaux::bytecode::ConstValue{true});
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst,     0},
      {fleaux::bytecode::Opcode::kPushConstI64, 10},
      {fleaux::bytecode::Opcode::kPushConstI64, 20},
      {fleaux::bytecode::Opcode::kSelect,        0},
      {fleaux::bytecode::Opcode::kPrint,         0},
      {fleaux::bytecode::Opcode::kHalt,          0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "10\n");
}

TEST_CASE("VM executes native kBranchCall opcode", "[vm]") {
  fleaux::bytecode::Module module;
  module.constants.push_back(fleaux::bytecode::ConstValue{false});

  fleaux::bytecode::FunctionDef add1;
  add1.name = "AddOne";
  add1.arity = 1;
  add1.instructions = {
      {fleaux::bytecode::Opcode::kLoadLocal,     0},
      {fleaux::bytecode::Opcode::kPushConstI64,  1},
      {fleaux::bytecode::Opcode::kAdd,           0},
      {fleaux::bytecode::Opcode::kReturn,        0},
  };

  fleaux::bytecode::FunctionDef sub1;
  sub1.name = "SubOne";
  sub1.arity = 1;
  sub1.instructions = {
      {fleaux::bytecode::Opcode::kLoadLocal,     0},
      {fleaux::bytecode::Opcode::kPushConstI64,  1},
      {fleaux::bytecode::Opcode::kSub,           0},
      {fleaux::bytecode::Opcode::kReturn,        0},
  };

  module.functions.push_back(std::move(add1));
  module.functions.push_back(std::move(sub1));

  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst,        0},
      {fleaux::bytecode::Opcode::kPushConstI64,    10},
      {fleaux::bytecode::Opcode::kMakeUserFuncRef,  0},
      {fleaux::bytecode::Opcode::kMakeUserFuncRef,  1},
      {fleaux::bytecode::Opcode::kBranchCall,       0},
      {fleaux::bytecode::Opcode::kPrint,            0},
      {fleaux::bytecode::Opcode::kHalt,             0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "9\n");
}

TEST_CASE("VM executes kMakeBuiltinFuncRef with native kBranchCall", "[vm]") {
  fleaux::bytecode::Module module;
  module.constants.push_back(fleaux::bytecode::ConstValue{true});
  module.builtin_names = {
      "Std.UnaryMinus",
      "Std.UnaryPlus",
  };

  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst,           0},
      {fleaux::bytecode::Opcode::kPushConstI64,       10},
      {fleaux::bytecode::Opcode::kMakeBuiltinFuncRef,  0},
      {fleaux::bytecode::Opcode::kMakeBuiltinFuncRef,  1},
      {fleaux::bytecode::Opcode::kBranchCall,          0},
      {fleaux::bytecode::Opcode::kPrint,               0},
      {fleaux::bytecode::Opcode::kHalt,                0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "-10\n");
}

TEST_CASE("VM executes native kLoopCall opcode", "[vm]") {
  fleaux::bytecode::Module module;

  fleaux::bytecode::FunctionDef continue_fn;
  continue_fn.name = "Continue";
  continue_fn.arity = 1;
  continue_fn.instructions = {
      {fleaux::bytecode::Opcode::kLoadLocal,     0},
      {fleaux::bytecode::Opcode::kPushConstI64,  0},
      {fleaux::bytecode::Opcode::kCmpGt,         0},
      {fleaux::bytecode::Opcode::kReturn,        0},
  };

  fleaux::bytecode::FunctionDef step_fn;
  step_fn.name = "Step";
  step_fn.arity = 1;
  step_fn.instructions = {
      {fleaux::bytecode::Opcode::kLoadLocal,     0},
      {fleaux::bytecode::Opcode::kPushConstI64,  1},
      {fleaux::bytecode::Opcode::kSub,           0},
      {fleaux::bytecode::Opcode::kReturn,        0},
  };

  module.functions.push_back(std::move(continue_fn));
  module.functions.push_back(std::move(step_fn));

  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConstI64,    3},
      {fleaux::bytecode::Opcode::kMakeUserFuncRef, 0},
      {fleaux::bytecode::Opcode::kMakeUserFuncRef, 1},
      {fleaux::bytecode::Opcode::kLoopCall,        0},
      {fleaux::bytecode::Opcode::kPrint,           0},
      {fleaux::bytecode::Opcode::kHalt,            0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "0\n");
}

TEST_CASE("VM executes native kLoopNCall opcode", "[vm]") {
  fleaux::bytecode::Module module;

  fleaux::bytecode::FunctionDef continue_fn;
  continue_fn.name = "Continue";
  continue_fn.arity = 1;
  continue_fn.instructions = {
      {fleaux::bytecode::Opcode::kLoadLocal,     0},
      {fleaux::bytecode::Opcode::kPushConstI64,  0},
      {fleaux::bytecode::Opcode::kCmpGt,         0},
      {fleaux::bytecode::Opcode::kReturn,        0},
  };

  fleaux::bytecode::FunctionDef step_fn;
  step_fn.name = "Step";
  step_fn.arity = 1;
  step_fn.instructions = {
      {fleaux::bytecode::Opcode::kLoadLocal,     0},
      {fleaux::bytecode::Opcode::kPushConstI64,  1},
      {fleaux::bytecode::Opcode::kSub,           0},
      {fleaux::bytecode::Opcode::kReturn,        0},
  };

  module.functions.push_back(std::move(continue_fn));
  module.functions.push_back(std::move(step_fn));

  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConstI64,    3},
      {fleaux::bytecode::Opcode::kMakeUserFuncRef, 0},
      {fleaux::bytecode::Opcode::kMakeUserFuncRef, 1},
      {fleaux::bytecode::Opcode::kPushConstI64,   10},
      {fleaux::bytecode::Opcode::kLoopNCall,       0},
      {fleaux::bytecode::Opcode::kPrint,           0},
      {fleaux::bytecode::Opcode::kHalt,            0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "0\n");
}

TEST_CASE("VM reports native kLoopNCall max-iteration failure", "[vm]") {
  fleaux::bytecode::Module module;

  fleaux::bytecode::FunctionDef continue_fn;
  continue_fn.name = "Continue";
  continue_fn.arity = 1;
  continue_fn.instructions = {
      {fleaux::bytecode::Opcode::kLoadLocal,     0},
      {fleaux::bytecode::Opcode::kPushConstI64,  0},
      {fleaux::bytecode::Opcode::kCmpGt,         0},
      {fleaux::bytecode::Opcode::kReturn,        0},
  };

  fleaux::bytecode::FunctionDef step_fn;
  step_fn.name = "Step";
  step_fn.arity = 1;
  step_fn.instructions = {
      {fleaux::bytecode::Opcode::kLoadLocal,     0},
      {fleaux::bytecode::Opcode::kPushConstI64,  1},
      {fleaux::bytecode::Opcode::kSub,           0},
      {fleaux::bytecode::Opcode::kReturn,        0},
  };

  module.functions.push_back(std::move(continue_fn));
  module.functions.push_back(std::move(step_fn));

  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConstI64,    3},
      {fleaux::bytecode::Opcode::kMakeUserFuncRef, 0},
      {fleaux::bytecode::Opcode::kMakeUserFuncRef, 1},
      {fleaux::bytecode::Opcode::kPushConstI64,    1},
      {fleaux::bytecode::Opcode::kLoopNCall,       0},
      {fleaux::bytecode::Opcode::kHalt,            0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message == "native 'loop_n_call' threw: LoopN: exceeded max_iters");
}

TEST_CASE("VM reports missing halt", "[vm]") {
  fleaux::bytecode::Module module;
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConstI64, 1},
      {fleaux::bytecode::Opcode::kPushConstI64, 2},
      {fleaux::bytecode::Opcode::kAddI64, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message == "program terminated without halt");
}

