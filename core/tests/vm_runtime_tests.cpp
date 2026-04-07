#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "fleaux/bytecode/module.hpp"
#include "fleaux/bytecode/opcode.hpp"
#include "fleaux/vm/runtime.hpp"

namespace {

std::int64_t push_i64_const(fleaux::bytecode::Module& module, const std::int64_t value) {
  module.constants.push_back(fleaux::bytecode::ConstValue{value});
  return static_cast<std::int64_t>(module.constants.size() - 1);
}

}  // namespace

TEST_CASE("VM executes arithmetic bytecode and prints result", "[vm]") {
  fleaux::bytecode::Module module;
  const auto c9 = push_i64_const(module, 9);
  const auto c3 = push_i64_const(module, 3);
  const auto c7 = push_i64_const(module, 7);
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, c9},
      {fleaux::bytecode::Opcode::kPushConst, c3},
      {fleaux::bytecode::Opcode::kDiv, 0},
      {fleaux::bytecode::Opcode::kPushConst, c7},
      {fleaux::bytecode::Opcode::kAdd, 0},
      {fleaux::bytecode::Opcode::kPrint, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "10\n");
}

// ---------------------------------------------------------------------------
// kJump: unconditional jump skips instructions.
//
//  [0] kPushConst idx(10)
//  [1] kJump 3          → jump to [3], skip [2]
//  [2] kPushConst idx(99) (never reached)
//  [3] kPrint
//  [4] kHalt
// Expected output: "10\n"
// ---------------------------------------------------------------------------
TEST_CASE("VM kJump skips instructions unconditionally", "[vm][jump]") {
  fleaux::bytecode::Module module;
  const auto c10 = push_i64_const(module, 10);
  const auto c99 = push_i64_const(module, 99);
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, c10},
      {fleaux::bytecode::Opcode::kJump,         3},
      {fleaux::bytecode::Opcode::kPushConst, c99},   // skipped
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
//  [3] kPushConst 1
//  [4] kPrint
//  [5] kHalt
// Expected output: "77\n"
// ---------------------------------------------------------------------------
TEST_CASE("VM kJumpIf jumps when condition is true", "[vm][jump]") {
  fleaux::bytecode::Module module;
  module.constants.push_back(fleaux::bytecode::ConstValue{true});
  const auto c77 = push_i64_const(module, 77);
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst,     0},   // push true
      {fleaux::bytecode::Opcode::kJumpIf,        3},   // true -> jump to [3]
      {fleaux::bytecode::Opcode::kNoOp,           0},   // skipped
      {fleaux::bytecode::Opcode::kPushConst,     c77},
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
//  [0] kPushConst 1
//  [1] kPushConst 0      -> push false
//  [2] kJumpIf 5         -> is false -> no jump, continue to [3]
//  [3] kPrint            -> prints 55
//  [4] kHalt
// Expected output: "55\n"
// ---------------------------------------------------------------------------
TEST_CASE("VM kJumpIf falls through when condition is false", "[vm][jump]") {
  fleaux::bytecode::Module module;
  module.constants.push_back(fleaux::bytecode::ConstValue{false});
  const auto c55 = push_i64_const(module, 55);
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst,      c55},
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
  const auto c99 = push_i64_const(module, 99);
  const auto c42 = push_i64_const(module, 42);
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst,      0},
      {fleaux::bytecode::Opcode::kJumpIfNot,      3},
      {fleaux::bytecode::Opcode::kPushConst,     c99},  // skipped
      {fleaux::bytecode::Opcode::kPushConst,     c42},
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
  const auto c10 = push_i64_const(module, 10);
  const auto c3 = push_i64_const(module, 3);
  const auto c2 = push_i64_const(module, 2);
  const auto c8 = push_i64_const(module, 8);
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst,     c10},
      {fleaux::bytecode::Opcode::kPushConst,      c3},
      {fleaux::bytecode::Opcode::kAdd,           0},
      {fleaux::bytecode::Opcode::kPrint,         0},

      {fleaux::bytecode::Opcode::kPushConst,     1},
      {fleaux::bytecode::Opcode::kPushConst,     2},
      {fleaux::bytecode::Opcode::kAdd,           0},
      {fleaux::bytecode::Opcode::kPrint,         0},

      {fleaux::bytecode::Opcode::kPushConst,      c2},
      {fleaux::bytecode::Opcode::kPushConst,      c8},
      {fleaux::bytecode::Opcode::kPow,           0},
      {fleaux::bytecode::Opcode::kPrint,         0},

      {fleaux::bytecode::Opcode::kPushConst,      c2},
      {fleaux::bytecode::Opcode::kPushConst,      c8},
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
  const auto c10 = push_i64_const(module, 10);
  const auto c20 = push_i64_const(module, 20);
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst,     0},
      {fleaux::bytecode::Opcode::kPushConst,    c10},
      {fleaux::bytecode::Opcode::kPushConst,    c20},
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
  const auto c1 = push_i64_const(module, 1);
  const auto c10 = push_i64_const(module, 10);

  fleaux::bytecode::FunctionDef add1;
  add1.name = "AddOne";
  add1.arity = 1;
  add1.instructions = {
      {fleaux::bytecode::Opcode::kLoadLocal,     0},
      {fleaux::bytecode::Opcode::kPushConst,     c1},
      {fleaux::bytecode::Opcode::kAdd,           0},
      {fleaux::bytecode::Opcode::kReturn,        0},
  };

  fleaux::bytecode::FunctionDef sub1;
  sub1.name = "SubOne";
  sub1.arity = 1;
  sub1.instructions = {
      {fleaux::bytecode::Opcode::kLoadLocal,     0},
      {fleaux::bytecode::Opcode::kPushConst,     c1},
      {fleaux::bytecode::Opcode::kSub,           0},
      {fleaux::bytecode::Opcode::kReturn,        0},
  };

  module.functions.push_back(std::move(add1));
  module.functions.push_back(std::move(sub1));

  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst,        0},
      {fleaux::bytecode::Opcode::kPushConst,      c10},
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
  const auto c10 = push_i64_const(module, 10);
  module.builtin_names = {
      "Std.UnaryMinus",
      "Std.UnaryPlus",
  };

  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst,           0},
      {fleaux::bytecode::Opcode::kPushConst,         c10},
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
  const auto c0 = push_i64_const(module, 0);
  const auto c1 = push_i64_const(module, 1);
  const auto c3 = push_i64_const(module, 3);

  fleaux::bytecode::FunctionDef continue_fn;
  continue_fn.name = "Continue";
  continue_fn.arity = 1;
  continue_fn.instructions = {
      {fleaux::bytecode::Opcode::kLoadLocal,     0},
      {fleaux::bytecode::Opcode::kPushConst,     c0},
      {fleaux::bytecode::Opcode::kCmpGt,         0},
      {fleaux::bytecode::Opcode::kReturn,        0},
  };

  fleaux::bytecode::FunctionDef step_fn;
  step_fn.name = "Step";
  step_fn.arity = 1;
  step_fn.instructions = {
      {fleaux::bytecode::Opcode::kLoadLocal,     0},
      {fleaux::bytecode::Opcode::kPushConst,     c1},
      {fleaux::bytecode::Opcode::kSub,           0},
      {fleaux::bytecode::Opcode::kReturn,        0},
  };

  module.functions.push_back(std::move(continue_fn));
  module.functions.push_back(std::move(step_fn));

  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst,      c3},
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
  const auto c0 = push_i64_const(module, 0);
  const auto c1 = push_i64_const(module, 1);
  const auto c3 = push_i64_const(module, 3);
  const auto c10 = push_i64_const(module, 10);

  fleaux::bytecode::FunctionDef continue_fn;
  continue_fn.name = "Continue";
  continue_fn.arity = 1;
  continue_fn.instructions = {
      {fleaux::bytecode::Opcode::kLoadLocal,     0},
      {fleaux::bytecode::Opcode::kPushConst,     c0},
      {fleaux::bytecode::Opcode::kCmpGt,         0},
      {fleaux::bytecode::Opcode::kReturn,        0},
  };

  fleaux::bytecode::FunctionDef step_fn;
  step_fn.name = "Step";
  step_fn.arity = 1;
  step_fn.instructions = {
      {fleaux::bytecode::Opcode::kLoadLocal,     0},
      {fleaux::bytecode::Opcode::kPushConst,     c1},
      {fleaux::bytecode::Opcode::kSub,           0},
      {fleaux::bytecode::Opcode::kReturn,        0},
  };

  module.functions.push_back(std::move(continue_fn));
  module.functions.push_back(std::move(step_fn));

  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst,      c3},
      {fleaux::bytecode::Opcode::kMakeUserFuncRef, 0},
      {fleaux::bytecode::Opcode::kMakeUserFuncRef, 1},
      {fleaux::bytecode::Opcode::kPushConst,     c10},
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
  const auto c0 = push_i64_const(module, 0);
  const auto c1 = push_i64_const(module, 1);
  const auto c3 = push_i64_const(module, 3);

  fleaux::bytecode::FunctionDef continue_fn;
  continue_fn.name = "Continue";
  continue_fn.arity = 1;
  continue_fn.instructions = {
      {fleaux::bytecode::Opcode::kLoadLocal,     0},
      {fleaux::bytecode::Opcode::kPushConst,     c0},
      {fleaux::bytecode::Opcode::kCmpGt,         0},
      {fleaux::bytecode::Opcode::kReturn,        0},
  };

  fleaux::bytecode::FunctionDef step_fn;
  step_fn.name = "Step";
  step_fn.arity = 1;
  step_fn.instructions = {
      {fleaux::bytecode::Opcode::kLoadLocal,     0},
      {fleaux::bytecode::Opcode::kPushConst,     c1},
      {fleaux::bytecode::Opcode::kSub,           0},
      {fleaux::bytecode::Opcode::kReturn,        0},
  };

  module.functions.push_back(std::move(continue_fn));
  module.functions.push_back(std::move(step_fn));

  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst,      c3},
      {fleaux::bytecode::Opcode::kMakeUserFuncRef, 0},
      {fleaux::bytecode::Opcode::kMakeUserFuncRef, 1},
      {fleaux::bytecode::Opcode::kPushConst,      c1},
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
  const auto c1 = push_i64_const(module, 1);
  const auto c2 = push_i64_const(module, 2);
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, c1},
      {fleaux::bytecode::Opcode::kPushConst, c2},
      {fleaux::bytecode::Opcode::kAdd, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message == "program terminated without halt");
}

