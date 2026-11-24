# Fleaux Language Usage Guide

This document is the compact quick-start companion to the fuller language manual in docs/fleaux_book/README.md.
If you want a chapter-by-chapter treatment of the language akin to a real language book, start there and use this file as the shorter overview.

## Purpose

This document is a practical guide for writing, running, and understanding Fleaux programs in the current core implementation. It focuses on the language itself and the native CLI, not the visual editor.

The authoritative builtin surface lives in stdlib/Std.fleaux.
The runnable examples live in samples/.

## What Fleaux is

Fleaux is an expression-oriented, pipeline-first language.
Most programs are written by flowing a value through one or more stages with ->.

A small Fleaux program often looks like this:

    import Std;

    ("Hello, World!") -> Std.Println;
    (3, 4) -> Std.Add -> Std.Println;

Key ideas:

- Everything at the top level is a statement ending with ;
- Function application is usually written as a pipeline stage
- Tuple values are the normal way to pass multiple inputs
- The standard library is namespaced under Std
- Current variable bindings are immutable

## Running Fleaux programs

The native CLI executable is fleaux.
Current help output shows these core forms:

    fleaux [--repl] [--no-run] [--disassemble] [--dump-ast] [--dump-ir] [--no-emit-bytecode] [--no-color] [file.fleaux|file.fleaux.bc] [-- <arg1> <arg2> ...]

Common workflows:

Run a source program:

    fleaux samples/01_hello_world.fleaux

Start the REPL:

    fleaux --repl

Inspect the parsed AST:

    fleaux --dump-ast samples/03_pipeline_chaining.fleaux

Inspect lowered IR:

    fleaux --dump-ir samples/03_pipeline_chaining.fleaux

Disassemble bytecode:

    fleaux --disassemble samples/03_pipeline_chaining.fleaux.bc

Pass program arguments after --:

    fleaux samples/25_fleaux_parser.fleaux -- input1 input2

## Program structure

A source file is a sequence of statements.
Every statement must end with ;

The top-level statement forms are:

- import statements
- strong type declarations
- transparent alias declarations
- let declarations for functions and named values
- expression statements

Comments are line comments starting with //.

Example:

    import Std;
    type UserId = Int64;
    alias Name = String;
    let Greeting: String = "hello";
    let Echo(x: Name): Name = x;
    (Greeting) -> Echo -> Std.Println;

## Imports and modules

Import a module by file stem:

    import Std;
    import my_library;

A module file named 20_export.fleaux can be imported as:

    import 20_export;

Digit-leading module names are intentionally supported.

Important import rules:

- Std is special, but you still must write import Std; before using Std symbols
- Importing Std does not make Std.Add callable as Add
- Qualified Std names stay qualified
- Normal imported symbols are introduced by their declared names

Example:

    import Std;
    import 20_export;

    (4.0) -> Add4 -> Std.Println;

The imported symbol Add4 is callable directly because it is exported from 20_export.fleaux.
By contrast, Std.Add must still be written as Std.Add.

## Declarations with let

Fleaux uses let for both functions and named values.

### Function declarations

Function syntax:

    let Name(param1: Type1, param2: Type2): ReturnType = expression;

Both = and :: are accepted as the declaration-definition separator for functions:

    let Add(x: Float64, y: Float64): Float64 = (x, y) -> Std.Add;
    let AddAlt(x: Float64, y: Float64): Float64 :: (x, y) -> +;

Zero-argument functions are allowed:

    let Pi(): Float64 = 3.14159265358979;

Qualified names are allowed:

    let MyMath.Square(x: Float64): Float64 = (x, x) -> Std.Multiply;

### Named value declarations

Value declaration syntax:

    let Name: Type = expression;

Example:

    let Base: Int64 = 40;
    let Greeting: String = "hello";

Current notes:

- Named values require explicit types
- Named values are immutable
- Top-level values and block-local values use the same explicit type style

## Types

### Primitive types

The built-in primitive names are:

- Int64
- UInt64
- Float64
- String
- Bool
- Null
- Any
- Never

### Tuple types

Tuple types use Tuple<...>:

    Tuple<Int64, String>
    Tuple<Float64...>

Tuple values are written with comma-separated expressions in parentheses:

    (1, 2, 3)
    ("name", 42)
    ()

A trailing comma always makes a tuple value, at any length. It is how a one-item tuple is written, and how a
written tuple is passed as one argument (see Calling functions):

    (5,)
    (1, 2,)

### Applied named types

