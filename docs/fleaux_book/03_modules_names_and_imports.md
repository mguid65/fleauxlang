# 03 Modules, Names, and Imports

This chapter explains how Fleaux resolves names, what an import actually does, and why qualification matters so much for Std.

## A Fleaux file is a module

Each source file acts as a module.
A file named math_helpers.fleaux is imported as:

    import math_helpers;

A file named 20_export.fleaux is imported as:

    import 20_export;

Digit-leading module names are intentionally supported by the parser and the sample set.

## Importing a module

The basic form is:

    import ModuleName;

Examples:

    import Std;
    import 20_export;
    import custom_module;

An import is a top-level statement, so it ends with ;

## What an import introduces

An import introduces the symbols declared by the imported file.
It does not import a namespace object that you can then treat loosely.
This matters most for Std.

### Std is special, but not magical in every way

The module Std is special in a few runtime and tooling paths, but from the point of view of authoring ordinary code, two rules matter most:

- you still need import Std; before using Std symbols
- imported Std symbols remain qualified as Std.Name

This succeeds:

    import Std;
    (1, 2) -> Std.Add -> Std.Println;

This does not:

    import Std;
    (1, 2) -> Add -> Std.Println;

The current test suite explicitly checks that qualified Std symbols are not callable unqualified.

## Why qualified names matter

Qualification keeps the surface clear and predictable.

Examples:

- Std.Add
- Std.Math.Sqrt
- Std.String.Join
- Std.Dict.Create
- Std.Task.Await

User-defined qualified names are also supported:

    let MyMath.Square(x: Float64): Float64 = (x, x) -> Std.Multiply;

And then called as:

    (6.0) -> MyMath.Square;

## Imported user symbols are different from Std builtins

When you import a normal module, its declared names are introduced as imported symbols.
That is why this works:

    import Std;
    import 20_export;

    (4.0) -> Add4 -> Std.Println;

The file 20_export.fleaux defines Add4, and the import makes that declared name available.
The language does not require you to write 20_export.Add4.

This is a key distinction:

- imported module declarations become available by their declared symbol names
- Std builtin use remains intentionally qualified

## Module names and file names

A module import corresponds to a source file stem.
In practice:

- import custom_module; expects a file custom_module.fleaux
- import 20_export; expects a file 20_export.fleaux

The current REPL tests also confirm that normal imports are resolved relative to the working directory.

## Qualified identifiers in declarations

Functions can be declared with qualified names:

    let MyMath.Square(x: Float64): Float64 = (x, x) -> Std.Multiply;

This is useful when you want a namespace-like naming style in user code.

Value declarations use an optionally qualified identifier at the grammar level too, but in practice the most common and well-exercised patterns are:

- unqualified named values
- qualified function declarations
- qualified builtin names in Std

## Name categories you will encounter

A Fleaux program usually works with several kinds of names:

- imported module names in import statements
- type names such as Int64, Dict, Result, or user-defined types
- alias names such as Name or MaybeValue
- function names such as Square, Add4, or MyMath.Square
- builtin names such as Std.Add or Std.String.Format
- local value names introduced by let inside a block
- closure parameter names

Keeping these roles separate makes the language easier to read.

## A practical import example

Suppose custom_module.fleaux contains:

    import Std;
    let Add4(x: Int64): Int64 = (x, 4) -> Std.Add;

A main file can use it as:

    import Std;
    import custom_module;

    (3) -> Add4 -> Std.Println;

The REPL tests confirm that this style works when the current working directory contains custom_module.fleaux.

## Importing Std in the REPL

The REPL behaves like source files in this respect: Std is not automatically in scope.
A session must import it before calling Std helpers.

Typical session shape:

    import Std;
    (1, 2) -> Std.Add -> Std.Println;

Without the import, the current tests show an unresolved symbol error for Std.Add or Std.Println.

## Help and discoverability through Std.Help

The standard library includes a builtin help function:

    ("Std.Add") -> Std.Help -> Std.Println;

The current REPL tests confirm that this prints canonical builtin documentation sourced from the standard library surface.
This is useful when you know a symbol name but want to confirm its parameters or meaning from inside the language.

## Strong types, aliases, and imports

Imported declarations can also interact with types and aliases.
The core implementation propagates imported typed signatures and imported transparent aliases during analysis.
For language users, the practical takeaway is simple:

- imported function names can participate in type-checked code like local ones
- imported aliases and named types matter for signature compatibility
- qualification still matters for Std

## Common mistakes

### Forgetting import Std;

Symptom:

- unresolved symbol errors when trying to call Std.Add or Std.Println

Fix:

    import Std;

### Assuming Std.Add becomes Add after importing Std

Symptom:

- unresolved symbol for Add

Fix:

    (1, 2) -> Std.Add;

### Treating import as if it imported a namespace object

In Fleaux, imports introduce the imported declarations.
They do not behave like a separate namespace handle that changes the qualified naming rules for Std.

## Good naming style

A few habits make Fleaux code easier to follow:

- use short local names inside a block
- use descriptive top-level function names
- reserve qualification for true grouping, such as MyMath.Square or Std.String.Join
- avoid relying on visual similarity between imported user names and Std builtin names

## Where to go next

Now that names and imports are clear, the next step is the type system surface that those names participate in.
Continue with 04 Types, Type Declarations, and Aliases.

