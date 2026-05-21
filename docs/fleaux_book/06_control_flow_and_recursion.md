# 06 Control Flow and Recursion

Fleaux currently expresses most control flow through ordinary values and callables rather than special statement syntax.
This chapter explains the main patterns.

## Expression-oriented control flow

Instead of relying on statement forms such as if, switch, or while, Fleaux commonly uses:

- Std.Select for eager value choice
- Std.Branch for lazy behavior choice
- Std.Match for ordered pattern dispatch
- Std.Loop for state iteration
- Std.LoopN for bounded state iteration
- recursion for naturally self-similar problems

These are not workarounds.
They are central idioms.

## Std.Select

Std.Select chooses between two values based on a condition.
The shape is:

    (condition, true_value, false_value) -> Std.Select;

Examples:

    (True, "yes", "no") -> Std.Select;
    ((10, 5) -> Std.GreaterThan, "greater", "lesser") -> Std.Select;

### Important property of Select

Both value branches are evaluated before Std.Select is called.
That makes Select suitable for choosing between already-computed values, not for avoiding work or side effects.

Example:

    let Abs(x: Float64): Float64 =
        ((x, 0.0) -> Std.GreaterOrEqual, x, (0.0, x) -> Std.Subtract) -> Std.Select;

This works well because both candidate values are cheap and side-effect free.

## Std.Branch

Std.Branch chooses between two callables and applies only the selected one.
The shape is:

    (condition, value, true_func, false_func) -> Std.Branch;

Example:

    let Double(x: Float64): Float64 = (x, 2.0) -> Std.Multiply;
    let Halve(x: Float64): Float64 = (x, 2.0) -> Std.Divide;

    ((10.0, 0.0) -> Std.GreaterThan, 10.0, Double, Halve) -> Std.Branch;

### When to use Branch instead of Select

Use Select when:

- you are choosing between values
- eager evaluation is fine

Use Branch when:

- you are choosing between behaviors
- you want only one callable to run
- you want the selected behavior to receive a value argument

This distinction is one of the most important semantic differences in the language's control-flow style.

## Branch with inline closures

You are not limited to named functions.
Closures work naturally with Branch.

Example shape:

    ((7.0, 0.0) -> Std.GreaterThan,
     (7.0) -> Std.ToString,
     (s: String): String = ("positive: {}", s) -> Std.String.Format,
     (s: String): String = ("non-positive: {}", s) -> Std.String.Format
    ) -> Std.Branch;

This is a good pattern when the two behaviors are local and short-lived.

## Std.Match

Std.Match is the main multi-way dispatch helper.
It accepts a subject value followed by ordered pattern-and-handler cases.

Literal patterns example:

    (0,
      (0, (): Any = "zero"),
      (1, (): Any = "one"),
      (_, (): Any = "many")
    ) -> Std.Match;

Here:

- the first tuple element is the subject
- each case is a pair of pattern and handler
- _ acts as the wildcard case pattern in this context

## Match handlers can accept the subject

A handler may declare a parameter when it wants the matched value.

Example:

    (7.0,
      (0.0, (): Any = "zero"),
      (_, (n: Float64): Any = n)
    ) -> Std.Match;

This is useful for default handling that still needs the actual input.

## Predicate patterns

Patterns can also be callables that return Bool.

Example:

    let IsEven(n: Float64): Bool = ((n, 2.0) -> Std.Mod, 0.0) -> Std.Equal;

    (8.0,
      (IsEven, (): Any = "even"),
      (_, (): Any = "odd")
    ) -> Std.Match;

This lets Match serve the role of if, else if, and richer pattern-driven branching.

## Match as if and else if

A common Fleaux pattern is to express an if ladder as a Match with predicate cases.

Example shape:

    let DescribeTemperature(x: Float64): String =
        (x,
         (BelowZero, (): String = "freezing"),
         (BelowTen, (): String = "cold"),
         (BelowTwenty, (): String = "mild"),
         (_, (): String = "warm")
        ) -> Std.Match;

This gives you ordered, readable, expression-level control flow.

## Std.Loop

