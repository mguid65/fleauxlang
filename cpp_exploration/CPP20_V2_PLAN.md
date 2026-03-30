# C++20 Transpiler Implementation Summary

## Completed

### 1. New Strongly-Typed Architecture
- **fleaux_runtime_v2.hpp**: Header-only C++20 runtime with:
  - `Value<T>` template class for type-safe value wrapping
  - Operator overloading for pipe operator `|`
  - Direct tuple support without type erasure
  - Arithmetic, comparison, and logical operators
  - Math functions (sqrt, sin, cos, tan)
  - Generic println for multiple types

### 2. V2 Transpiler
- **fleaux_cpp_transpiler_v2.py**: Generates strongly-typed C++20 code
  - Converts Fleaux types directly to C++ types (Number → double, etc.)
  - Generates actual tuple types: `std::tuple<double, double>` instead of `std::any`
  - Creates `std::function` wrappers for Fleaux functions
  - Supports operators via function calls instead of type erasure
  - No runtime overhead for type checking

### 3. Generated Code Quality
- Clean, readable C++20 code with proper namespacing
- Uses lambda functions with `std::function` for type safety
- Proper template instantiation for compile-time optimization
- Forward declarations in headers, implementations in .cpp files

## What Works
- ✅ Parsing Fleaux files to IR
- ✅ Type system mapping (Fleaux types → C++ types)
- ✅ Function declaration generation
- ✅ Lambda-based implementations
- ✅ Operator mapping to functions

## What Still Needs Implementation

### 1. Runtime Builtins
Need to add to `fleaux_runtime_v2.hpp`:
- **ElementAt(tuple, index)**: Extract element from tuple by index
- **Wrap(value)**: Wrap single value in tuple
- **Unwrap(tuple)**: Extract single value from 1-tuple
- **Println(value)**: Print and return value (already basic version exists)
- **In(count)**: Read N lines from stdin
- **Length(tuple)**: Get tuple length
- **Slice(tuple, start, end)**: Extract slice of tuple

### 2. Variadic ElementAt
Currently Std.fleaux has:
```fleaux
let Std.ElementAt(tuple: Tuple(Any...), count: Number) : Any :: __builtin__;
```

This needs a template-based C++20 implementation that can:
- Accept variadic tuples of any size
- Index them safely at compile-time or runtime

### 3. Pipe Operator Specialization
The current runtime has basic pipe support, but we need:
- Tuple-to-function piping: `std::tuple<Ts...> | std::function<R(Ts...)>`
- Value-to-function piping: `Value<T> | std::function<R(T)>`
- Chained piping support

### 4. Complete Std Module Integration
Generate proper Std module with:
- Mathematical constants (Pi, E, etc.)
- All operator builtins mapped correctly
- Proper extern declarations

## Next Steps

### Phase 1: Complete Runtime (HIGH PRIORITY)
1. Implement ElementAt as a variadic template
2. Add Wrap/Unwrap functionality
3. Improve pipe operator specialization
4. Add remaining builtins (In, Length, Slice)

### Phase 2: Testing Infrastructure
1. Create test files that compile and run
2. Verify type safety at compile-time
3. Benchmark performance vs. Python version
4. Test error handling

### Phase 3: Integration & Documentation
1. Update CMakeLists.txt for v2 transpiler
2. Create example build scripts for v2
3. Document generated code patterns
4. Compare v1 (type-erased) vs v2 (strongly-typed)

## Architecture Comparison

### V1 (Type-Erased with std::any)
- Pros: Flexible, handles any type at runtime
- Cons: Runtime overhead, less optimization, type errors at runtime
- Use case: Rapid prototyping, dynamic features

### V2 (Strongly-Typed)
- Pros: Compile-time safety, better optimization, zero runtime overhead
- Cons: More complex templates, requires IR type information
- Use case: Production, performance-critical code

## Technical Challenges & Solutions

### Challenge 1: Variadic ElementAt
```cpp
// Need to support: (tuple<A, B, C>, 0) -> A
template <typename Tuple>
auto element_at(const std::tuple<Tuple, double>& args) {
    return std::get<???>(std::get<0>(args)); // Index unknown at compile time
}
```

**Solution**: Use runtime index with std::variant and helper function

### Challenge 2: Generic Pipe Operator
```cpp
// Need to work with any tuple and any function signature
template <typename Tuple, typename Func>
auto operator|(const Tuple& lhs, const Func& rhs) {
    return rhs(lhs); // But rhs might not accept Tuple directly
}
```

**Solution**: Specialize for std::function types

### Challenge 3: Unpack Tuple to Function Arguments
```cpp
// Have: std::tuple<int, double> and std::function<bool(int, double)>
// Need: Call function with unpacked tuple elements
```

**Solution**: Use std::apply from C++17

## Recommended Reading Order

If implementing the next phase:
1. Read current fleaux_runtime_v2.hpp to understand existing operators
2. Review generated test_v2.cpp/hpp to see expected patterns
3. Study IR type system in fleaux_ast.py
4. Check Std.fleaux for all builtin definitions


