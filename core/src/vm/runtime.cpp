#include <algorithm>
#include <cctype>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string_view>
#include <variant>
#include <vector>

// The order here is important
#include "fleaux/runtime/runtime_support.hpp"
#include "repl_support.hpp"
#include "std_help_metadata.hpp"

#include "fleaux/bytecode/compiler.hpp"
#include "fleaux/common/overloaded.hpp"
#include "fleaux/common/thread_slot.hpp"
#include "fleaux/vm/builtin_catalog.hpp"
#include "fleaux/vm/runtime.hpp"

namespace fleaux::vm {
namespace {

using runtime::Value;

auto make_runtime_error(const std::string& message, const std::optional<std::string>& hint = std::nullopt,
                        const std::optional<frontend::diag::SourceSpan>& span = std::nullopt) -> RuntimeError {
  return RuntimeError{
      .message = message,
      .hint = hint,
      .span = span,
  };
}

[[nodiscard]] auto make_process_args_for_invocation(const std::vector<std::string>& process_args,
                                                    const std::string_view entry_label) -> std::vector<std::string> {
  std::vector<std::string> scoped_args;
  scoped_args.reserve(process_args.size() + 1U);
  scoped_args.emplace_back(entry_label.empty() ? "<runtime>" : std::string{entry_label});
  scoped_args.insert(scoped_args.end(), process_args.begin(), process_args.end());
  return scoped_args;
}

[[nodiscard]] auto resolve_runtime_input_stream(const std::optional<std::reference_wrapper<std::istream>>& input)
    -> std::istream& {
  return input.has_value() ? input->get() : std::cin;
}

[[nodiscard]] auto resolve_runtime_output_stream(const std::optional<std::reference_wrapper<std::ostream>>& output)
    -> std::ostream& {
  return output.has_value() ? output->get() : std::cout;
}

[[nodiscard]] auto find_exported_symbol(const bytecode::Module& bytecode_module,
                                        const std::string_view qualified_symbol) -> const bytecode::ExportedSymbol* {
  const auto it =
      std::ranges::find_if(bytecode_module.exports, [qualified_symbol](const bytecode::ExportedSymbol& sym) {
        return sym.name == qualified_symbol || sym.link_name == qualified_symbol;
      });
  if (it == bytecode_module.exports.end()) {
    return nullptr;
  }
  return &*it;
}

// Value helpers

auto const_to_value(const bytecode::ConstValue& const_value) -> Value {
  using namespace fleaux::runtime;
  return std::visit(common::overloaded{[](const std::int64_t& val) -> Value { return make_int(val); },
                                       [](const std::uint64_t& val) -> Value { return make_uint(val); },
                                       [](const double& val) -> Value { return make_float(val); },
                                       [](const bool& val) -> Value { return make_bool(val); },
                                       [](const std::string& val) -> Value { return make_string(val); },
                                       [](const std::monostate&) -> Value { return make_null(); }},
                    const_value.data);
}

// A module as one invocation runs it: its bytecode, and its constants made into values once, before any of it runs.
// Pushing a constant copies its value, which for a string only adds a reference. The function values the invocation
// makes refer to it, and none of them outlives the invocation. It does not change after it is made, so tasks on other
// threads read it freely.
class RunningModule {
public:
  explicit RunningModule(const bytecode::Module& bytecode_module) : bytecode_(&bytecode_module) {
    constants_.reserve(bytecode_module.constants.size());
    for (const auto& constant : bytecode_module.constants) {
      constants_.push_back(const_to_value(constant));
    }
  }
  RunningModule(const RunningModule&) = delete;
  RunningModule(RunningModule&&) = delete;
  auto operator=(const RunningModule&) -> RunningModule& = delete;
  auto operator=(RunningModule&&) -> RunningModule& = delete;
  ~RunningModule() = default;

  [[nodiscard]] auto bytecode() const -> const bytecode::Module& { return *bytecode_; }
  [[nodiscard]] auto constants() const -> const std::vector<Value>& { return constants_; }

private:
  const bytecode::Module* bytecode_;
  std::vector<Value> constants_;
};

// Stack helpers

auto stack_underflow(const char* context) -> RuntimeError {
  return make_runtime_error(std::string("stack underflow on ") + context);
}

auto pop_stack(std::vector<Value>& stack, const char* context) -> tl::expected<Value, RuntimeError> {
  if (stack.empty()) {
    return tl::unexpected(stack_underflow(context));
  }
  Value value = std::move(stack.back());
  stack.pop_back();
  return value;
}

// The function value a call opcode calls, and the callable it holds.
struct PoppedFunction {
  Value value;
  const runtime::Callable* callable;
};

// Pops the function value a call opcode calls, which lies above its `argument_count` argument values.
auto pop_function_value(std::vector<Value>& stack, const char* opname, const std::size_t argument_count)
    -> tl::expected<PoppedFunction, RuntimeError> {
  if (stack.size() <= argument_count) {
    return tl::unexpected(stack_underflow(opname));
  }
  Value function = std::move(stack.back());
  stack.pop_back();
  // The callable is checked for null, not only the kind, so that the compiler knows it is not null where it is used.
  const auto* callable =
      function.is_function() ? static_cast<const runtime::Callable*>(function.object()) : nullptr;
  if (callable == nullptr) {
    return tl::unexpected(make_runtime_error(std::format("{} expects a function value, not {}", opname,
                                                         runtime::runtime_value_kind_name(function))));
  }
  return PoppedFunction{.value = std::move(function), .callable = callable};
}

template <typename Fn>
auto run_native_op(const char* opname, Fn&& fn) -> tl::expected<Value, RuntimeError> {
  try {
    return fn();
  } catch (const std::exception& ex) {
    return tl::unexpected(make_runtime_error(std::string("native '") + opname + "' threw: " + ex.what()));
  }
}

// Why a kLoadLocalItem or kTakeLocalItem of `local` failed, with kLoadLocal's message for a slot past the frame's
// locals and kElementAt's otherwise.
auto local_item_read_error(const Value* local) -> RuntimeError {
  if (local == nullptr) {
    return make_runtime_error("local slot index out of range");
  }
  if (!local->is_tuple()) {
    return make_runtime_error(std::string("native 'element_at' threw: ") + runtime::kind_error("Tuple").what());
  }
  return make_runtime_error("native 'element_at' threw: ElementAt: index out of range");
}

// The helpers below run for every arithmetic, comparison, and logic opcode, so they fail with a message rather than a
// RuntimeError: GCC zero-fills all 128 bytes of a tl::expected<void, RuntimeError> on every success, which costs more
// than an addition. The loop makes the RuntimeError when one fails.

// Replaces the top two stack values, the left operand below the right, with op(left, right). The operands stay in
// their slots while op reads them, and the result goes into the left one's.
template <typename Op>
auto apply_binary_op(std::vector<Value>& stack, const char* opname, Op&& op) -> tl::expected<void, std::string> {
  if (stack.size() < 2U) {
    return tl::unexpected(stack_underflow(opname).message);
  }
  Value& lhs = stack[stack.size() - 2U];
  try {
    lhs = std::forward<Op>(op)(lhs, stack.back());
  } catch (const std::exception& ex) {
    return tl::unexpected(std::string("native '") + opname + "' threw: " + ex.what());
  }
  stack.pop_back();
  return {};
}

// Replaces the top stack value with op(value), in its slot.
template <typename Op>
auto apply_unary_op(std::vector<Value>& stack, const char* opname, Op&& op) -> tl::expected<void, std::string> {
  if (stack.empty()) {
    return tl::unexpected(stack_underflow(opname).message);
  }
  Value& value = stack.back();
  try {
    value = std::forward<Op>(op)(value);
  } catch (const std::exception& ex) {
    return tl::unexpected(std::string("native '") + opname + "' threw: " + ex.what());
  }
  return {};
}

// Calls an extension function and checks its result against the declared return type: extensions are outside the
// type checker, and code after the call relies on that type.
auto call_native_function(const bytecode::NativeFunctionRecord& record, const runtime::NativeFunction& function,
                          const Value& arg) -> tl::expected<Value, RuntimeError> {
  Value result;
  try {
    result = function(arg);
  } catch (const std::exception& ex) {
    return tl::unexpected(make_runtime_error(std::format("native function '{}' failed: {}", record.symbol, ex.what())));
  }
  if (!runtime::value_matches_type_check(result, record.return_check)) {
    return tl::unexpected(make_runtime_error(std::format("native function '{}' returned {}, but it is declared to return {}",
                                                         record.symbol, runtime::runtime_value_kind_name(result),
                                                         common::describe(record.return_check))));
  }
  return result;
}

// Binds every native function the module declares, before any of it runs, so that a missing extension stops the
// program at the start rather than partway through.
auto bind_native_functions(const bytecode::Module& bytecode_module, const RuntimeInvocationOptions& options,
                           runtime::RuntimeExecutionState& execution_state) -> tl::expected<void, RuntimeError> {
  execution_state.native_functions.clear();
  execution_state.native_functions.reserve(bytecode_module.native_functions.size());
  for (const auto& record : bytecode_module.native_functions) {
    std::optional<runtime::NativeFunction> function =
        options.native_resolver ? options.native_resolver(record.symbol) : std::nullopt;
    if (!function.has_value() || !*function) {
      return tl::unexpected(RuntimeError{
          .message = std::format("Native function '{}' is not provided by any loaded extension.", record.symbol),
          .hint = "Load the extension that implements it, for example with 'fleaux --extension <dir>'.",
          .span = std::nullopt,
      });
    }
    execution_state.native_functions.push_back(std::move(*function));
  }
  return {};
}

void collapse_stack_tail_into_tuple(std::vector<Value>& stack, const std::size_t tuple_size) {
  const auto base = stack.size() - tuple_size;
  Value tuple = runtime::make_tuple_moved(std::span{stack}.subspan(base));
  stack.resize(base);
  stack.push_back(std::move(tuple));
}

// The items of `source` from `first_index` on, moved into a tuple of their own.
auto make_array_tail_value(const std::span<Value> source, const std::size_t first_index) -> Value {
  return runtime::make_tuple_moved(source.subspan(first_index));
}

struct CallFrame {
  const std::vector<bytecode::Instruction>* instructions = nullptr;
  std::size_t ip = 0;
  std::array<Value, 2> inline_locals{};
  std::size_t inline_local_count = 0U;
  bool using_inline_locals = false;
  std::vector<Value> locals;
};

void reset_frame_locals(CallFrame& frame) {
  frame.inline_local_count = 0U;
  frame.using_inline_locals = false;
  frame.locals.clear();
}

void activate_inline_locals(CallFrame& frame, const std::size_t count) {
  frame.using_inline_locals = true;
  frame.inline_local_count = count;
  frame.locals.clear();
}

void promote_inline_locals_to_vector(CallFrame& frame, const std::size_t min_size) {
  if (!frame.using_inline_locals) {
    if (frame.locals.size() < min_size) {
      frame.locals.resize(min_size);
    }
    return;
  }

  frame.locals.clear();
  frame.locals.reserve(std::max(min_size, frame.inline_local_count));
  for (std::size_t index = 0; index < frame.inline_local_count; ++index) {
    frame.locals.push_back(std::move(frame.inline_locals[index]));
  }
  if (frame.locals.size() < min_size) {
    frame.locals.resize(min_size);
  }
  frame.inline_local_count = 0U;
  frame.using_inline_locals = false;
}

[[nodiscard]] auto try_get_local(const CallFrame& frame, const std::size_t slot) -> const Value* {
  if (frame.using_inline_locals) {
    return slot < frame.inline_local_count ? &frame.inline_locals[slot] : nullptr;
  }
  return slot < frame.locals.size() ? &frame.locals[slot] : nullptr;
}

[[nodiscard]] auto try_get_local(CallFrame& frame, const std::size_t slot) -> Value* {
  return const_cast<Value*>(try_get_local(std::as_const(frame), slot));
}

[[nodiscard]] auto get_or_create_local(CallFrame& frame, const std::size_t slot) -> Value& {
  if (frame.using_inline_locals && slot < 2U) {
    if (slot >= frame.inline_local_count) {
      frame.inline_local_count = slot + 1U;
    }
    return frame.inline_locals[slot];
  }

  promote_inline_locals_to_vector(frame, slot + 1U);
  return frame.locals[slot];
}

struct CallFrameStack {
  std::vector<CallFrame> storage;
  std::size_t active_size = 0U;

  [[nodiscard]] auto empty() const -> bool { return active_size == 0U; }

  [[nodiscard]] auto back() -> CallFrame& { return storage[active_size - 1U]; }

  [[nodiscard]] auto back() const -> const CallFrame& { return storage[active_size - 1U]; }

  auto push(const std::vector<bytecode::Instruction>* instructions) -> CallFrame& {
    if (active_size == storage.size()) {
      storage.emplace_back();
    }
    auto& frame = storage[active_size++];
    frame.instructions = instructions;
    frame.ip = 0;
    reset_frame_locals(frame);
    return frame;
  }