TEST_CASE("VM native kDiv by zero returns floating result", "[vm]") {
  fleaux::bytecode::Module module;
  const auto c10 = push_i64_const(module, 10);
  const auto c0 = push_i64_const(module, 0);
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, c10},
      {fleaux::bytecode::Opcode::kPushConst, c0},
      {fleaux::bytecode::Opcode::kDiv, 0},
      {fleaux::bytecode::Opcode::kPrint, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());

  std::string lowered = output.str();
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  REQUIRE(lowered.find("inf") != std::string::npos);
}

TEST_CASE("VM native kMod by zero returns NaN-like result", "[vm]") {
  fleaux::bytecode::Module module;
  const auto c10 = push_i64_const(module, 10);
  const auto c0 = push_i64_const(module, 0);
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, c10},
      {fleaux::bytecode::Opcode::kPushConst, c0},
      {fleaux::bytecode::Opcode::kMod, 0},
      {fleaux::bytecode::Opcode::kPrint, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());

  std::string lowered = output.str();
  std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  REQUIRE(lowered.find("nan") != std::string::npos);
}

TEST_CASE("VM native NaN is not equal to itself", "[vm]") {
  fleaux::bytecode::Module module;
  const auto c0 = push_i64_const(module, 0);
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, c0},
      {fleaux::bytecode::Opcode::kPushConst, c0},
      {fleaux::bytecode::Opcode::kDiv, 0},  // NaN
      {fleaux::bytecode::Opcode::kDup, 0},
      {fleaux::bytecode::Opcode::kCmpEq, 0},
      {fleaux::bytecode::Opcode::kPrint, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "False\n");
}

