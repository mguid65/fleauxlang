#pragma once

// Umbrella header for the Fleaux runtime support library.
//
// Include this single header to pull in the full native runtime surface:
//  #include "fleaux/runtime/runtime_support.hpp"
//
// Or include individual sub-headers for finer-grained dependencies.

#include "fleaux/runtime/value.hpp"          // Type aliases, registries, construction/extraction helpers, printing
#include "fleaux/runtime/builtins_core.hpp"  // Arithmetic, comparison, logical, Wrap/Unwrap/sequence, output, control flow
#include "fleaux/runtime/builtins_string.hpp"  // String ops, Math helpers
#include "fleaux/runtime/builtins_tuple.hpp"   // TupleMap/Filter/Sort/Reduce/Range/Zip etc.
#include "fleaux/runtime/builtins_io.hpp"      // Path, File, Dir, OS, file-streaming
#include "fleaux/runtime/builtins_dict.hpp"    // Dict builtins
#include "fleaux/runtime/builtins_array.hpp"   // Array utilities

