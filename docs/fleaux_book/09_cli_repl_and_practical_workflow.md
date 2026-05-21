# 09 CLI, REPL, and Practical Workflow

This chapter explains how to work with Fleaux from the command line, how to use the REPL productively, and how to connect the language manual to the sample set and inspection tools.

## The CLI at a glance

The current CLI help output includes:

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

And the general usage shape is:

    fleaux [options] [file.fleaux or file.fleaux.bc] [-- program arguments]

## Running a source file

Basic form:

    fleaux samples/01_hello_world.fleaux

A few representative programs worth running while reading this book are:

    fleaux samples/03_pipeline_chaining.fleaux
    fleaux samples/29_inline_closures.fleaux
    fleaux samples/52_variables_and_blocks.fleaux

These cover the most central language ideas with minimal setup.

## Running a bytecode file

The CLI also accepts .fleaux.bc input.
This is useful when you want to work directly with serialized bytecode artifacts.

## Passing arguments to a program

Arguments intended for the Fleaux program itself come after --.

Example:

    fleaux samples/25_fleaux_parser.fleaux -- input1 input2

Use Std.GetArgs inside the language to access the resulting argument tuple.

## Using --no-run

--no-run lets you skip execution and inspect what would run.
This can be useful during debugging or workflow experiments when you want parsing and loading behavior without executing the final program body.

## Using --dump-ast

--dump-ast parses a source file, prints the AST, and exits.

Example:

    fleaux --dump-ast samples/03_pipeline_chaining.fleaux

This is useful when:

- you are learning how the parser groups an expression
- you are investigating closure or type syntax
- you want to confirm the shape of nested tuples or blocks

## Using --dump-ir

--dump-ir parses and lowers a source file, prints the IR, and exits.

Example:

    fleaux --dump-ir samples/03_pipeline_chaining.fleaux

This is useful when:

- you want to understand how source constructs map into lowered program structure
- you are investigating typing or callable resolution behavior
- you are debugging a surprising pipeline or block shape

## Using --disassemble

--disassemble prints the disassembly for a .fleaux.bc module and exits.

Example:

    fleaux --disassemble samples/03_pipeline_chaining.fleaux.bc

This is the most useful CLI mode when you want to connect source-level constructs to bytecode-level behavior.

## Bytecode caching and --no-emit-bytecode

The CLI can write or refresh .fleaux.bc cache files while loading modules.
Use --no-emit-bytecode when you want to avoid that.

Example:

    fleaux --no-emit-bytecode samples/01_hello_world.fleaux

This is convenient during experiments where you do not want source runs to update bytecode artifacts.

## Optimization and value-ref options

The current CLI also exposes:

- --optimize
- --auto-value-ref
- --value-ref-byte-cutoff N

These are more advanced runtime and compilation controls.
For everyday learning, you can ignore them.
For performance or runtime-behavior experiments, they become more relevant.

## Starting the REPL

Run:

    fleaux --repl

The current tests confirm that the REPL executes snippets in VM mode.
A representative session is:

    import Std;
    let AddOne(x: Float64): Float64 = (x, 1.0) -> Std.Add;
    2.0 -> AddOne -> Std.Println;

The test suite also uses :quit to exit the session.

## REPL import behavior

The REPL follows the same key rules as source files:

- you must import Std before using Std symbols
- normal imports are resolved relative to the working directory
- unresolved normal imports are reported clearly

This consistency is important because it means learning the source language also teaches you the interactive workflow.

## Using Std.Help interactively

One of the best REPL features for discovery is builtin help.

Example:

    import Std;
    ("Std.Add") -> Std.Help -> Std.Println;

The current tests verify that this prints canonical help text for Std.Add, including a description and parameter information.

This is especially valuable when:

- you remember a builtin name but not its parameter order
- you want confirmation of a return type conceptually
- you are browsing the standard library from inside the language

## Choosing between the REPL and sample files

Use the REPL when:

- you want to try a tiny expression quickly
- you are checking a builtin behavior interactively
- you want immediate feedback on a closure, tuple, or pipeline form

Use sample files when:

- you are learning a feature in context
- the program spans several helper declarations
- you want something that can be rerun and versioned
- the example is rich enough to serve as lasting documentation

## The sample corpus as the practical lab

A strong workflow is:

1. read the relevant chapter of this book
2. run the corresponding sample files
3. inspect the AST or IR for one or two representative examples
4. adapt a sample into your own experiment

A good mapping is:

- basic syntax and pipelines: samples/01 through 04
- control flow: samples/05 through 09, then 30 and 39
- strings, tuples, math, and core builtins: samples/10 through 18
- imports and composition: samples/19 through 24
- parser-shaped and higher-order examples: samples/25 through 31
- parallel and tasks: samples/33 through 38
- advanced dataflow patterns: samples/40 through 52

## Working style for real programs

A practical way to build a new Fleaux program is:

1. start with import Std;
2. write one tiny expression and run it
3. extract repeated logic into a named function
4. use a block when intermediate names improve readability
5. keep complex dataflow explicit through tuples and well-named helpers
6. use samples/ for reference when you need a known-good pattern

## Inspecting parser-sensitive code

When you are exploring more unusual syntax, such as closure-heavy code or nested blocks, this sequence works well:

1. run the file normally
2. run with --dump-ast
3. run with --dump-ir
4. simplify or group the expression if the structure is not obvious

This is particularly helpful for dense closure or pipeline combinations.

## Using the quick start and the book together

The quick start document in docs/fleaux_language_usage_guide.md is good for a compact overview.
The present book is intended to be the long-form manual.
A realistic workflow is:

- quick start for the first impression
- this book for sustained learning and reference
- samples/ for executable confirmation
- Std.Help and stdlib/Std.fleaux for exact builtin details

## Where to go next

The final chapter gathers practical patterns, caveats, feature boundaries, and a map from book topics to the most relevant sample files and source-of-truth references.
Continue with 10 Patterns, Caveats, and a Feature Map.