Std.Loop iterates a state value while a continue callable returns True.
The shape is:

    (state, continue_func, step_func) -> Std.Loop;

Simple countdown example:

    let CountContinue(n: Float64): Bool = (n, 0.0) -> Std.GreaterThan;
    let CountStep(n: Float64): Float64 = (n, 1.0) -> Std.Subtract;

    (10.0, CountContinue, CountStep) -> Std.Loop;

### Loop state is often a tuple

For accumulators, the state is usually a tuple.

Example pattern:

    let SumContinue(state: Tuple(Float64, Float64)): Bool =
        ((state, 0) -> Std.ElementAt, 0.0) -> Std.GreaterThan;

    let SumStep(state: Tuple(Float64, Float64)): Tuple(Float64, Float64) =
        (((state, 0) -> Std.ElementAt, 1.0) -> Std.Subtract,
         ((state, 1) -> Std.ElementAt, (state, 0) -> Std.ElementAt) -> Std.Add);

    (10.0, 0.0) -> (_, SumContinue, SumStep) -> Std.Loop;

This is a very typical Fleaux way to model iterative state.

## Std.LoopN

Std.LoopN is like Std.Loop but adds a maximum iteration bound.

Shape:

    (state, continue_func, step_func, max_iters) -> Std.LoopN;

Example:

    (10.0, CountContinue, CountStep, 100) -> Std.LoopN;

Use LoopN when you want the same iteration model but with an explicit safety cap.

## Inline closures with Loop and LoopN

Loops work well with inline closures when the logic is local.

Example shape:

    ((5.0, 0.0),
     (state: Tuple(Float64, Float64)): Bool = ((state, 0) -> Std.ElementAt, 0.0) -> Std.GreaterThan,
     (state: Tuple(Float64, Float64)): Tuple(Float64, Float64) =
         (((state, 0) -> Std.ElementAt, 1.0) -> Std.Subtract,
          ((state, 1) -> Std.ElementAt, (state, 0) -> Std.ElementAt) -> Std.Add)
    ) -> Std.Loop;

This is concise, but as always, blocks and named helpers are available when readability starts to suffer.

## Recursion

Recursion works through ordinary function declarations.
It is often paired with Match.

Factorial example:

    let Factorial(n: Int64): Int64 =
        (n,
         (0, (): Int64 = 1),
         (_, (): Int64 = (n, ((n, 1) -> Std.Subtract) -> Factorial) -> Std.Multiply)
        ) -> Std.Match;

This mirrors the structure of recursive definitions from functional languages but keeps the dataflow style intact.

## Recursion over collection-like data

Another pattern is recursion that builds tuples.

Example shape:

    let Repeat<T>(value: T, count: Int64): Tuple(T...) =
        (count,
         (0, (): Tuple(T...) = ()),
         (_, (): Tuple(T...) = (((value, ((count, 1) -> Std.Subtract)) -> Repeat), value) -> Std.Tuple.Prepend)
        ) -> Std.Match;

This is expressive and idiomatic, though you should still consider loop-style state when iteration is the clearer mental model.

## Choosing the right control-flow tool

A good rule of thumb is:

- Select for eager value choice
- Branch for lazy callable choice
- Match for multi-way or pattern-like choice
- Loop for state iteration
- LoopN for bounded state iteration
- recursion when the problem is naturally recursive or structurally inductive

## Control flow is often type-driven

Because the language is expression-oriented, control-flow helpers usually return a value.
That means the result type of a branch matters immediately.

Examples:

- both Select branches should produce compatible values
- Branch callables should return compatible results
- Match handlers should collectively produce a coherent result type
- loop state must maintain the same type shape across iterations

Thinking about control flow and type shape together is part of fluent Fleaux style.

## Practical style advice

- keep Select for simple value choice
- prefer Branch when one side would be expensive or effectful
- use Match for more than two cases or when you want a wildcard fallback
- model loop state as a tuple when you need several accumulators
- break large loop or Match expressions into named helpers when the expression becomes difficult to scan

## Where to go next

Now that control flow is clear, the next chapter turns to the concrete data structures and everyday standard-library helpers you use to build real programs.
Continue with 07 Collections, Strings, and Data Modeling.

