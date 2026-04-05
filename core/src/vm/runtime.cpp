#include "fleaux/vm/runtime.hpp"

#include <iostream>
#include <vector>

namespace fleaux::vm {

RuntimeResult Runtime::execute(const bytecode::Module& module) const {
  std::vector<std::int64_t> stack;

  for (const auto& instruction : module.instructions) {
    switch (instruction.opcode) {
      case bytecode::Opcode::kNoOp:
        break;
      case bytecode::Opcode::kPushConstI64:
        stack.push_back(instruction.operand);
        break;
      case bytecode::Opcode::kAddI64: {
        if (stack.size() < 2) {
          return tl::unexpected(RuntimeError{"stack underflow on add"});
        }
        const auto rhs = stack.back();
        stack.pop_back();
        const auto lhs = stack.back();
        stack.pop_back();
        stack.push_back(lhs + rhs);
        break;
      }
      case bytecode::Opcode::kSubI64: {
        if (stack.size() < 2) {
          return tl::unexpected(RuntimeError{"stack underflow on sub"});
        }
        const auto rhs = stack.back();
        stack.pop_back();
        const auto lhs = stack.back();
        stack.pop_back();
        stack.push_back(lhs - rhs);
        break;
      }
      case bytecode::Opcode::kMulI64: {
        if (stack.size() < 2) {
          return tl::unexpected(RuntimeError{"stack underflow on mul"});
        }
        const auto rhs = stack.back();
        stack.pop_back();
        const auto lhs = stack.back();
        stack.pop_back();
        stack.push_back(lhs * rhs);
        break;
      }
      case bytecode::Opcode::kDivI64: {
        if (stack.size() < 2) {
          return tl::unexpected(RuntimeError{"stack underflow on div"});
        }
        const auto rhs = stack.back();
        stack.pop_back();
        const auto lhs = stack.back();
        stack.pop_back();
        if (rhs == 0) {
          return tl::unexpected(RuntimeError{"division by zero"});
        }
        stack.push_back(lhs / rhs);
        break;
      }
      case bytecode::Opcode::kPrint:
        if (stack.empty()) {
          return tl::unexpected(RuntimeError{"stack underflow on print"});
        }
        std::cout << stack.back() << '\n';
        break;
      case bytecode::Opcode::kHalt:
        return ExecutionResult{0};
    }
  }

  return ExecutionResult{0};
}

}  // namespace fleaux::vm

