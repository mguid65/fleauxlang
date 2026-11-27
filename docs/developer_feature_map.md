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
  - `core/src/frontend/lowering.cpp`, with `--dump-ir`'s printer in `core/src/frontend/ir_dump.cpp`
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
  - Reads of an item of a tuple in a local, such as `(s, 0) -> Std.ElementAt`, run as one instruction that does
    not copy the tuple. A copy costs an atomic increment and decrement, and a loop that carries its state in a tuple
    reads it several times per iteration.
    - **The pass.** `LocalItemReadFusionPass` turns `LoadLocal s; PushConst k; ElementAt` with an Int64 constant `k` of 0 or more
      into `kLoadLocalItem`, and the same read starting with `kTakeLocal` into `kTakeLocalItem`. It is always on, and
      runs after the other passes because folding can make an index constant.
    - **The operand.** The slot in the low 32 bits and the index in the 31 above (`local_item_operand` in
      `opcode.hpp`). The index is not a constant pool index, so linking leaves the operand alone.
    - **The VM.** The two cases sit at the end of `run_loop`'s switch: next to `kTakeLocal`, they made other programs
      4 to 7% slower from layout alone.
    - **Where it runs.** Only the module loader runs the optimizer, so code compiled for the REPL or
      `VmHost::run_source` keeps the three instructions.
  - **In-place forms.** A builtin call whose function arguments allow it is written as a loop or a branch in the
    caller (`kInPlaceForms` in `compiler.cpp`, keyed by `BuiltinId`, one emitter each); otherwise it calls the builtin
    with `kCallBuiltinArgs` like any other, and the builtin calls the function from C++. The table's comment gives
    which functions each form takes and the measurement behind it.
  - `Std.Branch`, `Std.Loop`, and `Std.Tuple.Map` run in place when their functions are inline one-parameter closures
    or named user functions (`try_emit_branch_in_place`, `try_emit_loop_in_place`, `try_emit_map_in_place` in
    `compiler.cpp`): the argument goes into a local slot and the code jumps between the bodies, so calls stay in one
    interpreter loop. A map steps through its items with `kIterate` and collects the results with
    `kBuildTupleCounted`.
  - `Std.Tuple.Filter` and `Std.Tuple.Reduce` run in place only when their function is an inline closure
    (`try_emit_filter_in_place`, `try_emit_reduce_in_place`); any other function goes to the builtin. Run in place, a
    named function costs a `kCallUserFunc` per item, as much as the builtin's call of it.
    - **Filter** uses Map's loop and pushes each item it keeps. It does not count them: `kMarkStack` stores the stack's
      height in a slot before the loop, and `kBuildTupleSinceMark` builds the tuple of everything above it after.
    - **Reduce** keeps its accumulator in a slot and stores the closure's result back each time. The closure's last
      read of the accumulator moves it, so an accumulator held once is still changed in place.
    - **The slots.** The mark and the accumulator go in the slot below the loop's three, so the item slot stays the
      highest and the closure's own locals cannot overwrite them.
  - Calls of small named functions are inlined (`try_emit_inlined_call`, and `emit_in_place_call` for the named
    functions Map, Loop, Branch and Match run in place): the body is emitted in place of `kCallUserFunc`, with the
    arguments in slots above the caller's. A call that is not inlined costs about 9 ns.
    - **Which.** A function of the module being compiled, with one or more parameters, none variadic and no hidden
      operator parameters, whose body has at most `kInlineBodyNodeLimit` (32) IR nodes. Never the function being
      compiled or one being inlined, so recursion stays a call, and at most `kInlineDepthLimit` (3) deep. A call of
      several parameters is inlined only when it writes one argument per parameter.
    - **Still compiled as functions**, for function values, host calls, exports, imports, recursion, and calls with an
      argument tuple.
    - **Tests** of call instructions and of the VM's calls compile with `CompileOptions::inline_functions` off.
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
    `Runtime::invoke_symbol` never returns one to the host. A Bool is 0 or 1 in the payload's whole 64 bits: stored in
    one byte of a zeroed payload, it stalled the next 8-byte copy of the value on a failed store-to-load forward.
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
    `Std.Tuple.Append` and `Std.Tuple.Concat` grow in amortized constant time. `First`, `Second`, `ElementAt`,
    spreads, and in-place `Std.Tuple.Map` move items out of a tuple held once.
  - An in-place `Std.Tuple.Map` (`try_emit_map_in_place`) is a loop tested at the bottom:
    `Jump C  B: <func x>  C: Iterate s  JumpIf B`.
    - **`kIterate`.** It stores the next item in the slot after the index (`s + 2`), so the item never goes through the
      stack. It reaches its three slots with one lookup.
    - **Moving or copying.** It moves the item out of a tuple held once through `Value::unshared_tuple_items()`, and
      copies from a shared one through `take_tuple_item`. The copy stays a call on purpose: inlined as well, it made
      `run_loop` big enough that GCC stopped inlining other hot code.
  - Every empty tuple is one shared value (`empty_tuple()` in `value_core.hpp`): `make_tuple()`, `BuildTuple 0`, and a
    builder that finishes empty allocate nothing.
  - A dict of up to `Dict::kLinearEntries` (8) entries finds keys by comparing them with each entry's and has no hash
    index; the index is built when a ninth key arrives and dropped when an erase brings the dict back to 8.
  - Heap objects come from the runtime's allocator (`core/include/fleaux/runtime/heap.hpp`, with its slow paths in
    `core/src/runtime/heap.cpp`, library `fleaux_heap`):
    - **Blocks.** Blocks of up to 8 KiB come from 36 size classes: 16 bytes apart up to 256, then four for each
      doubling. They are kept on a free list per thread, with a shared depot for the blocks a thread does not keep;
      bigger blocks come from `operator new`. `TupleObject::allocate`, `HeapObject`'s `operator new` and sized
      `operator delete`, and a dict's entries and index (`heap::Allocator`) use it.
    - **Classes and the ABI.** Every library must map a size to the same class, so changing the classes changes
      `kBindingPluginAbiVersion`.
    - **Releasing.** `HeapObject::release()` skips the atomic decrement when the count reads 1. The caller then holds
      the only reference, and no other thread can change the count.
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
    `Std.Println`, `Std.String.Join`, and `Std.ToString` all use it. Building a `std::ostringstream` costs more
    than writing most values, so the runtime does not make one per value.
  - `Std.Task.*` and `Std.Parallel.*` run on one shared pool of up to 16 workers (`TaskRuntime` in
    `core/include/fleaux/runtime/builtins_parallel.hpp`), one task per item.
    - **A parallel map's items.** `Std.Parallel.Map` and `ForEach` queue every item at once through `submit_all`: one
      lock, and as many `notify_one` calls as there are items, up to the worker count. Submitted one at a time, items
      shorter than a submission left the workers asleep between them, and each item cost about 4.7 us; waking every
      worker made runs with a few tasks up to 2.3 times slower.
    - **A window.** `Std.Parallel.WithOptions` with a `max_workers` below the item count submits each item as the
      one `max_workers` before it finishes.
    - **The job's context.** Each job carries the submitting run's execution state, streams, and process arguments;
      the jobs of one `submit_all` share one copy of the arguments.
    - **Waiting** (`await_task`). A pool worker that waits for a task no worker has started takes the task's job out of
      the queue and runs it itself. Otherwise a parallel map inside a parallel item, or a task awaited inside one,
      waits forever once every worker waits. A worker runs only the task it waits for, so the jobs it nests are as deep
      as the program's calls. Other threads only wait, and `Std.Task.WithTimeout` never runs the task, so its timeout
      holds. A worker is known by `RuntimeThreadContext::pool_worker`.
    - **A failed map.** After an item fails, `run_parallel_pooled` waits for every item it has submitted before it
      returns the Err: the run that called it can end as soon as it returns, and an item still running then would run
      code that the run has freed.
  - `Std.Println`, `Std.Printf`, and the prompt of `Std.Input` write through `write_runtime_output` (`value.hpp`), under
    one lock, since parallel items and tasks print to the stream of the run that started them.
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
  - **Binding.** Every call binds its arguments to the new frame in one of two ways: `bind_argument`, from one
    argument value (the argument itself for one parameter, the tuple of the arguments otherwise), and
    `bind_arguments`, from consecutive values (the top of the stack, through `bind_stack_arguments`, or the pair of
    `call_binary`). A user function binds as a function value that holds nothing. A count that does not fit the
    parameters is an error, also for a user function, whose shape the checker guarantees for compiled code but not
    for a bytecode file.
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
  - Under GCC, `runtime.cpp` compiles with `--param=inline-unit-growth=400` (set in `core/CMakeLists.txt`).
    - **Why.** The file compiles the whole runtime, and at GCC's default cap the loop's helpers (value assignment,
      the stack's `push_back`, `get_or_create_local`) stayed calls. With the larger cap, tight loops are a quarter to
      a third faster (`internal_docs/vm_loop_overhead_plan_2026-11-23.md`).
    - **Checking.** `-fopt-info-inline-missed` shows what GCC still leaves out of line.
    - **Warnings it exposes.** More inlining can turn up `-Wnull-dereference` reports. Fix them where the pointer is
      dereferenced, as `Value::callable_if_function()` does for function values, rather than turning the warning off.
  - Each invocation runs a `RunningModule` (`runtime.cpp`): the bytecode with its constant pool made into values
    once, at the start, so `kPushConst` copies a value instead of building one (a string only gains a reference).
    It is made before the invocation's `TaskScope`, so tasks still running at the end can use it.
  - Opcodes that pop and push work on the top of the stack in place (`apply_binary_op`, `apply_unary_op`): the
    operands stay in their slots and the result replaces the left one, with one size check per instruction.
  - A call of a user function with two or more parameters, written with one argument per parameter, compiles to
    `kCallUserFuncArgs` (`try_emit_user_args_call_fast_path`), which binds the arguments from the stack without an
    argument tuple. `Std.Bit.And`, `Or`, `Xor`, `ShiftLeft`, and `ShiftRight` have opcodes of their own, like the
    arithmetic builtins.
- Runtime error locations (`internal_docs/e1_plan_2026-11-25.md`):
  - **The table.** Each `Instruction` (`core/include/fleaux/bytecode/module.hpp`) carries a location number in what
    was its padding, so it stays 16 bytes. The number is an index plus one into `Module::locations` (`kNoLocation` is
    0). A `SourceLocation` holds the file (an index into `Module::source_files`, which records each path and a hash
    of its text), the span, and, for code of an inlined body, the function (or `inlined_closure` for an inline
    closure run in place) and the location it was inlined at. A call that an in-place form makes records the builtin
    in `called_by`.
  - **Compiling.** `emit_expr` stamps each instruction an expression emits that no inner expression claimed, and
    `location_numbers` gives equal locations one number. A closure's body is not marked as inlined code
    (`ClosureLocationScope`). An in-place form's call of its function gets `in_place_call_location`, the builtin
    call's own location with `called_by`, so a failure in the function names the builtin as when the builtin calls it
    from C++. The optimizer keeps the location of the last instruction it replaces. Linking appends
    each module's files and locations and rebases the numbers (`MergeMaps::location_base`), and
    `verify_module_locations` checks them when bytecode loads.
  - **The chain.** It is built only when a run fails. Every error return of the loop (`run_instructions`) goes through
    `fail_at`, which only stores the failing frame's position, and `run_loop`, around the loop, reads the location of
    the failing instruction and of each frame's call below it into `RuntimeError::calls` once (`append_calls`). A call
    at each of the 91 failure sites made a loop of arithmetic and item reads 6% slower, though none of them runs while
    the program succeeds. The position is passed by value: a lambda that captured `ip` made the loop 4 to 10% slower.
    A failure in a function value that a builtin calls back crosses the C++ call as a `ReportedFailure`, which keeps
    the calls so far, and the builtin's frame adds "called by Std.X".
  - **The notes.** `describe_runtime_calls` (`core/src/vm/runtime_calls.cpp`) makes the notes: calls repeated from
    one place become one line with a count, and a chain longer than `kRuntimeCallNoteLimit` lines keeps its first 15
    and last 5. `runtime_failure_span` gives the CLI and the REPL the span to show, without the source line when the
    file's hash no longer matches. The embedding API passes the calls on as `HostError::calls`.
  - **Messages.** A builtin's message is "Std.Name: what failed". The implementation throws the text alone and
    `error_from_exception` adds the name; a `runtime::ProgramFailure` (`Std.Panic`, `Std.Assert`) is shown as it is.
- The bytecode trust boundary. A `.fleaux.bc` file is untrusted input, and its checksum only catches accidents:
  - **Reading** (`deserialize_module` in `serialization.cpp`): every count is checked against the bytes left, at the
    fewest bytes one item takes (`fits`), before anything is allocated for it; a bool is a byte of 0 or 1
    (`read_bool`, and `read_pod` refuses `bool`); enums are range checked; and data after the last section is refused.
  - **Verifying** (`verifier.cpp`), before the loader links a file's module: `verify_module_operands` checks every
    operand that names a constant, function, closure, type check, native function, jump target, or local slot (under
    `kMaxLocalSlots`, since a store grows a frame's locals to its slot), each closure's function, and each exported
    function; then the builtin call sites and the locations. Linking reads those indices unchecked. The VM keeps its
    own checks.
- Nesting. The parser refuses expressions and types nested more than 256 levels (`kMaxNesting` in `parser.cpp`,
  counted in `primary` and `type`), since the parser and every pass after it recurse once per level. A union parses
  as a list of its alternatives, without recursing per `|`. The executables give their main thread 8 MB of stack on
  Windows (`fleaux_set_main_thread_stack` in `core/CMakeLists.txt`), which Linux and macOS give by default: a Debug
  build overflowed Windows' 1 MB below 200 levels.
- Fuzzing (`core/fuzz/`): libFuzzer harnesses for the parser and formatter (`fleaux_parser_fuzzer`) and for reading
  and verifying bytecode (`fleaux_bytecode_fuzzer`, which writes the input's checksum first). Built with the Conan
  option `build_fuzzers` (`FLEAUX_BUILD_FUZZERS`), with Clang, AddressSanitizer, and UBSan. CI's `fuzz` job runs each
  for 60 seconds on the samples and their bytecode, and keeps any failing input. A longer run by hand:
  `fleaux_parser_fuzzer corpus/ -max_total_time=3600`. A failing input goes into the unit tests as a regression case.
- Shared import resolution primitives: `core/include/fleaux/frontend/import_resolution.hpp`

## Build-target ownership
Reference: `core/CMakeLists.txt`

- `fleaux_frontend_diagnostics`: diagnostics formatting and spans
- `fleaux_frontend_parser`: lexer, parser, and source formatter
- `fleaux_frontend_lowering`: lowering and type system
- `fleaux_heap`: the allocator for values; everything that makes values links it, extensions included
- `fleaux_bytecode`: bytecode compiler, loader, optimizer, serializer
- `fleaux_vm`: VM runtime
- `fleaux_line_editor`: the REPL's line editor, which the CLI and the tests link
- `fleaux`: CLI executable
- `fleaux_core_tests`: test runner

Every target sets its warnings with `fleaux_set_project_warnings`, and warnings are errors in every configuration.

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
Touch, each checked by a `static_assert` or a test:
- `stdlib/Std.fleaux` (language signature)
- `core/include/fleaux/vm/builtin_catalog.hpp`: the `BuiltinId` enum (callable builtins go before `Half`), and the
  builtin's row in `kBuiltinSpecs`, in the same order, with its arity and its slots. A slot is an argument the compiler
  records at each call and the verifier checks when it loads bytecode: a function (`callable_slot(index, arity)`), or a
  value that must come from an exact UInt64 or Int64, a TaskHandle, RandomGenerator, or FileHandle, or a
  `Dict<String, Any>` (`value_slot(index, kind)`). The test "Builtin slots follow the parameter types in
  stdlib/Std.fleaux" derives each builtin's slots from its Std parameter types and checks the row.
- The implementation in `core/include/fleaux/runtime/builtins_*.hpp`. The regex builtins are in
  `core/src/vm/builtins_regex.cpp`, since only the VM links PCRE2.
- Its entry in `kBuiltinImplementations` (`core/src/vm/runtime.cpp`): `takes_value` for a builtin of one parameter (and
  `Std.Println` and `Std.Printf`, which return their argument tuple), which takes its argument as a `Value`;
  `takes_arguments` for every other builtin, which takes `const runtime::Arguments&` (`args[i]` reads one, `args.take(i)`
  moves one out). `vm::builtin_takes_arguments` decides which, and a `static_assert` checks that every callable builtin
  has the one its kind needs. The compiler calls the latter with `kCallBuiltinArgs` on the argument values on the
  stack, with no argument tuple; an argument list with spreads, or a call through a function value, passes the tuple,
  and `Arguments` lends its items.
- `core/src/frontend/type_system/builtin_contracts.cpp`, for a rule a type cannot state (scalar Dict keys, parallel
  options).

Rules a builtin follows:
- **How it fails.** A failure the program cannot rule out before the call (the file system or the OS refuses, text is
  not a number, a lookup finds nothing) is a `Result<T, String>` whose Err names the builtin and says why. Any other
  failure is a broken precondition: the implementation throws, and the VM reports it as a runtime error with its
  location. Every Err of Std is a String, so Results of different builtins compose.
- **Positions and counts.** Every position, count, length, size, and duration is Int64. Read a position with
  `position_index` (a negative one is out of range, as one past the end is) and a count with `count_index` (a
  negative one throws, naming it), both in `value.hpp`.
- **Constants.** A constant is a value let with a literal body in `stdlib/Std.fleaux`
  (`let Std.Pi: Float64 = 3.141592653589793;`), with no catalog row or implementation. The checker records the literal
  (`FunctionSig::literal_value`, then `IRNameRef::constant_value`) and the compiler pushes it with `kPushConst`. Any
  other top-level value let is evaluated each time it is read; a block-local let is evaluated once.

No `BYTECODE_VERSION` change is needed: the bytecode header records `vm::kBuiltinTableHash`, a hash of each builtin's
position, name, symbol key, arity, and slots, and the loader refuses bytecode compiled against another table.

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
- Hidden operator parameters in bytecode: a function with operator requirements has its hidden parameters first in
  its compiled parameters: `Total<T: Summable>(xs, zero)` of sample 59 has `arity: 3`, and its `param_type_names`
  in the disassembly begin with the operator's `(T, T) => T`. `FunctionDef::hidden_operator_params` says how many. A caller passes the operator functions there; an import, the
  REPL's overload key (`repl_signature_key`), and a host call skip them to see the declared parameters.
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

- Runtime errors: `core/src/vm/runtime_calls.cpp` (the call notes), `run_loop` and `fail_at` in `core/src/vm/runtime.cpp`, and
  `print_diag_and_return` in `core/src/cli/vm_main.cpp`

Tests:
- `core/tests/diagnostics_tests.cpp`
- `core/tests/runtime_calls_tests.cpp`, and the location cases in `bytecode_tests.cpp` and `vm_cli_tests.cpp`
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

## Benchmarks
`bench/` times release builds:
- `bench/ab.py BINARY,BINARY PROGRAM,PROGRAM`: an A/B of binaries, the best median of several `--bench` runs, with the
  binaries taking turns.
- `bench/suite.py run BINARY --out FILE`: the standard set (the timed samples, `bench/programs/`, and
  `fleaux_runtime_microbench` with `--microbench`). `bench/suite.py compare bench/baseline.json FILE` marks what got
  slower by more than 5%; a stabilization cycle ends with that check.
- `bench/python/compare.py BINARY`: the algorithmic samples against Python ports of them.

The scripts time copies of the programs without their `.fleaux.bc` caches, which `--no-emit-bytecode` would still
read. Set `FLEAUX_BENCH_PATH` to a directory to put in front of `PATH`, such as MinGW's `bin`.

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

