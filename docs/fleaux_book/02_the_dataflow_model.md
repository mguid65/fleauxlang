# 02 The Dataflow Model

Fleaux becomes much easier once you stop reading it as a traditional statement-heavy language and start reading it as dataflow.
This chapter explains that model in detail.

## The basic idea

Most Fleaux expressions are written as a value followed by one or more stages:

    (3, 4) -> Std.Add -> Std.Println;

Read this as:

- create the tuple (3, 4)
- send it into Std.Add
- send the result into Std.Println

This left-to-right orientation is the language's central habit.

## Pipeline stages are the default call syntax

If you already know ordinary call syntax from other languages, think of a pipeline stage as Fleaux's preferred way to invoke a callable.

These examples show the common patterns:

Single argument:

    (7.0) -> Std.Math.Sqrt;

Multiple arguments:

    (3, 4) -> Std.Add;

No arguments:

    () -> Std.GetArgs;

Longer chain:

    (3, 4) -> Std.Add -> (_, 2) -> Std.Multiply;

## Why tuples matter so much

Tuples are the primary way to package multiple values.
They appear everywhere:

- calling a multi-parameter function
- carrying loop state
- collecting variadic arguments
- returning grouped results
- building dictionary entries or nested structures

Examples:

    (1, 2, 3)
    ("name", 42)
    ()
    ((1, 2), (3, 4))

Because the language uses pipelines heavily, tuples effectively become the shape language for function input.

## Stages can be named functions, builtins, operators, or closures

A pipeline stage is not limited to one kind of target.
Common stage forms include:

- user-defined functions
- Std builtins such as Std.Add or Std.String.Join
- operator tokens such as + or ==
- inline closures

Examples:

    (8.0, 5.0) -> +;
    (10, 7) -> >;
    (10.0) -> (x: Float64): Float64 = (x, 1.0) -> Std.Add;

## The flowing value and the placeholder _

The placeholder _ stands for the value produced by the previous stage.
It is used inside tuple-template stages after ->.

Example:

    (3, 4) -> Std.Add -> (_, 2) -> Std.Multiply;

Read it as:

- first compute 3 + 4
- then construct a new input tuple from the previous result and 2
- then multiply

Another example:

    (20, 5) -> Std.Divide -> (_, 10) -> Std.Add -> Std.Println;

This computes 20 / 5, then adds 10, then prints.

Important rule:

- _ is only valid inside tuple-template stages after ->

It is not a general wildcard variable you can use anywhere.

## Fanout by repeating work explicitly

Sometimes you want to compute multiple outputs from the same input.
You do that by building a tuple of expressions that each reference the same value.

Example:

    let TypeAndValueText<T>(value: T): Tuple(String, String) =
        ((value) -> Std.Type, (value) -> Std.ToString);

This function takes one input and fans it out into two derived values.

The language does not try to hide that structure.
It is often better to make the tuple shape visible.

## A pipeline is still an expression

A useful mental model is:

- tuples build values
- pipelines transform values
- blocks structure values and scope
- declarations bind names to values or callables

That means a pipeline can be nested inside another expression.

Examples:

    ((a, b) -> Std.Add, c) -> Std.Multiply;

    ((x, 0.0) -> Std.GreaterThan, "positive", "negative") -> Std.Select;

Fleaux programs are full of expressions nested into tuple positions.
At first this can look dense, but it becomes predictable once you trust the dataflow model.

## Reading complex expressions

A useful way to read a dense Fleaux line is:

1. Find the outermost tuple or starting value
2. Follow each -> in order
3. When you hit a tuple-template stage, substitute _ with the previous result
4. When you hit a nested expression, evaluate it the same way

Example:

    ((x, 0.0) -> Std.GreaterOrEqual, x, (0.0, x) -> Std.Subtract) -> Std.Select;

Read it as:

- compute whether x >= 0
- provide x as the true branch value
- provide 0 - x as the false branch value
- feed the three-tuple into Std.Select

## Operator stages are ordinary call targets

Operator tokens can act like named callables.
This lets short arithmetic or logical transforms read very directly.

Examples:

    (8.0, 5.0) -> + -> Std.Println;
    (3, 4) -> != -> Std.Println;
    (True, True) -> && -> Std.Println;

This is mostly a readability feature.
If Std.Add or Std.Equal reads more clearly in context, you can use those too.

## Functions are values too

A function is not only something you call directly.
It can be passed around as data.

Examples:

    let Double(x: Float64): Float64 = (x, 2.0) -> Std.Multiply;
    let ApplyTwice(x: Float64, f: (Float64) => Float64): Float64 =
        (x, f) -> Std.Apply -> (_, f) -> Std.Apply;

    (3.0, Double) -> ApplyTwice -> Std.Println;

This is one reason the pipeline model works so well: callables and ordinary values fit into the same left-to-right flow.

## Closures fit naturally into the same model

Inline closures can be passed as stage inputs or used directly as stage targets.

Examples:

    (10.0, (x: Float64): Float64 = (x, 1.0) -> Std.Add) -> Std.Apply;

    (10.0) -> (x: Float64): Float64 = (x, 10.0) -> Std.Add;

Because closures are ordinary expression values, they slot into pipelines without special syntax beyond their own literal form.

## Dataflow over statements

Many languages force control flow and state changes into statements.
Fleaux often prefers to model them as expression-level data transformations.

That is why you will often see:

- Std.Select rather than a built-in if statement
- Std.Branch rather than statement-based branching
- Std.Match rather than a statement switch
- Std.Loop and Std.LoopN rather than statement-based loops
- blocks that evaluate to a value rather than statement-only scopes

This is not just syntax.
It is the organizing design choice of the language.

## Why some programs look nested

A program that branches or loops in Fleaux often builds tuples of:

- the current state
- one or more callables
- one or more values that should flow into those callables

That is why code can start to look tree-shaped even though the outer reading order is still linear.
The nested structure is usually expressing one of two things:

- grouped input
- delayed behavior

If you can spot which one it is, the expression is usually easier to understand.

## The block expression is a bridge between flow and naming

Pipelines are great when a computation reads naturally as a straight flow.
Blocks help when you need names for intermediate results.

Example:

    let Compute(): Int64 = {
      let base: Int64 = 40;
      let offset: Int64 = 2;
      (base, offset) -> Std.Add;
    };

This is still expression-oriented.
The block is simply a structured way to produce one final value.

## The most important habits to build

When reading or writing Fleaux, these habits help most:

- think in values, not statements
- use tuples deliberately to model grouped input
- read arrows left to right
- use _ when the previous result needs to be threaded into a new tuple
- treat functions and closures as ordinary values
- use blocks when a pipeline stops being readable as a straight line

## Where to go next

The dataflow model explains how computation moves.
The next step is understanding how names, modules, and imports determine what can be referenced in that computation.
Continue with 03 Modules, Names, and Imports.

