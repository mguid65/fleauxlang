# The Fleaux Book

## Purpose

This book is a comprehensive usage guide for the current Fleaux language and CLI.
It is written for people who want more than a quick start: the goal is to explain how Fleaux programs are structured, how the current core implementation behaves, and how to approach the standard library idiomatically.

This book focuses on the language and the native CLI.
It does not cover the visual editor.

The authoritative implementation sources remain:

- stdlib/Std.fleaux for builtin signatures, constants, and doc comments
- samples/ for runnable examples
- fleaux_grammar.tx for parser-aligned syntax reference
- core/tests/ for executable behavior checks and edge cases

## How to read this book

There are several good reading paths.

If you are new to Fleaux:

1. 01 Getting Started
2. 02 The Dataflow Model
3. 03 Modules, Names, and Imports
4. 04 Types, Type Declarations, and Aliases
5. 05 Functions, Closures, and Blocks
6. 06 Control Flow and Recursion
7. 07 Collections, Strings, and Data Modeling
8. 08 Effects, Files, the OS, and Concurrency
9. 09 CLI, REPL, and Practical Workflow
10. 10 Patterns, Caveats, and a Feature Map

If you already know the syntax and want practical reference material:

- 04 Types, Type Declarations, and Aliases
- 07 Collections, Strings, and Data Modeling
- 08 Effects, Files, the OS, and Concurrency
- 10 Patterns, Caveats, and a Feature Map

If you are exploring parser and semantics boundaries:

- 02 The Dataflow Model
- 05 Functions, Closures, and Blocks
- 06 Control Flow and Recursion
- 10 Patterns, Caveats, and a Feature Map

## Chapter list

- [01 Getting Started](./01_getting_started.md)
- [02 The Dataflow Model](./02_the_dataflow_model.md)
- [03 Modules, Names, and Imports](./03_modules_names_and_imports.md)
- [04 Types, Type Declarations, and Aliases](./04_types_type_declarations_and_aliases.md)
- [05 Functions, Closures, and Blocks](./05_functions_closures_and_blocks.md)
- [06 Control Flow and Recursion](./06_control_flow_and_recursion.md)
- [07 Collections, Strings, and Data Modeling](./07_collections_strings_and_data_modeling.md)
- [08 Effects, Files, the OS, and Concurrency](./08_effects_files_os_and_concurrency.md)
- [09 CLI, REPL, and Practical Workflow](./09_cli_repl_and_practical_workflow.md)
- [10 Patterns, Caveats, and a Feature Map](./10_patterns_caveats_and_feature_map.md)

## Conventions used in this book

### The standard library import

Most examples begin with:

    import Std;

That import is required before Std symbols are visible.
Even after importing Std, builtin names remain qualified, so you still write Std.Add rather than Add.

### Semicolons

Every statement ends with ;
This includes top-level declarations and expression statements.
Inside blocks, the final expression also ends with ;

### Pipelines first

Fleaux is easiest to understand when you read expressions as a flow of data through stages.
If you keep that model in mind, the rest of the language becomes much easier to learn.

### Current implementation language

This book describes the current core implementation as of May 2026.
Where behavior is intentionally restricted, the book says so plainly rather than speculating about future features.

## What Fleaux feels like

Fleaux is an expression-oriented dataflow language with a strong pipeline idiom.
A value is created, transformed, inspected, branched on, collected, or emitted by flowing it through stages.

Small examples:

    import Std;

    ("Hello, World!") -> Std.Println;
    (3, 4) -> Std.Add -> Std.Println;
    (20, 5) -> Std.Divide -> (_, 10) -> Std.Add -> Std.Println;

That last line is a good summary of the language:

- values are grouped with tuples
- computation flows left to right
- the previous stage can be reused with the placeholder _
- named builtins are ordinary pipeline targets

## The role of the sample set

The sample set in samples/ is not just a demo folder.
It is also the main integration corpus for the language.
If you want examples that reflect the real implementation, start there.

Good entry points:

- samples/01_hello_world.fleaux
- samples/03_pipeline_chaining.fleaux
- samples/04_function_definitions.fleaux
- samples/21_import.fleaux
- samples/29_inline_closures.fleaux
- samples/30_pattern_matching.fleaux
- samples/35_concurrency_tasks.fleaux
- samples/51_strong_type_casts.fleaux
- samples/52_variables_and_blocks.fleaux

## Relationship to the quick start

The file docs/fleaux_language_usage_guide.md is still useful as a compact entry point.
This book is the longer-form companion intended to play the role of a real language manual.

## Suggested next step

If you are starting from scratch, continue with [01 Getting Started](./01_getting_started.md).
If you mainly want to understand the mental model, jump to [02 The Dataflow Model](./02_the_dataflow_model.md).


