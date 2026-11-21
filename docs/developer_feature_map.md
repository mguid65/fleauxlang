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

Pipeline invariant (parameter lists):
- Parentheses written directly left of `->` are a parameter list: each item is one argument. `(x...)` inside it
  spreads a tuple's items; a `Tuple<T...>` may only be spread last, into a variadic parameter.
- Anything else on the left is one argument: a name, a stage result, a tuple with a trailing comma `(a, b,)`, or a
  tuple nested inside a parameter list. `((1, 2))` is a parameter list holding one tuple.
- `flow_args_from_lhs` and `apply_call_shape` in `check_pipeline.cpp` apply the rule and set `IRFlowExpr::call_shape`:
  `kDirectValue` passes the one argument (the item of a one-item parameter list, `ir::single_parameter_list_item`),
  `kTupleExpanded` passes the parameter list as the argument tuple. A variadic callee always takes the argument tuple.
  The compiler emits by the shape (`emit_flow_expr`, `emit_spread_tuple`); its only own rule is the builtin calling
  convention that one-parameter builtins using `unwrap_singleton_arg` get a one-item tuple
  (`builtin_takes_singleton_tuple`). Spreads are joined at runtime with the `ConcatTuples` opcode.
- Do not flatten singleton tuple types or unwrap runtime tuple values: a one-item tuple is a real value, including
  when passed through a callable (`resolve_callable_ref`).
- Function value calls are `(arguments, function) -> Std.Apply` flows with `IRFlowExpr::calls_function_value` set.
  Lowering produces them for closure stages (`try_apply_closure_stage`); the checker rewrites a target naming a local,
  a parameter, or a value let (`FunctionSig::is_value_binding`) into one (`infer_function_value_call`). The checker
  types the arguments against the function type and shapes `arguments` into the value Std.Apply passes
  (`infer_function_value_apply`, `check_function_value_call`), so the compiler emits an ordinary Std.Apply call. A
  generic closure is checked as a generic signature (`open_function_signature`). A local sharing a function's name in
  call position is an error.
- Closure captures are the free names of the body (`collect_free_names_from_expr` in `lowering.cpp`), including names
  used inside blocks and nested closures, minus what those scopes bind.

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
- Operators are resolved by the type checker (`infer_operator_flow_expr` in `check_pipeline.cpp`) using the table in `core/include/fleaux/frontend/operators.hpp`. The checker replaces each `IROperatorRef` flow target with an `IRNameRef` to the resolved function, so the bytecode compiler never sees operators; an unresolved operator reaching the compiler is an error. The Std builtin always wins; when it rejects the operands, the operator calls the user operator function `Name.Op` of a strong operand type (`Money.Add` for `+`). Operator functions that the Std builtin already covers (`Money.Equal`) are rejected by `validate_operator_functions` in `check_decl.cpp`.
- Bytecode `FunctionDef` signature metadata stores canonical type text (`lowering::format_type_text`); the module loader parses it back with `Parser::parse_type` when only bytecode is available. `Module::type_decls` and `Module::alias_decls` keep the module's own type and alias declarations the same way, so a bytecode-only import seeds them like a source import does (`seed_imports_from_bytecode_module`).

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
- Operator requirements: an operator on a type parameter whose bound contains a strong type (`+` on `T: Int64 | Money`) is resolved for every alternative of the bound and becomes `((a, b), 'op0) -> Std.Apply` through a hidden leading parameter (`infer_operator_requirement` in `check_pipeline.cpp`). `discover_operator_requirements` in `check_decl.cpp` finds each let's requirements, including ones inherited through calls, by checking copies of the bodies until nothing changes; `FunctionSig::operator_requirements` carries them to call sites, which prepend the operator functions (`hidden_operator_args`, `prepend_hidden_args`). `add_hidden_operator_params` adds the hidden parameters to the let (`IRLet::hidden_operator_params`) and to the captures of closures that use them. The let records them (`IRLet::operator_requirements`, with the function serving each bound alternative) and bytecode keeps them in `FunctionDef::operator_requirements` and `hidden_operator_params`, so importing modules (`FunctionIndex` skips the hidden parameters) and host calls (`prepend_host_operator_args` in `runtime.cpp`, using the alternative the host entry check chose) pass the operators. A value of such a function needs explicit type arguments (`Total<Money>`); `IRNameRef::bound_operator_args` holds the operators and the compiler binds them like closure captures.
- A generic function used as a value without type arguments (a reference or a generic closure) has an open function type (`make_open_function_type`, `Type::function_generic_params`). `check_invocation` binds open arguments after the others and instantiates them from the parameter's function type (`instantiate_open_function`); `is_consistent` does the same for declared function types. An open function never fits a non-function type, including `Any`. A generic function whose type parameter appears only in its return type is not made open, since nothing could infer that type argument.
- `Any` is a strict top type (`is_consistent` in `type.cpp`): every value fits an `Any` slot, but an `Any` value fits only `Any` or an unbounded generic parameter, and satisfies no bound. Diagnostics append `any_cast_note` to suggest a `Std.Cast`. Generic binding keeps a binding that a later argument fits and widens it to a later argument that covers it; `check_invocation` then checks every argument again against the final bindings, and explicit type arguments are never widened. A name known to exist without a type signature is an error (`make_untyped_symbol_error`), not `Any`.
- Host calls are the one untyped entry: `Runtime::invoke_symbol` checks the host's argument against `ExportedSymbol::host_entry_check`, built per let by `host_entry_check_for_let` (`checker.cpp`) and checked by `check_host_argument` (`runtime/type_check.hpp`). Function parameters are checked by callable tag, and every use of a bounded generic parameter must match one alternative of its bound common to all uses. Runtime builtins and the VM therefore read argument kinds without validating them (`int64_value`, `uint64_value`, `uint64_index` in `value.hpp`); only real runtime failures (overflow, ranges, IO, `Unwrap` of `Err`, stale handles) stay checked.
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

