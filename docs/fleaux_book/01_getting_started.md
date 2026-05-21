# 01 Getting Started

This chapter gets a Fleaux program running, explains the structure of a source file, and shows the smallest set of rules you need before the larger ideas in the rest of the book start to feel natural.

## Your first Fleaux program

Create a file containing:

    import Std;

    ("Hello, World!") -> Std.Println;

This program does three things:

1. Imports the standard library module
2. Creates a string value
3. Pipes that value into Std.Println

The language reads most naturally from left to right:

- produce a value
- send it to a callable stage
- optionally keep transforming it

## Running a file

The native CLI executable is fleaux.
Run a source file like this:

    fleaux samples/01_hello_world.fleaux

A representative successful run prints:

    Hello, World!
    Hello from Fleaux

That behavior is verified by the current debug binary against samples/01_hello_world.fleaux.

## The smallest useful program shape

A typical beginner program looks like this:

    import Std;

    let Square(x: Float64): Float64 = (x, x) -> Std.Multiply;

    (6.0) -> Square -> Std.Println;

This file already demonstrates several important ideas:

- imports are ordinary top-level statements
- functions are declared with let
- types are explicit
- application is usually written as a pipeline
- each statement ends with ;

## Every statement ends with ;

Semicolons are mandatory.
This is one of the most consistent rules in the language and the parser diagnostics are built around it.

Top-level examples:

    import Std;
    let Answer: Int64 = 42;
    (Answer) -> Std.Println;

Inside blocks, the rule still holds:

    let Value: Int64 = {
      let x: Int64 = 20;
      let y: Int64 = 22;
      (x, y) -> Std.Add;
    };

The final expression in a block also ends with ;

## What can appear at the top level

A source file is a sequence of top-level statements.
The current top-level forms are:

- import statements
- type declarations
- alias declarations
- let declarations for functions and values
- expression statements

Example:

    import Std;
    type UserId = Int64;
    alias Name = String;
    let Greeting: String = "hello";
    let Echo(name: Name): Name = name;
    (Greeting) -> Echo -> Std.Println;

## Comments

Single-line comments begin with //.

    import Std;

    // This prints one line.
    ("hi") -> Std.Println;

## Running samples

The fastest way to see the language in action is to run the sample set.
Start with:

- samples/01_hello_world.fleaux
- samples/03_pipeline_chaining.fleaux
- samples/04_function_definitions.fleaux
- samples/05_select.fleaux
- samples/29_inline_closures.fleaux
- samples/52_variables_and_blocks.fleaux

Useful one-file invocations:

    fleaux samples/03_pipeline_chaining.fleaux
    fleaux samples/29_inline_closures.fleaux
    fleaux samples/52_variables_and_blocks.fleaux

## The standard library import is explicit

Most ordinary programs begin with:

    import Std;

That is not decorative.
It is required before Std symbols are visible.
Without it, expressions like this fail:

    (1, 2) -> Std.Add -> Std.Println;

One more rule matters here:

- importing Std does not make Std.Add callable as Add

So this is valid:

    import Std;
    (1, 2) -> Std.Add -> Std.Println;

And this is not:

    import Std;
    (1, 2) -> Add -> Std.Println;

## Source files, modules, and imports

A Fleaux file is also a module.
A file named my_library.fleaux is imported as:

    import my_library;

Digit-leading names are supported too.
The sample file 20_export.fleaux is imported as:

    import 20_export;

This is a real part of the language, not a workaround.

## A first example with an imported function

Suppose one file contains:

    import Std;

    let Add4(x: Float64): Float64 = (4.0, x) -> Std.Add;

And another contains:

    import Std;
    import 20_export;

    (4.0) -> Add4 -> Std.Println;

The imported name Add4 is callable directly because normal imports introduce declared symbols.
Builtin Std names remain qualified.

## Using the CLI for inspection

The CLI can do more than run a program.
Current help output supports these useful modes:

- --repl
- --no-run
- --optimize
- --no-emit-bytecode
- --auto-value-ref
- --value-ref-byte-cutoff N
- --disassemble
- --dump-ast
- --dump-ir
- --no-color

Examples:

    fleaux --dump-ast samples/03_pipeline_chaining.fleaux
    fleaux --dump-ir samples/03_pipeline_chaining.fleaux
    fleaux --disassemble samples/03_pipeline_chaining.fleaux.bc

If you are learning the language, --dump-ast and --dump-ir are especially useful because they let you see how the parser and lowering stages understand a source file.

## Passing command line arguments to a program

Program arguments come after --.
For example:

    fleaux samples/25_fleaux_parser.fleaux -- input1 input2

Inside the language, Std.GetArgs returns the argument tuple.

## A quick tour of the REPL

Start the REPL with:

    fleaux --repl

The current test suite confirms that the REPL:

- executes snippets in VM mode
- requires import Std; before using Std symbols
- resolves normal imports relative to the working directory
- supports Std.Help for builtin documentation

A representative REPL session looks like this:

    import Std;
    let AddOne(x: Float64): Float64 = (x, 1.0) -> Std.Add;
    2.0 -> AddOne -> Std.Println;

## The beginner checklist

Before you go further, make sure these rules are comfortable:

- start ordinary programs with import Std;
- end every statement with ;
- think in terms of values flowing left to right
- use tuples when you need multiple inputs
- expect explicit types on declarations
- remember that Std names remain qualified

## Where to go next

Now that you can read and run a small program, the next step is to understand the central idea that unifies the language: dataflow through pipelines.
Continue with 02 The Dataflow Model.

