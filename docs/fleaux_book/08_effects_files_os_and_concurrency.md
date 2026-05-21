# 08 Effects, Files, the OS, and Concurrency

This chapter covers the parts of Fleaux that interact with the outside world: process state, the filesystem, streaming file handles, task helpers, and parallel tuple operations.

## A note on effects in a pipeline language

Even though Fleaux is expression-oriented and strongly pipeline-based, it is not limited to pure transformation.
The standard library exposes effectful operations in the same value-flow style.

Examples:

- printing
- reading input
- querying the OS
- working with files and directories
- spawning tasks
- mapping or reducing in parallel

The language does not switch into a separate imperative sublanguage for these operations.
They are expressed through ordinary values and callables.

## Core effectful helpers

At the broadest level, some of the most-used effectful builtins are:

- Std.Println
- Std.Printf
- Std.Input
- Std.Help
- Std.Exit
- Std.GetArgs

These are often enough for small command-line utilities.

## Operating-system helpers

Std.OS contains process and environment helpers.
Current examples include:

- Std.OS.Cwd
- Std.OS.Home
- Std.OS.TempDir
- Std.OS.Exec
- Std.OS.MakeTempFile
- Std.OS.MakeTempDir
- Std.OS.Env
- Std.OS.HasEnv
- Std.OS.SetEnv
- Std.OS.UnsetEnv
- Std.OS.IsWindows
- Std.OS.IsLinux
- Std.OS.IsMacOS

Examples of the style:

    () -> Std.OS.Cwd -> Std.Println;
    ("PATH") -> Std.OS.Env -> Std.Println;
    ("echo hello") -> Std.OS.Exec -> Std.Println;

Because these are regular callables, they compose naturally with formatting, matching, and result-handling patterns.

## Path helpers

Std.Path provides path manipulation helpers.
Representative members include:

- Std.Path.Join
- Std.Path.Normalize
- Std.Path.Basename
- Std.Path.Dirname
- Std.Path.Exists
- Std.Path.IsFile
- Std.Path.IsDir
- Std.Path.Absolute
- Std.Path.Extension
- Std.Path.Stem
- Std.Path.WithExtension
- Std.Path.WithBasename

Examples:

    ("src", "main.fleaux") -> Std.Path.Join -> Std.Println;
    ("./samples/../samples/01_hello_world.fleaux") -> Std.Path.Normalize -> Std.Println;

## Whole-file helpers

Std.File provides whole-file helpers such as:

- Std.File.ReadText
- Std.File.WriteText
- Std.File.AppendText
- Std.File.ReadLines
- Std.File.Delete
- Std.File.Size

These are the easiest way to work with files when you do not need streaming access.

Examples:

    ("notes.txt") -> Std.File.ReadText -> Std.Println;
    ("notes.txt", "hello") -> Std.File.WriteText;
    ("notes.txt", "more") -> Std.File.AppendText;

## Directory helpers

Std.Dir provides directory-level operations:

- Std.Dir.Create
- Std.Dir.Delete
- Std.Dir.List
- Std.Dir.ListFull

Examples:

    ("tmp/work") -> Std.Dir.Create;
    ("tmp/work") -> Std.Dir.List -> Std.Println;

## Streaming file handles

For streaming and structured file usage, the language exposes the FileHandle surface type and related helpers.

Key helpers include:

- Std.File.Open
- Std.File.ReadLine
- Std.File.ReadChunk
- Std.File.WriteChunk
- Std.File.Flush
- Std.File.Close
- Std.File.WithOpen

### WithOpen pattern

Std.File.WithOpen is especially helpful because it wraps open, callback, and close into one operation.
Its shape is:

    (path, mode, func) -> Std.File.WithOpen;

This lets you write resource-oriented logic while staying close to the same functional flow style as the rest of the language.

## Command-line programs and Std.GetArgs

Programs can receive arguments after -- at the CLI.
Inside the language, call:

    () -> Std.GetArgs;

This returns a tuple of strings.
Samples/25_fleaux_parser.fleaux uses this sort of environment for parser-oriented behavior.

## Results and effectful failure

