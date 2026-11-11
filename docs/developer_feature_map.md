# Developer Feature Map (Core)

## Purpose
This guide helps contributors identify exactly where to implement a feature in `core/`, which tests to update, and what focused VM checks to run before opening a pull request.

## Scope
- Core implementation: `core/`
- Language standard surface: `stdlib/Std.fleaux`
- Integration fixtures: `samples/*.fleaux`

## High-level architecture
- CLI entry and execution routing: `core/src/cli/vm_main.cpp`
- REPL orchestration and input: `core/src/cli/repl_driver.cpp`, `core/src/cli/line_editor.cpp`
- Frontend parse and lowering:
  - `core/src/frontend/parser.cpp`
  - `core/src/frontend/lowering.cpp`
  - `core/include/fleaux/frontend/ast.hpp`
- Type analysis:
  - `core/src/frontend/type_check.cpp`
  - `core/src/frontend/type_system/*.cpp`
- Bytecode compile, optimize, serialize, load:
  - `core/src/bytecode/compiler.cpp`
  - `core/src/bytecode/optimizer.cpp`
  - `core/src/bytecode/serialization.cpp`
  - `core/src/bytecode/module_loader.cpp`
- Execution:
  - VM runtime: `core/src/vm/runtime.cpp`
- Shared import resolution primitives: `core/include/fleaux/frontend/import_resolution.hpp`

## Build-target ownership
Reference: `core/CMakeLists.txt`

- `fleaux_frontend_diagnostics`: diagnostics formatting and spans
- `fleaux_frontend_parser`: parser
- `fleaux_frontend_lowering`: lowering and type system
- `fleaux_bytecode`: bytecode compiler, loader, optimizer, serializer
- `fleaux_vm`: VM runtime
- `fleaux`: CLI executable
- `fleaux_core_tests`: test runner

## If you are adding X, start here

### 1) New syntax or grammar rule
Touch:
- `fleaux_grammar.tx` (This is only a loose grammar definition that is a reference for parser implementation)
- `core/src/frontend/parser.cpp`
- `core/include/fleaux/frontend/ast.hpp` (if AST shape changes)
- `core/src/frontend/lowering.cpp` (if lowering output changes)

Tests:
- `core/tests/parser_tests.cpp`
- `core/tests/parser_type_syntax_tests.cpp`
- `core/tests/lowering_tests.cpp`
- A representative sample in `samples/` when user-visible

### 2) New lowering behavior or pipeline semantics
Touch:
- `core/src/frontend/lowering.cpp`
- `core/include/fleaux/frontend/ast.hpp` (if IR fields change)
- `core/src/frontend/type_system/check_pipeline.cpp` (if typing rules change)

Pipeline invariant:
- The singleton unwrap rule is per pipeline expression, not a general tuple simplification rule.
- For each lhs -> target boundary, only the outermost lhs layer may be unwrapped once when adapting arguments.
- Nested pipeline expressions each get their own one-level lhs adaptation.
- Do not flatten singleton tuple types or globally unwrap runtime tuple values.
- Keep valid parenthesized singleton lhs forms working in nested pipelines.

Tests:
- `core/tests/lowering_tests.cpp`
- `core/tests/typecheck_tests.cpp`
- `core/tests/vm_samples_tests.cpp` representative VM case

### 3) Type-system change (inference, compatibility, diagnostics)
Touch:
- `core/src/frontend/type_check.cpp`
- `core/src/frontend/type_system/checker.cpp`
- `core/src/frontend/type_system/check_expr.cpp`
- `core/src/frontend/type_system/check_decl.cpp`
- `core/src/frontend/type_system/check_pipeline.cpp`
- `core/src/frontend/type_system/function_index.cpp` (overload/index behavior)

