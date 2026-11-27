#pragma once

// Umbrella header for the Fleaux runtime support library.
//
// Include this single header to pull in the full native runtime surface:
//  #include "fleaux/runtime/runtime_support.hpp"
//
// Or include individual sub-headers for finer-grained dependencies.

#include "fleaux/runtime/builtins_array.hpp"     // Tuple utilities
#include "fleaux/runtime/builtins_core.hpp"      // Comparison, logical, Wrap/Unwrap/sequence, output, control flow
#include "fleaux/runtime/builtins_dict.hpp"      // Dict builtins
#include "fleaux/runtime/builtins_help.hpp"      // Help/introspection
#include "fleaux/runtime/builtins_io.hpp"        // Path, File, Dir, OS, file-streaming
#include "fleaux/runtime/builtins_math.hpp"      // Math builtins
#include "fleaux/runtime/builtins_parallel.hpp"  // Parallel and Task builtins
#include "fleaux/runtime/builtins_random.hpp"    // Deterministic random generator builtins
#include "fleaux/runtime/builtins_string.hpp"    // String ops
#include "fleaux/runtime/builtins_time.hpp"      // Clocks and Std.Time.Sleep
#include "fleaux/runtime/builtins_tuple.hpp"     // TupleMap/Filter/Sort/Reduce/Range/Zip etc.
#include "fleaux/runtime/type_check.hpp"         // Runtime checks for casts from Any
#include "fleaux/runtime/value.hpp"              // Type aliases, registries, construction/extraction helpers, printing