Applied types look like a generic named type followed by type arguments in angle brackets:

    Dict<String, Any>
    Result<Float64, String>
    TaskHandle<Int64>

The name must be a declared generic type and the number of type arguments must match its declaration.

### Function types

Function types use the form:

    (Input1, Input2) => Output

Examples:

    (String) => Bool
    () => String
    (Float64, Float64) => Float64

A function of two parameters and a function of one tuple parameter are different types:
(Int64, Int64) => Int64 takes two arguments, while (Tuple<Int64, Int64>) => Int64 takes one.

### Union types

Union types use | between alternatives:

    String | Null
    Int64 | UInt64 | Float64

These appear throughout the standard library, for example in values that may be missing.

### Strong types

A type declaration creates a strong nominal type:

    type UserId = Int64;
    type Distance :: Float64;

Use strong types when you want a distinct named type rather than a purely structural synonym.

Strong types can be generic. Each set of type arguments is a distinct type:

    type User = Null;
    type Id<T> = Int64;
    let MakeUserId(raw: Int64): Id<User> = raw -> Std.Cast<Id<User>>;

Runtime-provided types are declared with __builtin__ and have no underlying type, so Std.Cast only allows identity casts for them:

    type Result<T, E> :: __builtin__;
    type TaskHandle<T> :: __builtin__;

### Transparent aliases

An alias declaration creates a transparent synonym:

    alias Name = String;
    alias MaybeName = String | Null;
    alias Handler = (String, Int64) => Bool;

Practical difference:

- type creates a distinct nominal type
- alias creates a transparent shorthand for an existing type expression

Current limitation:

- Generic aliases are not supported

## Generic functions and explicit type arguments

Functions may declare generic parameters:

    let Identity<T>(value: T): T = value;

Some builtins also use generics, for example Std.Cast.
You can supply explicit type arguments on named targets:

    (42) -> Std.Cast<UserId> -> Std.Println;

Generic aliases are not supported, but generic functions are.

Generic parameters can be bounded. A bound is a type, usually a union, and the parameter must be exactly one of its alternatives:

    let Twice<T: Numeric>(x: T): T = (x, x) -> Std.Add;
    let Square<T: Int64 | UInt64>(x: T): T = (x, x) -> Std.Multiply;

Std declares the bounds Numeric, Integer, Signed, and Comparable as aliases.

### Numeric semantics

Arithmetic builtins take two operands of the same numeric type and return that type.

- Int64 and UInt64 arithmetic is exact. Overflow in Add, Subtract, Multiply, Pow, and UnaryMinus is a runtime error.
- Integer division truncates toward zero, Mod has the sign of the dividend, and integer division by zero is a runtime error.
- Integer Pow requires a non-negative exponent.
- Float64 arithmetic follows IEEE-754, so 1.0 / 0.0 is infinity.
- Std.WrappingAdd, Std.WrappingSubtract, and Std.WrappingMultiply wrap around on overflow; Std.SaturatingAdd, Std.SaturatingSubtract, and Std.SaturatingMultiply clamp. These take Int64 or UInt64 only.

## Expressions and pipelines

The most characteristic Fleaux syntax is the flow operator ->.

Simple examples:

    (7.0) -> Std.Math.Sqrt -> Std.Println;
    (3, 4) -> Std.Add -> Std.Println;

A pipeline stage can be:

- a named function
- a qualified builtin such as Std.Add
- an inline closure
- an operator token such as + or ==

Example with several stages:

    (20, 5) -> Std.Divide -> (_, 10) -> Std.Add -> Std.Println;

This computes 20 / 5, then adds 10 to the flowing result.

### The placeholder _

The placeholder _ stands for the value currently flowing through the pipeline.
It is used inside tuple-template stages after ->.

Example:

    (3, 4) -> Std.Add -> (_, 2) -> Std.Multiply;

This means:

- first compute (3, 4) -> Std.Add
- then build a new tuple from the previous result and 2
- then call Std.Multiply

Important rule:

- _ is only valid in tuple-template stages after ->

## Calling functions

In practice, function calls are usually expressed with pipelines.

Single argument:

    6.0 -> MyMath.Square;
    (6.0) -> MyMath.Square;

Multiple arguments:

    (10.0, 20.0) -> Average;

Calling a zero-argument function:

    () -> Pi;

### Parameter lists

