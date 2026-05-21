# 10 Patterns, Caveats, and a Feature Map

This final chapter gathers practical patterns, language boundaries, caveats worth knowing early, and a map from the book
to the sample corpus and source-of-truth files.

## A realistic picture of the language today

Fleaux is already capable of expressing:

- straight-line numeric and string transformations
- higher-order programming with functions and closures
- multi-way control flow with Match
- state iteration with Loop and LoopN
- immutable block-local naming
- dictionary-driven dispatch
- file and OS interaction
- task and parallel helpers
- strong nominal typing plus transparent aliases

At the same time, the current language intentionally keeps some surfaces narrow.
That is often a strength rather than a weakness because it keeps the mental model coherent.

## Practical patterns worth reusing

### 1. Straight pipeline for simple transforms

Use this when each step naturally follows the previous one.

    (3, 4) -> Std.Add -> (_, 2) -> Std.Multiply -> Std.Println;

### 2. Block for named intermediates

Use this when a one-line expression stops being readable.

    let Compute(): Int64 = {
      let base: Int64 = 40;
      let offset: Int64 = 2;
      (base, offset) -> Std.Add;
    };

### 3. Branch for lazy choice between behaviors

    ((x, 0.0) -> Std.GreaterThan, x, Positive, NonPositive) -> Std.Branch;

### 4. Match for ordered multi-way logic

    (x,
      (BelowZero, (): String = "freezing"),
      (BelowTen, (): String = "cold"),
      (_, (): String = "warm")
    ) -> Std.Match;

### 5. Dictionary-backed dispatch

    let BuildOps(): Dict(String, (Float64) => Float64) =
        () -> Std.Dict.Create
        -> (_, "double", Double) -> Std.Dict.Set
        -> (_, "square", Square) -> Std.Dict.Set;

### 6. Closure factory for parameterized behavior

    let MakeAdder(n: Float64): (Float64) => Float64 =
        (x: Float64): Float64 = (x, n) -> Std.Add;

### 7. Tuple state for iteration

    ((0, 0, 0, 0), continue_func, step_func) -> Std.Loop;

## Language boundaries that are deliberate

### Variables are immutable

let introduces immutable bindings.
There is no assignment syntax in the current language model.

### Local let bindings require explicit types

Inside blocks, local lets use the same explicit type style as top-level named values.

### Nested named local functions are not supported

Use closures instead when you need local callable behavior.

### Generic aliases are not supported

Use generic functions and ordinary aliases for non-generic shorthand.

### Std names stay qualified

Even after import Std;, builtin names are still written as Std.Add, Std.String.Join, and so on.

### Semicolons are mandatory

Semicolons are required to terminate statements and separate them from following expressions.

## A parser-aware readability caveat

Ungrouped inline closure pipeline targets are supported and useful.
However, closure-heavy expressions become easier to understand and safer to author when their intended grouping is made
visually obvious.

In practice, that means:

- keep short inline closures short
- extract large closure bodies into named helpers or enclosing blocks
- use grouping and structure deliberately when the body itself chains several stages

This is partly a readability point and partly a way to stay on the clearest path through complex parser-sensitive forms.

## When to prefer a block over a clever pipeline

Prefer a block when any of these are true:

- you need three or more intermediate names
- the tuple shape of the pipeline is becoming hard to track
- a nested closure is hiding too much logic
- the same subexpression appears several times
- the human reader would benefit from named steps

The language is dataflow-first, not unreadability-first.

## When to prefer named functions over inline closures

Prefer named functions when:

- the behavior has a stable meaning in the program
- you want reuse
- the signature deserves a name
- the same logic appears in several places
- the closure would span several lines and obscure the surrounding expression

Prefer inline closures when:

- the behavior is truly local
- the callback is small and obvious
- a one-off higher-order call reads more clearly inline than through a distant helper name

## A sample map by topic

### Basics

- samples/01_hello_world.fleaux
- samples/02_arithmetic.fleaux
- samples/03_pipeline_chaining.fleaux
- samples/04_function_definitions.fleaux

### Control flow

- samples/05_select.fleaux
- samples/06_branch.fleaux
- samples/08_loop.fleaux
- samples/09_loop_n.fleaux
- samples/30_pattern_matching.fleaux
- samples/39_if_else_match.fleaux
- samples/40_recursion.fleaux

### Strings, tuples, and core library usage

- samples/10_strings.fleaux
- samples/11_tuples.fleaux
- samples/12_math.fleaux
- samples/13_comparison_and_logic.fleaux
- samples/17_printf_and_tostring.fleaux
- samples/26_format_specifiers.fleaux
- samples/28_variadics.fleaux

### Modules and composition

- samples/19_composition.fleaux
- samples/20_export.fleaux
- samples/21_import.fleaux
- samples/41_function_factory.fleaux
- samples/42_dict_dispatch.fleaux

### Files and system interaction

- samples/14_os.fleaux
- samples/15_path.fleaux
- samples/16_file_and_dir.fleaux
- samples/22_file_streaming.fleaux
- samples/25_fleaux_parser.fleaux

### Results, tasks, and parallel helpers

- samples/27_error_handling_branching.fleaux
- samples/31_result_ok_err.fleaux
- samples/33_exp_parallel.fleaux
- samples/35_concurrency_tasks.fleaux
- samples/36_parallel_options_and_empty_inputs.fleaux
- samples/37_parallel_error_paths.fleaux
- samples/38_parallel_inline_closures.fleaux
- samples/45_result_recovery.fleaux

### Advanced expression shaping and blocks

- samples/29_inline_closures.fleaux
- samples/48_repeat_value_tuple.fleaux
- samples/49_same_input_fanout.fleaux
- samples/50_multi_accumulator_loop_state.fleaux
- samples/51_strong_type_casts.fleaux
- samples/52_variables_and_blocks.fleaux

## Source-of-truth map by topic

For exact builtin signatures and doc comments:

- stdlib/Std.fleaux

For parser-aligned syntax structure:

- fleaux_grammar.tx
- core/tests/parser_tests.cpp
- core/tests/parser_type_syntax_tests.cpp

For type-system and compatibility behavior:

- core/tests/typecheck_tests.cpp

For runtime-level sample behavior:

- core/tests/vm_samples_tests.cpp
- samples/

For CLI and REPL behavior:

- core/tests/vm_cli_tests.cpp

## A practical authoring checklist

When writing Fleaux code, ask:

- did I import Std explicitly where needed?
- are all statements terminated with ;?
- are my value and local declarations explicitly typed?
- are Std builtin names still qualified?
- would a block make this expression easier to read?
- should this closure become a named helper?
- am I choosing Select, Branch, Match, Loop, or recursion for the right reason?
- is a dictionary or tuple the clearer data representation here?

## What this book does and does not promise

This book aims to be comprehensive for usage of the current core language.
It is not a promise that every future idea will look the same.
Where the implementation has intentionally narrow edges, those limits are described plainly rather than glossed over.

## Final advice

If you keep these three ideas in mind, most Fleaux code will start to feel coherent:

1. data flows left to right through callables
2. tuples are the language's main shape for grouped values
3. blocks, closures, and helper functions exist to make that flow readable

Once those habits click, the rest of the language tends to follow naturally.

## Next places to keep open while working

For reference, keep these nearby:

- docs/fleaux_language_usage_guide.md for a compact summary
- stdlib/Std.fleaux for exact builtin surface details
- samples/ for runnable examples