TEST_CASE("VM kCallBuiltin uses native dispatch for Std.Add", "[vm]") {
  fleaux::bytecode::Module module;
  const auto c4 = push_i64_const(module, 4);
  const auto c5 = push_i64_const(module, 5);
  module.builtin_names = {"Std.Add"};
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, c4},
      {fleaux::bytecode::Opcode::kPushConst, c5},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 0},
      {fleaux::bytecode::Opcode::kPrint, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "9\n");
}

TEST_CASE("VM kCallBuiltin falls back for unported builtin", "[vm]") {
  fleaux::bytecode::Module module;
  const auto c8 = push_i64_const(module, 8);
  module.builtin_names = {"Std.Println"};
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, c8},
      {fleaux::bytecode::Opcode::kCallBuiltin, 0},
      {fleaux::bytecode::Opcode::kPop, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
}

TEST_CASE("VM strict mode executes native Std.Println", "[vm]") {
  fleaux::bytecode::Module module;
  const auto c8 = push_i64_const(module, 8);
  module.builtin_names = {"Std.Println"};
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, c8},
      {fleaux::bytecode::Opcode::kCallBuiltin, 0},
      {fleaux::bytecode::Opcode::kPop, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime(
      fleaux::vm::RuntimeOptions{.allow_runtime_fallback = false});
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
}

TEST_CASE("VM kCallBuiltin uses native dispatch for comparison and logical builtins", "[vm]") {
  fleaux::bytecode::Module module;
  const auto c2 = push_i64_const(module, 2);
  const auto c8 = push_i64_const(module, 8);
  module.constants.push_back(fleaux::bytecode::ConstValue{true});
  module.builtin_names = {"Std.LessThan", "Std.And", "Std.Println"};
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, c2},
      {fleaux::bytecode::Opcode::kPushConst, c8},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 0},
      {fleaux::bytecode::Opcode::kPushConst, 2},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 1},
      {fleaux::bytecode::Opcode::kCallBuiltin, 2},
      {fleaux::bytecode::Opcode::kPop, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
}

TEST_CASE("VM kCallBuiltin uses native dispatch for unary builtins", "[vm]") {
  fleaux::bytecode::Module module;
  const auto c10 = push_i64_const(module, 10);
  module.builtin_names = {"Std.UnaryMinus", "Std.UnaryPlus"};
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, c10},
      {fleaux::bytecode::Opcode::kCallBuiltin, 0},
      {fleaux::bytecode::Opcode::kPrint, 0},
      {fleaux::bytecode::Opcode::kPushConst, c10},
      {fleaux::bytecode::Opcode::kBuildTuple, 1},
      {fleaux::bytecode::Opcode::kCallBuiltin, 1},
      {fleaux::bytecode::Opcode::kPrint, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "-10\n10\n");
}

TEST_CASE("VM kCallBuiltin uses native dispatch for Std.Select", "[vm]") {
  fleaux::bytecode::Module module;
  module.constants.push_back(fleaux::bytecode::ConstValue{false});
  const auto c10 = push_i64_const(module, 10);
  const auto c20 = push_i64_const(module, 20);
  module.builtin_names = {"Std.Select"};
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kPushConst, c10},
      {fleaux::bytecode::Opcode::kPushConst, c20},
      {fleaux::bytecode::Opcode::kBuildTuple, 3},
      {fleaux::bytecode::Opcode::kCallBuiltin, 0},
      {fleaux::bytecode::Opcode::kPrint, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "20\n");
}

