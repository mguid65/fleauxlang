# 07 Collections, Strings, and Data Modeling

This chapter covers the collection and text-oriented parts of the current language surface, with an emphasis on how Fleaux programs model data through tuples, dictionaries, arrays, and strings.

## Tuples as the foundational data structure

Tuples are the most fundamental aggregate value in Fleaux.
They are used for:

- argument grouping
- return-value grouping
- lightweight records
- loop state
- variadic payloads
- collection-like operations through Std.Tuple and related helpers

Examples:

    (1, 2, 3)
    ("name", 42)
    (True, "ok", 5.0)
    ()

## Tuple helpers

The standard library exposes many tuple-oriented helpers.
Representative examples include:

- Std.ElementAt
- Std.Length
- Std.Take
- Std.Drop
- Std.Slice
- Std.Tuple.Append
- Std.Tuple.Prepend
- Std.Tuple.Reverse
- Std.Tuple.Contains
- Std.Tuple.Zip
- Std.Tuple.Map
- Std.Tuple.Filter
- Std.Tuple.Sort
- Std.Tuple.Unique
- Std.Tuple.Min
- Std.Tuple.Max
- Std.Tuple.Reduce
- Std.Tuple.FindIndex
- Std.Tuple.Any
- Std.Tuple.All
- Std.Tuple.Range
- Std.Wrap
- Std.Unwrap

## Common tuple patterns

### Indexing

    ((1, 2, 3), 0) -> Std.ElementAt -> Std.Println;

### Mapping

    let Double(x: Float64): Float64 = (x, 2.0) -> Std.Multiply;
    ((1.0, 2.0, 3.0), Double) -> Std.Tuple.Map -> Std.Println;

### Filtering

    let IsEven(x: Float64): Bool = (x, 2.0) -> Std.Mod -> (_, 0.0) -> Std.Equal;
    ((1.0, 2.0, 3.0, 4.0), IsEven) -> Std.Tuple.Filter -> Std.Println;

### Reducing

    let Add(acc: Int64, x: Int64): Int64 = (acc, x) -> Std.Add;
    ((1, 2, 3, 4), 0, Add) -> Std.Tuple.Reduce;

### Zipping

    ((1, 2, 3), (4, 5, 6)) -> Std.Tuple.Zip -> Std.Println;

## Strings

Strings are a first-class built-in type with extensive support under Std.String.

Important helpers include:

- Upper and Lower
- Trim, TrimStart, TrimEnd
- Split and Join
- Replace
- Contains, StartsWith, EndsWith
- Length, CharAt, Slice, Find
- Format
- Regex helpers
- ParseInt64, ParseUInt64, ParseFloat64

## Basic string operations

Examples:

    ("hello world") -> Std.String.Upper -> Std.Println;
    ("HELLO WORLD") -> Std.String.Lower -> Std.Println;
    ("  fleaux  ") -> Std.String.Trim -> Std.Println;
    ("hello world", "world") -> Std.String.Contains -> Std.Println;

## Building strings from parts

Joining and formatting are the two most important text-building tools.

Join example:

    (",", ("a", "b", "c")) -> Std.String.Join -> Std.Println;

Format example:

    ("{} + {} = {}", 2, 3, 5) -> Std.String.Format -> Std.Println;

You can often choose between explicit tuple assembly plus Join, or a format string, depending on which is clearer.

## String formatting

The format helper supports:

- ordinary replacement fields
- escaped braces
- alignment
- width
- fill
- integer formatting
- float formatting
- indexed formatting

Representative examples from the sample set include:

    ("|{:>10}|{:^10}|{:<10}|", "right", "center", "left") -> Std.String.Format;
    ("dec={} hex={:#x} HEX={:#X} bin={:#b}", 255, 255, 255, 10) -> Std.String.Format;
    ("fixed={:.2f} sci={:.4e} general={:.3g}", 3.14159265, 1234.5678, 1234.5678) -> Std.String.Format;

See samples/26_format_specifiers.fleaux for a compact tour.

## Regex helpers

The current standard library includes regex-oriented string helpers:

- Std.String.Regex.IsMatch
- Std.String.Regex.Find
- Std.String.Regex.Replace
- Std.String.Regex.Split

These let text-heavy programs stay within the same pipeline style rather than bouncing into a separate host-language tool.