Notes:
- Transparent aliases use a dedicated alias environment, not the strong nominal type environment.
- If alias behavior changes, verify both local declarations and imported typed signatures.
- Type arguments use angle brackets everywhere (`Tuple<...>`, `Result<T, E>`). Applied type names are validated against declared types and their generic parameter counts (`KnownTypes` in `checker_internal.hpp`).
- `Result`, `Dict`, `TaskHandle<T>`, `FileHandle`, and `RandomGenerator` are declared in `stdlib/Std.fleaux` as `type Name<...> :: __builtin__;`. Builtin types have no underlying type; use `StrongTypeIndex::is_builtin_type` instead of matching type names.
- Generic parameters may carry bounds (`<T: Bound>`). In-scope generic parameters travel as a `GenericScope` (`type.hpp`); `rewrite_generic_type` attaches each bound to the type variable it creates, `FunctionSig::generic_bounds` holds a signature's bounds, and `check_generic_bounds` enforces them at call sites and on explicitly instantiated value references. Bounds are serialized in bytecode `FunctionDef::generic_bounds`.
- Int64 and UInt64 arithmetic is exact and shared by the VM opcodes, the builtins, and constant folding through `core/include/fleaux/runtime/integer_arithmetic.hpp`. The optimizer must not fold an operation the runtime would reject.
- `kBuiltinSpecs` is indexed by `BuiltinId`; a `static_assert` in `builtin_catalog.hpp` checks the order. New callable builtins go before `Half` in the enum and in the same position in `kBuiltinSpecs`.
- Operators are resolved by the type checker (`infer_operator_flow_expr` in `check_pipeline.cpp`) using the table in `core/include/fleaux/frontend/operators.hpp`. The checker replaces each `IROperatorRef` flow target with an `IRNameRef` to the resolved Std builtin, so the bytecode compiler never sees operators; an unresolved operator reaching the compiler is an error.
- Bytecode `FunctionDef` signature metadata stores canonical type text (`lowering::format_type_text`); the module loader parses it back with `Parser::parse_type` when only bytecode is available.

Tests:
- `core/tests/typecheck_tests.cpp`
- `core/tests/lowering_tests.cpp` when lowered assumptions change
- `core/tests/vm_samples_tests.cpp` VM spot-check

### 4) New builtin or builtin signature change
Touch all relevant layers:
- `stdlib/Std.fleaux` (language signature)
- `core/include/fleaux/vm/builtin_catalog.hpp`
- `core/src/vm/runtime.cpp`
- `core/src/vm/builtin_map.hpp`
- `core/src/frontend/type_system/builtin_contracts.cpp` (when contract-validated)

Tests:
- `core/tests/runtime_builtins_tests.cpp`
- `core/tests/bytecode_tests.cpp`
- `core/tests/vm_runtime_tests.cpp`
- `core/tests/vm_samples_tests.cpp`

### 5) Import or module-loading behavior
Touch:
- `core/include/fleaux/frontend/import_resolution.hpp`
- `core/include/fleaux/frontend/source_loader.hpp`
- `core/src/bytecode/module_loader.cpp`
- `core/src/cli/vm_main.cpp` (messages/hints only)
- `core/src/vm/repl_support.hpp` (REPL symbolic-import policy)

Tests:
- `core/tests/vm_samples_tests.cpp` import contract cases
- `core/tests/bytecode_tests.cpp` staged import/link behavior
- `core/tests/vm_boundary_matrix_tests.cpp` coverage rows

Notes:
- Imported transparent aliases must be propagated alongside imported typed lets and strong nominal type declarations.
- Symbolic Std seeding must keep alias declarations in step with builtin lets and strong types.
- `Any` is a strict top type (`is_consistent` in `type.cpp`): every value fits an `Any` slot, but an `Any` value fits only `Any` or an unbounded generic parameter, and satisfies no bound. Diagnostics append `any_cast_note` to suggest a `Std.Cast`. Generic binding keeps a binding that a later argument fits and widens it to a later argument that covers it; `check_invocation` then checks every argument again against the final bindings, and explicit type arguments are never widened. A name known to exist without a type signature is an error (`make_untyped_symbol_error`), not `Any`.
- A `Std.Cast<T>` or `Std.TryCast<T>` whose argument has static type `Any` is checked at runtime. The checker builds a `common::RuntimeTypeCheck` (`core/include/fleaux/common/runtime_type_check.hpp`) with `runtime_type_check_for_cast` and stores it on the cast's `IRNameRef`; the compiler emits the `Std.Cast` builtin followed by `kCheckType` or `kTryCheckType`, whose operand indexes `Module::type_checks`. Matching lives in `core/include/fleaux/runtime/type_check.hpp`. Targets that cannot be checked at runtime (function types, `TaskHandle<T>` other than `TaskHandle<Any>`, generic parameters) are compile errors.