TEST_CASE("VM kCallBuiltin uses native dispatch for Std.Branch", "[vm]") {
  fleaux::bytecode::Module module;
  module.constants.push_back(fleaux::bytecode::ConstValue{true});
  const auto c1 = push_i64_const(module, 1);
  const auto c10 = push_i64_const(module, 10);

  fleaux::bytecode::FunctionDef add1;
  add1.name = "AddOne";
  add1.arity = 1;
  add1.instructions = {
      {fleaux::bytecode::Opcode::kLoadLocal, 0},
      {fleaux::bytecode::Opcode::kPushConst, c1},
      {fleaux::bytecode::Opcode::kAdd, 0},
      {fleaux::bytecode::Opcode::kReturn, 0},
  };

  fleaux::bytecode::FunctionDef sub1;
  sub1.name = "SubOne";
  sub1.arity = 1;
  sub1.instructions = {
      {fleaux::bytecode::Opcode::kLoadLocal, 0},
      {fleaux::bytecode::Opcode::kPushConst, c1},
      {fleaux::bytecode::Opcode::kSub, 0},
      {fleaux::bytecode::Opcode::kReturn, 0},
  };

  module.functions.push_back(std::move(add1));
  module.functions.push_back(std::move(sub1));
  module.builtin_names = {"Std.Branch"};
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kPushConst, c10},
      {fleaux::bytecode::Opcode::kMakeUserFuncRef, 0},
      {fleaux::bytecode::Opcode::kMakeUserFuncRef, 1},
      {fleaux::bytecode::Opcode::kBuildTuple, 4},
      {fleaux::bytecode::Opcode::kCallBuiltin, 0},
      {fleaux::bytecode::Opcode::kPrint, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "11\n");
}

TEST_CASE("VM executes native mul/neg/comparison/or opcodes", "[vm]") {
  fleaux::bytecode::Module module;
  module.constants.push_back(fleaux::bytecode::ConstValue{false});
  module.constants.push_back(fleaux::bytecode::ConstValue{true});
  const auto c6 = push_i64_const(module, 6);
  const auto c7 = push_i64_const(module, 7);
  const auto c3 = push_i64_const(module, 3);

  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, c6},
      {fleaux::bytecode::Opcode::kPushConst, c7},
      {fleaux::bytecode::Opcode::kMul,       0},
      {fleaux::bytecode::Opcode::kPrint,     0},

      {fleaux::bytecode::Opcode::kPushConst, c3},
      {fleaux::bytecode::Opcode::kNeg,       0},
      {fleaux::bytecode::Opcode::kPrint,     0},

      {fleaux::bytecode::Opcode::kPushConst, c6},
      {fleaux::bytecode::Opcode::kPushConst, c7},
      {fleaux::bytecode::Opcode::kCmpNe,     0},
      {fleaux::bytecode::Opcode::kPrint,     0},

      {fleaux::bytecode::Opcode::kPushConst, c6},
      {fleaux::bytecode::Opcode::kPushConst, c7},
      {fleaux::bytecode::Opcode::kCmpLe,     0},
      {fleaux::bytecode::Opcode::kPrint,     0},

      {fleaux::bytecode::Opcode::kPushConst, c7},
      {fleaux::bytecode::Opcode::kPushConst, c6},
      {fleaux::bytecode::Opcode::kCmpGe,     0},
      {fleaux::bytecode::Opcode::kPrint,     0},

      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kPushConst, 1},
      {fleaux::bytecode::Opcode::kOr,        0},
      {fleaux::bytecode::Opcode::kPrint,     0},

      {fleaux::bytecode::Opcode::kHalt,      0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "42\n-3\nTrue\nTrue\nTrue\nTrue\n");
}

TEST_CASE("VM executes kCallUserFunc opcode", "[vm]") {
  fleaux::bytecode::Module module;
  const auto c21 = push_i64_const(module, 21);

  fleaux::bytecode::FunctionDef twice;
  twice.name = "Twice";
  twice.arity = 1;
  twice.instructions = {
      {fleaux::bytecode::Opcode::kLoadLocal, 0},
      {fleaux::bytecode::Opcode::kLoadLocal, 0},
      {fleaux::bytecode::Opcode::kAdd,       0},
      {fleaux::bytecode::Opcode::kReturn,    0},
  };
  module.functions.push_back(std::move(twice));

  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst,    c21},
      {fleaux::bytecode::Opcode::kCallUserFunc,   0},
      {fleaux::bytecode::Opcode::kPrint,          0},
      {fleaux::bytecode::Opcode::kHalt,           0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "42\n");
}

TEST_CASE("VM kCallBuiltin uses native dispatch for more arithmetic/logical builtins", "[vm]") {
  fleaux::bytecode::Module module;
  module.constants.push_back(fleaux::bytecode::ConstValue{false});
  module.constants.push_back(fleaux::bytecode::ConstValue{true});
  const auto c10 = push_i64_const(module, 10);
  const auto c4 = push_i64_const(module, 4);
  const auto c3 = push_i64_const(module, 3);
  const auto c7 = push_i64_const(module, 7);

  module.builtin_names = {
      "Std.Subtract",
      "Std.Multiply",
      "Std.Or",
      "Std.Equal",
      "Std.NotEqual",
  };

  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, c10},
      {fleaux::bytecode::Opcode::kPushConst, c4},
      {fleaux::bytecode::Opcode::kBuildTuple,  2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 0},
      {fleaux::bytecode::Opcode::kPrint,       0},

      {fleaux::bytecode::Opcode::kPushConst, c3},
      {fleaux::bytecode::Opcode::kPushConst, c7},
      {fleaux::bytecode::Opcode::kBuildTuple,  2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 1},
      {fleaux::bytecode::Opcode::kPrint,       0},

      {fleaux::bytecode::Opcode::kPushConst,   0},
      {fleaux::bytecode::Opcode::kPushConst,   1},
      {fleaux::bytecode::Opcode::kBuildTuple,  2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 2},
      {fleaux::bytecode::Opcode::kPrint,       0},

      {fleaux::bytecode::Opcode::kPushConst, c10},
      {fleaux::bytecode::Opcode::kPushConst, c10},
      {fleaux::bytecode::Opcode::kBuildTuple,  2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 3},
      {fleaux::bytecode::Opcode::kPrint,       0},

      {fleaux::bytecode::Opcode::kPushConst, c10},
      {fleaux::bytecode::Opcode::kPushConst, c4},
      {fleaux::bytecode::Opcode::kBuildTuple,  2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 4},
      {fleaux::bytecode::Opcode::kPrint,       0},

      {fleaux::bytecode::Opcode::kHalt,        0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "6\n21\nTrue\nTrue\nTrue\n");
}

