# Oops! All Postfix Operators

```
(2, 1) -> +; // 3
(2, 1) -> -; // 1
(2, 1) -> *; // 2
(2, 1) -> /; // 2
```

# Oops! Significant parentheses that look redundant...

```
(2, 1) -> Std.Println; // 2 1
((2, 1)) -> Std.Println; // (2, 1)
```
The first one is piping 2 arguments 2 and 1 into Std.Println. 
The second on is piping a single argument into Std.Println, that being the tuple `(2, 1)`.

# Parser Source Of Truth

`fleaux_hand_parser.py` (via `fleaux_parser.py`) is the runtime parser source of truth.

`fleaux_grammar.tx` is the reference syntax specification used to document and evolve the language grammar.

For now, `FlowExpression` intentionally requires a parenthesized left-hand side. This keeps the grammar simpler, even if it means extra parentheses in chained flows.

Example:

```fleaux
let AddPrint(x: Number, y: Number): Number = ((x, y) -> Std.Add) -> Std.Println;
```

Generate an EBNF artifact from the textX grammar:

```bash
python3 fleaux_grammar_ebnf.py --grammar fleaux_grammar.tx --out fleaux_grammar.ebnf
```

# Vertical Slice Workflow

Transpile a Fleaux file to Python:

```bash
python3 fleaux_transpiler.py test.fleaux
```

Transpile and immediately execute (preferred):

```bash
./fleaux test.fleaux
```

You can still use:

```bash
python3 run_fleaux.py test.fleaux
```

Emit a graph and still execute:

```bash
./fleaux test.fleaux --emit-graph --graph-format svg --graph-out /tmp/test.svg
```

Emit only the graph (skip transpile/execute):

```bash
./fleaux test.fleaux --emit-graph --graph-only --graph-format dot --graph-out /tmp/test.dot
```

Optional one-time setup to call it like `python3` from anywhere:

```bash
chmod +x fleaux
mkdir -p "$HOME/.local/bin"
ln -sf "$PWD/fleaux" "$HOME/.local/bin/fleaux"
# Ensure ~/.local/bin is on PATH (add to ~/.bashrc if needed)
```

Then run:

```bash
fleaux test.fleaux
```

Run smoke tests:

```bash
python3 -m unittest discover -s tests -p "test_*.py"
```


# Todo

- [ ] Add more samples to samples/*.fleaux
- [ ] Maybe add a shorter file extension like .flx?
- [ ] Add more examples to documentation
- [ ] Add more tests for edge cases and error handling
- [ ] Explore more complex pipeline patterns and transformations
- [ ] Consider adding a REPL for interactive experimentation
- [ ] Finalize core language features in python before expanding to C++ transpiler
- [ ] From C++ transpiler, transition to a true compiler and runtime for better performance and standalone executables
- [ ] Explore embedding within C++
- [ ] Better diagnostics that point back at the fleaux source code with line numbers and suggestions
- [ ] Formal analysis of the language grammar to identify ambiguities and ensure it is LL(1) for the hand-rolled parser
- [ ] Possibly create a frontend visual programming interface that generates Fleaux code
- [ ] Syntax highlighting support for editors (VSCode, Jetbrains Tools, etc.)
- [ ] Performance optimizations in the transpiler and runtime, especially for larger programs
- [ ] More exploration into the type system, including generics, type inference, and better error messages for type errors
- [ ] Consider making a setup script for easier installation and usage of the `fleaux` command