  // Releases the values in the frame's inline locals, which binding the next call would only overwrite: the frame
  // stacks outlive a run (see ScopedInvocationScratch), so a value left there would outlive it too.
  void pop() {
    auto& frame = storage[active_size - 1U];
    frame.instructions = nullptr;
    frame.ip = 0;
    for (Value& local : frame.inline_locals) {
      if (runtime::is_heap_kind(local.kind())) {
        local = Value{};
      }
    }
    reset_frame_locals(frame);
    --active_size;
  }

  void clear_active() {
    while (!empty()) {
      pop();
    }
  }

  void reserve(const std::size_t capacity) { storage.reserve(capacity); }
};

struct InvocationScratch {
  std::vector<Value> stack;
  CallFrameStack frames;
};

// The stacks that runs and nested calls on one thread reuse. A run (Runtime::execute), and a call from native code into
// a compiled function, takes the next one and gives it back when it returns. They are kept in a deque so that taking a
// new one leaves references to the others valid.
struct InvocationScratchPool {
  std::deque<InvocationScratch> scratches;
  std::size_t depth = 0U;
};

// The VM's state for one thread: the stacks runs and calls reuse, the active run's execution state, its streams and
// process arguments, and the help metadata Std.Help reads. One ThreadSlot finds it, rather than a thread_local for each
// part: MinGW emulates thread_local, and every access of an emulated one takes a lock.
struct VmThreadState {
  InvocationScratchPool scratch;
  runtime::RuntimeExecutionStateThreadContext execution{runtime::default_runtime_execution_state()};
  runtime::RuntimeThreadContext streams;
  runtime::HelpMetadataThreadContext help{runtime::default_help_metadata_store()};
};

void end_vm_thread_state(void* state) noexcept { delete static_cast<VmThreadState*>(state); }

constinit common::ThreadSlot g_vm_state_slot{&end_vm_thread_state};

auto make_vm_thread_state() -> VmThreadState& {
  auto state = std::make_unique<VmThreadState>();
  if (g_vm_state_slot.set(state.get())) {
    return *state.release();
  }
  // The thread is ending, or the process is exiting: a state of the thread's own for the rest of it, never cleaned up.
  thread_local VmThreadState* late = nullptr;
  if (late == nullptr) {
    late = state.release();
  }
  return *late;
}

auto vm_thread_state() -> VmThreadState& {
  if (void* const state = g_vm_state_slot.get(); state != nullptr) {
    return *static_cast<VmThreadState*>(state);
  }
  return make_vm_thread_state();
}

}  // namespace
}  // namespace fleaux::vm

namespace fleaux::runtime {

auto runtime_execution_state_thread_context() -> RuntimeExecutionStateThreadContext& {
  return vm::vm_thread_state().execution;
}

auto runtime_thread_context() -> RuntimeThreadContext& { return vm::vm_thread_state().streams; }

auto help_metadata_thread_context() -> HelpMetadataThreadContext& { return vm::vm_thread_state().help; }

}  // namespace fleaux::runtime

namespace fleaux::vm {
namespace {

// A call reaches the thread's state once, when it takes its scratch, and keeps references to the pool and the scratch
// from then on.
class ScopedInvocationScratch {
public:
  ScopedInvocationScratch(const std::size_t operand_stack_capacity, const std::size_t frame_stack_capacity)
      : pool_(vm_thread_state().scratch), depth_index_(pool_.depth++) {
    if (depth_index_ == pool_.scratches.size()) {
      pool_.scratches.emplace_back();
    }
    scratch_ = &pool_.scratches[depth_index_];
    scratch_->stack.clear();
    scratch_->frames.clear_active();
    scratch_->stack.reserve(operand_stack_capacity);
    scratch_->frames.reserve(frame_stack_capacity);
  }

  ScopedInvocationScratch(const ScopedInvocationScratch&) = delete;
  auto operator=(const ScopedInvocationScratch&) -> ScopedInvocationScratch& = delete;

  ~ScopedInvocationScratch() {
    scratch_->stack.clear();
    scratch_->frames.clear_active();
    pool_.depth = depth_index_;
  }

  [[nodiscard]] auto stack() const -> std::vector<Value>& { return scratch_->stack; }

  [[nodiscard]] auto frames() const -> CallFrameStack& { return scratch_->frames; }

private:
  InvocationScratchPool& pool_;
  std::size_t depth_index_;
  InvocationScratch* scratch_ = nullptr;
};

// Loop exit type
// std::monostate -> kHalt was hit (top-level completion).
// Value          -> kReturn emptied the frame stack (standalone function result).

using LoopExit = std::variant<std::monostate, Value>;
using LoopResult = tl::expected<LoopExit, RuntimeError>;

constexpr std::size_t k_min_stack_capacity = 16U;
constexpr std::size_t k_min_frame_capacity = 4U;

[[nodiscard]] auto suggested_operand_stack_capacity(const std::size_t instruction_count) -> std::size_t {
  return std::max(k_min_stack_capacity, instruction_count);
}

[[nodiscard]] auto suggested_frame_stack_capacity(const std::size_t function_count) -> std::size_t {
  return std::max(k_min_frame_capacity, function_count + 1U);
}

auto constant_builtin_value(const BuiltinId builtin_id) -> std::optional<double> {
  for (const auto& spec : all_constant_builtin_specs()) {
    if (spec.id == builtin_id) {
      return spec.value;
    }
  }
  return std::nullopt;
}

// Forward declarations.
auto run_loop(const RunningModule& running_module, std::vector<Value>& stack, CallFrameStack& frames,
              std::ostream& output) -> LoopResult;

auto dispatch_builtin(BuiltinId builtin_id, BuiltinCallShape /*shape*/, Value arg) -> tl::expected<Value, RuntimeError>;
auto dispatch_builtin_args(BuiltinId builtin_id, std::span<Value> values) -> tl::expected<Value, RuntimeError>;
auto builtin_constant(BuiltinId builtin_id) -> Value;
using ArgumentsBuiltin = Value (*)(const runtime::Arguments&);
[[nodiscard]] auto arguments_builtin(BuiltinId builtin_id) -> ArgumentsBuiltin;

// The two-argument form of a builtin, for the builtins that have one.
using BinaryBuiltin = auto (*)(const Value&, const Value&) -> Value;

[[nodiscard]] auto builtin_binary_specialization(const BuiltinId builtin_id) -> BinaryBuiltin {
  switch (builtin_id) {
    case BuiltinId::Add:
      return &runtime::AddBinary;
    case BuiltinId::Subtract:
      return &runtime::SubtractBinary;
    case BuiltinId::Multiply:
      return &runtime::MultiplyBinary;
    case BuiltinId::Divide:
      return &runtime::DivideBinary;
    case BuiltinId::Mod:
      return &runtime::ModBinary;
    case BuiltinId::Pow:
      return &runtime::PowBinary;
    case BuiltinId::BitAnd:
      return &runtime::BitAndBinary;
    case BuiltinId::BitOr:
      return &runtime::BitOrBinary;
    case BuiltinId::BitXor:
      return &runtime::BitXorBinary;
    case BuiltinId::BitShiftLeft:
      return &runtime::BitShiftLeftBinary;
    case BuiltinId::BitShiftRight:
      return &runtime::BitShiftRightBinary;
    case BuiltinId::Equal:
      return &runtime::EqualBinary;
    case BuiltinId::NotEqual:
      return &runtime::NotEqualBinary;
    case BuiltinId::LessThan:
      return &runtime::LessThanBinary;
    case BuiltinId::GreaterThan:
      return &runtime::GreaterThanBinary;
    case BuiltinId::GreaterOrEqual:
      return &runtime::GreaterOrEqualBinary;
    case BuiltinId::LessOrEqual:
      return &runtime::LessOrEqualBinary;
    case BuiltinId::And:
      return &runtime::AndBinary;
    case BuiltinId::Or:
      return &runtime::OrBinary;
    case BuiltinId::ElementAt:
      return &runtime::ElementAtBinary;
    default:
      return nullptr;
  }
}

// Binds a call argument to the parameters of a user function. Argument counts and shapes are guaranteed by the type
// checker for Fleaux callers and by the host entry check for host callers, so they are not checked again here.
void bind_user_function_locals(CallFrame& frame, const std::uint32_t arity, const bool has_variadic_tail, Value arg) {
  reset_frame_locals(frame);
  if (frame.locals.capacity() < arity) {
    frame.locals.reserve(arity);
  }

  if (arity == 0U) {
    return;
  }

  if (!has_variadic_tail) {
    if (arity == 1U) {
      activate_inline_locals(frame, 1U);
      // Pipeline/user-call lhs adaptation is syntax-aware and already happens before runtime binding.
      // Do not unwrap again here, because real singleton tuple values are meaningful arguments.
      frame.inline_locals[0] = std::move(arg);
      return;
    }

    const std::span<Value> arr = arg.mutable_tuple();
    if (arity == 2U) {
      activate_inline_locals(frame, 2U);
      frame.inline_locals[0] = std::move(arr[0]);
      frame.inline_locals[1] = std::move(arr[1]);
      return;
    }

    for (std::uint32_t arg_index = 0; arg_index < arity; ++arg_index) {
      frame.locals.emplace_back(std::move(arr[static_cast<std::size_t>(arg_index)]));
    }
    return;
  }

  // Variadic: final parameter captures remaining args as a tuple.
  if (arity == 1U) {
    activate_inline_locals(frame, 1U);
    frame.inline_locals[0] = arg.is_tuple() ? std::move(arg) : runtime::make_tuple(std::move(arg));
    return;
  }

  const auto fixed_count = static_cast<std::size_t>(arity - 1U);
  const std::span<Value> arr = arg.mutable_tuple();
  for (std::size_t arg_index = 0; arg_index < fixed_count; ++arg_index) {
    frame.locals.emplace_back(std::move(arr[arg_index]));
  }
  frame.locals.emplace_back(make_array_tail_value(arr, fixed_count));
}

// Binds the top `arity` values of the stack, the first argument deepest, to the parameters of a frame just pushed,
// and pops them.
void bind_stack_arguments(CallFrame& frame, std::vector<Value>& stack, const std::size_t arity) {
  const auto base = stack.size() - arity;
  if (arity <= frame.inline_locals.size()) {
    activate_inline_locals(frame, arity);
    for (std::size_t index = 0; index < arity; ++index) {
      frame.inline_locals[index] = std::move(stack[base + index]);
    }
  } else {
    frame.locals.reserve(arity);
    for (std::size_t index = 0; index < arity; ++index) {
      frame.locals.push_back(std::move(stack[base + index]));
    }
  }
  stack.resize(base);
}

// Function values
//
// A function value is a compiled function and the values it holds: none for a named function, the captures of a
// closure, or the hidden operator arguments of a generic function. Entering one puts those values in the first slots
// of the new frame, where its body reads them, and binds the arguments to the parameters it declares. The arguments
// come either as the one value Callable::call takes (the argument itself for one parameter, the tuple of the arguments
// otherwise) or as separate values on top of the stack.

// The parameters a function value declares, after the values it holds.
struct DeclaredParameters {
  std::uint32_t arity{0U};
  bool variadic{false};
};

// Writes the slots of a frame just pushed, in order.
class FrameSlotWriter {
public:
  FrameSlotWriter(CallFrame& frame, const std::size_t count)
      : frame_(&frame), inline_(count <= frame.inline_locals.size()) {
    if (inline_) {
      activate_inline_locals(frame, count);
    } else {
      frame.locals.reserve(count);
    }
  }

  void push(Value value) {
    if (inline_) {
      frame_->inline_locals[next_++] = std::move(value);
    } else {
      frame_->locals.push_back(std::move(value));
    }
  }