TEST_CASE("VM kCallBuiltin uses native dispatch for apply/wrap/unwrap/to_num", "[vm]") {
  fleaux::bytecode::Module module;
  const auto c1 = push_i64_const(module, 1);
  const auto c41 = push_i64_const(module, 41);
  const auto c7 = push_i64_const(module, 7);
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"42"}});

  fleaux::bytecode::FunctionDef add_one;
  add_one.name = "AddOne";
  add_one.arity = 1;
  add_one.instructions = {
      {fleaux::bytecode::Opcode::kLoadLocal, 0},
      {fleaux::bytecode::Opcode::kPushConst, c1},
      {fleaux::bytecode::Opcode::kAdd, 0},
      {fleaux::bytecode::Opcode::kReturn, 0},
  };
  module.functions.push_back(std::move(add_one));

  module.builtin_names = {
      "Std.Apply",
      "Std.Wrap",
      "Std.Unwrap",
      "Std.ToNum",
  };

  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, c41},
      {fleaux::bytecode::Opcode::kMakeUserFuncRef, 0},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 0},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, c7},
      {fleaux::bytecode::Opcode::kCallBuiltin, 1},
      {fleaux::bytecode::Opcode::kCallBuiltin, 2},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, 3},
      {fleaux::bytecode::Opcode::kCallBuiltin, 3},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "42\n7\n42\n");
}

TEST_CASE("VM kCallBuiltin uses native dispatch for tuple/math helper builtins", "[vm]") {
  fleaux::bytecode::Module module;
  const auto c10 = push_i64_const(module, 10);
  const auto c20 = push_i64_const(module, 20);
  const auto c30 = push_i64_const(module, 30);
  const auto c1 = push_i64_const(module, 1);
  const auto c2 = push_i64_const(module, 2);
  const auto c3 = push_i64_const(module, 3);
  const auto c9 = push_i64_const(module, 9);
  const auto c0 = push_i64_const(module, 0);
  const auto c5 = push_i64_const(module, 5);

  module.builtin_names = {
      "Std.Length",
      "Std.ElementAt",
      "Std.Take",
      "Std.Drop",
      "Std.Slice",
      "Std.Sqrt",
      "Std.Math.Sqrt",
      "Std.Math.Clamp",
  };

  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, c10},
      {fleaux::bytecode::Opcode::kPushConst, c20},
      {fleaux::bytecode::Opcode::kPushConst, c30},
      {fleaux::bytecode::Opcode::kBuildTuple, 3},
      {fleaux::bytecode::Opcode::kBuildTuple, 1},
      {fleaux::bytecode::Opcode::kCallBuiltin, 0},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, c10},
      {fleaux::bytecode::Opcode::kPushConst, c20},
      {fleaux::bytecode::Opcode::kPushConst, c30},
      {fleaux::bytecode::Opcode::kBuildTuple, 3},
      {fleaux::bytecode::Opcode::kPushConst, c1},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 1},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, c10},
      {fleaux::bytecode::Opcode::kPushConst, c20},
      {fleaux::bytecode::Opcode::kPushConst, c30},
      {fleaux::bytecode::Opcode::kBuildTuple, 3},
      {fleaux::bytecode::Opcode::kPushConst, c2},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 2},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, c10},
      {fleaux::bytecode::Opcode::kPushConst, c20},
      {fleaux::bytecode::Opcode::kPushConst, c30},
      {fleaux::bytecode::Opcode::kBuildTuple, 3},
      {fleaux::bytecode::Opcode::kPushConst, c2},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 3},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, c10},
      {fleaux::bytecode::Opcode::kPushConst, c20},
      {fleaux::bytecode::Opcode::kPushConst, c30},
      {fleaux::bytecode::Opcode::kBuildTuple, 3},
      {fleaux::bytecode::Opcode::kPushConst, c1},
      {fleaux::bytecode::Opcode::kPushConst, c3},
      {fleaux::bytecode::Opcode::kBuildTuple, 3},
      {fleaux::bytecode::Opcode::kCallBuiltin, 4},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, c9},
      {fleaux::bytecode::Opcode::kCallBuiltin, 5},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, c9},
      {fleaux::bytecode::Opcode::kBuildTuple, 1},
      {fleaux::bytecode::Opcode::kCallBuiltin, 6},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, c9},
      {fleaux::bytecode::Opcode::kPushConst, c0},
      {fleaux::bytecode::Opcode::kPushConst, c5},
      {fleaux::bytecode::Opcode::kBuildTuple, 3},
      {fleaux::bytecode::Opcode::kCallBuiltin, 7},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "3\n20\n10 20\n30\n20 30\n3\n3\n5\n");
}