Many effectful or concurrency-oriented operations surface failure through Result values rather than hidden exceptions at the language level.
That means you often pair them with:

- Std.Result.IsOk
- Std.Result.IsErr
- Std.Result.Unwrap
- Std.Result.UnwrapErr
- Std.Try
- Std.Match
- Std.Branch

This keeps error paths visible in the data model.

## Task helpers

The task API provides a way to spawn and observe asynchronous work.
Current task helpers include:

- Std.Task.Spawn
- Std.Task.Await
- Std.Task.AwaitAll
- Std.Task.Cancel
- Std.Task.WithTimeout

Example shape:

    (Inc, 41) -> Std.Task.Spawn -> Std.Task.Await -> Std.Result.Unwrap -> Std.Println;

This reads very directly:

- spawn a task from a callable and value
- await the result
- unwrap success
- print it

## Task handles are opaque surface types

The standard library declares TaskHandle as an opaque handle surface type.
You work with it by passing it back into task helpers rather than by inspecting its internal representation.

This is a good example of the language exposing a capability through typed handles rather than exposing host-specific implementation details.

## Waiting for many tasks

AwaitAll collects results from multiple task handles.
The sample set uses the style:

    (((Inc, 1) -> Std.Task.Spawn,
      (Inc, 2) -> Std.Task.Spawn,
      (Inc, 3) -> Std.Task.Spawn))
      -> Std.Task.AwaitAll
      -> Std.Result.Unwrap
      -> Std.Println;

Because the task handles are just values, they fit cleanly into tuples and pipelines.

## Parallel tuple helpers

The parallel helper family focuses on applying callables across tuple items.
Current helpers include:

- Std.Parallel.Map
- Std.Parallel.WithOptions
- Std.Parallel.ForEach
- Std.Parallel.Reduce

### Parallel.Map

Apply a callable to each tuple item and collect a Result.

    ((1.0, 3.0, 4.0), Inc) -> Std.Parallel.Map -> Std.Result.Unwrap -> Std.Println;

### Parallel.WithOptions

Like Map, but with explicit options in a dictionary.

Example style:

    let BuildOptions(): Dict(String, Any) =
        () -> Std.Dict.Create
        -> (_, "max_workers", 2) -> Std.Dict.Set;

    ((1.0, 2.0, 3.0), (x: Float64): Float64 = (x, 10.0) -> Std.Add, () -> BuildOptions)
      -> Std.Parallel.WithOptions
      -> Std.Result.Unwrap
      -> Std.Println;

### Parallel.ForEach

Use when the main purpose is side effects rather than collecting values.

### Parallel.Reduce

Use when you want a fold over the input with a reduction callable.

## Inline closures with task and parallel helpers

The sample set shows that concurrency APIs work naturally with closures.
Examples include:

- inline closure mapping
- closure factories that capture outer values
- reducer closures returned from helper functions

This is one of the places where the language's first-class callable model is especially valuable.

## Cancellation and timeouts

Task helpers include cancellation and timeout operations.
The sample set demonstrates:

- spawning a task
- requesting cancellation
- awaiting with a timeout

Because these operations return ordinary values, you can route them into Result handling or type inspection like any other pipeline stage.

## Practical advice for effectful code

- keep pure transformation and effectful emission separate when possible
- use blocks to name intermediate paths in file or task-heavy code
- prefer Result-aware helpers when failure should stay visible
- use WithOpen to reduce resource-management boilerplate
- keep parallel callbacks small and well typed
- use dictionaries for option bundles when APIs ask for them

## When not to overuse the effect APIs

Fleaux can express effectful logic cleanly, but not every program becomes clearer when every low-level operation is densely nested in one pipeline.
If a file-processing or task-heavy expression becomes difficult to scan:

- extract helpers
- use a block and local lets
- name intermediate paths, options, or results

The language encourages expression orientation, but it does not require unreadable density.

## Where to go next

After learning the language surface, you also need an operational workflow: the CLI, the REPL, AST and IR dumps, bytecode behavior, and how to navigate the sample corpus.
Continue with 09 CLI, REPL, and Practical Workflow.

