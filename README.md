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