TEST_CASE("VM kCallBuiltin uses native dispatch for Std.ToString and Std.String helpers", "[vm]") {
  fleaux::bytecode::Module module;
  const auto c42 = push_i64_const(module, 42);
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"hElLo"}});
  const auto cHello = static_cast<std::int64_t>(module.constants.size() - 1);
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"MiXeD"}});
  const auto cMixed = static_cast<std::int64_t>(module.constants.size() - 1);
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"  trim me  "}});
  const auto cTrim = static_cast<std::int64_t>(module.constants.size() - 1);
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"a,b,c"}});
  const auto cCsv = static_cast<std::int64_t>(module.constants.size() - 1);
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{","}});
  const auto cComma = static_cast<std::int64_t>(module.constants.size() - 1);
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"-"}});
  const auto cDash = static_cast<std::int64_t>(module.constants.size() - 1);
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"_"}});
  const auto cUnderscore = static_cast<std::int64_t>(module.constants.size() - 1);
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"abc"}});
  const auto cAbc = static_cast<std::int64_t>(module.constants.size() - 1);
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"ab"}});
  const auto cAb = static_cast<std::int64_t>(module.constants.size() - 1);
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"bc"}});
  const auto cBc = static_cast<std::int64_t>(module.constants.size() - 1);
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"b"}});
  const auto cB = static_cast<std::int64_t>(module.constants.size() - 1);
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"abcd"}});
  const auto cAbcd = static_cast<std::int64_t>(module.constants.size() - 1);
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"  left"}});
  const auto cTrimStartOnly = static_cast<std::int64_t>(module.constants.size() - 1);
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"right  "}});
  const auto cTrimEndOnly = static_cast<std::int64_t>(module.constants.size() - 1);

  module.builtin_names = {
      "Std.ToString",
      "Std.String.Upper",
      "Std.String.Lower",
      "Std.String.Trim",
      "Std.String.Split",
      "Std.String.Join",
      "Std.String.Replace",
      "Std.String.Contains",
      "Std.String.StartsWith",
      "Std.String.EndsWith",
      "Std.String.Length",
      "Std.String.TrimStart",
      "Std.String.TrimEnd",
  };

  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, c42},
      {fleaux::bytecode::Opcode::kCallBuiltin, 0},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, cHello},
      {fleaux::bytecode::Opcode::kCallBuiltin, 1},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, cMixed},
      {fleaux::bytecode::Opcode::kCallBuiltin, 2},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, cTrim},
      {fleaux::bytecode::Opcode::kCallBuiltin, 3},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, cCsv},
      {fleaux::bytecode::Opcode::kPushConst, cComma},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 4},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, cComma},
      {fleaux::bytecode::Opcode::kPushConst, cAbc},
      {fleaux::bytecode::Opcode::kPushConst, cB},
      {fleaux::bytecode::Opcode::kPushConst, cBc},
      {fleaux::bytecode::Opcode::kBuildTuple, 3},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 5},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, cCsv},
      {fleaux::bytecode::Opcode::kPushConst, cComma},
      {fleaux::bytecode::Opcode::kPushConst, cUnderscore},
      {fleaux::bytecode::Opcode::kBuildTuple, 3},
      {fleaux::bytecode::Opcode::kCallBuiltin, 6},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, cAbc},
      {fleaux::bytecode::Opcode::kPushConst, cB},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 7},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, cAbc},
      {fleaux::bytecode::Opcode::kPushConst, cAb},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 8},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, cAbc},
      {fleaux::bytecode::Opcode::kPushConst, cBc},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 9},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, cAbcd},
      {fleaux::bytecode::Opcode::kCallBuiltin, 10},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, cTrimStartOnly},
      {fleaux::bytecode::Opcode::kCallBuiltin, 11},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, cTrimEndOnly},
      {fleaux::bytecode::Opcode::kCallBuiltin, 12},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "42\nHELLO\nmixed\ntrim me\na b c\nabc,b,bc\na_b_c\nTrue\nTrue\nTrue\n4\nleft\nright\n");
}

