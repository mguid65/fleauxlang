# Developer Feature Map (Core)

## Purpose
This guide helps contributors identify exactly where to implement a feature in `core/`, which tests to update, and what focused VM checks to run before opening a pull request.

## Scope
- Core implementation: `core/`
- Language standard surface: `Std.fleaux`
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

Tests:
- `core/tests/typecheck_tests.cpp`
- `core/tests/lowering_tests.cpp` when lowered assumptions change
- `core/tests/vm_samples_tests.cpp` VM spot-check

### 4) New builtin or builtin signature change
Touch all relevant layers:
- `Std.fleaux` (language signature)
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
- REPL imports are intentionally symbolic-only (`Std`, `StdBuiltins`) unless explicitly redesigned.

## Test matrix by change type
- Parser and grammar: `parser_tests`, `parser_type_syntax_tests`, `lowering_tests`
- Type system: `typecheck_tests` plus at least one VM sample case in `vm_samples_tests`
- Import and module loading: `vm_samples_tests` contract cases plus staged `bytecode_tests`
- VM and runtime boundaries: `bytecode_tests`, `vm_runtime_tests`, `vm_boundary_matrix_tests`
- REPL and line editor: `repl_line_editor_tests` plus targeted REPL session checks

## Contributor checklist
- Implement in canonical sources (avoid generated artifacts)
- Update `Std.fleaux` when builtin signatures or constants change
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