Parentheses written directly to the left of -> are a parameter list: each item is one argument.
Anything else on the left of -> is a single argument, including a name, the result of an earlier stage, a tuple
written with a trailing comma, and a tuple nested inside a parameter list.
A parameter list is not a tuple value; it only exists at the call.

    let Sum(a: Int64, b: Int64): Int64 = (a, b) -> +;
    let Whole(t: Tuple<Int64, Int64>): Int64 = t -> Std.Length;
    let P: Tuple<Int64, Int64> = (1, 2);

    (1, 2) -> Sum;        // two arguments
    P -> Whole;           // one argument, a tuple
    (1, 2,) -> Whole;     // a trailing comma makes one tuple value
    ((1, 2)) -> Whole;    // a parameter list holding one tuple
    (1, 2) -> Whole;      // error: two arguments, Whole takes one
    P -> Sum;             // error: one argument, Sum takes two

Inside a parameter list, x... spreads the items of the tuple x as separate arguments, and can be mixed with other
arguments:

    (P...) -> Sum;                         // 3
    let Count(xs: Int64...): Int64 = xs -> Std.Length;
    (9, P...) -> Count;                    // 3

A tuple of unknown length, such as Tuple<Int64...>, can only be spread last, into a variadic parameter.
The spread x... is only valid inside a parameter list, and not in a tuple written with a trailing comma.

Argument count errors suggest the fix: spreading a tuple whose items are the arguments, or adding a trailing comma
to pass several values as one tuple.

### Calling function values

A parameter, a local, or a value let of function type can be named as a call target. Its arguments follow the same
parameter-list rule:

    let OnPair(f: (Int64, Int64) => Int64): Int64 = (3, 4) -> f;
    let Twice: (Int64, Int64) => Int64 = Sum;
    (1, 2) -> Twice;

A local whose name is also the name of a function cannot be a call target; rename the local. A value that is not
of function type cannot be called.

Std.Apply passes one value to a one-parameter function:

    (10.0, Double) -> Std.Apply;

An inline closure can also be used directly as a pipeline target.
It is called like any function value: the left side of -> is its argument list.

## Operators as callable stages

Many operators can appear directly as stage targets.
Examples include:

- +
- -
- *
- /
- %
- ==
- !=
- <
- >
- <=
- >=
- !
- &&
- ||

Examples:

    (8.0, 5.0) -> + -> Std.Println;
    (10, 7) -> > -> Std.Println;
    (True, True) -> && -> Std.Println;

Use either the operator token or the corresponding Std builtin, depending on which reads more clearly.

Each operator is shorthand for a Std builtin and is type-checked exactly like a call to it, so using operators requires import Std:

- + is Std.Add, * is Std.Multiply, / is Std.Divide, % is Std.Mod, ^ is Std.Pow
- == is Std.Equal, != is Std.NotEqual, < > <= >= are the Std comparison builtins
- ! is Std.Not, && is Std.And, || is Std.Or
- - is Std.Subtract with two operands and Std.UnaryMinus (negation of an Int64 or Float64) with one

A single operand is negated; two operands are subtracted. Spread a tuple to use its items as the operands:

    7 -> - -> Std.Println;                      // -7
    let Pair: Tuple<Int64, Int64> = (10, 4);
    (Pair...) -> - -> Std.Println;              // 6

## Blocks and lexical scope

A block is an expression wrapped in braces.
It introduces a new lexical scope.

Example:

    let Offset: Int64 = {
      let left: Int64 = 1;
      let right: Int64 = 1;
      (left, right) -> Std.Add;
    };

Rules for blocks:

- A block evaluates to its final expression
- The final expression still ends with ; inside the block
- Local lets are scoped to the block that declares them
- Inner bindings can shadow outer bindings

Example with nesting:

    let Compute(): Int64 = {
      let base: Int64 = 40;
      let nested: Int64 = {
        let base: Int64 = 3;
        base;
      };
      (base, nested) -> Std.Add;
    };

Current constraints:

- Local let bindings require explicit types
- Local named function declarations are not supported inside blocks
- Mutation and assignment are not part of the current variable model

## Inline closures

Closures are first-class values.
Basic syntax:

    (param1: Type1, param2: Type2): ReturnType = expression

Examples:

    (10.0, (x: Float64): Float64 = (x, 1.0) -> Std.Add) -> Std.Apply;

    (10.0) -> (x: Float64): Float64 = (x, 10.0) -> Std.Add;

A closure used as a pipeline target takes its arguments from the left side by the parameter-list rule:

    (3.0, 4.0) -> (a: Float64, b: Float64): Float64 = (a, b) -> Std.Multiply;

A tuple template can supply those arguments, as for any call target:

    10.0 -> (_, 2.0) -> (a: Float64, b: Float64): Float64 = (a, b) -> Std.Divide;