TEST_CASE("VM kCallBuiltin uses native dispatch for Std.Path and Std.OS helpers", "[vm]") {
  fleaux::bytecode::Module module;
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"/tmp"}});
  const auto cTmp = static_cast<std::int64_t>(module.constants.size() - 1);
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"file.txt"}});
  const auto cFile = static_cast<std::int64_t>(module.constants.size() - 1);
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"/tmp/file.txt"}});
  const auto cFull = static_cast<std::int64_t>(module.constants.size() - 1);
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"."}});
  const auto cDot = static_cast<std::int64_t>(module.constants.size() - 1);
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"log"}});
  const auto cLogExt = static_cast<std::int64_t>(module.constants.size() - 1);
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"other.bin"}});
  const auto cOtherBin = static_cast<std::int64_t>(module.constants.size() - 1);

  module.builtin_names = {
      "Std.Path.Join",
      "Std.Path.Basename",
      "Std.Path.Extension",
      "Std.Path.Stem",
      "Std.Path.Exists",
      "Std.Path.IsDir",
      "Std.OS.IsLinux",
      "Std.Path.WithExtension",
      "Std.Path.WithBasename",
  };

  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, cTmp},
      {fleaux::bytecode::Opcode::kPushConst, cFile},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 0},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, cFull},
      {fleaux::bytecode::Opcode::kCallBuiltin, 1},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, cFull},
      {fleaux::bytecode::Opcode::kCallBuiltin, 2},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, cFull},
      {fleaux::bytecode::Opcode::kCallBuiltin, 3},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, cDot},
      {fleaux::bytecode::Opcode::kCallBuiltin, 4},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, cDot},
      {fleaux::bytecode::Opcode::kCallBuiltin, 5},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kBuildTuple, 0},
      {fleaux::bytecode::Opcode::kCallBuiltin, 6},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, cFull},
      {fleaux::bytecode::Opcode::kPushConst, cLogExt},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 7},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, cFull},
      {fleaux::bytecode::Opcode::kPushConst, cOtherBin},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 8},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "/tmp/file.txt\nfile.txt\n.txt\nfile\nTrue\nTrue\nTrue\n/tmp/file.log\n/tmp/other.bin\n");
}

TEST_CASE("VM strict mode executes native Std.String and Std.Path builtins", "[vm]") {
  fleaux::bytecode::Module module;
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"abc"}});
  const auto cAbc = static_cast<std::int64_t>(module.constants.size() - 1);
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"/tmp/file.txt"}});
  const auto cPath = static_cast<std::int64_t>(module.constants.size() - 1);
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"log"}});
  const auto cLog = static_cast<std::int64_t>(module.constants.size() - 1);

  module.builtin_names = {
      "Std.String.Upper",
      "Std.Path.WithExtension",
  };

  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, cAbc},
      {fleaux::bytecode::Opcode::kCallBuiltin, 0},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, cPath},
      {fleaux::bytecode::Opcode::kPushConst, cLog},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 1},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime(
      fleaux::vm::RuntimeOptions{.allow_runtime_fallback = false});
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "ABC\n/tmp/file.log\n");
}

TEST_CASE("VM strict mode executes native Std.OS, Std.Tuple, and Std.Dict builtins", "[vm]") {
  fleaux::bytecode::Module module;
  const auto c1 = push_i64_const(module, 1);
  const auto c2 = push_i64_const(module, 2);
  const auto c3 = push_i64_const(module, 3);
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"k"}});
  const auto cKey = static_cast<std::int64_t>(module.constants.size() - 1);

  module.builtin_names = {
      "Std.OS.IsLinux",
      "Std.Tuple.Append",
      "Std.Dict.Create",
      "Std.Dict.Set",
      "Std.Dict.Get",
  };

  module.instructions = {
      {fleaux::bytecode::Opcode::kBuildTuple, 0},
      {fleaux::bytecode::Opcode::kCallBuiltin, 0},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, c1},
      {fleaux::bytecode::Opcode::kPushConst, c2},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kPushConst, c3},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 1},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kBuildTuple, 0},
      {fleaux::bytecode::Opcode::kCallBuiltin, 2},
      {fleaux::bytecode::Opcode::kPushConst, cKey},
      {fleaux::bytecode::Opcode::kPushConst, c3},
      {fleaux::bytecode::Opcode::kBuildTuple, 3},
      {fleaux::bytecode::Opcode::kCallBuiltin, 3},
      {fleaux::bytecode::Opcode::kPushConst, cKey},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 4},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime(
      fleaux::vm::RuntimeOptions{.allow_runtime_fallback = false});
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "True\n1 2 3\n3\n");
}

TEST_CASE("VM strict mode executes native Std.OS env and Std.File/Std.Dir builtins", "[vm]") {
  const auto base = std::filesystem::temp_directory_path() / "fleaux_vm_native_fs_test";
  const auto file_path = (base / "data.txt").string();
  const auto dir_path = base.string();
  const std::string env_key = "FLEAUX_VM_TEST_ENV_KEY";
  const std::string env_value = "vm_native_ok";
  std::filesystem::remove_all(base);

  fleaux::bytecode::Module module;
  module.constants.push_back(fleaux::bytecode::ConstValue{env_key});
  const auto cEnvKey = static_cast<std::int64_t>(module.constants.size() - 1);
  module.constants.push_back(fleaux::bytecode::ConstValue{env_value});
  const auto cEnvVal = static_cast<std::int64_t>(module.constants.size() - 1);
  module.constants.push_back(fleaux::bytecode::ConstValue{dir_path});
  const auto cDir = static_cast<std::int64_t>(module.constants.size() - 1);
  module.constants.push_back(fleaux::bytecode::ConstValue{file_path});
  const auto cFile = static_cast<std::int64_t>(module.constants.size() - 1);
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"hello"}});
  const auto cHello = static_cast<std::int64_t>(module.constants.size() - 1);

  module.builtin_names = {
      "Std.OS.SetEnv",
      "Std.OS.Env",
      "Std.OS.UnsetEnv",
      "Std.Dir.Create",
      "Std.File.WriteText",
      "Std.File.ReadText",
      "Std.File.Delete",
      "Std.Dir.Delete",
  };

  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, cEnvKey},
      {fleaux::bytecode::Opcode::kPushConst, cEnvVal},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 0},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, cEnvKey},
      {fleaux::bytecode::Opcode::kCallBuiltin, 1},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, cEnvKey},
      {fleaux::bytecode::Opcode::kCallBuiltin, 2},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, cDir},
      {fleaux::bytecode::Opcode::kCallBuiltin, 3},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, cFile},
      {fleaux::bytecode::Opcode::kPushConst, cHello},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 4},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, cFile},
      {fleaux::bytecode::Opcode::kCallBuiltin, 5},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, cFile},
      {fleaux::bytecode::Opcode::kCallBuiltin, 6},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, cDir},
      {fleaux::bytecode::Opcode::kCallBuiltin, 7},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime(
      fleaux::vm::RuntimeOptions{.allow_runtime_fallback = false});
  const auto result = runtime.execute(module, output);

  std::filesystem::remove_all(base);