## Dictionaries

Dictionaries use the applied type Dict(KeyType, ValueType).
They are immutable from the language user's perspective, in the sense that Std.Dict.Set and related helpers return updated dictionary values rather than mutating a visible variable in place.

Important helpers include:

- Std.Dict.Create
- Std.Dict.Set
- Std.Dict.Get
- Std.Dict.GetDefault
- Std.Dict.Contains
- Std.Dict.Delete
- Std.Dict.Merge
- Std.Dict.Keys
- Std.Dict.Values
- Std.Dict.Entries
- Std.Dict.Clear
- Std.Dict.Length

## Building dictionaries functionally

A common style is:

    let Build(): Dict(Any, Any) =
        () -> Std.Dict.Create
        -> (_, "name", "fleaux") -> Std.Dict.Set
        -> (_, "year", 2026) -> Std.Dict.Set;

This is a good example of how the pipeline model works well with immutable data construction.

## Looking up values

Examples:

    (() -> Build, "name") -> Std.Dict.Get -> Std.Println;
    (() -> Build, "missing", "n/a") -> Std.Dict.GetDefault -> Std.Println;
    () -> Build -> Std.Dict.Keys -> Std.Println;

## Dictionaries of callables

A very useful Fleaux pattern is storing functions in dictionaries.

Example:

    let BuildOps(): Dict(String, (Float64) => Float64) =
        () -> Std.Dict.Create
        -> (_, "double", Double) -> Std.Dict.Set
        -> (_, "square", Square) -> Std.Dict.Set;

    let ApplyNamedOp(name: String, value: Float64): Float64 =
        () -> BuildOps
        -> (_, name, Identity) -> Std.Dict.GetDefault
        -> (value, _) -> Std.Apply;

This lets you express dispatch tables directly in the language.

## Arrays and rectangular data helpers

The current standard library also provides array-oriented helpers over tuple-backed data.
These include:

- Std.Array.GetAt
- Std.Array.SetAt
- Std.Array.InsertAt
- Std.Array.RemoveAt
- Std.Array.Slice
- Std.Array.Concat
- Std.Array.SetAt2D
- Std.Array.Fill
- Std.Array.Transpose2D
- Std.Array.Slice2D
- Std.Array.Reshape
- Std.Array.Rank
- Std.Array.Shape
- Std.Array.Flatten
- Std.Array.GetAtND
- Std.Array.SetAtND
- Std.Array.ReshapeND

These helpers are useful when you want array vocabulary and rectangular structure operations while staying in the tuple-centric value model.

## Results as data

Result values are important enough to mention here even though the next chapter covers error handling more directly.
Their constructors and helpers include:

- Std.Result.Ok
- Std.Result.Err
- Std.Result.Tag
- Std.Result.Payload
- Std.Result.IsOk
- Std.Result.IsErr
- Std.Result.Unwrap
- Std.Result.UnwrapErr

Because Result is just another value shape, it composes naturally with Match, Branch, dictionaries, and tuple helpers.

## Type introspection and conversion

Useful general helpers for data shaping include:

- Std.Type
- Std.ToString
- Std.ToInt64
- Std.ToUInt64
- Std.ToFloat64
- Std.Cast<T>

Example:

    let TypeAndValueText<T>(value: T): Tuple(String, String) =
        ((value) -> Std.Type, (value) -> Std.ToString);

This demonstrates that many reflective and formatting tasks are still handled as plain value transformations.

## Data-modeling advice

A few guidelines help keep Fleaux programs understandable:

- use tuples for small structured groups and transient pipeline payloads
- use aliases when tuple or function types become too noisy
- use dictionaries for keyed lookup and dispatch tables
- use strong named types when a bare primitive would hide domain meaning
- use Result when failure should remain explicit in the type

## When to step from tuples to named functions or blocks

A large nested tuple expression can be a sign that the code wants more names.
That does not mean the tuple-based model is wrong.
It often means one of these steps will help:

- extract a helper function
- introduce a block with local lets
- name a predicate or reducer that is currently inlined

## Where to go next

The next chapter turns from data modeling to interaction with the outside world and runtime services: files, the OS, task helpers, and parallel work.
Continue with 08 Effects, Files, the OS, and Concurrency.

