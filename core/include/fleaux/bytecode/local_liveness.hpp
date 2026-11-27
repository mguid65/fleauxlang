#pragma once

// The last reads of local slots.
//
// A local slot holds its value until the slot is written again or the frame ends, so a value read from a slot has at
// least two references while the operation it was read for runs: the slot's and the read copy's. An operation that
// could change the value in place when it holds the only reference (Std.Dict.Set, Std.Tuple.SetAt, ...) would then
// copy it. The compiler therefore turns the last read of a slot, a kLoadLocal after which no path through the stream
// reads the slot again before writing it, into a kTakeLocal, which moves the value out.

#include <vector>

#include "fleaux/bytecode/module.hpp"

namespace fleaux::bytecode {

// Rewrites the last reads in one instruction stream. The stream is one frame: its slots are dead where it ends.
void move_last_local_reads(std::vector<Instruction>& instructions);

// Rewrites the last reads in every stream of the module: the top level and each function.
void move_last_local_reads(Module& module);

}  // namespace fleaux::bytecode
