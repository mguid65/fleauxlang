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
  - Source spans (`core/include/fleaux/frontend/diagnostics.hpp`): every span holds a `SourceFile`, which the parser
    makes once per parse and every span shares, so copying a span never copies the file. Keep per-span data to
    positions; a string per span multiplies by every node of every file, Std included.
- Source formatter (`fleaux --format`, the editor's Format button through the WASM coordinator):
  - `core/src/frontend/format/formatter.cpp`: prints the AST, taking every token from the source token stream
  - `core/src/frontend/format/layout.cpp`: the Wadler-style layout engine (groups, line breaks, trailing comments)
- Type analysis:
  - `core/src/frontend/type_check.cpp`
  - `core/src/frontend/type_system/*.cpp`
- Bytecode compile, optimize, serialize, load:
  - `core/src/bytecode/compiler.cpp`
  - `core/src/bytecode/optimizer.cpp`: every pass builds its output through `StreamRewrite`, which keeps jump
    targets pointing at the same code and refuses to remove or fold a run that a jump lands inside.
  - `Std.Branch`, `Std.Loop`, and `Std.Tuple.Map` run in place when their functions are inline one-parameter closures
    or named user functions (`try_emit_branch_in_place`, `try_emit_loop_in_place`, `try_emit_map_in_place` in
    `compiler.cpp`): the argument goes into a local slot and the code jumps between the bodies, so calls stay in one
    interpreter loop. A map steps through its items with `kIterate` and collects the results with
    `kBuildTupleCounted`. Other function arguments use the `kBranchCall` and `kLoopCall` opcodes and the
    `Std.Tuple.Map` builtin.
  - `Std.Match` runs in place (`try_emit_match_in_place`) when every pattern is `_`, a constant, or a function that
    can run in place, and every handler can run in place or is an inline closure with no parameter. It becomes a
    chain of tests on a subject slot: `kCmpEq` for a constant, a call for a predicate, and `kMatchFailed` at the end
    when there is no `_`. Patterns after the matching case are not evaluated. A local, a value let, or a computed
    pattern or handler may hold a function or a value, so those calls use the `Std.Match` builtin. The `_` marker
    lowering writes is `common::kMatchWildcardSentinel` (`fleaux/common/match_wildcard.hpp`).
  - Every call of a function value lowers to `(arguments, function) -> Std.Apply`. It runs in place
    (`try_emit_apply`) when the function is a named user function (a direct `kCallUserFunc` or `kCallUserFuncArgs`)
    or an inline closure that is not generic and has no variadic parameter (its arguments go into slots and its body
    runs there). Any other function compiles to `kCallFunctionValue`, or to `kCallFunctionValueArgs n` when the
    arguments are written as a list of two or more, which passes them on the stack instead of as a tuple.
  - `core/src/bytecode/serialization.cpp`
  - `core/src/bytecode/module_loader.cpp`
- Execution:
  - VM runtime: `core/src/vm/runtime.cpp`
  - Runtime values: `core/include/fleaux/runtime/value_core.hpp` (the 16-byte `Value`: numbers and Bool inline,
    strings, tuples, dicts, functions, file handles, and tasks as atomically refcounted heap objects that copy
    on write) and `core/include/fleaux/runtime/value.hpp` (construction, reading, printing). Function values are
    `Callable` subclasses; the VM's own (`CompiledFunctionValue` for named functions and closures alike, and
    `BuiltinCallable`) are in `runtime.cpp` and refer to the module of the invocation that made them, so
    `Runtime::invoke_symbol` never returns one to the host.
  - A tuple is one heap block: a 16-byte header (`TupleObject`: the reference count, and the size and capacity as
    32-bit counts, so a tuple holds at most 4,294,967,295 items) and then the items. Code reads a tuple as a
    `TupleView` (`std::span<const Value>`) and builds one with a `TupleBuilder`, which constructs the items in the
    block the tuple keeps. Nothing builds a tuple from a `std::vector<Value>`: moving a vector's items into the block
    costs more than the allocation it saves, 4.6 times the cost of building in place for a 12,288-item tuple. A
    builder grows by doubling, and `finish()` trades a block more than half unused for one of the exact size. A
    closure's `CompiledFunctionValue` holds its capture tuple itself.
  - Values change in place when the reference doing so is their only one, which no program can observe. The last
    read of a local slot moves the value out (`kTakeLocal`): `move_last_local_reads`
    (`core/src/bytecode/local_liveness.cpp`) runs at the end of `BytecodeCompiler::compile`, so the REPL gets it too,
    and turns a `kLoadLocal` into a `kTakeLocal` when no path reads the slot again before writing it. Its table of
    each opcode's effect on locals is a `switch` with no `default`: a new opcode that reads or writes a slot must be
    added there. The builtins then take their tuple, dict, or string out of the argument (`take_tuple_item`, which
    copies only the item when the argument is shared) and change it through `mutable_tuple` or `mutable_dict`, which
    copy only shared data. `TupleBuilder::reopen` continues a tuple in its own block (`push`, `insert`, `erase`), so
    `Std.Tuple.Append` and `Std.Array.Concat` grow in amortized constant time. `First`, `Second`, `ElementAt`,
    spreads, and in-place `Std.Tuple.Map` move items out of a tuple held once.
  - Every empty tuple is one shared value (`empty_tuple()` in `value_core.hpp`): `make_tuple()`, `BuildTuple 0`, and a
    builder that finishes empty allocate nothing.
  - A dict of up to `Dict::kLinearEntries` (8) entries finds keys by comparing them with each entry's and has no hash
    index; the index is built when a ninth key arrives and dropped when an erase brings the dict back to 8.
  - Heap objects come from the runtime's allocator (`core/include/fleaux/runtime/heap.hpp`, with its slow paths in
    `core/src/runtime/heap.cpp`, library `fleaux_heap`):
    - **Blocks.** Blocks of up to 256 bytes come from 16 size classes, kept on a free list per thread, with a shared
      depot for the blocks a thread does not keep; bigger blocks come from `operator new`. `TupleObject::allocate`,
      `HeapObject`'s `operator new` and sized `operator delete`, and a dict's entries and index (`heap::Allocator`)
      use it.
    - **Libraries.** Chunks are never freed, and a block's class follows from the size handed back with it, so each
      library's copy of the allocator (the host's, and each extension's) frees the others' blocks. Everything that
      makes values links `fleaux_heap`.
    - **Thread lookup.** A thread's lists are found through a `ThreadSlot` (see below), whose cleanup empties them
      into the depot when the thread ends or the library unloads.
    - **Checks.** Debug builds check each block's size and liveness when it is freed. The AddressSanitizer and
      ThreadSanitizer builds take every block from `operator new` (`FLEAUX_HEAP_SYSTEM_ALLOCATOR`).
  - Per-thread state goes through `common::ThreadSlot` (`core/include/fleaux/common/thread_slot.hpp`,
    `core/src/common/thread_slot.cpp`, in `fleaux_common`), not `thread_local`. MinGW emulates `thread_local`, and
    every access takes a winpthreads lock.
    - **The slot.** A `ThreadSlot` is a `constinit` pointer per thread with a cleanup. On Windows `get()` is
      `TlsGetValue`, and a fiber-local callback runs the cleanup when the thread ends, and for every thread when the
      library unloads or the process exits. Elsewhere the pointers live in a `thread_local` array.
    - **The VM's state.** The VM keeps all of its per-thread state in one `VmThreadState` (`runtime.cpp`), found
      through one slot: the scratch stacks, the active execution state, the streams and process arguments, and the
      help metadata store. `runtime_thread_context()`, `runtime_execution_state_thread_context()` and
      `help_metadata_thread_context()` return its parts.
    - **Adding state.** New per-thread state that hot code reaches belongs there, not in a new `thread_local`.
  - A run (`Runtime::execute`) takes the thread's operand and frame stacks from the scratch pool, as calls from
    native code into compiled functions do. Runs repeated on one thread (`--bench`, the REPL, `VmHost`) therefore
    keep the stacks' capacity. `CallFrameStack::pop` releases the frame's inline locals, so a value a run leaves
    there does not outlive it.
  - Values become text by `append_value_repr` (`value.hpp`), which writes into a `std::string`: `to_string`,
    `print_value_repr`, `Std.String.Join`, and `Std.ToString` all use it. Building a `std::ostringstream` costs more
    than writing most values, so the runtime does not make one per value.
  - A failure inside a function value reaches `Std.Try` and the task worker as a value where it can:
    `Callable::call_or_fail` returns no value and the error text instead of throwing, for the VM's builtins
    (`BuiltinCallable`) and compiled functions (`CompiledFunctionValue::run_or_fail`). A function value made from C++
    still throws, so both keep their `catch`. Each throw costs microseconds with MinGW's SEH unwinding.
  - Native extensions compile `Value` and the heap objects inline, so a change to their layout or to how they
    allocate changes `kBindingPluginAbiVersion` (`core/include/fleaux/embed/binding_plugin.hpp`). An extension built
    for another version is refused when it loads.
  - `kCallFunctionValue` and `kCallFunctionValueArgs` enter a function value the VM made for the running module
    (its `Callable::owner()` is that `RunningModule`) as a frame in the same loop: the values it holds (a closure's
    captures, a generic function's hidden operator arguments) go into the first slots, then the arguments. Recursion
    through function values therefore does not use the native stack. Builtin and host function values go through
    `Callable::call`, as do the function values that builtins such as `Std.Tuple.Filter` call, which run on a scratch
    stack of their own.
  - The hot helpers of the loop do not return `tl::expected<void, RuntimeError>`: GCC zero-fills all 128 bytes of
    one on every success, which costs more than an addition. Binding a call returns the argument count that did not
    fit (`BindFailure`), and `apply_binary_op` and `apply_unary_op` return a message; the loop makes the
    `RuntimeError` when one fails.
  - `run_loop` keeps the running frame, its instructions, and a pointer to the next instruction in locals.
    - **Leaving the frame.** The position goes back into the frame only when a call leaves it (`leave_frame`).
    - **Entering a frame.** After a call pushes a frame or a return pops one, the loop enters the frame on top again
      (`enter_top_frame`). A push may move the frame storage, so no frame pointer is kept across one.
    - **Adding an opcode.** An opcode that pushes a frame must leave the running frame before the push and enter the
      new one after it.
  - `Value`'s assignments take the new contents first and release the old object last, without a temporary
    (`Value::release`, `destroy_object`).
  - GCC does not inline the loop's helpers into `run_loop`: value assignment, the stack's `push_back`, and
    `get_or_create_local`. The reason is that `runtime.cpp` reaches GCC's `inline-unit-growth` cap.
    `internal_docs/vm_loop_overhead_plan_2026-11-23.md` measures raising the cap for that file, which was not done.
  - Each invocation runs a `RunningModule` (`runtime.cpp`): the bytecode with its constant pool made into values
    once, at the start, so `kPushConst` copies a value instead of building one (a string only gains a reference).
    It is made before the invocation's `TaskScope`, so tasks still running at the end can use it.
  - Opcodes that pop and push work on the top of the stack in place (`apply_binary_op`, `apply_unary_op`): the
    operands stay in their slots and the result replaces the left one, with one size check per instruction.
  - A call of a user function with two or more parameters, written with one argument per parameter, compiles to
    `kCallUserFuncArgs` (`try_emit_user_args_call_fast_path`), which binds the arguments from the stack without an
    argument tuple. `Std.Bit.And`, `Or`, `Xor`, `ShiftLeft`, and `ShiftRight` have opcodes of their own, like the
    arithmetic builtins.
- Shared import resolution primitives: `core/include/fleaux/frontend/import_resolution.hpp`

## Build-target ownership
Reference: `core/CMakeLists.txt`

- `fleaux_frontend_diagnostics`: diagnostics formatting and spans
- `fleaux_frontend_parser`: lexer, parser, and source formatter
- `fleaux_frontend_lowering`: lowering and type system
- `fleaux_heap`: the allocator for values; everything that makes values links it, extensions included
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

- `core/src/frontend/format/formatter.cpp` (the printer must take the new tokens in source order)

Tests:
- `core/tests/parser_tests.cpp`
- `core/tests/parser_type_syntax_tests.cpp`
- `core/tests/lowering_tests.cpp`
- `core/tests/formatter_tests.cpp` (it formats every sample, so new syntax in a sample is covered)
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
  The compiler emits by the shape (`emit_flow_expr`, `emit_spread_tuple`). A builtin with one parameter receives
  its argument value itself on every path, a direct call, a function value or a host call, and never a tuple holding
  it (`builtin_takes_one_argument`). Spreads are joined at runtime with the `ConcatTuples` opcode.
- Do not flatten singleton tuple types or unwrap runtime tuple values: a one-item tuple is a real value, including
  when passed through a function value.
- Function value calls are `(arguments, function) -> Std.Apply` flows with `IRFlowExpr::calls_function_value` set.
  Lowering produces them for closure stages, including a closure after a tuple template stage, whose replaced
  template is the closure's parameter list (`try_apply_closure_stage`, `lower_flow`); the checker rewrites a target naming a local,
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
- `core/src/vm/runtime.cpp`: a builtin of one parameter (and `Std.Println` and `Std.Printf`, which return their
  argument tuple) takes its argument as a `Value` and goes in the switch of `dispatch_builtin`; every other builtin
  takes `const runtime::Arguments&` (`args[i]` reads one, `args.take(i)` moves one out) and goes in
  `kArgumentsBuiltins`, which a `static_assert` checks is complete (`vm::builtin_takes_arguments` decides which).
  The compiler calls the latter with `kCallBuiltinArgs` on the argument values on the stack, with no argument tuple;
  an argument list with spreads, or a call through a function value, passes the tuple, and `Arguments` lends its
  items.
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
- Host calls are the one untyped entry: `Runtime::invoke_symbol` checks the host's argument against `ExportedSymbol::host_entry_check`, built per let by `host_entry_check_for_let` (`checker.cpp`) and checked by `check_host_argument` (`runtime/type_check.hpp`). Function parameters are checked by value kind, and every use of a bounded generic parameter must match one alternative of its bound common to all uses. Runtime builtins and the VM therefore read argument kinds without validating them (`int64_value`, `uint64_value`, `uint64_index` in `value.hpp`); only real runtime failures (overflow, ranges, IO, `Unwrap` of `Err`, closed handles) stay checked.
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

### 9) Native functions and extensions
Touch:
- `core/src/frontend/parser.cpp` and `core/src/frontend/lexer.cpp` (`__native__`)
- `core/src/frontend/type_system/checker.cpp` (`native_return_check_for_let`) and `check_decl.cpp`
- `core/src/bytecode/compiler.cpp` (`compile_native_function_body`), `serialization.cpp` and `module_loader.cpp`
  (the `native_functions` table and `kCallNative`)
- `core/src/vm/runtime.cpp` (`bind_native_functions`, `kCallNative`) and `core/src/embed/vm_host.cpp` (the resolver)
- `core/src/cli/vm_main.cpp` (`--extension`), `core/include/fleaux/frontend/import_resolution.hpp` (search paths)
- `extensions/gl/`: the OpenGL extension, built with `FLEAUX_BUILD_GL_EXTENSION`. An extension links `fleaux_heap`

Tests:
- `core/tests/native_call_tests.cpp`
- The native cases in `parser_tests.cpp`, `bytecode_tests.cpp`, `formatter_tests.cpp`, and `vm_cli_tests.cpp`

### 10) Diagnostics formatting and source spans
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
- Parser and grammar: `parser_tests`, `parser_type_syntax_tests`, `lowering_tests`, `formatter_tests`
- Formatter: `formatter_tests`, plus the format mode case in `vm_cli_tests`
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
./core/cmake-build-debug/bin/fleaux --format samples/01_hello_world.fleaux
./core/cmake-build-debug/bin/fleaux --format --check samples/01_hello_world.fleaux
```