### 6) Bytecode instruction/opcode/compiler behavior
Touch:
- `core/include/fleaux/bytecode/opcode.hpp`
- `core/src/bytecode/compiler.cpp`
- `core/src/bytecode/optimizer.cpp`
- `core/src/bytecode/serialization.cpp`
- `core/src/vm/runtime.cpp` (execution semantics)

Tests:
- `core/tests/bytecode_tests.cpp`
- `core/tests/vm_runtime_tests.cpp`
- `core/tests/vm_boundary_matrix_tests.cpp`
- `core/tests/vm_samples_tests.cpp`

### 7) Runtime semantics and VM contract behavior
Touch:
- `core/src/vm/runtime.cpp`
- `core/src/bytecode/compiler.cpp` when call-shape assumptions change

Tests:
- `core/tests/vm_samples_tests.cpp`
- `core/tests/vm_runtime_tests.cpp`
- `core/tests/vm_boundary_matrix_tests.cpp`

### 8) CLI or REPL UX
Touch:
- `core/src/cli/vm_main.cpp`
- `core/src/cli/repl_driver.cpp`
- `core/src/cli/line_editor.cpp`
- `core/include/fleaux/cli/line_editor.hpp`

Tests:
- `core/tests/repl_line_editor_tests.cpp`
- `core/tests/vm_cli_tests.cpp`
- Targeted session coverage in `core/tests/vm_samples_tests.cpp`

### 9) Diagnostics formatting and source spans
Touch:
- `core/include/fleaux/frontend/diagnostics.hpp`
- `core/src/frontend/diagnostics.cpp`
- Call-site messages in parser/lowering/type/runtime paths as needed

Tests:
- `core/tests/diagnostics_tests.cpp`
- Any suite asserting message/hint text

## Cross-cutting rules
- Keep VM loader and VM runtime semantics aligned for imports and callable resolution.
- For import changes, verify both VM stages: `bytecode::load_linked_module` and `fleaux::vm::Runtime` execution when applicable.
- For cross-module type contracts, verify direct-import seeding and qualifier/symbol-key behavior.
- REPL imports are intentionally symbolic-only (`Std`) unless explicitly redesigned.
- REPL session state persists lets, strong types, and transparent aliases separately; do not collapse aliases into strong-type storage.

## Test matrix by change type
- Parser and grammar: `parser_tests`, `parser_type_syntax_tests`, `lowering_tests`
- Type system: `typecheck_tests` plus at least one VM sample case in `vm_samples_tests`
- Import and module loading: `vm_samples_tests` contract cases plus staged `bytecode_tests`
- VM and runtime boundaries: `bytecode_tests`, `vm_runtime_tests`, `vm_boundary_matrix_tests`
- REPL and line editor: `repl_line_editor_tests` plus targeted REPL session checks

## Contributor checklist
- Implement in canonical sources (avoid generated artifacts)
- Update `stdlib/Std.fleaux` when builtin signatures or constants change
- Add tests in the owning suite and one VM-oriented sample or runtime suite
- Run focused tests for touched areas, then broader VM regression runs

## Useful commands
```bash
ctest --test-dir core/cmake-build-debug -N
ctest --test-dir core/cmake-build-debug --output-on-failure
```

```bash
./core/cmake-build-debug/bin/fleaux --help
./core/cmake-build-debug/bin/fleaux samples/01_hello_world.fleaux
./core/cmake-build-debug/bin/fleaux --repl
```