Zero-argument closures are also allowed:

    () -> (): Any = ("Empty Closure") -> Std.Println;

Closures capture surrounding lexical values:

    let MakeAdder(n: Float64): (Float64) => Float64 =
        (x: Float64): Float64 = (x, n) -> Std.Add;

### Practical note on grouping

Ungrouped inline closure pipeline targets are supported and convenient.
When a closure body itself becomes visually complex or chains multiple pipeline stages, adding explicit grouping around the closure is the safest style.
That keeps the intended boundary of the closure body obvious to readers and to the parser.

## Variadic parameters

Variadic parameters use ... and must be the last parameter.

Examples:

    let Collect(items: Any...): Tuple<Any...> = items;
    let JoinWith(sep: String, parts: Any...): String =
        (sep, parts) -> Std.String.Join;

Behavior notes:

- A scalar flowing into a variadic-only parameter is lifted into a one-element tuple
- A tuple value is one argument too: P -> Collect gives a one-element tuple holding P, while (P...) -> Collect
  passes its items
- A fixed prefix can be followed by a variadic tail
- The variadic tail may be empty

## Control flow patterns

Fleaux currently expresses most control flow through functions rather than statement syntax.

### Select

Std.Select chooses between two already-computed values.

    (True, "yes", "no") -> Std.Select;

Important:

- Both value branches are evaluated before Std.Select is called

### Branch

Std.Branch chooses one of two callables and only calls the selected one.

    ((10.0, 0.0) -> Std.GreaterThan, 10.0, Double, Halve) -> Std.Branch;

Use Branch when you want lazy choice between behaviors rather than eager choice between values.

### Match

Std.Match handles ordered pattern-and-handler cases.

Example with literal patterns and wildcard fallback:

    (0,
      (0, (): Any = "zero"),
      (1, (): Any = "one"),
      (_, (): Any = "many")
    ) -> Std.Match;

Patterns may also be predicate callables:

    let IsEven(n: Float64): Bool = ((n, 2.0) -> Std.Mod, 0.0) -> Std.Equal;

    (8.0,
      (IsEven, (): Any = "even"),
      (_, (): Any = "odd")
    ) -> Std.Match;

This is the most common way to express if, else if, and recursive case analysis.

### Loops

Std.Loop iterates a state value while a continue function returns True.
Std.LoopN adds a maximum iteration cap.

Simple pattern:

    let CountContinue(n: Float64): Bool = (n, 0.0) -> Std.GreaterThan;
    let CountStep(n: Float64): Float64 = (n, 1.0) -> Std.Subtract;
    (10.0, CountContinue, CountStep) -> Std.Loop;

State is often a tuple when you need accumulators.

## Recursion and higher-order functions

Recursion works through ordinary function declarations.
Example factorial:

    let Factorial(n: Int64): Int64 =
        (n,
         (0, (): Int64 = 1),
         (_, (): Int64 = (n, ((n, 1) -> Std.Subtract) -> Factorial) -> Std.Multiply)
        ) -> Std.Match;

Functions and closures are first-class.
You can:

- call a function value held in a parameter or local, such as x -> f
- return a function
- store functions in dictionaries
- pass functions to tuple, task, and branch helpers

Function factory example:

    let MakeScaler(factor: Float64): (Float64) => Float64 =
        (x: Float64): Float64 = (x, factor) -> Std.Multiply;

## Strings

Strings are standard values with rich Std.String helpers.
Common operations include:

- Upper and Lower
- Trim, TrimStart, TrimEnd
- Split and Join
- Replace
- Contains, StartsWith, EndsWith
- Length, CharAt, Slice, Find
- Format
- Regex helpers
- ParseInt64, ParseUInt64, ParseFloat64

Examples:

    ("hello world") -> Std.String.Upper -> Std.Println;
    ("a,b,c", ",") -> Std.String.Split -> Std.Println;
    (",", ("a", "b", "c")) -> Std.String.Join -> Std.Println;
    ("{} + {} = {}", 2, 3, 5) -> Std.String.Format -> Std.Println;

The formatting helper supports width, alignment, precision, fill, and indexed placeholders.
See samples/26_format_specifiers.fleaux for a focused tour.

### Raw strings

A raw string starts and ends with three or more quotes. Its content is taken as written: backslashes are not escapes and quotes need no escaping.

    """C:\temp\new "quoted" \d+""" -> Std.Println;

