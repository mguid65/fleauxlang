#pragma once

// Umbrella header for the Fleaux C++ runtime.
//
// Include this single header to pull in everything:
//   #include "fleaux/runtime/fleaux_runtime.hpp"
//
// Or include individual sub-headers for finer-grained dependencies.

#include "value.hpp"         // Type aliases, registries, construction/extraction helpers, printing
#include "builtins_core.hpp" // Arithmetic, comparison, logical, Wrap/Unwrap/sequence, output, control flow
#include "builtins_string.hpp" // String ops, Math helpers
#include "builtins_tuple.hpp"  // TupleMap/Filter/Sort/Reduce/Range/Zip etc.
#include "builtins_io.hpp"     // Path, File, Dir, OS, file-streaming
#include "builtins_dict.hpp"   // Dict builtins

