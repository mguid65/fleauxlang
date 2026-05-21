# 04 Types, Type Declarations, and Aliases

This chapter covers the type language that Fleaux programs use in declarations, callables, collections, and strong domain modeling.

## Why types matter so much in Fleaux

The current core implementation expects explicit types in many declaration sites.
That makes types highly visible in day-to-day code.

You will see types in:

- function parameters
- function return types
- top-level named values
- block-local lets
- closure signatures
- explicit type arguments such as Std.Cast<UserId>

## Primitive types

The built-in primitive type names are:

- Int64
- UInt64
- Float64
- String
- Bool
- Null
- Any
- Never

A few notes:

- Int64 and UInt64 are integer types
- Float64 is the main floating-point type
- Null is the type associated with null-like absence
- Any is the most permissive general-purpose type
- Never is used for computations that do not return, such as Std.Exit

## Literal overview

Common literal shapes include:

- integer literals such as 1 or 42
- floating values such as 2.0 or 3.14159
- string literals such as "hello"
- boolean literals such as True and False
- the null literal

Type names and literal syntax are different concepts.
For example, the type is Null, while the literal form is null.

## Function parameter and return type syntax

A typical function declaration looks like this:

    let Square(x: Float64): Float64 = (x, x) -> Std.Multiply;

The pattern is:

- parameter names each have a type
- the function itself declares a return type
- the definition body appears after = or ::

Multiple parameters:

    let Average(a: Float64, b: Float64): Float64 =
        (a, b) -> Std.Add -> (_, 2.0) -> Std.Divide;

Zero parameters:

    let Pi(): Float64 = 3.14159265358979;

## Tuple types

Tuple types use the form Tuple(...).

Examples:

    Tuple(Int64, Int64)
    Tuple(String, Bool)
    Tuple(Float64...)

Tuple types are heavily used because tuples are the normal way to package multiple values.

Examples from real usage:

- loop state such as Tuple(Float64, Float64)
- result groups such as Tuple(String, String)
- collection-like values such as Tuple(Int64...)

## Applied named types

Applied types are named types with type arguments in parentheses.

Examples:

    Dict(String, Any)
    Result(Float64, String)
    Result(Tuple(Any...), Tuple(Int64, String))

These show up often in the standard library, especially for dictionaries, results, and callable-heavy APIs.

## Function types

Function types use this form:

    (Input1, Input2) => Output

Examples:

    (String) => Bool
    () => String
    (Float64, Float64) => Float64

Function types matter whenever a value is itself callable.
For example:

    let ApplyTwice(x: Float64, f: (Float64) => Float64): Float64 =
        (x, f) -> Std.Apply -> (_, f) -> Std.Apply;

They also appear in dictionary values and closure-returning functions.

## Nested function types

Function types can nest.
For example, a function can accept a callable that itself accepts another callable.
The parser tests explicitly cover nested function type syntax.

You do not need to reach for this immediately, but it is important to know that callable values are part of the ordinary type language.

## Union types

Union types combine alternatives with |.

Examples:

    String | Null
    Int64 | UInt64 | Float64
    Result(String | Null, String)

Union types are common in APIs that may succeed with one of several compatible shapes or may return a missing value.
The standard library uses them in several signatures, such as environment lookups.

## Strong nominal types with type

A type declaration creates a strong named type.

Examples:

    type UserId = Int64;
    type Distance :: Float64;

The grammar accepts both = and :: for strong type declarations.

Use a strong type when you want a domain concept to stay distinct even if its underlying representation is simple.
For example, a UserId may be represented by Int64 at runtime but still carry its own meaning at the language level.

## Transparent aliases with alias

An alias declaration creates a transparent shorthand for an existing type expression.

Examples:

    alias Name = String;
    alias Pair = Tuple(Int64, Int64);
    alias Handler = (String, Int64) => Bool;
    alias MaybeName = String | Null;

Transparent aliases do not behave like strong nominal barriers.
They are meant for readability and convenience.

The type checker tests explicitly verify that aliases are expanded transparently across parameter and return signatures.

## Strong types versus aliases

This distinction is important enough to repeat plainly.

Use type when:

- you want a distinct named domain type
- you want the name to carry semantic weight beyond its structure
- you want code to say more than just the underlying representation

Use alias when:

- you want a shorter or clearer name for an existing type expression
- you want no extra nominal barrier
- you want to simplify verbose tuple, union, or function types

## Example: strong type for domain identity

    type UserId = Int64;

    let EchoUserId(value: UserId): UserId = value;

    let RenderUserId(value: UserId): String =
        value -> Std.Cast<Int64> -> Std.ToString;

    (42) -> Std.Cast<UserId> -> EchoUserId -> RenderUserId -> Std.Println;

This pattern appears in samples/51_strong_type_casts.fleaux.

## Generic functions

Functions may declare generic parameters.

Example shape:

    let Identity<T>(value: T): T = value;

Many Std helpers are generic, including callable adapters and collection operations.
Examples from the standard library include:

- Std.Cast<T>
- Std.Apply<T, U>
- Std.Select<T>
- Std.Tuple.Map<T, U>
- Std.Dict.Set<K, V>

Generic functions are a normal part of the language surface.

## Explicit type arguments

Named targets can receive explicit type arguments.
This is how Std.Cast<UserId> is written.

Example:

    (42) -> Std.Cast<UserId> -> Std.Println;

The parser has direct coverage for explicit type argument application on named targets.

## Current restriction: generic aliases are not supported

This is one of the most important current type-language restrictions.

This kind of declaration is rejected:

    alias Box<T> = T;

The parser tests explicitly check that generic transparent alias declarations are rejected with a clear diagnostic.

## Variadic types and parameters

Variadic parameters use ... and must appear last.
This shows up both in declaration syntax and tuple-shaped types.

Examples:

    let Collect(items: Any...): Tuple(Any...) = items;
    let JoinWith(sep: String, parts: Any...): String =
        (sep, parts) -> Std.String.Join;

Important rule:

- variadic parameters must be the final parameter

This rule is enforced by lowering and runtime behavior.

## Types for values and local lets

Named values use explicit type annotations.

Top level:

    let Base: Int64 = 40;

Inside a block:

    let Offset: Int64 = {
      let left: Int64 = 1;
      let right: Int64 = 1;
      (left, right) -> Std.Add;
    };

In the current implementation, both top-level and block-local lets require explicit types.

## Callable values in collections and dictionaries

One of Fleaux's more interesting type patterns is storing callables in data structures.
For example:

    let BuildOps(): Dict(String, (Float64) => Float64) =
        () -> Std.Dict.Create
        -> (_, "double", Double) -> Std.Dict.Set
        -> (_, "square", Square) -> Std.Dict.Set;

This is a powerful pattern because it lets you build dispatch tables without adding special language syntax.

## How much type inference should you expect

The current language is not designed around pervasive declaration inference.
You should expect to write explicit types in declaration sites.
This makes programs more verbose, but it also makes the current implementation more predictable.

## Practical style advice

A few patterns help keep typed Fleaux code readable:

- use aliases to simplify long union or function types
- use strong types for domain boundaries such as identifiers and units
- keep tuple types small enough to stay legible
- extract very long callable or result types into aliases where it improves clarity
- be explicit about Result payload and error types

## Where to go next

Types describe values and callables.
The next step is to see how those typed values are declared, captured, and composed through functions, closures, and block scope.
Continue with 05 Functions, Closures, and Blocks.