When only whitespace follows the opening quotes, the raw string is multi-line. The closing quotes stand on a line of their own, and the whitespace before them is removed from every line, so the literal can be indented with the surrounding code:

    let Shader: String = """
      #version 330 core
      void main() {
        gl_FragColor = vec4(1.0);
      }
      """;

Rules:

- the line breaks right after the opening quotes and right before the closing quotes are not part of the value
- a content line indented less than the closing quotes is an error
- lines of only whitespace become empty, and every line ends with \n whatever the source file uses
- a run of quotes inside must be shorter than the delimiter, so use four quotes to put three in a row inside
- an empty multi-line raw string is an error; write "" instead

The formatter keeps the content's indentation relative to the closing quotes and moves the whole literal with the surrounding code.
See samples/61_raw_strings.fleaux.

## Tuples

Tuples are central to Fleaux.
They serve as:

- ordinary grouped values
- argument sources for multi-parameter calls, through a spread such as (pair...) -> Add
- loop state
- variadic containers
- lightweight collections

Useful tuple helpers include:

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
- Std.Tuple.Reduce
- Std.Tuple.Any
- Std.Tuple.All
- Std.Tuple.Range
- Std.Wrap and Std.Unwrap

Examples:

    ((1, 2, 3), 1) -> Std.ElementAt -> Std.Println;
    ((1.0, 2.0, 3.0), Double) -> Std.Tuple.Map -> Std.Println;
    ((1.0, 2.0, 3.0, 4.0), IsEven) -> Std.Tuple.Filter -> Std.Println;

## Dictionaries

Dictionary values use Dict<KeyType, ValueType>.
Create and extend them functionally:

    let Build(): Dict<Any, Any> =
        () -> Std.Dict.Create
        -> (_, "name", "fleaux") -> Std.Dict.Set
        -> (_, "year", 2026) -> Std.Dict.Set;

Common helpers:

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

Functions can be stored as dictionary values too, which enables callable dispatch tables.
See samples/42_dict_dispatch.fleaux.

## Arrays and rectangular data helpers

The standard library also exposes array-oriented helpers over tuple-backed data.
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

These are useful when you want more array-like naming over tuple-shaped data.

## Results and error handling

Result values are used for explicit success and error transport.
The standard helpers include:

- Std.Result.Ok
- Std.Result.Err
- Std.Result.Tag
- Std.Result.Payload
- Std.Result.IsOk
- Std.Result.IsErr
- Std.Result.Unwrap
- Std.Result.UnwrapErr
- Std.Try

Example pattern:

    let ReciprocalOk(x: Float64): Result<Float64, String> =
        (1.0, x) -> Std.Divide -> Std.Result.Ok;

    let ReciprocalErr(x: Float64): Result<Float64, String> =
        ("division by zero") -> Std.Result.Err;

    let SafeReciprocalResult(x: Float64): Result<Float64, String> =
        ((x, 0.0) -> Std.Equal, ReciprocalErr, ReciprocalOk) -> Std.Select -> (x, _) -> Std.Apply;

When using Result values, Match and Branch often pair well with IsOk and IsErr.

Std.Result.Ok returns Result<T, Never> and Std.Result.Err returns Result<Never, E>.
Never is the type with no values, so it combines with any other type: branches that produce Ok and Err values join into one Result type.

    let Pick(c: Bool): Result<Int64, String> =
        (c, (1) -> Std.Result.Ok, ("bad") -> Std.Result.Err) -> Std.Select;

Unwrapping an Err value has type Never, which fits wherever a value is expected because it never produces one.

## OS, path, file, and directory helpers

The standard library provides host interaction helpers grouped by namespace.

### Std.OS

Examples:

- Cwd
- Home
- TempDir
- Exec
- MakeTempFile
- MakeTempDir
- Env
- HasEnv
- SetEnv
- UnsetEnv
- IsWindows
- IsLinux
- IsMacOS

### Std.Path

Examples:

- Join
- Normalize
- Basename
- Dirname
- Exists
- IsFile
- IsDir
- Absolute
- Extension
- Stem
- WithExtension
- WithBasename

### Std.File

Examples:

- ReadText
- WriteText
- AppendText
- ReadLines
- Delete
- Size
- Open
- ReadLine
- ReadChunk
- WriteChunk
- Flush
- Close
- WithOpen

### Std.Dir

Examples:

- Create
- Delete
- List
- ListFull

See samples/14_os.fleaux, samples/15_path.fleaux, samples/16_file_and_dir.fleaux, and samples/22_file_streaming.fleaux.

## Parallel and task helpers

