# Fleaux C++20 Transpiler

This is the new C++20 transpiler backend for Fleaux, targeting modern C++20 with compile-time safety and performance.

## Overview

The transpiler converts Fleaux dataflow programs into C++20 code with the following design:

- **Runtime Library**: `fleaux_runtime.hpp` - Header-only C++20 runtime providing:
  - `FlexValue`: Type-erased value type using `std::any` for runtime flexibility
  - `FlowNode`: Wrapper for dataflow functions with pipe operator support
  - Built-in operators and math functions
  - Tuple manipulation utilities

- **Code Generation**: Generates two files per Fleaux module:
  - `.hpp` - Header with extern declarations for all defined functions
  - `.cpp` - Implementation with lambda-based flow nodes

## Architecture

### FlexValue and FlowNode

The system uses `std::any` for type-erased values during pipeline execution:

```cpp
using FlexValue = std::any;
using FlowFn = std::function<FlexValue(const FlexValue&)>;
class FlowNode {
    FlexValue operator()(const FlexValue& input) const;
    friend FlexValue operator|(const FlexValue& lhs, const FlowNode& rhs);
};
```

### Pipeline Operator

The `|` operator is overloaded to allow natural dataflow syntax:

```cpp
// Fleaux: (2, 1) -> Add;
// C++20: fleaux::FlexValue(std::make_tuple(2, 1)) | fleaux::Add
```

### Built-in Operators

All Fleaux operators are implemented as FlowNode instances in `fleaux_runtime.hpp`:

```cpp
// Arithmetic
Add, Subtract, Multiply, Divide, Mod, Pow

// Comparison
Equal, NotEqual, LessThan, GreaterThan, LessOrEqual, GreaterOrEqual

// Logic
And, Or, Not

// Math
Sqrt, Sin, Cos, Tan
```

## Generated Code Example

### Fleaux Source
```fleaux
import Std;

let Add3(tpl: Tuple(Number, Number, Number)) : Number = 
    (((tpl, 0) -> Std.ElementAt, (tpl, 1) -> Std.ElementAt) -> Std.Add, 
     (tpl, 2) -> Std.ElementAt) -> Std.Add;

(1, 2, 3) -> Add3;
```

### Generated C++20

**Header (fleaux_generated_test.hpp):**
```cpp
#ifndef FLEAUX_GENERATED_TEST_HPP
#define FLEAUX_GENERATED_TEST_HPP

#include "fleaux_runtime.hpp"
#include "fleaux_generated_Std.hpp"

namespace fleaux {
    extern fleaux::FlowNode Add3;
} // namespace fleaux

#endif // FLEAUX_GENERATED_TEST_HPP
```

**Implementation (fleaux_generated_test.cpp):**
```cpp
#include "fleaux_generated_test.hpp"

namespace fleaux {

fleaux::FlowNode Add3 = fleaux::FlowNode([](const fleaux::FlexValue& input) -> fleaux::FlexValue {
    auto tpl = input;
    return (fleaux::FlexValue(std::make_tuple(
        (fleaux::FlexValue(std::make_tuple(tpl, fleaux::FlexValue(0))) | Std::Std_ElementAt) |
        fleaux::Add) | Std::Std_Add);
});

} // namespace fleaux
```

## Building with CMake

The included `CMakeLists.txt` provides integration:

```cmake
# Add fleaux target from source
add_fleaux_target(myapp myapp.fleaux)
```

This automatically:
1. Runs the Python transpiler
2. Generates C++20 header/implementation
3. Creates an executable linked with fleaux_runtime

## Building Manually

```bash
# Generate C++20 files
python3 fleaux_cpp_transpiler.py myprogram.fleaux

# Compile with C++20
c++ -std=c++20 -I. -o myprogram \
    fleaux_generated_myprogram.cpp \
    main.cpp
```

## Type System

The transpiler currently maps Fleaux types to C++ as follows:

| Fleaux Type | C++20 Type |
|-------------|-----------|
| `Number` | `double` |
| `String` | `std::string` |
| `Bool` | `bool` |
| `Any` | `fleaux::FlexValue` |
| `Null` | `std::nullptr_t` |
| `Tuple(T1, T2, ...)` | `std::tuple<T1, T2, ...>` |

## Type Erasure Strategy

Because Fleaux is a dynamically typed dataflow language, the runtime uses `std::any` for flexibility:

- Values are wrapped in `FlexValue` (using `std::any_cast`)
- Tuples are represented as `std::tuple` wrapped in `FlexValue`
- Type errors become runtime errors with descriptive messages

## Future Enhancements

1. **Compile-time Type Checking**: Use C++ concepts for better static type safety
2. **Template Specialization**: Generate specialized functions for common type patterns
3. **SIMD Support**: Leverage C++20 features for vectorized operations
4. **Async/Await**: Support dataflow with asynchronous operators
5. **Memory Optimization**: Use move semantics and value semantics more aggressively
6. **User Defined Types**: Support for custom tuple type aliases

## Design Notes

- The transpiler preserves the compositional dataflow style of Fleaux
- Lazy evaluation is not currently supported (eager evaluation only)
- All functions are inline lambda-based for maximum inlining opportunities
- The runtime is header-only to enable aggressive optimization