  void push_all(const runtime::TupleView values) {
    for (const Value& value : values) {
      push(value);
    }
  }

private:
  CallFrame* frame_;
  bool inline_;
  std::size_t next_{0U};
};

// A binding that failed: the number of arguments the call passed, which do not fit the parameters. Binding returns it
// rather than a tl::expected<void, RuntimeError>, whose success GCC zero-fills in full, because calls through function
// values are hot; the caller makes the error from it (function_value_argument_error).
using BindFailure = std::optional<std::size_t>;

auto function_value_argument_error(const std::string& name, const DeclaredParameters parameters,
                                   const std::size_t count) -> RuntimeError {
  const std::size_t expected = parameters.variadic ? parameters.arity - 1U : parameters.arity;
  return make_runtime_error(std::format("function value '{}' takes {}{} argument{}, but was called with {}", name,
                                        parameters.variadic ? "at least " : "", expected, expected == 1U ? "" : "s",
                                        count));
}

// Binds the argument value of a call of a function value, after the values it holds.
auto bind_function_value_argument(CallFrame& frame, const runtime::TupleView held,
                                  const DeclaredParameters parameters, Value argument) -> BindFailure {
  if (parameters.arity < 2U) {
    FrameSlotWriter slots(frame, held.size() + parameters.arity);
    slots.push_all(held);
    if (parameters.arity == 1U) {
      // A variadic parameter collects its arguments into a tuple: a single argument that is not one becomes one.
      slots.push(parameters.variadic && !argument.is_tuple() ? runtime::make_tuple(std::move(argument))
                                                             : std::move(argument));
    }
    return std::nullopt;
  }

  const std::size_t fixed = parameters.variadic ? parameters.arity - 1U : parameters.arity;
  if (!argument.is_tuple()) {
    return BindFailure{1U};
  }
  const std::size_t count = argument.tuple().size();
  if (count < fixed) {
    return BindFailure{count};
  }
  // The arguments move out of a tuple that nothing else refers to, and are copied out of any other.
  const bool owned = argument.object()->unique();
  const std::span<Value> owned_items = owned ? argument.mutable_tuple() : std::span<Value>{};
  const runtime::TupleView items = argument.tuple();
  const auto take = [&](const std::size_t index) -> Value {
    if (owned) {
      return std::move(owned_items[index]);
    }
    return items[index];
  };

  FrameSlotWriter slots(frame, held.size() + parameters.arity);
  slots.push_all(held);
  for (std::size_t index = 0; index < fixed; ++index) {
    slots.push(take(index));
  }
  if (parameters.variadic) {
    runtime::TupleBuilder rest(count - fixed);
    for (std::size_t index = fixed; index < count; ++index) {
      rest.push(take(index));
    }
    slots.push(std::move(rest).finish());
  }
  return std::nullopt;
}

// Binds `count` arguments on top of the stack, the first deepest, to a call of a function value, after the values it
// holds, and pops them.
auto bind_function_value_stack_arguments(CallFrame& frame, const runtime::TupleView held,
                                         const DeclaredParameters parameters, std::vector<Value>& stack,
                                         const std::size_t count) -> BindFailure {
  const std::size_t fixed = parameters.variadic ? parameters.arity - 1U : parameters.arity;
  if (parameters.arity < 2U || count < fixed) {
    // A function of one parameter takes the arguments as one tuple, and too few arguments are reported as for a
    // tuple of them.
    collapse_stack_tail_into_tuple(stack, count);
    Value argument = std::move(stack.back());
    stack.pop_back();
    return bind_function_value_argument(frame, held, parameters, std::move(argument));
  }

  const std::size_t base = stack.size() - count;
  FrameSlotWriter slots(frame, held.size() + parameters.arity);
  slots.push_all(held);
  for (std::size_t index = 0; index < fixed; ++index) {
    slots.push(std::move(stack[base + index]));
  }
  if (parameters.variadic) {
    slots.push(runtime::make_tuple_moved(std::span{stack}.subspan(base + fixed)));
  }
  stack.resize(base);
  return std::nullopt;
}

// Binds the two arguments of a call of a function value of two parameters, after the values it holds.
void bind_function_value_pair(CallFrame& frame, const runtime::TupleView held, Value lhs, Value rhs) {
  FrameSlotWriter slots(frame, held.size() + 2U);
  slots.push_all(held);
  slots.push(std::move(lhs));
  slots.push(std::move(rhs));
}

auto run_loop_intrinsic(Value state, const Value& continue_func, const Value& step_func,
                        const std::optional<std::size_t> max_iters) -> tl::expected<Value, RuntimeError> {
  try {
    const runtime::Callable& continue_callable = runtime::as_callable(continue_func);
    const runtime::Callable& step_callable = runtime::as_callable(step_func);
    std::size_t iterations = 0;
    while (runtime::as_bool(continue_callable.call(state))) {
      if (max_iters.has_value() && iterations >= *max_iters) {
        throw std::runtime_error("LoopN: exceeded max_iters");
      }
      state = step_callable.call(std::move(state));
      ++iterations;
    }
    return state;
  } catch (const std::exception& ex) {
    return tl::unexpected(make_runtime_error(std::string("native 'loop' threw: ") + ex.what()));
  }
}

// run_user_function

// The value a function run on a scratch stack of its own returned.
auto function_result(LoopResult loop_result, const std::string& name) -> tl::expected<Value, RuntimeError> {
  if (!loop_result) {
    return tl::unexpected(loop_result.error());
  }
  if (std::holds_alternative<std::monostate>(*loop_result)) {
    return tl::unexpected(make_runtime_error("halt inside function '" + name + "'"));
  }
  return std::get<Value>(std::move(*loop_result));
}

auto run_user_function(const RunningModule& running_module, const std::size_t fn_idx, Value arg,
                       std::ostream& output) -> tl::expected<Value, RuntimeError> {
  const auto& functions = running_module.bytecode().functions;
  if (fn_idx >= functions.size()) {
    return tl::unexpected(make_runtime_error("function index out of range"));
  }
  const auto& function = functions[fn_idx];
  const ScopedInvocationScratch scratch{suggested_operand_stack_capacity(function.instructions.size()),
                                        suggested_frame_stack_capacity(functions.size())};
  auto& frame = scratch.frames().push(&function.instructions);
  bind_user_function_locals(frame, function.arity, function.has_variadic_tail, std::move(arg));
  return function_result(run_loop(running_module, scratch.stack(), scratch.frames(), output), function.name);
}

// The result of a call made through a function value: function values are called from builtins and host code, which
// report failures by throwing.
auto value_or_throw(tl::expected<Value, RuntimeError> result) -> Value {
  if (!result) {
    throw std::runtime_error(result.error().message);
  }
  return std::move(*result);
}

// A function value for a compiled function, with the values it holds (see "Function values" above): a closure's capture
// tuple, or nothing (a null value) for a named function. It refers to the running module and the output stream of the
// invocation that made it; both outlive it, because Runtime::invoke_symbol does not let a function value leave the
// invocation. The loop enters it as a frame of its own (kCallFunctionValue); when a builtin or host code calls it, it
// runs on a scratch stack of its own.
class CompiledFunctionValue final : public runtime::Callable {
public:
  CompiledFunctionValue(const RunningModule& running_module, const bytecode::FunctionDef& function, Value held,
                        const DeclaredParameters parameters, std::ostream& output)
      : Callable(&running_module),
        module_(&running_module),
        function_(&function),
        output_(&output),
        held_(std::move(held)),
        held_items_(held_.is_tuple() ? held_.tuple() : runtime::TupleView{}),
        parameters_(parameters),
        operand_stack_capacity_(suggested_operand_stack_capacity(function.instructions.size())),
        frame_stack_capacity_(suggested_frame_stack_capacity(running_module.bytecode().functions.size())) {}

  [[nodiscard]] auto function() const -> const bytecode::FunctionDef& { return *function_; }

  // Binds a call's argument value to the frame entered for the call.
  [[nodiscard]] auto bind(CallFrame& frame, Value argument) const -> BindFailure {
    return bind_function_value_argument(frame, held_items_, parameters_, std::move(argument));
  }

  // Binds `count` arguments on top of the stack to the frame entered for the call, and pops them.
  [[nodiscard]] auto bind_from_stack(CallFrame& frame, std::vector<Value>& stack, const std::size_t count) const
      -> BindFailure {
    return bind_function_value_stack_arguments(frame, held_items_, parameters_, stack, count);
  }

  // The error for a call whose `count` arguments do not fit the parameters.
  [[nodiscard]] auto argument_error(const std::size_t count) const -> RuntimeError {
    return function_value_argument_error(function_->name, parameters_, count);
  }

  [[nodiscard]] auto call(Value argument) const -> Value override {
    return run([&](CallFrame& frame) { return bind(frame, std::move(argument)); });
  }

  [[nodiscard]] auto call_or_fail(Value argument, std::string& error) const -> std::optional<Value> override {
    return run_or_fail([&](CallFrame& frame) { return bind(frame, std::move(argument)); }, error);
  }

  [[nodiscard]] auto call_binary(Value lhs, Value rhs) const -> Value override {
    if (parameters_.variadic || parameters_.arity != 2U) {
      return Callable::call_binary(std::move(lhs), std::move(rhs));
    }
    return run([&](CallFrame& frame) -> BindFailure {
      bind_function_value_pair(frame, held_items_, std::move(lhs), std::move(rhs));
      return std::nullopt;
    });
  }

private:
  // Runs the function on a scratch stack of its own. Builtins and host code call function values often, so this reads
  // the loop's result in place rather than through function_result.
  template <typename Bind>
  [[nodiscard]] auto run(Bind&& bind_arguments) const -> Value {
    std::string error;
    if (auto value = run_or_fail(std::forward<Bind>(bind_arguments), error); value.has_value()) {
      return std::move(*value);
    }
    throw std::runtime_error(error);
  }

  template <typename Bind>
  [[nodiscard]] auto run_or_fail(Bind&& bind_arguments, std::string& error) const -> std::optional<Value> {
    const ScopedInvocationScratch scratch{operand_stack_capacity_, frame_stack_capacity_};
    auto& frame = scratch.frames().push(&function_->instructions);
    if (const BindFailure failure = std::forward<Bind>(bind_arguments)(frame); failure.has_value()) {
      error = argument_error(*failure).message;
      return std::nullopt;
    }
    auto loop_result = run_loop(*module_, scratch.stack(), scratch.frames(), *output_);
    if (!loop_result) {
      error = std::move(loop_result.error().message);
      return std::nullopt;
    }
    if (auto* value = std::get_if<Value>(&*loop_result); value != nullptr) {
      return std::move(*value);
    }
    error = "halt inside function '" + function_->name + "'";
    return std::nullopt;
  }

  const RunningModule* module_;
  const bytecode::FunctionDef* function_;
  std::ostream* output_;
  Value held_;
  // The items of held_, which no one changes while the function value holds it.
  runtime::TupleView held_items_;
  DeclaredParameters parameters_;
  std::size_t operand_stack_capacity_;
  std::size_t frame_stack_capacity_;
};

// A function value for a builtin.
class BuiltinCallable final : public runtime::Callable {
public:
  BuiltinCallable(const BuiltinId builtin_id, const BuiltinCallShape shape)
      : builtin_id_(builtin_id), shape_(shape), binary_(builtin_binary_specialization(builtin_id)) {}

  [[nodiscard]] auto call(Value argument) const -> Value override {
    return value_or_throw(dispatch_builtin(builtin_id_, shape_, std::move(argument)));
  }

  [[nodiscard]] auto call_or_fail(Value argument, std::string& error) const -> std::optional<Value> override {
    auto result = dispatch_builtin(builtin_id_, shape_, std::move(argument));
    if (!result) {
      error = std::move(result.error().message);
      return std::nullopt;
    }
    return std::move(*result);
  }

