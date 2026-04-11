#pragma once

// Umbrella header for the Fleaux C++ runtime.
//
// Include this single header to pull in everything:
//   #include "fleaux/runtime/fleaux_runtime.hpp"
//
// Or include individual sub-headers for finer-grained dependencies.

#include "fleaux/runtime/value.hpp"         // Type aliases, registries, construction/extraction helpers, printing
#include "fleaux/runtime/builtins_core.hpp" // Arithmetic, comparison, logical, Wrap/Unwrap/sequence, output, control flow
#include "fleaux/runtime/builtins_string.hpp" // String ops, Math helpers
#include "fleaux/runtime/builtins_tuple.hpp"  // TupleMap/Filter/Sort/Reduce/Range/Zip etc.
#include "fleaux/runtime/builtins_io.hpp"     // Path, File, Dir, OS, file-streaming
#include "fleaux/runtime/builtins_dict.hpp"   // Dict builtins