Fleaux includes experimental parallel and task-oriented helpers in Std.

Parallel tuple helpers:

- Std.Parallel.Map
- Std.Parallel.WithOptions
- Std.Parallel.ForEach
- Std.Parallel.Reduce

Task helpers:

- Std.Task.Spawn
- Std.Task.Await
- Std.Task.AwaitAll
- Std.Task.Cancel
- Std.Task.WithTimeout

Example shape:

    (Inc, 41) -> Std.Task.Spawn -> Std.Task.Await -> Std.Result.Unwrap -> Std.Println;

These helpers return Result values where failure needs to be surfaced explicitly.
See samples/33_exp_parallel.fleaux through samples/38_parallel_inline_closures.fleaux.

## Strong typing and casts

Strong nominal types are useful for domain values such as identifiers.
Example:

    type UserId = Int64;

    let EchoUserId(value: UserId): UserId = value;

    let RenderUserId(value: UserId): String =
      value -> Std.Cast<Int64> -> Std.ToString;

    (42) -> Std.Cast<UserId> -> EchoUserId -> RenderUserId -> Std.Println;

Use this pattern when you want domain-level type separation without changing the runtime representation.

## Builtin naming conventions

The standard library follows a namespace-style naming scheme:

- Std for core builtins and constants
- Std.Math for math helpers
- Std.String for string helpers
- Std.String.Regex for regex helpers
- Std.OS for operating-system helpers
- Std.Path for path helpers
- Std.File for file helpers
- Std.Dir for directory helpers
- Std.Tuple for tuple-focused helpers
- Std.Dict for dictionary helpers
- Std.Array for array-shaped helpers
- Std.Result for result helpers
- Std.Parallel for parallel helpers
- Std.Task for task helpers
- Std.Bit for bitwise helpers

This keeps most functionality discoverable by category.

## Important current language rules and limitations

These are worth knowing up front when authoring programs.

### Semicolons are mandatory

Every statement ends with ;
Parser diagnostics are written around that rule.

### Std symbols stay qualified

Even after import Std; you still write Std.Add rather than Add.

### Variables are immutable

Current let bindings introduce immutable names.
There is no assignment syntax in the current model.

### Local lets need explicit types

Inside blocks, local lets currently require explicit type annotations.

### Generic aliases are not supported

Use generic functions instead.

### Nested named local functions are not supported

Use inline closures when you need local callable behavior inside a block.

### Placeholder usage is restricted

Use _ only in tuple-template stages after ->.

### Prefer grouping when inline closures get complex

Ungrouped inline closure pipeline targets are supported, but adding grouping is the safer style when the closure body becomes visually complex or continues through several stages.

## Sample-driven learning path

The sample set in samples/ is the best executable tour of the language.
A practical reading order is:

- 01 through 04 for basics, pipelines, and functions
- 05 through 09 for value selection, branching, and looping
- 10 through 18 for strings, tuples, math, comparison, OS, path, files, and constants
- 19 through 24 for composition, imports, streaming files, and dictionaries
- 25 through 31 for parser ideas, formatting, error handling, variadics, closures, pattern matching, and results
- 33 through 38 for parallel and task helpers
- 39 through 47 for match-driven control flow, recursion, higher-order functions, and dispatch styles
- 48 through 52 for more dataflow idioms, strong type casts, and block-scoped values

Representative sample files:

- samples/01_hello_world.fleaux
- samples/03_pipeline_chaining.fleaux
- samples/04_function_definitions.fleaux
- samples/21_import.fleaux
- samples/28_variadics.fleaux
- samples/29_inline_closures.fleaux
- samples/30_pattern_matching.fleaux
- samples/35_concurrency_tasks.fleaux
- samples/51_strong_type_casts.fleaux
- samples/52_variables_and_blocks.fleaux

## Practical style advice

- Import Std explicitly at the top of ordinary programs
- Use pipelines for the main flow and helper functions for named substeps
- Keep tuple shapes simple and deliberate
- Use Match for multi-way decisions and recursive case analysis
- Use Branch when you need lazy choice between behaviors
- Use blocks when a computation benefits from local names and scope
- Use strong types for domain boundaries and aliases for ergonomic shorthand
- Prefer explicit grouping when a closure or pipeline becomes visually dense

## Where to look next

For exact builtin signatures and constants:

- stdlib/Std.fleaux

For executable examples:

- samples/

For syntax reference and parser-aligned structure:

- fleaux_grammar.tx

For embedding and host integration:

- docs/embedding.md