#if defined(_WIN32)
  _putenv_s(env_key.c_str(), "");
#else
  unsetenv(env_key.c_str());
#endif

  REQUIRE(result.has_value());
  REQUIRE(output.str() ==
          "vm_native_ok\nvm_native_ok\nTrue\n" + dir_path + "\n" + file_path + "\nhello\nTrue\nTrue\n");
}

TEST_CASE("VM native Std.Path.Join reports native error prefix", "[vm]") {
  fleaux::bytecode::Module module;
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"/tmp"}});
  module.builtin_names = {"Std.Path.Join"};
  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, 0},
      {fleaux::bytecode::Opcode::kBuildTuple, 1},
      {fleaux::bytecode::Opcode::kCallBuiltin, 0},
      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().message ==
          "native builtin 'Std.Path.Join' threw: PathJoin expects at least 2 arguments");
}

TEST_CASE("VM kCallBuiltin uses native dispatch for Std.Tuple and Std.Dict helpers", "[vm]") {
  fleaux::bytecode::Module module;
  const auto c0 = push_i64_const(module, 0);
  const auto c1 = push_i64_const(module, 1);
  const auto c2 = push_i64_const(module, 2);
  const auto c3 = push_i64_const(module, 3);
  const auto c4 = push_i64_const(module, 4);
  const auto c9 = push_i64_const(module, 9);
  const auto c42 = push_i64_const(module, 42);
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"k"}});
  const auto cKey = static_cast<std::int64_t>(module.constants.size() - 1);
  module.constants.push_back(fleaux::bytecode::ConstValue{std::string{"missing"}});
  const auto cMissing = static_cast<std::int64_t>(module.constants.size() - 1);

  module.builtin_names = {
      "Std.Tuple.Append",
      "Std.Tuple.Prepend",
      "Std.Tuple.Contains",
      "Std.Tuple.Zip",
      "Std.Dict.Create",
      "Std.Dict.Set",
      "Std.Dict.Get",
      "Std.Dict.GetDefault",
      "Std.Dict.Length",
      "Std.Dict.Keys",
      "Std.Dict.Values",
  };

  module.instructions = {
      {fleaux::bytecode::Opcode::kPushConst, c1},
      {fleaux::bytecode::Opcode::kPushConst, c2},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kPushConst, c3},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 0},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, c1},
      {fleaux::bytecode::Opcode::kPushConst, c2},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kPushConst, c0},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 1},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, c1},
      {fleaux::bytecode::Opcode::kPushConst, c2},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kPushConst, c2},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 2},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, c1},
      {fleaux::bytecode::Opcode::kPushConst, c2},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kPushConst, c3},
      {fleaux::bytecode::Opcode::kPushConst, c4},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 3},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kBuildTuple, 0},
      {fleaux::bytecode::Opcode::kCallBuiltin, 4},
      {fleaux::bytecode::Opcode::kPushConst, cKey},
      {fleaux::bytecode::Opcode::kPushConst, c9},
      {fleaux::bytecode::Opcode::kBuildTuple, 3},
      {fleaux::bytecode::Opcode::kCallBuiltin, 5},

      {fleaux::bytecode::Opcode::kDup, 0},
      {fleaux::bytecode::Opcode::kPushConst, cKey},
      {fleaux::bytecode::Opcode::kBuildTuple, 2},
      {fleaux::bytecode::Opcode::kCallBuiltin, 6},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kDup, 0},
      {fleaux::bytecode::Opcode::kBuildTuple, 1},
      {fleaux::bytecode::Opcode::kCallBuiltin, 8},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kDup, 0},
      {fleaux::bytecode::Opcode::kBuildTuple, 1},
      {fleaux::bytecode::Opcode::kCallBuiltin, 9},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kDup, 0},
      {fleaux::bytecode::Opcode::kBuildTuple, 1},
      {fleaux::bytecode::Opcode::kCallBuiltin, 10},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kPushConst, cMissing},
      {fleaux::bytecode::Opcode::kPushConst, c42},
      {fleaux::bytecode::Opcode::kBuildTuple, 3},
      {fleaux::bytecode::Opcode::kCallBuiltin, 7},
      {fleaux::bytecode::Opcode::kPrint, 0},

      {fleaux::bytecode::Opcode::kHalt, 0},
  };

  std::ostringstream output;
  const fleaux::vm::Runtime runtime;
  const auto result = runtime.execute(module, output);

  REQUIRE(result.has_value());
  REQUIRE(output.str() == "1 2 3\n0 1 2\nTrue\n(1, 3) (2, 4)\n9\n1\nk\n9\n42\n");
}
