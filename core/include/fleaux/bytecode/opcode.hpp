#pragma once

namespace fleaux::bytecode {

enum class Opcode {
  kNoOp,
  kPushConstI64,
  kAddI64,
  kSubI64,
  kMulI64,
  kDivI64,
  kPrint,
  kHalt,
};

}  // namespace fleaux::bytecode