  [[nodiscard]] auto call_binary(Value lhs, Value rhs) const -> Value override {
    if (binary_ != nullptr) {
      return binary_(lhs, rhs);
    }
    if (!builtin_takes_arguments(builtin_id_)) {
      return Callable::call_binary(std::move(lhs), std::move(rhs));
    }
    // The two values are the arguments; no tuple of them is built.
    std::array<Value, 2> pair{std::move(lhs), std::move(rhs)};
    return value_or_throw(dispatch_builtin_args(builtin_id_, std::span{pair}));
  }

private:
  BuiltinId builtin_id_;
  BuiltinCallShape shape_;
  BinaryBuiltin binary_;
};

// Whether a value holds a function value anywhere inside it. A function value refers to the invocation that made it,
// so the host is never given one.
[[nodiscard]] auto contains_function_value(const Value& value) -> bool {
  switch (value.kind()) {
    case runtime::ValueKind::kFunction:
      return true;
    case runtime::ValueKind::kTuple:
      return std::ranges::any_of(value.tuple(), contains_function_value);
    case runtime::ValueKind::kDict:
      return std::ranges::any_of(value.dict(), [](const runtime::Dict::Entry& entry) -> bool {
        return contains_function_value(entry.first) || contains_function_value(entry.second);
      });
    default:
      return false;
  }
}

// The value an invocation hands back to the host.
[[nodiscard]] auto host_result(const std::string_view qualified_symbol, Value result) -> RuntimeValueResult {
  if (contains_function_value(result)) {
    return tl::unexpected(make_runtime_error(
        "'" + std::string{qualified_symbol} + "' returned a function value, which cannot leave the program.",
        "Return the function's results instead of the function."));
  }
  return result;
}

// A function value for a named function: it holds no values and declares the function's own parameters.
auto make_function_value(const RunningModule& running_module, const bytecode::FunctionDef& function,
                         std::ostream& output) -> Value {
  return runtime::adopt_function(new CompiledFunctionValue(
      running_module, function, Value{},
      DeclaredParameters{.arity = function.arity, .variadic = function.has_variadic_tail}, output));
}

// Calls a function value the VM did not make for the running module (a builtin, a host function, or one of another
// module) with `argument`, which it takes. It returns the result for the caller to store rather than storing it and
// returning tl::expected<void, RuntimeError>, whose success is zero-filled in full (see apply_binary_op): builtins
// called as values, such as the operators of generic functions, come through here on every call.
auto call_function_value_natively(Value& argument, const runtime::Callable& callable, const char* opname)
    -> tl::expected<Value, RuntimeError> {
  return run_native_op(opname, [&]() -> Value { return callable.call(std::move(argument)); });
}

auto make_builtin_function_value(const BuiltinId builtin_id, const BuiltinCallShape shape) -> Value {
  return runtime::adopt_function(new BuiltinCallable(builtin_id, shape));
}

// run_loop

auto run_loop(const RunningModule& running_module, std::vector<Value>& stack, CallFrameStack& frames,
              std::ostream& output) -> LoopResult {
  const bytecode::Module& bytecode_module = running_module.bytecode();
  const std::vector<Value>& constants = running_module.constants();
  if (frames.empty()) {
    return tl::unexpected(make_runtime_error("program terminated without halt"));
  }

  // The running frame, its instructions, and the next one to run, held here rather than read from the frame stack for
  // every instruction. The position goes back into the frame only when a call leaves it; after a call pushes a frame
  // or a return pops one, the loop enters the frame on top.
  CallFrame* frame = nullptr;
  const bytecode::Instruction* code = nullptr;
  const bytecode::Instruction* end = nullptr;
  const bytecode::Instruction* ip = nullptr;
  const auto enter_top_frame = [&]() {
    frame = &frames.back();
    code = frame->instructions->data();
    end = code + frame->instructions->size();
    ip = code + frame->ip;
  };
  // Leaves the running frame for a call: its position is kept in it until the call returns.
  const auto leave_frame = [&]() { frame->ip = static_cast<std::size_t>(ip - code); };
  enter_top_frame();

  for (;;) {
    if (ip >= end) {
      return tl::unexpected(make_runtime_error("program terminated without halt"));
    }
    const auto opcode = ip->opcode;
    const auto operand = ip->operand;
    ++ip;

    switch (opcode) {
      case bytecode::Opcode::kNoOp:
        break;

      case bytecode::Opcode::kPushConst: {
        const auto idx = static_cast<std::size_t>(operand);
        if (idx >= constants.size()) {
          return tl::unexpected(make_runtime_error("constant pool index out of range"));
        }
        stack.push_back(constants[idx]);
        break;
      }

      case bytecode::Opcode::kPop:
        if (stack.empty()) {
          return tl::unexpected(stack_underflow("pop"));
        }
        stack.pop_back();
        break;

      case bytecode::Opcode::kDup: {
        if (stack.empty()) {
          return tl::unexpected(make_runtime_error("stack underflow on dup"));
        }
        stack.push_back(stack.back());
        break;
      }

      case bytecode::Opcode::kBuildTuple: {
        const auto tuple_size = static_cast<std::size_t>(operand);
        if (stack.size() < tuple_size) {
          return tl::unexpected(make_runtime_error("stack underflow on build_tuple"));
        }
        collapse_stack_tail_into_tuple(stack, tuple_size);
        break;
      }

      case bytecode::Opcode::kConcatTuples: {
        const auto count = static_cast<std::size_t>(operand);
        if (operand < 0 || stack.size() < count) {
          return tl::unexpected(make_runtime_error("stack underflow on concat_tuples"));
        }
        const std::size_t first = stack.size() - count;
        std::size_t total = 0;
        for (std::size_t position = first; position < stack.size(); ++position) {
          total += runtime::as_tuple(stack[position]).size();
        }
        runtime::TupleBuilder joined(total);
        for (std::size_t position = first; position < stack.size(); ++position) {
          runtime::push_tuple_items(joined, std::move(stack[position]));
        }
        stack.resize(first);
        stack.push_back(std::move(joined).finish());
        break;
      }

      case bytecode::Opcode::kCheckType:
      case bytecode::Opcode::kTryCheckType: {
        const auto idx = static_cast<std::size_t>(operand);
        if (operand < 0 || idx >= bytecode_module.type_checks.size()) {
          return tl::unexpected(make_runtime_error("type check index out of range"));
        }
        auto value = pop_stack(stack, "check_type");
        if (!value)
          return tl::unexpected(value.error());
        const auto& check = bytecode_module.type_checks[idx];
        if (opcode == bytecode::Opcode::kTryCheckType) {
          stack.push_back(runtime::try_check_cast(std::move(*value), check));
          break;
        }
        if (!runtime::value_matches_type_check(*value, check)) {
          return tl::unexpected(make_runtime_error(runtime::type_check_failure_message("Std.Cast", *value, check)));
        }
        stack.push_back(std::move(*value));
        break;
      }

      case bytecode::Opcode::kCallNative: {
        const auto idx = static_cast<std::size_t>(operand);
        const auto& bound = runtime::runtime_execution_state().native_functions;
        if (operand < 0 || idx >= bytecode_module.native_functions.size() || idx >= bound.size()) {
          return tl::unexpected(make_runtime_error("native function index out of range"));
        }
        auto arg = pop_stack(stack, "call_native");
        if (!arg)
          return tl::unexpected(arg.error());
        const auto& record = bytecode_module.native_functions[idx];
        auto result = call_native_function(record, bound[idx], *arg);
        if (!result)
          return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kBuildTupleCounted: {
        auto count = pop_stack(stack, "build_tuple_counted");
        if (!count) {
          return tl::unexpected(count.error());
        }
        if (!count->is_uint()) {
          return tl::unexpected(make_runtime_error("build_tuple_counted expects a UInt64 count"));
        }
        const std::size_t tuple_size = runtime::to_index(count->uint_value());
        if (stack.size() < tuple_size) {
          return tl::unexpected(make_runtime_error("stack underflow on build_tuple_counted"));
        }
        collapse_stack_tail_into_tuple(stack, tuple_size);
        break;
      }

      case bytecode::Opcode::kIterate: {
        const auto items_slot = static_cast<std::size_t>(operand);
        if (operand < 0) {
          return tl::unexpected(make_runtime_error("iterate slot index out of range"));
        }
        // The items, the index, and the item are three slots in a row. Making the item's, the highest, makes the other
        // two and keeps the locals in the frame's vector, so one lookup finds all three.
        (void)get_or_create_local(*frame, items_slot + 2U);
        Value* const slots = frame->locals.data() + items_slot;
        Value& items = slots[0];
        Value& index = slots[1];
        Value& item = slots[2];
        if (!items.is_tuple() || !index.is_uint()) {
          return tl::unexpected(make_runtime_error("iterate expects a tuple and a UInt64 index"));
        }
        if (const auto position = runtime::to_index(index.uint_value()); position < items.tuple().size()) {
          // Nothing but this loop reads the slot, so the items move out of a tuple nothing else refers to. The copy
          // from a shared tuple stays a call: inlined as well, it made run_loop big enough that GCC stopped inlining
          // other hot code in it.
          if (items.object()->unique()) {
            item = std::move(items.unshared_tuple_items()[position]);
          } else {
            item = runtime::take_tuple_item(items, position);
          }
          index = runtime::make_uint(index.uint_value() + 1U);
          stack.push_back(runtime::make_bool(true));
        } else {
          stack.push_back(runtime::make_bool(false));
        }
        break;
      }

      case bytecode::Opcode::kElementAt:
        if (auto applied = apply_binary_op(stack, "element_at",
                                           [](Value& lhs, const Value& rhs) -> Value { return runtime::take_element(lhs, rhs); });
            !applied) {
          return tl::unexpected(make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kCallBuiltin: {
        const auto builtin_id = builtin_id_from_operand(operand);
        if (!builtin_id.has_value()) {
          return tl::unexpected(make_runtime_error("builtin index out of range"));
        }
        const auto builtin_shape = builtin_call_shape_from_operand(operand);
        if (!builtin_shape.has_value()) {
          return tl::unexpected(make_runtime_error("builtin operand shape is invalid"));
        }

        if (stack.empty()) {
          return tl::unexpected(stack_underflow("call_builtin"));
        }
        auto result = dispatch_builtin(*builtin_id, *builtin_shape, std::move(stack.back()));
        if (!result)
          return tl::unexpected(result.error());
        stack.back() = std::move(*result);
        break;
      }

      case bytecode::Opcode::kCallBuiltinArgs: {
        const auto builtin_id = builtin_id_from_operand(operand);
        if (!builtin_id.has_value()) {
          return tl::unexpected(make_runtime_error("unknown builtin id"));
        }
        const std::size_t count = builtin_argument_count_from_operand(operand);
        if (stack.size() < count) {
          return tl::unexpected(stack_underflow("call_builtin_args"));
        }
        // The builtin works on the arguments where they are: nothing it calls back into uses this stack.
        const std::size_t base = stack.size() - count;
        auto result = dispatch_builtin_args(*builtin_id, std::span{stack}.subspan(base));
        if (!result) {
          return tl::unexpected(result.error());
        }
        // The result takes the first argument's slot, and the rest go from the top.
        if (count == 0U) {
          stack.push_back(std::move(*result));
        } else {
          stack[base] = std::move(*result);
          stack.erase(stack.begin() + static_cast<std::ptrdiff_t>(base + 1U), stack.end());
        }
        break;
      }

      case bytecode::Opcode::kCallUserFunc: {
        const auto fn_idx = static_cast<std::size_t>(operand);
        if (fn_idx >= bytecode_module.functions.size()) {
          return tl::unexpected(make_runtime_error("function index out of range"));
        }
        if (stack.empty()) {
          return tl::unexpected(stack_underflow("call_user_func"));
        }
        Value arg = std::move(stack.back());
        stack.pop_back();

        const auto& function = bytecode_module.functions[fn_idx];
        leave_frame();
        bind_user_function_locals(frames.push(&function.instructions), function.arity, function.has_variadic_tail,
                                  std::move(arg));
        enter_top_frame();
        break;
      }

      case bytecode::Opcode::kCallUserFuncArgs: {
        const auto fn_idx = static_cast<std::size_t>(operand);
        if (fn_idx >= bytecode_module.functions.size()) {
          return tl::unexpected(make_runtime_error("function index out of range"));
        }
        const auto& function = bytecode_module.functions[fn_idx];
        const std::size_t arity = function.arity;
        if (function.has_variadic_tail || arity < 2U) {
          return tl::unexpected(make_runtime_error(
              "call_user_func_args requires a non-variadic function of two or more parameters, not '" + function.name +
              "'"));
        }
        if (stack.size() < arity) {
          return tl::unexpected(stack_underflow("call_user_func_args"));
        }
        leave_frame();
        bind_stack_arguments(frames.push(&function.instructions), stack, arity);
        enter_top_frame();
        break;
      }

      case bytecode::Opcode::kCallFunctionValue: {
        auto function = pop_function_value(stack, "call_function_value", 1U);
        if (!function) {
          return tl::unexpected(function.error());
        }
        const runtime::Callable& callable = *function->callable;
        if (callable.owner() != &running_module) {
          auto result = call_function_value_natively(stack.back(), callable, "call_function_value");
          if (!result) {
            return tl::unexpected(result.error());
          }
          stack.back() = std::move(*result);
          break;
        }
        const auto& compiled = static_cast<const CompiledFunctionValue&>(callable);
        Value argument = std::move(stack.back());
        stack.pop_back();
        leave_frame();
        if (const auto failure = compiled.bind(frames.push(&compiled.function().instructions), std::move(argument));
            failure.has_value()) {
          return tl::unexpected(compiled.argument_error(*failure));
        }
        enter_top_frame();
        break;
      }

      case bytecode::Opcode::kCallFunctionValueArgs: {
        if (operand < 2) {
          return tl::unexpected(make_runtime_error("call_function_value_args takes two or more arguments"));
        }
        const auto count = static_cast<std::size_t>(operand);
        auto function = pop_function_value(stack, "call_function_value_args", count);
        if (!function) {
          return tl::unexpected(function.error());
        }
        const runtime::Callable& callable = *function->callable;
        if (callable.owner() != &running_module) {
          // Any other function value takes the tuple of the arguments.
          collapse_stack_tail_into_tuple(stack, count);
          auto result = call_function_value_natively(stack.back(), callable, "call_function_value_args");
          if (!result) {
            return tl::unexpected(result.error());
          }
          stack.back() = std::move(*result);
          break;
        }
        const auto& compiled = static_cast<const CompiledFunctionValue&>(callable);
        leave_frame();
        if (const auto failure = compiled.bind_from_stack(frames.push(&compiled.function().instructions), stack, count);
            failure.has_value()) {
          return tl::unexpected(compiled.argument_error(*failure));
        }
        enter_top_frame();
        break;
      }

      case bytecode::Opcode::kMakeUserFuncRef: {
        const auto fn_idx = static_cast<std::size_t>(operand);
        if (fn_idx >= bytecode_module.functions.size()) {
          return tl::unexpected(make_runtime_error("function index out of range"));
        }
        stack.push_back(make_function_value(running_module, bytecode_module.functions[fn_idx], output));
        break;
      }

      case bytecode::Opcode::kMakeBuiltinFuncRef: {
        const auto builtin_id = builtin_id_from_operand(operand);
        if (!builtin_id.has_value()) {
          return tl::unexpected(make_runtime_error("builtin index out of range"));
        }
        const auto builtin_shape = builtin_call_shape_from_operand(operand);
        if (!builtin_shape.has_value()) {
          return tl::unexpected(make_runtime_error("builtin operand shape is invalid"));
        }
        stack.push_back(make_builtin_function_value(*builtin_id, *builtin_shape));
        break;
      }

      case bytecode::Opcode::kMakeClosureRef: {
        const auto closure_idx = static_cast<std::size_t>(operand);
        if (closure_idx >= bytecode_module.closures.size()) {
          return tl::unexpected(make_runtime_error("closure index out of range"));
        }

        auto captured_tuple = pop_stack(stack, "make_closure_ref");
        if (!captured_tuple) {
          return tl::unexpected(captured_tuple.error());
        }

        const auto& [function_index, capture_count, declared_arity, declared_has_variadic_tail] =
            bytecode_module.closures[closure_idx];
        if (!captured_tuple->is_tuple() || captured_tuple->tuple().size() != capture_count) {
          return tl::unexpected(make_runtime_error("closure capture tuple size mismatch"));
        }

        // The function value holds the capture tuple itself.
        stack.push_back(runtime::adopt_function(new CompiledFunctionValue(
            running_module, bytecode_module.functions[function_index], std::move(*captured_tuple),
            DeclaredParameters{.arity = declared_arity, .variadic = declared_has_variadic_tail}, output)));
        break;
      }

      case bytecode::Opcode::kReturn: {
        if (stack.empty()) {
          return tl::unexpected(stack_underflow("return"));
        }
        frames.pop();
        if (frames.empty()) {
          // Standalone function execution: the result IS the return value.
          Value result = std::move(stack.back());
          stack.pop_back();
          return LoopExit{std::move(result)};
        }
        // The caller's stack is the same stack: the result is already on top of it.
        enter_top_frame();
        break;
      }

      case bytecode::Opcode::kLoadLocal: {
        const auto slot = static_cast<std::size_t>(operand);
        const Value* local = try_get_local(*frame, slot);
        if (local == nullptr) {
          return tl::unexpected(make_runtime_error("local slot index out of range"));
        }
        stack.push_back(*local);
        break;
      }

      case bytecode::Opcode::kTakeLocal: {
        Value* local = try_get_local(*frame, static_cast<std::size_t>(operand));
        if (local == nullptr) {
          return tl::unexpected(make_runtime_error("local slot index out of range"));
        }
        stack.push_back(std::move(*local));
        break;
      }

      case bytecode::Opcode::kStoreLocal: {
        if (stack.empty()) {
          return tl::unexpected(stack_underflow("store_local"));
        }
        get_or_create_local(*frame, static_cast<std::size_t>(operand)) = std::move(stack.back());
        stack.pop_back();
        break;
      }

      case bytecode::Opcode::kJump: {
        const auto target = static_cast<std::size_t>(operand);
        if (target > static_cast<std::size_t>(end - code)) {
          return tl::unexpected(make_runtime_error("jump target out of range"));
        }
        ip = code + target;
        break;
      }

      case bytecode::Opcode::kJumpIf: {
        if (stack.empty()) {
          return tl::unexpected(stack_underflow("jump_if"));
        }
        const bool condition = runtime::as_bool(stack.back());
        stack.pop_back();
        const auto target = static_cast<std::size_t>(operand);
        if (target > static_cast<std::size_t>(end - code)) {
          return tl::unexpected(make_runtime_error("jump_if target out of range"));
        }
        if (condition) {
          ip = code + target;
        }
        break;
      }

      case bytecode::Opcode::kJumpIfNot: {
        if (stack.empty()) {
          return tl::unexpected(stack_underflow("jump_if_not"));
        }
        const bool condition = runtime::as_bool(stack.back());
        stack.pop_back();
        const auto target = static_cast<std::size_t>(operand);
        if (target > static_cast<std::size_t>(end - code)) {
          return tl::unexpected(make_runtime_error("jump_if_not target out of range"));
        }
        if (!condition) {
          ip = code + target;
        }
        break;
      }

      case bytecode::Opcode::kAdd:
        if (auto applied = apply_binary_op(stack, "add",
                                           [](const Value& lhs, const Value& rhs) -> Value {
                                             return runtime::AddBinary(lhs, rhs);
                                           });
            !applied) {
          return tl::unexpected(make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kSub:
        if (auto applied = apply_binary_op(stack, "sub",
                                           [](const Value& lhs, const Value& rhs) -> Value {
                                             return runtime::SubtractBinary(lhs, rhs);
                                           });
            !applied) {
          return tl::unexpected(make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kMul:
        if (auto applied = apply_binary_op(stack, "mul",
                                           [](const Value& lhs, const Value& rhs) -> Value {
                                             return runtime::MultiplyBinary(lhs, rhs);
                                           });
            !applied) {
          return tl::unexpected(make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kDiv:
        if (auto applied = apply_binary_op(stack, "div",
                                           [](const Value& lhs, const Value& rhs) -> Value {
                                             return runtime::DivideBinary(lhs, rhs);
                                           });
            !applied) {
          return tl::unexpected(make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kMod:
        if (auto applied = apply_binary_op(stack, "mod",
                                           [](const Value& lhs, const Value& rhs) -> Value {
                                             return runtime::ModBinary(lhs, rhs);
                                           });
            !applied) {
          return tl::unexpected(make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kPow:
        if (auto applied = apply_binary_op(stack, "pow",
                                           [](const Value& lhs, const Value& rhs) -> Value {
                                             return runtime::PowBinary(lhs, rhs);
                                           });
            !applied) {
          return tl::unexpected(make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kNeg:
        if (auto applied =
                apply_unary_op(stack, "neg", [](const Value& value) -> Value { return runtime::UnaryMinusValue(value); });
            !applied) {
          return tl::unexpected(make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kCmpEq:
        if (auto applied = apply_binary_op(stack, "cmp_eq",
                                           [](const Value& lhs, const Value& rhs) -> Value {
                                             return runtime::make_bool(lhs == rhs);
                                           });
            !applied) {
          return tl::unexpected(make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kCmpNe:
        if (auto applied = apply_binary_op(stack, "cmp_ne",
                                           [](const Value& lhs, const Value& rhs) -> Value {
                                             return runtime::make_bool(lhs != rhs);
                                           });
            !applied) {
          return tl::unexpected(make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kCmpLt:
        if (auto applied = apply_binary_op(stack, "cmp_lt",
                                           [](const Value& lhs, const Value& rhs) -> Value {
                                             return runtime::make_bool(runtime::compare_numbers(lhs, rhs) < 0);
                                           });
            !applied) {
          return tl::unexpected(make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kCmpGt:
        if (auto applied = apply_binary_op(stack, "cmp_gt",
                                           [](const Value& lhs, const Value& rhs) -> Value {
                                             return runtime::make_bool(runtime::compare_numbers(lhs, rhs) > 0);
                                           });
            !applied) {
          return tl::unexpected(make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kCmpLe:
        if (auto applied = apply_binary_op(stack, "cmp_le",
                                           [](const Value& lhs, const Value& rhs) -> Value {
                                             return runtime::make_bool(runtime::compare_numbers(lhs, rhs) <= 0);
                                           });
            !applied) {
          return tl::unexpected(make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kCmpGe:
        if (auto applied = apply_binary_op(stack, "cmp_ge",
                                           [](const Value& lhs, const Value& rhs) -> Value {
                                             return runtime::make_bool(runtime::compare_numbers(lhs, rhs) >= 0);
                                           });
            !applied) {
          return tl::unexpected(make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kAnd:
        if (auto applied = apply_binary_op(stack, "and",
                                           [](const Value& lhs, const Value& rhs) -> Value {
                                             return runtime::make_bool(runtime::as_bool(lhs) && runtime::as_bool(rhs));
                                           });
            !applied) {
          return tl::unexpected(make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kOr:
        if (auto applied = apply_binary_op(stack, "or",
                                           [](const Value& lhs, const Value& rhs) -> Value {
                                             return runtime::make_bool(runtime::as_bool(lhs) || runtime::as_bool(rhs));
                                           });
            !applied) {
          return tl::unexpected(make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kNot:
        if (auto applied =
                apply_unary_op(stack, "not", [](const Value& value) -> Value { return runtime::make_bool(!runtime::as_bool(value)); });
            !applied) {
          return tl::unexpected(make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kSelect: {
        if (stack.size() < 3U) {
          return tl::unexpected(stack_underflow("select"));
        }
        const auto base = stack.size() - 3U;
        const bool condition = runtime::as_bool(stack[base]);
        stack[base] = std::move(stack[condition ? base + 1U : base + 2U]);
        stack.resize(base + 1U);
        break;
      }

      case bytecode::Opcode::kBranchCall: {
        auto false_func = pop_stack(stack, "branch_call");
        if (!false_func)
          return tl::unexpected(false_func.error());
        auto true_func = pop_stack(stack, "branch_call");
        if (!true_func)
          return tl::unexpected(true_func.error());
        auto value = pop_stack(stack, "branch_call");
        if (!value)
          return tl::unexpected(value.error());
        auto cond = pop_stack(stack, "branch_call");
        if (!cond)
          return tl::unexpected(cond.error());
        auto result = run_native_op("branch_call", [&]() -> Value {
          const Value& chosen = runtime::as_bool(*cond) ? *true_func : *false_func;
          return runtime::invoke_callable(chosen, std::move(*value));
        });
        if (!result)
          return tl::unexpected(result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kLoopCall: {
        auto step_func = pop_stack(stack, "loop_call");
        if (!step_func)
          return tl::unexpected(step_func.error());
        auto continue_func = pop_stack(stack, "loop_call");
        if (!continue_func)
          return tl::unexpected(continue_func.error());
        auto state = pop_stack(stack, "loop_call");
        if (!state)
          return tl::unexpected(state.error());

        auto result = run_loop_intrinsic(std::move(*state), *continue_func, *step_func, std::nullopt);
        if (!result) {
          const std::string prefix = "native 'loop' threw: ";
          std::string msg = result.error().message;
          if (msg.starts_with(prefix)) {
            msg.replace(0, prefix.size(), "native 'loop_call' threw: ");
          }
          return tl::unexpected(make_runtime_error(msg));
        }
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kLoopNCall: {
        auto max_iters = pop_stack(stack, "loop_n_call");
        if (!max_iters)
          return tl::unexpected(max_iters.error());
        auto step_func = pop_stack(stack, "loop_n_call");
        if (!step_func)
          return tl::unexpected(step_func.error());
        auto continue_func = pop_stack(stack, "loop_n_call");
        if (!continue_func)
          return tl::unexpected(continue_func.error());
        auto state = pop_stack(stack, "loop_n_call");
        if (!state)
          return tl::unexpected(state.error());

        // max_iters is a UInt64 by type.
        const std::size_t limit = runtime::uint64_index(*max_iters);

        auto result = run_loop_intrinsic(std::move(*state), *continue_func, *step_func, limit);
        if (!result) {
          const std::string prefix = "native 'loop' threw: ";
          std::string msg = result.error().message;
          if (msg.starts_with(prefix)) {
            msg.replace(0, prefix.size(), "native 'loop_n_call' threw: ");
          }
          return tl::unexpected(make_runtime_error(msg));
        }
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kBitAnd:
        if (auto applied = apply_binary_op(
                stack, "bit_and", [](const Value& lhs, const Value& rhs) -> Value { return runtime::BitAndBinary(lhs, rhs); });
            !applied) {
          return tl::unexpected(make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kBitOr:
        if (auto applied = apply_binary_op(
                stack, "bit_or", [](const Value& lhs, const Value& rhs) -> Value { return runtime::BitOrBinary(lhs, rhs); });
            !applied) {
          return tl::unexpected(make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kBitXor:
        if (auto applied = apply_binary_op(
                stack, "bit_xor", [](const Value& lhs, const Value& rhs) -> Value { return runtime::BitXorBinary(lhs, rhs); });
            !applied) {
          return tl::unexpected(make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kShiftLeft:
        if (auto applied = apply_binary_op(stack, "shift_left",
                                           [](const Value& lhs, const Value& rhs) -> Value {
                                             return runtime::BitShiftLeftBinary(lhs, rhs);
                                           });
            !applied) {
          return tl::unexpected(make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kShiftRight:
        if (auto applied = apply_binary_op(stack, "shift_right",
                                           [](const Value& lhs, const Value& rhs) -> Value {
                                             return runtime::BitShiftRightBinary(lhs, rhs);
                                           });
            !applied) {
          return tl::unexpected(make_runtime_error(applied.error()));
        }
        break;

      // The item reads and the stack mark sit at the end of the switch: placed next to kTakeLocal, the item reads made
      // other programs 4 to 7% slower, from the layout of run_loop alone.
      case bytecode::Opcode::kLoadLocalItem: {
        const Value* local = try_get_local(*frame, bytecode::local_item_slot(operand));
        const std::size_t position = bytecode::local_item_index(operand);
        if (local == nullptr || !local->is_tuple() || position >= local->tuple().size()) [[unlikely]] {
          return tl::unexpected(local_item_read_error(local));
        }
        stack.push_back(local->tuple()[position]);
        break;
      }

      case bytecode::Opcode::kTakeLocalItem: {
        Value* local = try_get_local(*frame, bytecode::local_item_slot(operand));
        const std::size_t position = bytecode::local_item_index(operand);
        if (local == nullptr || !local->is_tuple() || position >= local->tuple().size()) [[unlikely]] {
          return tl::unexpected(local_item_read_error(local));
        }
        if (local->object()->unique()) {
          stack.push_back(std::move(local->unshared_tuple_items()[position]));
        } else {
          stack.push_back(local->tuple()[position]);
        }
        *local = Value{};
        break;
      }

      case bytecode::Opcode::kMarkStack:
        get_or_create_local(*frame, static_cast<std::size_t>(operand)) = runtime::make_uint(stack.size());
        break;

      case bytecode::Opcode::kBuildTupleSinceMark: {
        const Value* mark = try_get_local(*frame, static_cast<std::size_t>(operand));
        if (mark == nullptr || !mark->is_uint() || runtime::to_index(mark->uint_value()) > stack.size()) [[unlikely]] {
          return tl::unexpected(make_runtime_error("build_tuple_since_mark expects a stack height in its slot"));
        }
        collapse_stack_tail_into_tuple(stack, stack.size() - runtime::to_index(mark->uint_value()));
        break;
      }

      case bytecode::Opcode::kMatchFailed:
        return tl::unexpected(make_runtime_error(std::string{runtime::k_match_no_case_message}));

      case bytecode::Opcode::kHalt:
        return LoopExit{std::monostate{}};
      default:
        return tl::unexpected(make_runtime_error(std::format("invalid opcode {}", static_cast<int>(opcode))));
    }
  }
}

auto dispatch_builtin(const BuiltinId builtin_id, const BuiltinCallShape /*shape*/, Value arg)
    -> tl::expected<Value, RuntimeError> {
  try {
    if (builtin_takes_arguments(builtin_id)) {
      // An argument tuple, built at run time (a spread) or by a call through a function value.
      if (const ArgumentsBuiltin builtin = arguments_builtin(builtin_id); builtin != nullptr) {
        return builtin(runtime::Arguments{std::move(arg)});
      }
      return builtin_constant(builtin_id);
    }
    switch (builtin_id) {
      case BuiltinId::UnaryPlus:
        return runtime::UnaryPlus(std::move(arg));
      case BuiltinId::UnaryMinus:
        return runtime::UnaryMinus(std::move(arg));
      case BuiltinId::BitNot:
        return runtime::BitNot(std::move(arg));
      case BuiltinId::Not:
        return runtime::Not(std::move(arg));
      case BuiltinId::Printf:
        return runtime::Printf(std::move(arg));
      case BuiltinId::Println:
        return runtime::Println(std::move(arg));
      case BuiltinId::Type:
        return runtime::Type(std::move(arg));
      case BuiltinId::InputString:
        return runtime::Input_String(std::move(arg));
      case BuiltinId::Help:
        return runtime::Help(std::move(arg));
      case BuiltinId::ExitInt64:
        return runtime::Exit_Int64(std::move(arg));
      case BuiltinId::OSEnv:
        return runtime::OSEnv(std::move(arg));
      case BuiltinId::OSHasEnv:
        return runtime::OSHasEnv(std::move(arg));
      case BuiltinId::OSUnsetEnv:
        return runtime::OSUnsetEnv(std::move(arg));
      case BuiltinId::OSExec:
        return runtime::OSExec(std::move(arg));
      case BuiltinId::PathNormalize:
        return runtime::PathNormalize(std::move(arg));
      case BuiltinId::PathBasename:
        return runtime::PathBasename(std::move(arg));
      case BuiltinId::PathDirname:
        return runtime::PathDirname(std::move(arg));
      case BuiltinId::PathExists:
        return runtime::PathExists(std::move(arg));
      case BuiltinId::PathIsFile:
        return runtime::PathIsFile(std::move(arg));
      case BuiltinId::PathIsDir:
        return runtime::PathIsDir(std::move(arg));
      case BuiltinId::PathAbsolute:
        return runtime::PathAbsolute(std::move(arg));
      case BuiltinId::PathExtension:
        return runtime::PathExtension(std::move(arg));
      case BuiltinId::PathStem:
        return runtime::PathStem(std::move(arg));
      case BuiltinId::FileReadText:
        return runtime::FileReadText(std::move(arg));
      case BuiltinId::FileReadLines:
        return runtime::FileReadLines(std::move(arg));
      case BuiltinId::FileDelete:
        return runtime::FileDelete(std::move(arg));
      case BuiltinId::FileSize:
        return runtime::FileSize(std::move(arg));
      case BuiltinId::FileReadLine:
        return runtime::FileReadLine(std::move(arg));
      case BuiltinId::FileFlush:
        return runtime::FileFlush(std::move(arg));
      case BuiltinId::FileClose:
        return runtime::FileClose(std::move(arg));
      case BuiltinId::DirCreate:
        return runtime::DirCreate(std::move(arg));
      case BuiltinId::DirDelete:
        return runtime::DirDelete(std::move(arg));
      case BuiltinId::DirList:
        return runtime::DirList(std::move(arg));
      case BuiltinId::DirListFull:
        return runtime::DirListFull(std::move(arg));
      case BuiltinId::TupleReverse:
        return runtime::TupleReverse(std::move(arg));
      case BuiltinId::TupleSort:
        return runtime::TupleSort(std::move(arg));
      case BuiltinId::TupleUnique:
        return runtime::TupleUnique(std::move(arg));
      case BuiltinId::TupleMin:
        return runtime::TupleMin(std::move(arg));
      case BuiltinId::TupleMax:
        return runtime::TupleMax(std::move(arg));
      case BuiltinId::TupleRangeInt64:
        return runtime::TupleRangeInt64(std::move(arg));
      case BuiltinId::ArrayTranspose2D:
        return runtime::ArrayTranspose2D(std::move(arg));
      case BuiltinId::ArrayRank:
        return runtime::ArrayRank(std::move(arg));
      case BuiltinId::ArrayShape:
        return runtime::ArrayShape(std::move(arg));
      case BuiltinId::ArrayFlatten:
        return runtime::ArrayFlatten(std::move(arg));
      case BuiltinId::DictCreateDict:
        return runtime::DictCreate_Dict(std::move(arg));
      case BuiltinId::DictKeys:
        return runtime::DictKeys(std::move(arg));
      case BuiltinId::DictValues:
        return runtime::DictValues(std::move(arg));
      case BuiltinId::DictEntries:
        return runtime::DictEntries(std::move(arg));
      case BuiltinId::DictClear:
        return runtime::DictClear(std::move(arg));
      case BuiltinId::DictLength:
        return runtime::DictLength(std::move(arg));
      case BuiltinId::Cast:
        return runtime::Cast(std::move(arg));
      case BuiltinId::TryCast:
        return runtime::TryCast(std::move(arg));
      case BuiltinId::ToInt64:
        return runtime::ToInt64(std::move(arg));
      case BuiltinId::ToUInt64:
        return runtime::ToUInt64(std::move(arg));
      case BuiltinId::ToFloat64:
        return runtime::ToFloat64(std::move(arg));
      case BuiltinId::RandomCreate:
        return runtime::RandomCreate(std::move(arg));
      case BuiltinId::RandomNextUInt64:
        return runtime::RandomNextUInt64(std::move(arg));
      case BuiltinId::RandomNextInt64:
        return runtime::RandomNextInt64(std::move(arg));
      case BuiltinId::RandomNextFloat64:
        return runtime::RandomNextFloat64(std::move(arg));
      case BuiltinId::RandomNextBool:
        return runtime::RandomNextBool(std::move(arg));
      case BuiltinId::RandomSplit:
        return runtime::RandomSplit(std::move(arg));
      case BuiltinId::MathFloor:
        return runtime::MathFloor(std::move(arg));
      case BuiltinId::MathCeil:
        return runtime::MathCeil(std::move(arg));
      case BuiltinId::MathAbs:
        return runtime::MathAbs(std::move(arg));
      case BuiltinId::MathLog:
        return runtime::MathLog(std::move(arg));
      case BuiltinId::Sqrt:
        return runtime::Sqrt(std::move(arg));
      case BuiltinId::Sin:
        return runtime::Sin(std::move(arg));
      case BuiltinId::Cos:
        return runtime::Cos(std::move(arg));
      case BuiltinId::Tan:
        return runtime::Tan(std::move(arg));
      case BuiltinId::ResultOk:
        return runtime::ResultOk(std::move(arg));
      case BuiltinId::ResultErr:
        return runtime::ResultErr(std::move(arg));
      case BuiltinId::ResultTag:
        return runtime::ResultTag(std::move(arg));
      case BuiltinId::ResultPayload:
        return runtime::ResultPayload(std::move(arg));
      case BuiltinId::ResultIsOk:
        return runtime::ResultIsOk(std::move(arg));
      case BuiltinId::ResultIsErr:
        return runtime::ResultIsErr(std::move(arg));
      case BuiltinId::ResultUnwrap:
        return runtime::ResultUnwrap(std::move(arg));
      case BuiltinId::ResultUnwrapErr:
        return runtime::ResultUnwrapErr(std::move(arg));
      case BuiltinId::TaskAwait:
        return runtime::TaskAwait(std::move(arg));
      case BuiltinId::TaskAwaitAll:
        return runtime::TaskAwaitAll(std::move(arg));
      case BuiltinId::TaskCancel:
        return runtime::TaskCancel(std::move(arg));
      case BuiltinId::Wrap:
        return runtime::Wrap(std::move(arg));
      case BuiltinId::Unwrap:
        return runtime::Unwrap(std::move(arg));
      case BuiltinId::First:
        return runtime::First(std::move(arg));
      case BuiltinId::Second:
        return runtime::Second(std::move(arg));
      case BuiltinId::Length:
        return runtime::Length(std::move(arg));
      case BuiltinId::ToString:
        return runtime::ToString(std::move(arg));
      case BuiltinId::ToNum:
        return runtime::ToNum(std::move(arg));
      case BuiltinId::StringParseInt64:
        return runtime::StringParseInt64(std::move(arg));
      case BuiltinId::StringParseUInt64:
        return runtime::StringParseUInt64(std::move(arg));
      case BuiltinId::StringParseFloat64:
        return runtime::StringParseFloat64(std::move(arg));
      case BuiltinId::StringUpper:
        return runtime::StringUpper(std::move(arg));
      case BuiltinId::StringLower:
        return runtime::StringLower(std::move(arg));
      case BuiltinId::StringTrim:
        return runtime::StringTrim(std::move(arg));
      case BuiltinId::StringTrimStart:
        return runtime::StringTrimStart(std::move(arg));
      case BuiltinId::StringTrimEnd:
        return runtime::StringTrimEnd(std::move(arg));
      case BuiltinId::StringLength:
        return runtime::StringLength(std::move(arg));
      default:
        return tl::unexpected(make_runtime_error("unknown builtin id"));
    }
  } catch (const std::exception& ex) {
    return tl::unexpected(
        make_runtime_error(std::string("builtin '") + std::string(builtin_name(builtin_id)) + "' threw: " + ex.what()));
  }
}

// The builtins that take runtime::Arguments (vm::builtin_takes_arguments), by BuiltinId; the constants have none. The
// call is one indirect call, and the check below makes a builtin that takes Arguments without an entry a compile
// error.
constexpr auto kArgumentsBuiltins = []() {
  std::array<ArgumentsBuiltin, builtin_count()> table{};
  table[static_cast<std::size_t>(BuiltinId::Add)] = &runtime::Add;
  table[static_cast<std::size_t>(BuiltinId::Subtract)] = &runtime::Subtract;
  table[static_cast<std::size_t>(BuiltinId::Multiply)] = &runtime::Multiply;
  table[static_cast<std::size_t>(BuiltinId::WrappingAdd)] = &runtime::WrappingAdd;
  table[static_cast<std::size_t>(BuiltinId::WrappingSubtract)] = &runtime::WrappingSubtract;
  table[static_cast<std::size_t>(BuiltinId::WrappingMultiply)] = &runtime::WrappingMultiply;
  table[static_cast<std::size_t>(BuiltinId::SaturatingAdd)] = &runtime::SaturatingAdd;
  table[static_cast<std::size_t>(BuiltinId::SaturatingSubtract)] = &runtime::SaturatingSubtract;
  table[static_cast<std::size_t>(BuiltinId::SaturatingMultiply)] = &runtime::SaturatingMultiply;
  table[static_cast<std::size_t>(BuiltinId::Divide)] = &runtime::Divide;
  table[static_cast<std::size_t>(BuiltinId::Mod)] = &runtime::Mod;
  table[static_cast<std::size_t>(BuiltinId::Pow)] = &runtime::Pow;
  table[static_cast<std::size_t>(BuiltinId::BitAnd)] = &runtime::BitAnd;
  table[static_cast<std::size_t>(BuiltinId::BitOr)] = &runtime::BitOr;
  table[static_cast<std::size_t>(BuiltinId::BitXor)] = &runtime::BitXor;
  table[static_cast<std::size_t>(BuiltinId::BitShiftLeft)] = &runtime::BitShiftLeft;
  table[static_cast<std::size_t>(BuiltinId::BitShiftRight)] = &runtime::BitShiftRight;
  table[static_cast<std::size_t>(BuiltinId::Equal)] = &runtime::Equal;
  table[static_cast<std::size_t>(BuiltinId::NotEqual)] = &runtime::NotEqual;
  table[static_cast<std::size_t>(BuiltinId::LessThan)] = &runtime::LessThan;
  table[static_cast<std::size_t>(BuiltinId::GreaterThan)] = &runtime::GreaterThan;
  table[static_cast<std::size_t>(BuiltinId::GreaterOrEqual)] = &runtime::GreaterOrEqual;
  table[static_cast<std::size_t>(BuiltinId::LessOrEqual)] = &runtime::LessOrEqual;
  table[static_cast<std::size_t>(BuiltinId::And)] = &runtime::And;
  table[static_cast<std::size_t>(BuiltinId::Or)] = &runtime::Or;
  table[static_cast<std::size_t>(BuiltinId::Select)] = &runtime::Select;
  table[static_cast<std::size_t>(BuiltinId::Match)] = &runtime::Match;
  table[static_cast<std::size_t>(BuiltinId::Apply)] = &runtime::Apply;
  table[static_cast<std::size_t>(BuiltinId::Branch)] = &runtime::Branch;
  table[static_cast<std::size_t>(BuiltinId::Loop)] = &runtime::Loop;
  table[static_cast<std::size_t>(BuiltinId::LoopN)] = &runtime::LoopN;
  table[static_cast<std::size_t>(BuiltinId::GetArgs)] = &runtime::GetArgs;
  table[static_cast<std::size_t>(BuiltinId::InputVoid)] = &runtime::Input_Void;
  table[static_cast<std::size_t>(BuiltinId::ExitVoid)] = &runtime::Exit_Void;
  table[static_cast<std::size_t>(BuiltinId::Cwd)] = &runtime::Cwd;
  table[static_cast<std::size_t>(BuiltinId::OSSetEnv)] = &runtime::OSSetEnv;
  table[static_cast<std::size_t>(BuiltinId::OSIsWindows)] = &runtime::OSIsWindows;
  table[static_cast<std::size_t>(BuiltinId::OSIsLinux)] = &runtime::OSIsLinux;
  table[static_cast<std::size_t>(BuiltinId::OSIsMacOS)] = &runtime::OSIsMacOS;
  table[static_cast<std::size_t>(BuiltinId::OSHome)] = &runtime::OSHome;
  table[static_cast<std::size_t>(BuiltinId::OSTempDir)] = &runtime::OSTempDir;
  table[static_cast<std::size_t>(BuiltinId::OSMakeTempFile)] = &runtime::OSMakeTempFile;
  table[static_cast<std::size_t>(BuiltinId::OSMakeTempDir)] = &runtime::OSMakeTempDir;
  table[static_cast<std::size_t>(BuiltinId::PathJoin)] = &runtime::PathJoin;
  table[static_cast<std::size_t>(BuiltinId::PathWithExtension)] = &runtime::PathWithExtension;
  table[static_cast<std::size_t>(BuiltinId::PathWithBasename)] = &runtime::PathWithBasename;
  table[static_cast<std::size_t>(BuiltinId::FileWriteText)] = &runtime::FileWriteText;
  table[static_cast<std::size_t>(BuiltinId::FileAppendText)] = &runtime::FileAppendText;
  table[static_cast<std::size_t>(BuiltinId::FileOpen)] = &runtime::FileOpen;
  table[static_cast<std::size_t>(BuiltinId::FileReadChunk)] = &runtime::FileReadChunk;
  table[static_cast<std::size_t>(BuiltinId::FileWriteChunk)] = &runtime::FileWriteChunk;
  table[static_cast<std::size_t>(BuiltinId::FileWithOpen)] = &runtime::FileWithOpen;
  table[static_cast<std::size_t>(BuiltinId::TupleAppend)] = &runtime::TupleAppend;
  table[static_cast<std::size_t>(BuiltinId::TuplePrepend)] = &runtime::TuplePrepend;
  table[static_cast<std::size_t>(BuiltinId::TupleContains)] = &runtime::TupleContains;
  table[static_cast<std::size_t>(BuiltinId::TupleZip)] = &runtime::TupleZip;
  table[static_cast<std::size_t>(BuiltinId::TupleMap)] = &runtime::TupleMap;
  table[static_cast<std::size_t>(BuiltinId::TupleFilter)] = &runtime::TupleFilter;
  table[static_cast<std::size_t>(BuiltinId::TupleReduce)] = &runtime::TupleReduce;
  table[static_cast<std::size_t>(BuiltinId::TupleFindIndex)] = &runtime::TupleFindIndex;
  table[static_cast<std::size_t>(BuiltinId::TupleAny)] = &runtime::TupleAny;
  table[static_cast<std::size_t>(BuiltinId::TupleAll)] = &runtime::TupleAll;
  table[static_cast<std::size_t>(BuiltinId::TupleRangeInt64Int64)] = &runtime::TupleRangeInt64Int64;
  table[static_cast<std::size_t>(BuiltinId::TupleRangeInt64Int64Int64)] = &runtime::TupleRangeInt64Int64Int64;
  table[static_cast<std::size_t>(BuiltinId::ArrayGetAt)] = &runtime::ArrayGetAt;
  table[static_cast<std::size_t>(BuiltinId::ArraySetAt)] = &runtime::ArraySetAt;
  table[static_cast<std::size_t>(BuiltinId::ArrayInsertAt)] = &runtime::ArrayInsertAt;
  table[static_cast<std::size_t>(BuiltinId::ArrayRemoveAt)] = &runtime::ArrayRemoveAt;
  table[static_cast<std::size_t>(BuiltinId::ArraySlice)] = &runtime::ArraySlice;
  table[static_cast<std::size_t>(BuiltinId::ArrayConcat)] = &runtime::ArrayConcat;
  table[static_cast<std::size_t>(BuiltinId::ArraySetAt2D)] = &runtime::ArraySetAt2D;
  table[static_cast<std::size_t>(BuiltinId::ArrayFill)] = &runtime::ArrayFill;
  table[static_cast<std::size_t>(BuiltinId::ArraySlice2D)] = &runtime::ArraySlice2D;
  table[static_cast<std::size_t>(BuiltinId::ArrayReshape)] = &runtime::ArrayReshape;
  table[static_cast<std::size_t>(BuiltinId::ArrayGetAtND)] = &runtime::ArrayGetAtND;
  table[static_cast<std::size_t>(BuiltinId::ArraySetAtND)] = &runtime::ArraySetAtND;
  table[static_cast<std::size_t>(BuiltinId::ArrayReshapeND)] = &runtime::ArrayReshapeND;
  table[static_cast<std::size_t>(BuiltinId::DictCreateVoid)] = &runtime::DictCreate_Void;
  table[static_cast<std::size_t>(BuiltinId::DictSet)] = &runtime::DictSet;
  table[static_cast<std::size_t>(BuiltinId::DictGet)] = &runtime::DictGet;
  table[static_cast<std::size_t>(BuiltinId::DictGetDefault)] = &runtime::DictGetDefault;
  table[static_cast<std::size_t>(BuiltinId::DictContains)] = &runtime::DictContains;
  table[static_cast<std::size_t>(BuiltinId::DictDelete)] = &runtime::DictDelete;
  table[static_cast<std::size_t>(BuiltinId::DictMerge)] = &runtime::DictMerge;
  table[static_cast<std::size_t>(BuiltinId::MathClamp)] = &runtime::MathClamp;
  table[static_cast<std::size_t>(BuiltinId::Try)] = &runtime::Try;
  table[static_cast<std::size_t>(BuiltinId::ParallelMap)] = &runtime::ParallelMap;
  table[static_cast<std::size_t>(BuiltinId::ParallelWithOptions)] = &runtime::ParallelWithOptions;
  table[static_cast<std::size_t>(BuiltinId::ParallelForEach)] = &runtime::ParallelForEach;
  table[static_cast<std::size_t>(BuiltinId::ParallelReduce)] = &runtime::ParallelReduce;
  table[static_cast<std::size_t>(BuiltinId::TaskSpawn)] = &runtime::TaskSpawn;
  table[static_cast<std::size_t>(BuiltinId::TaskWithTimeout)] = &runtime::TaskWithTimeout;
  table[static_cast<std::size_t>(BuiltinId::ElementAt)] = &runtime::ElementAt;
  table[static_cast<std::size_t>(BuiltinId::Take)] = &runtime::Take;
  table[static_cast<std::size_t>(BuiltinId::Drop)] = &runtime::Drop;
  table[static_cast<std::size_t>(BuiltinId::SliceTupleUInt64)] = &runtime::SliceTupleUInt64;
  table[static_cast<std::size_t>(BuiltinId::SliceTupleUInt64UInt64)] = &runtime::SliceTupleUInt64UInt64;
  table[static_cast<std::size_t>(BuiltinId::SliceTupleUInt64UInt64UInt64)] = &runtime::SliceTupleUInt64UInt64UInt64;
  table[static_cast<std::size_t>(BuiltinId::StringSplit)] = &runtime::StringSplit;
  table[static_cast<std::size_t>(BuiltinId::StringJoin)] = &runtime::StringJoin;
  table[static_cast<std::size_t>(BuiltinId::StringReplace)] = &runtime::StringReplace;
  table[static_cast<std::size_t>(BuiltinId::StringContains)] = &runtime::StringContains;
  table[static_cast<std::size_t>(BuiltinId::StringStartsWith)] = &runtime::StringStartsWith;
  table[static_cast<std::size_t>(BuiltinId::StringEndsWith)] = &runtime::StringEndsWith;
  table[static_cast<std::size_t>(BuiltinId::StringCharAt)] = &runtime::StringCharAt;
  table[static_cast<std::size_t>(BuiltinId::StringSliceStringUInt64)] = &runtime::StringSliceStringUInt64;
  table[static_cast<std::size_t>(BuiltinId::StringSliceStringUInt64UInt64)] = &runtime::StringSliceStringUInt64UInt64;
  table[static_cast<std::size_t>(BuiltinId::StringFindStringString)] = &runtime::StringFindStringString;
  table[static_cast<std::size_t>(BuiltinId::StringFindStringStringUInt64)] = &runtime::StringFindStringStringUInt64;
  table[static_cast<std::size_t>(BuiltinId::StringFormat)] = &runtime::StringFormat;
  table[static_cast<std::size_t>(BuiltinId::StringRegexIsMatch)] = &runtime::StringRegexIsMatch;
  table[static_cast<std::size_t>(BuiltinId::StringRegexFind)] = &runtime::StringRegexFind;
  table[static_cast<std::size_t>(BuiltinId::StringRegexReplace)] = &runtime::StringRegexReplace;
  table[static_cast<std::size_t>(BuiltinId::StringRegexSplit)] = &runtime::StringRegexSplit;
  return table;
}();

[[nodiscard]] consteval auto arguments_builtins_are_complete() -> bool {
  for (std::size_t index = 0; index < builtin_count(); ++index) {
    const auto builtin_id = static_cast<BuiltinId>(index);
    const bool constant = std::ranges::any_of(all_constant_builtin_specs(), [builtin_id](const auto& spec) {
      return spec.id == builtin_id;
    });
    if (builtin_takes_arguments(builtin_id) && !constant && kArgumentsBuiltins[index] == nullptr) {
      return false;
    }
  }
  return true;
}
static_assert(arguments_builtins_are_complete(), "every builtin that takes runtime::Arguments has an entry");

// The function of a builtin that takes runtime::Arguments, or null for a constant. Both dispatches call it
// themselves.
auto arguments_builtin(const BuiltinId builtin_id) -> ArgumentsBuiltin {
  return kArgumentsBuiltins[static_cast<std::size_t>(builtin_id)];
}

// The value of a constant (Std.Pi, ...), which takes no arguments.
auto builtin_constant(const BuiltinId builtin_id) -> Value {
  if (const auto constant_value = constant_builtin_value(builtin_id); constant_value.has_value()) {
    return runtime::make_float(*constant_value);
  }
  throw std::logic_error("builtin does not take Arguments");
}

// Calls a builtin that takes runtime::Arguments with `values`, which the call owns, as kCallBuiltinArgs does. The
// arguments are made here rather than by the caller, so that the code to release what they hold stays out of the VM
// loop.
auto dispatch_builtin_args(const BuiltinId builtin_id, const std::span<Value> values)
    -> tl::expected<Value, RuntimeError> {
  if (!builtin_takes_arguments(builtin_id)) {
    return tl::unexpected(make_runtime_error(std::string("builtin '") + std::string(builtin_name(builtin_id)) +
                                             "' does not take its arguments from the stack"));
  }
  try {
    if (const ArgumentsBuiltin builtin = arguments_builtin(builtin_id); builtin != nullptr) {
      return builtin(runtime::Arguments{values});
    }
    return builtin_constant(builtin_id);
  } catch (const std::exception& ex) {
    return tl::unexpected(
        make_runtime_error(std::string("builtin '") + std::string(builtin_name(builtin_id)) + "' threw: " + ex.what()));
  }
}

}  // namespace

struct RuntimeSession::Impl {
  explicit Impl(const std::vector<std::string>& initial_process_args,
                const RuntimeCompileOptions& session_compile_options)
      : compile_options(session_compile_options), process_args(initial_process_args),
        source_path((std::filesystem::current_path() / "__repl__.fleaux").lexically_normal()) {}

  RuntimeCompileOptions compile_options;
  std::vector<std::string> process_args;
  std::filesystem::path source_path;
  std::vector<frontend::ir::IRLet> lets;
  std::vector<frontend::ir::IRTypeDecl> type_decls;
  std::vector<frontend::ir::IRAliasDecl> alias_decls;
};

RuntimeSession::RuntimeSession(const std::vector<std::string>& process_args,
                               const RuntimeCompileOptions& compile_options)
    : impl_(std::in_place, process_args, compile_options) {}

RuntimeSession::RuntimeSession(const RuntimeSession& other) = default;

RuntimeSession::RuntimeSession(RuntimeSession&& other) noexcept = default;

auto RuntimeSession::operator=(const RuntimeSession& other) -> RuntimeSession& = default;

auto RuntimeSession::operator=(RuntimeSession&& other) noexcept -> RuntimeSession& = default;

RuntimeSession::~RuntimeSession() = default;

auto RuntimeSession::run_snippet(const std::string& snippet_text, std::ostream& output) const -> RuntimeResult {
  return run_snippet(snippet_text, output, std::cin);
}

auto RuntimeSession::run_snippet(const std::string& snippet_text, std::ostream& output, std::istream& input) const
    -> RuntimeResult {
  auto analyzed = detail::parse_and_analyze_repl_text(snippet_text, impl_->source_path, impl_->lets, impl_->type_decls,
                                                      impl_->alias_decls);
  if (!analyzed) {
    return tl::unexpected(make_runtime_error(analyzed.error().message, analyzed.error().hint, analyzed.error().span));
  }

  auto merged_lets = detail::merge_repl_session_lets(impl_->lets, analyzed->lets, impl_->source_path);
  auto merged_type_decls =
      detail::merge_repl_session_type_decls(impl_->type_decls, analyzed->type_decls, impl_->source_path);
  auto merged_alias_decls =
      detail::merge_repl_session_alias_decls(impl_->alias_decls, analyzed->alias_decls, impl_->source_path);
  auto program_to_execute = analyzed.value();
  program_to_execute.lets = merged_lets;
  program_to_execute.type_decls = merged_type_decls;
  program_to_execute.alias_decls = merged_alias_decls;

  constexpr bytecode::BytecodeCompiler compiler;
  auto compiled = compiler.compile(
      program_to_execute,
      bytecode::CompileOptions{
          .source_path = impl_->source_path,
          .source_text = snippet_text,
          .module_name = std::string{"repl"},
          .imported_modules = {},
          .enable_experimental_builtin_reductions = impl_->compile_options.enable_experimental_builtin_reductions,
      });
  if (!compiled) {
    return tl::unexpected(
        make_runtime_error(compiled.error().message, "This REPL snippet is not yet supported by the VM compiler."));
  }

  impl_->lets = std::move(merged_lets);
  impl_->type_decls = std::move(merged_type_decls);
  impl_->alias_decls = std::move(merged_alias_decls);

  const Runtime runtime{impl_->process_args};
  return runtime.execute(*compiled, RuntimeInvocationOptions{
                                        .entry_label = "<repl>", .input = std::ref(input), .output = std::ref(output)});
}

// Runtime::execute

Runtime::Runtime(std::vector<std::string> process_args) : process_args_(std::move(process_args)) {}

auto Runtime::create_session(const std::vector<std::string>& process_args,
                             const RuntimeCompileOptions& compile_options) const -> RuntimeSession {
  return RuntimeSession(process_args, compile_options);
}

auto Runtime::execute(const bytecode::Module& bytecode_module) const -> RuntimeResult {
  return execute(bytecode_module, RuntimeInvocationOptions{});
}

auto Runtime::execute(const bytecode::Module& bytecode_module, const RuntimeInvocationOptions& options) const
    -> RuntimeResult {
  auto& output = resolve_runtime_output_stream(options.output);

  // Before the task scope: tasks the program leaves running finish before the running module goes away.
  const RunningModule running_module(bytecode_module);
  runtime::RuntimeExecutionState execution_state;
  runtime::ActiveRuntimeExecutionStateScope execution_state_scope(execution_state);
  runtime::TaskScope task_scope;
  detail::StdHelpMetadataScope help_scope;
  runtime::RuntimeOutputStreamScope output_scope(output);
  runtime::RuntimeInputStreamScope input_scope(resolve_runtime_input_stream(options.input));
  runtime::RuntimeProcessArgsScope process_args_scope(
      make_process_args_for_invocation(process_args_, options.entry_label));
  if (auto bound = bind_native_functions(bytecode_module, options, execution_state); !bound) {
    return tl::unexpected(bound.error());
  }

  // The thread's stacks, which keep their capacity from one run to the next.
  const ScopedInvocationScratch scratch{suggested_operand_stack_capacity(bytecode_module.instructions.size()),
                                        suggested_frame_stack_capacity(bytecode_module.functions.size())};
  (void)scratch.frames().push(&bytecode_module.instructions);

  auto loop_result = run_loop(running_module, scratch.stack(), scratch.frames(), output);
  if (!loop_result)
    return tl::unexpected(loop_result.error());

  if (std::get_if<Value>(&*loop_result) != nullptr) {
    // A top-level kReturn would be a compiler bug.
    return tl::unexpected(make_runtime_error("top-level code returned a value instead of halting"));
  }
  return ExecutionResult{0};
}

// A host call to a function with hidden operator parameters: the host entry check found which alternative each
// bounded type parameter takes, and this passes the operator functions recorded for those alternatives in front of
// the host's arguments.
auto prepend_host_operator_args(const RunningModule& running_module, const bytecode::FunctionDef& function,
                                const bytecode::ExportedSymbol& exported, Value arg, std::ostream& output)
    -> tl::expected<Value, RuntimeError> {
  const auto& bytecode_module = running_module.bytecode();
  if (!exported.host_entry_check.has_value()) {
    return tl::unexpected(make_runtime_error("'" + exported.name + "' has no host entry check for its operators"));
  }
  const auto& entry = *exported.host_entry_check;
  std::vector<std::size_t> chosen;
  if (auto mismatch = runtime::check_host_argument(arg, entry, exported.name, &chosen)) {
    return tl::unexpected(make_runtime_error(*mismatch));
  }

  runtime::TupleBuilder arguments(function.operator_requirements.size() + (arg.is_tuple() ? arg.tuple().size() : 1U));
  for (const auto& requirement : function.operator_requirements) {
    const auto param_it = std::ranges::find(entry.type_params, requirement.type_param);
    const auto param_index = static_cast<std::size_t>(std::distance(entry.type_params.begin(), param_it));
    const auto alternative = param_index < chosen.size() ? chosen[param_index] : 0U;
    if (alternative >= requirement.alternative_functions.size()) {
      return tl::unexpected(make_runtime_error("'" + exported.name + "' has no function for '" + requirement.op +
                                               "' on " + requirement.type_param));
    }
    const auto& key = requirement.alternative_functions[alternative];
    if (const auto builtin_id = builtin_id_from_symbol_key(key); builtin_id.has_value()) {
      arguments.push(make_builtin_function_value(*builtin_id, builtin_callable_call_shape(*builtin_id)));
      continue;
    }
    const auto function_it = std::ranges::find_if(
        bytecode_module.functions, [&](const bytecode::FunctionDef& candidate) { return candidate.name == key; });
    if (function_it == bytecode_module.functions.end()) {
      return tl::unexpected(make_runtime_error("Operator function '" + key + "' is not in the module"));
    }
    arguments.push(make_function_value(running_module, *function_it, output));
  }

  // The declared arguments follow, laid out as the VM binds them.
  const auto declared = function.arity - function.hidden_operator_params;
  if (declared == 1U && !function.has_variadic_tail) {
    arguments.push(std::move(arg));
  } else if (declared > 0U) {
    if (arg.is_tuple()) {
      for (const Value& item : arg.tuple()) {
        arguments.push(item);
      }
    } else {
      arguments.push(std::move(arg));
    }
  }
  return std::move(arguments).finish();
}

auto Runtime::invoke_symbol(const bytecode::Module& bytecode_module, const std::string_view qualified_symbol,
                            runtime::Value arg) const -> RuntimeValueResult {
  return invoke_symbol(bytecode_module, qualified_symbol, std::move(arg),
                       RuntimeInvocationOptions{.entry_label = std::string{qualified_symbol}});
}

auto Runtime::invoke_symbol(const bytecode::Module& bytecode_module, const std::string_view qualified_symbol,
                            runtime::Value arg, const RuntimeInvocationOptions& options) const -> RuntimeValueResult {
  auto& output = resolve_runtime_output_stream(options.output);

  // Before the task scope: tasks the call leaves running finish before the running module goes away.
  const RunningModule running_module(bytecode_module);
  runtime::RuntimeExecutionState execution_state;
  runtime::ActiveRuntimeExecutionStateScope execution_state_scope(execution_state);
  runtime::TaskScope task_scope;
  detail::StdHelpMetadataScope help_scope;
  runtime::RuntimeOutputStreamScope output_scope(output);
  runtime::RuntimeInputStreamScope input_scope(resolve_runtime_input_stream(options.input));
  runtime::RuntimeProcessArgsScope process_args_scope(make_process_args_for_invocation(
      process_args_, options.entry_label.empty() ? qualified_symbol : options.entry_label));
  if (auto bound = bind_native_functions(bytecode_module, options, execution_state); !bound) {
    return tl::unexpected(bound.error());
  }

  const auto* exported_symbol = find_exported_symbol(bytecode_module, qualified_symbol);
  if (exported_symbol == nullptr) {
    return tl::unexpected(make_runtime_error("Unresolved exported symbol: '" + std::string{qualified_symbol} + "'."));
  }

  // The host's argument has not been type-checked: check it against the declared parameter types before any Fleaux
  // code or builtin sees it. Code past this point relies on those types.
  if (exported_symbol->host_entry_check.has_value()) {
    if (auto mismatch = runtime::check_host_argument(arg, *exported_symbol->host_entry_check, qualified_symbol)) {
      return tl::unexpected(make_runtime_error(*mismatch));
    }
  }

  if (exported_symbol->kind == bytecode::ExportKind::kBuiltinAlias) {
    const auto builtin_name =
        exported_symbol->builtin_name.empty() ? exported_symbol->name : exported_symbol->builtin_name;
    const auto builtin_id = builtin_id_from_symbol_key(builtin_name);
    if (!builtin_id.has_value()) {
      return tl::unexpected(make_runtime_error("Unknown builtin alias symbol: '" + builtin_name + "'."));
    }
    auto dispatched = dispatch_builtin(*builtin_id, builtin_callable_call_shape(*builtin_id), std::move(arg));
    if (!dispatched) {
      return tl::unexpected(dispatched.error());
    }
    return host_result(qualified_symbol, std::move(*dispatched));
  }

  const auto function_index = static_cast<std::size_t>(exported_symbol->index);
  if (function_index < bytecode_module.functions.size() &&
      bytecode_module.functions[function_index].hidden_operator_params > 0U) {
    auto prepared = prepend_host_operator_args(running_module, bytecode_module.functions[function_index],
                                               *exported_symbol, std::move(arg), output);
    if (!prepared) {
      return tl::unexpected(prepared.error());
    }
    arg = std::move(*prepared);
  }
  auto called = run_user_function(running_module, function_index, std::move(arg), output);
  if (!called) {
    return tl::unexpected(called.error());
  }
  return host_result(qualified_symbol, std::move(*called));
}

auto Runtime::invoke_symbol(const bytecode::Module& bytecode_module, const std::string_view qualified_symbol,
                            runtime::Value arg, std::ostream& output) const -> RuntimeValueResult {
  return invoke_symbol(
      bytecode_module, qualified_symbol, std::move(arg),
      RuntimeInvocationOptions{.entry_label = std::string{qualified_symbol}, .output = std::ref(output)});
}

auto Runtime::invoke_symbol(const bytecode::Module& bytecode_module, const std::string_view qualified_symbol,
                            runtime::Value arg, std::ostream& output, const RuntimeInvocationOptions& options) const
    -> RuntimeValueResult {
  auto merged_options = options;
  merged_options.output = std::ref(output);
  return invoke_symbol(bytecode_module, qualified_symbol, std::move(arg), merged_options);
}

auto Runtime::execute(const bytecode::Module& bytecode_module, std::ostream& output) const -> RuntimeResult {
  return execute(bytecode_module, RuntimeInvocationOptions{.output = std::ref(output)});
}

auto Runtime::execute(const bytecode::Module& bytecode_module, std::ostream& output,
                      const RuntimeInvocationOptions& options) const -> RuntimeResult {
  auto merged_options = options;
  merged_options.output = std::ref(output);
  return execute(bytecode_module, merged_options);
}

}  // namespace fleaux::vm
