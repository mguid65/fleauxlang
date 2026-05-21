# 05 Functions, Closures, and Blocks

This chapter covers the main building blocks used to structure computation: named functions, first-class closures, and block expressions with lexical scope.

## Named functions

Named functions are introduced with let.

Basic form:

    let Name(param1: Type1, param2: Type2): ReturnType = expression;

Example:

    let Square(x: Float64): Float64 = (x, x) -> Std.Multiply;

### Both = and :: work for function bodies

Functions accept either = or :: between the signature and the body.

Examples:

    let Add(x: Float64, y: Float64): Float64 = (x, y) -> Std.Add;
    let AddAlt(x: Float64, y: Float64): Float64 :: (x, y) -> +;

This is a real, tested part of the language.

## Zero-argument functions

Functions do not need parameters.

Example:

    let Pi(): Float64 = 3.14159265358979;

Called as:

    () -> Pi -> Std.Println;

The empty tuple is the normal zero-argument call payload.

## Qualified function names

Functions may be declared with qualified names.

Example:

    let MyMath.Square(x: Float64): Float64 = (x, x) -> Std.Multiply;

Called as:

    (6.0) -> MyMath.Square -> Std.Println;

Use this when a namespace-like organization improves readability.

## Functions can call other functions

Ordinary named functions compose naturally.

Example:

    let Square(x: Float64): Float64 = (x, x) -> Std.Multiply;

    let SumOfSquares(a: Float64, b: Float64): Float64 =
        (a) -> Square -> (_, (b) -> Square) -> Std.Add;

This is an important Fleaux pattern:

- compute a value with one pipeline
- compute another related value with a second pipeline
- group them into a new tuple
- continue flowing

## Functions are first-class values

A function can be passed around like any other value.

Example:

    let ApplyTwice(x: Float64, f: (Float64) => Float64): Float64 =
        (x, f) -> Std.Apply -> (_, f) -> Std.Apply;

    (3.0, Square) -> ApplyTwice -> Std.Println;

This works because function types are part of the normal type system and named functions are values at expression level.

## Closures

A closure literal has the form:

    (params...): ReturnType = body

Examples:

    (x: Float64): Float64 = (x, 1.0) -> Std.Add;
    (): Any = ("Empty Closure") -> Std.Println;

A closure is an expression value, which means it can appear:

- as an argument to another function
- as a pipeline stage target
- inside a tuple
- as the return value of a function

## Closures as arguments

A direct argument example:

    (10.0, (x: Float64): Float64 = (x, 1.0) -> Std.Add) -> Std.Apply;

This creates a closure value and passes it to Std.Apply.

## Closures as direct pipeline targets

A closure can also be used directly as the next stage in a pipeline.

Example:

    (10.0) -> (x: Float64): Float64 = (x, 10.0) -> Std.Add;

This is a compact and expressive style when the closure is only used once.

## Capturing outer values

Closures capture surrounding lexical bindings.

Example:

    let MakeAdder(n: Float64): (Float64) => Float64 =
        (x: Float64): Float64 = (x, n) -> Std.Add;

Here the closure captures n from the outer function scope.

The sample set uses this pattern heavily in higher-order examples, including function factories and parallel helpers.

## Returning closures

Because closures are values, a function can return one.

Example:

    let MakeScaler(factor: Float64): (Float64) => Float64 =
        (x: Float64): Float64 = (x, factor) -> Std.Multiply;

This lets you build parameterized behavior without introducing special object syntax.

## Blocks are expressions

A block is enclosed in braces and evaluates to its final expression.

Example:

    let Offset: Int64 = {
      let left: Int64 = 1;
      let right: Int64 = 1;
      (left, right) -> Std.Add;
    };

This is not a statement-only block.
It is an expression that produces a value.

## What can appear inside a block

A block contains:

- local let bindings
- expression statements
- a final expression whose value becomes the value of the block

The current grammar model and implementation use semicolon-terminated items throughout.

## Local lets inside blocks

A local let has the same explicit type style as a top-level named value.

Example:

    let result: Int64 = {
      let x: Int64 = 20;
      let y: Int64 = 22;
      (x, y) -> Std.Add;
    };

Current rules:

- local lets require explicit types
- local lets are immutable
- local lets are lexically scoped to the enclosing block

## Block result values

The final expression of a block becomes the block's value.

Example:

    let Answer: Int64 = {
      let a: Int64 = 40;
      let b: Int64 = 2;
      (a, b) -> Std.Add;
    };

The block's result is the value produced by the last expression.
That final expression still ends with ;

## Shadowing

Inner bindings can shadow outer bindings.

Example:

    let Compute(): Int64 = {
      let base: Int64 = 40;
      let nested: Int64 = {
        let base: Int64 = 3;
        base;
      };
      (base, nested) -> Std.Add;
    };

This is a normal lexical-scoping behavior and is exercised by samples/52_variables_and_blocks.fleaux.

## When to use a block instead of a long pipeline

A straight pipeline is often the clearest choice when each stage obviously follows the last one.
A block is better when:

- you need to name an intermediate result
- you want to compute several intermediate values before combining them
- a line is becoming too dense to scan
- an inner scope improves clarity or avoids name leakage

This is a stylistic guideline, not a parser requirement, but it maps well to the current language design.

## Local functions versus local closures

At the moment, nested named local functions are not supported inside blocks.
If you need local callable behavior inside a block, use a closure value instead.

That means this style is current and idiomatic:

    let Process(x: Float64): Float64 = {
      let bias: Float64 = 2.0;
      ((x), (y: Float64): Float64 = (y, bias) -> Std.Add) -> Std.Apply;
    };

## Ungrouped closure stages and readability

Ungrouped inline closure pipeline targets are supported.
They are convenient, but when the closure body itself becomes visually complex, extra grouping is the safer and clearer style.

For example, this concise form is fine when the body is simple:

    (10.0) -> (x: Float64): Float64 = (x, 1.0) -> Std.Add;

As the closure body grows, consider structuring the surrounding expression more explicitly or introducing a named helper.
This is primarily a readability recommendation, but it also aligns with the parser's strongest path through complex closure-heavy expressions.

## Practical design patterns

### Function factory

    let MakeScaler(factor: Float64): (Float64) => Float64 =
        (x: Float64): Float64 = (x, factor) -> Std.Multiply;

### Dictionary of callables

    let BuildOps(): Dict(String, (Float64) => Float64) =
        () -> Std.Dict.Create
        -> (_, "double", Double) -> Std.Dict.Set
        -> (_, "square", Square) -> Std.Dict.Set;

### Local naming with a block

    let Compute(): Int64 = {
      let base: Int64 = 40;
      let offset: Int64 = 2;
      (base, offset) -> Std.Add;
    };

### Closure callback

    ((1.0, 2.0, 3.0), (x: Float64): Float64 = (x, 2.0) -> Std.Multiply) -> Std.Tuple.Map;

## Current constraints worth remembering

- declaration-site type annotations are expected and common
- local let bindings are immutable
- nested named local functions are not supported
- mutation and assignment are not part of the current variable model
- closure-heavy expressions remain easier to read when grouped deliberately

## Where to go next

Functions, closures, and blocks give you the vocabulary for structuring computation.
The next step is the other half of real programs: choosing between paths, repeating work, and expressing recursion.
Continue with 06 Control Flow and Recursion.

