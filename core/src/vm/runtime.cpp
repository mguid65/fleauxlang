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

// A failure already reported, thrown through C++ code such as a builtin that calls a function value: the error the
// function value's run returned, with the calls it failed in, or the error of a builtin a function value stands for.
// Whatever catches it reports the error as it is, rather than as a failure of its own.
class ReportedFailure final : public std::runtime_error {
public:
  explicit ReportedFailure(RuntimeError error) : std::runtime_error(error.message), error_(std::move(error)) {}

  [[nodiscard]] auto error() const -> const RuntimeError& { return error_; }

private:
  RuntimeError error_;
};

// The error of `name` (a builtin, an opcode's Std function, or an extension function) when it threw `ex`: a reported
// failure as it is, and any other exception as `name: what`.
// A failure the program reports itself (Std.Panic) has its own message alone.
auto error_from_exception(const std::exception& ex, const std::string_view name) -> RuntimeError {
  if (const auto* reported = dynamic_cast<const ReportedFailure*>(&ex); reported != nullptr) {
    return reported->error();
  }
  if (dynamic_cast<const runtime::ProgramFailure*>(&ex) != nullptr) {
    return make_runtime_error(ex.what());
  }
  return make_runtime_error(std::format("{}: {}", name, ex.what()));
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

// Runs an opcode's operation that can call function values; `name` is the Std function the opcode stands for.
template <typename Fn>
auto run_native_op(const std::string_view name, Fn&& fn) -> tl::expected<Value, RuntimeError> {
  try {
    return fn();
  } catch (const std::exception& ex) {
    return tl::unexpected(error_from_exception(ex, name));
  }
}

// Why a kLoadLocalItem or kTakeLocalItem of `local` failed, with kLoadLocal's message for a slot past the frame's
// locals and kElementAt's otherwise.
auto local_item_read_error(const Value* local, const std::uint32_t item) -> RuntimeError {
  if (local == nullptr) {
    return make_runtime_error("local slot index out of range");
  }
  if (!local->is_tuple()) {
    return make_runtime_error(std::format("Std.ElementAt: {}", runtime::kind_error("Tuple").what()));
  }
  return make_runtime_error(std::format("Std.ElementAt: {}", runtime::index_out_of_range_message(item, local->tuple().size())));
}

// The helpers below run for every arithmetic, comparison, and logic opcode, so they fail with a message rather than a
// RuntimeError: GCC zero-fills all 128 bytes of a tl::expected<void, RuntimeError> on every success, which costs more
// than an addition. The loop makes the RuntimeError when one fails.

// Replaces the top two stack values, the left operand below the right, with op(left, right). The operands stay in
// their slots while op reads them, and the result goes into the left one's.
// `name` is the Std function the opcode stands for.
template <typename Op>
auto apply_binary_op(std::vector<Value>& stack, const char* name, Op&& op) -> tl::expected<void, std::string> {
  if (stack.size() < 2U) {
    return tl::unexpected(stack_underflow(name).message);
  }
  Value& lhs = stack[stack.size() - 2U];
  try {
    lhs = std::forward<Op>(op)(lhs, stack.back());
  } catch (const std::exception& ex) {
    return tl::unexpected(std::format("{}: {}", name, ex.what()));
  }
  stack.pop_back();
  return {};
}

// Replaces the top stack value with op(value), in its slot.
template <typename Op>
auto apply_unary_op(std::vector<Value>& stack, const char* name, Op&& op) -> tl::expected<void, std::string> {
  if (stack.empty()) {
    return tl::unexpected(stack_underflow(name).message);
  }
  Value& value = stack.back();
  try {
    value = std::forward<Op>(op)(value);
  } catch (const std::exception& ex) {
    return tl::unexpected(std::format("{}: {}", name, ex.what()));
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
    return tl::unexpected(error_from_exception(ex, record.symbol));
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

// The calls of a failed run

// A function's name as a chain of calls shows it: without its overload number, and "a closure" for a closure.
auto display_function_name(const std::string_view name) -> std::string {
  if (name.starts_with("__closure_")) {
    return "a closure";
  }
  return std::string{name.substr(0, name.find('#'))};
}

// The Std function or extension function that an instruction calls, when it can call a function value back from C++,
// and empty otherwise.
auto calling_back_name(const bytecode::Module& bytecode_module, const bytecode::Instruction& instruction) -> std::string {
  switch (instruction.opcode) {
    case bytecode::Opcode::kCallBuiltin:
    case bytecode::Opcode::kCallBuiltinArgs:
      if (const auto builtin_id = builtin_id_from_operand(instruction.operand); builtin_id.has_value()) {
        return std::string{builtin_name(*builtin_id)};
      }
      return {};
    case bytecode::Opcode::kCallNative:
      if (const auto index = static_cast<std::size_t>(instruction.operand);
          instruction.operand >= 0 && index < bytecode_module.native_functions.size()) {
        return bytecode_module.native_functions[index].symbol;
      }
      return {};
    default:
      return {};
  }
}

// Appends the calls of the frames on the stack to `calls`, innermost first. Each frame's position is one past the
// instruction it was running or calling; a location inlined into a call expands into the call too. A call names the
// builtin it went through: the builtin whose in-place form made it, from its location, or, when `calls` already holds
// the calls of a function value that failed inside the innermost frame's instruction, the builtin that instruction
// called, which called the function value back.
void append_calls(std::vector<RuntimeCall>& calls, const bytecode::Module& bytecode_module, const CallFrameStack& frames) {
  for (std::size_t depth = frames.active_size; depth > 0U; --depth) {
    const CallFrame& frame = frames.storage[depth - 1U];
    if (frame.instructions == nullptr || frame.ip == 0U || frame.ip > frame.instructions->size()) {
      continue;
    }
    const bytecode::Instruction& instruction = (*frame.instructions)[frame.ip - 1U];
    const auto function = std::ranges::find(bytecode_module.functions, frame.instructions,
                                            [](const bytecode::FunctionDef& def) { return &def.instructions; });
    const std::string frame_function = function == bytecode_module.functions.end() ? std::string{} : display_function_name(function->name);
    std::string through = depth == frames.active_size && !calls.empty() ? calling_back_name(bytecode_module, instruction) : std::string{};

    const bytecode::SourceLocation* location = bytecode::find_location(bytecode_module.locations, instruction.location);
    if (location == nullptr) {
      calls.push_back(RuntimeCall{.function = frame_function, .through = std::move(through)});
      continue;
    }
    while (location != nullptr) {
      const bytecode::SourceFileRecord& file = bytecode_module.source_files[location->file];
      std::string function_name = frame_function;
      if (location->inlined_function.has_value()) {
        function_name = display_function_name(bytecode_module.functions[*location->inlined_function].name);
      } else if (location->inlined_closure) {
        function_name = "a closure";
      }
      if (location->called_by.has_value()) {
        through = std::string{builtin_name(static_cast<BuiltinId>(*location->called_by))};
      }
      calls.push_back(RuntimeCall{
          .function = std::move(function_name),
          .through = std::exchange(through, std::string{}),
          .file = file.path,
          .file_hash = file.hash,
          .line = location->line,
          .column = location->column,
          .end_line = location->end_line,
          .end_column = location->end_column,
      });
      location = bytecode::find_location(bytecode_module.locations, location->inlined_at);
    }
  }
}

[[nodiscard]] auto suggested_frame_stack_capacity(const std::size_t function_count) -> std::size_t {
  return std::max(k_min_frame_capacity, function_count + 1U);
}

// Forward declarations.
auto run_loop(const RunningModule& running_module, std::vector<Value>& stack, CallFrameStack& frames,
              std::ostream& output) -> LoopResult;

auto dispatch_builtin(BuiltinId builtin_id, Value arg) -> tl::expected<Value, RuntimeError>;
auto dispatch_builtin_args(BuiltinId builtin_id, std::span<Value> values) -> tl::expected<Value, RuntimeError>;

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

// Binding a call
//
// A function value is a compiled function and the values it holds: none for a named function, the captures of a
// closure, or the hidden operator arguments of a generic function. Entering one puts those values in the first slots
// of the new frame, where its body reads them, and binds the arguments to the parameters it declares. A call of a user
// function binds the same way, as a function value that holds nothing. The arguments come either as one value (the
// argument itself for one parameter, the tuple of the arguments otherwise: bind_argument) or as consecutive values,
// such as the top of the stack (bind_arguments).

// The parameters a function declares, after the values it holds.
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

// Binds the argument value of a call to a frame just pushed, after the values the function holds.
auto bind_argument(CallFrame& frame, const runtime::TupleView held, const DeclaredParameters parameters, Value argument) -> BindFailure {
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

// Binds the consecutive argument values of a call, the first first, to a frame just pushed, after the values the
// function holds. It moves the values out; the caller drops what is left of them.
auto bind_arguments(CallFrame& frame, const runtime::TupleView held, const DeclaredParameters parameters, const std::span<Value> arguments)
    -> BindFailure {
  const std::size_t fixed = parameters.variadic ? parameters.arity - 1U : parameters.arity;
  if (parameters.arity < 2U || arguments.size() < fixed) {
    // A function of one parameter takes the arguments as one tuple, and too few arguments are reported as for a
    // tuple of them.
    return bind_argument(frame, held, parameters, runtime::make_tuple_moved(arguments));
  }

  FrameSlotWriter slots(frame, held.size() + parameters.arity);
  slots.push_all(held);
  for (std::size_t index = 0; index < fixed; ++index) {
    slots.push(std::move(arguments[index]));
  }
  if (parameters.variadic) {
    slots.push(runtime::make_tuple_moved(arguments.subspan(fixed)));
  }
  return std::nullopt;
}

// Binds the top `count` values of the stack to a frame just pushed, and pops them.
auto bind_stack_arguments(CallFrame& frame, const runtime::TupleView held, const DeclaredParameters parameters, std::vector<Value>& stack,
                          const std::size_t count) -> BindFailure {
  const std::size_t base = stack.size() - count;
  const BindFailure failure = bind_arguments(frame, held, parameters, std::span{stack}.subspan(base));
  stack.resize(base);
  return failure;
}

// The parameters of a user function, which holds no values.
auto user_function_parameters(const bytecode::FunctionDef& function) -> DeclaredParameters {
  return DeclaredParameters{.arity = function.arity, .variadic = function.has_variadic_tail};
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
  if (const BindFailure failure = bind_argument(frame, {}, user_function_parameters(function), std::move(arg)); failure.has_value()) {
    return tl::unexpected(function_value_argument_error(function.name, user_function_parameters(function), *failure));
  }
  return function_result(run_loop(running_module, scratch.stack(), scratch.frames(), output), function.name);
}

// The result of a call made through a function value: function values are called from builtins and host code, which
// report failures by throwing.
auto value_or_throw(tl::expected<Value, RuntimeError> result) -> Value {
  if (!result) {
    throw ReportedFailure(std::move(result.error()));
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
    return bind_argument(frame, held_items_, parameters_, std::move(argument));
  }

  // Binds `count` arguments on top of the stack to the frame entered for the call, and pops them.
  [[nodiscard]] auto bind_from_stack(CallFrame& frame, std::vector<Value>& stack, const std::size_t count) const
      -> BindFailure {
    return bind_stack_arguments(frame, held_items_, parameters_, stack, count);
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
      std::array<Value, 2> pair{std::move(lhs), std::move(rhs)};
      return bind_arguments(frame, held_items_, parameters_, pair);
    });
  }

private:
  // Runs the function on a scratch stack of its own. Builtins and host code call function values often, so this reads
  // the loop's result in place rather than through function_result.
  template <typename Bind>
  [[nodiscard]] auto run(Bind&& bind_arguments) const -> Value {
    auto result = run_to_result(std::forward<Bind>(bind_arguments));
    if (!result) {
      throw ReportedFailure(std::move(result.error()));
    }
    return std::move(*result);
  }

  template <typename Bind>
  [[nodiscard]] auto run_or_fail(Bind&& bind_arguments, std::string& error) const -> std::optional<Value> {
    auto result = run_to_result(std::forward<Bind>(bind_arguments));
    if (!result) {
      error = std::move(result.error().message);
      return std::nullopt;
    }
    return std::move(*result);
  }

  template <typename Bind>
  [[nodiscard]] auto run_to_result(Bind&& bind_arguments) const -> tl::expected<Value, RuntimeError> {
    const ScopedInvocationScratch scratch{operand_stack_capacity_, frame_stack_capacity_};
    auto& frame = scratch.frames().push(&function_->instructions);
    if (const BindFailure failure = std::forward<Bind>(bind_arguments)(frame); failure.has_value()) {
      return tl::unexpected(argument_error(*failure));
    }
    auto loop_result = run_loop(*module_, scratch.stack(), scratch.frames(), *output_);
    if (!loop_result) {
      return tl::unexpected(std::move(loop_result.error()));
    }
    if (auto* value = std::get_if<Value>(&*loop_result); value != nullptr) {
      return std::move(*value);
    }
    return tl::unexpected(make_runtime_error("halt inside function '" + function_->name + "'"));
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
  explicit BuiltinCallable(const BuiltinId builtin_id) : builtin_id_(builtin_id), binary_(builtin_binary_specialization(builtin_id)) {}

  [[nodiscard]] auto call(Value argument) const -> Value override {
    return value_or_throw(dispatch_builtin(builtin_id_, std::move(argument)));
  }

  [[nodiscard]] auto call_or_fail(Value argument, std::string& error) const -> std::optional<Value> override {
    auto result = dispatch_builtin(builtin_id_, std::move(argument));
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
auto call_function_value_natively(Value& argument, const runtime::Callable& callable, const std::string_view name)
    -> tl::expected<Value, RuntimeError> {
  return run_native_op(name, [&]() -> Value { return callable.call(std::move(argument)); });
}

auto make_builtin_function_value(const BuiltinId builtin_id) -> Value { return runtime::adopt_function(new BuiltinCallable(builtin_id)); }

// The loop's result when it fails in `frame` at `position`, one past the failing instruction: `error`, with the position
// stored in the frame for run_loop to read the calls from. It takes the position by value, so that the loop keeps its
// own in a register.
[[nodiscard]] auto fail_at(CallFrame& frame, const std::ptrdiff_t position, RuntimeError error) -> LoopResult {
  frame.ip = static_cast<std::size_t>(position);
  return tl::unexpected(std::move(error));
}

// run_instructions

// Runs the frames on the stack until the bottom one returns or halts. A failure leaves the frames as they were, with
// the failing frame's position stored, for run_loop to read its calls from.
auto run_instructions(const RunningModule& running_module, std::vector<Value>& stack, CallFrameStack& frames, std::ostream& output)
    -> LoopResult {
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
      return fail_at(*frame, ip - code, make_runtime_error("program terminated without halt"));
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
          return fail_at(*frame, ip - code, make_runtime_error("constant pool index out of range"));
        }
        stack.push_back(constants[idx]);
        break;
      }

      case bytecode::Opcode::kPop:
        if (stack.empty()) {
          return fail_at(*frame, ip - code, stack_underflow("pop"));
        }
        stack.pop_back();
        break;

      case bytecode::Opcode::kDup: {
        if (stack.empty()) {
          return fail_at(*frame, ip - code, make_runtime_error("stack underflow on dup"));
        }
        stack.push_back(stack.back());
        break;
      }

      case bytecode::Opcode::kBuildTuple: {
        const auto tuple_size = static_cast<std::size_t>(operand);
        if (stack.size() < tuple_size) {
          return fail_at(*frame, ip - code, make_runtime_error("stack underflow on build_tuple"));
        }
        collapse_stack_tail_into_tuple(stack, tuple_size);
        break;
      }

      case bytecode::Opcode::kConcatTuples: {
        const auto count = static_cast<std::size_t>(operand);
        if (operand < 0 || stack.size() < count) {
          return fail_at(*frame, ip - code, make_runtime_error("stack underflow on concat_tuples"));
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
          return fail_at(*frame, ip - code, make_runtime_error("type check index out of range"));
        }
        auto value = pop_stack(stack, "check_type");
        if (!value)
          return fail_at(*frame, ip - code, value.error());
        const auto& check = bytecode_module.type_checks[idx];
        if (opcode == bytecode::Opcode::kTryCheckType) {
          stack.push_back(runtime::try_check_cast(std::move(*value), check));
          break;
        }
        if (!runtime::value_matches_type_check(*value, check)) {
          return fail_at(*frame, ip - code, make_runtime_error(runtime::type_check_failure_message("Std.Cast", *value, check)));
        }
        stack.push_back(std::move(*value));
        break;
      }

      case bytecode::Opcode::kCallNative: {
        const auto idx = static_cast<std::size_t>(operand);
        const auto& bound = runtime::runtime_execution_state().native_functions;
        if (operand < 0 || idx >= bytecode_module.native_functions.size() || idx >= bound.size()) {
          return fail_at(*frame, ip - code, make_runtime_error("native function index out of range"));
        }
        auto arg = pop_stack(stack, "call_native");
        if (!arg)
          return fail_at(*frame, ip - code, arg.error());
        const auto& record = bytecode_module.native_functions[idx];
        auto result = call_native_function(record, bound[idx], *arg);
        if (!result)
          return fail_at(*frame, ip - code, result.error());
        stack.push_back(std::move(*result));
        break;
      }

      case bytecode::Opcode::kBuildTupleCounted: {
        auto count = pop_stack(stack, "build_tuple_counted");
        if (!count) {
          return fail_at(*frame, ip - code, count.error());
        }
        if (!count->is_uint()) {
          return fail_at(*frame, ip - code, make_runtime_error("build_tuple_counted expects a UInt64 count"));
        }
        const std::size_t tuple_size = runtime::to_index(count->uint_value());
        if (stack.size() < tuple_size) {
          return fail_at(*frame, ip - code, make_runtime_error("stack underflow on build_tuple_counted"));
        }
        collapse_stack_tail_into_tuple(stack, tuple_size);
        break;
      }

      case bytecode::Opcode::kIterate: {
        const auto items_slot = static_cast<std::size_t>(operand);
        if (operand < 0) {
          return fail_at(*frame, ip - code, make_runtime_error("iterate slot index out of range"));
        }
        // The items, the index, and the item are three slots in a row. Making the item's, the highest, makes the other
        // two and keeps the locals in the frame's vector, so one lookup finds all three.
        (void)get_or_create_local(*frame, items_slot + 2U);
        Value* const slots = frame->locals.data() + items_slot;
        Value& items = slots[0];
        Value& index = slots[1];
        Value& item = slots[2];
        if (!items.is_tuple() || !index.is_uint()) {
          return fail_at(*frame, ip - code, make_runtime_error("iterate expects a tuple and a UInt64 index"));
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
        if (auto applied = apply_binary_op(stack, "Std.ElementAt",
                                           [](Value& lhs, const Value& rhs) -> Value { return runtime::take_element(lhs, rhs); });
            !applied) {
          return fail_at(*frame, ip - code, make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kCallBuiltin: {
        const auto builtin_id = builtin_id_from_operand(operand);
        if (!builtin_id.has_value()) {
          return fail_at(*frame, ip - code, make_runtime_error("builtin index out of range"));
        }
        if (stack.empty()) {
          return fail_at(*frame, ip - code, stack_underflow("call_builtin"));
        }
        auto result = dispatch_builtin(*builtin_id, std::move(stack.back()));
        if (!result)
          return fail_at(*frame, ip - code, result.error());
        stack.back() = std::move(*result);
        break;
      }

      case bytecode::Opcode::kCallBuiltinArgs: {
        const auto builtin_id = builtin_id_from_operand(operand);
        if (!builtin_id.has_value()) {
          return fail_at(*frame, ip - code, make_runtime_error("unknown builtin id"));
        }
        const std::size_t count = builtin_argument_count_from_operand(operand);
        if (stack.size() < count) {
          return fail_at(*frame, ip - code, stack_underflow("call_builtin_args"));
        }
        // The builtin works on the arguments where they are: nothing it calls back into uses this stack.
        const std::size_t base = stack.size() - count;
        auto result = dispatch_builtin_args(*builtin_id, std::span{stack}.subspan(base));
        if (!result) {
          return fail_at(*frame, ip - code, result.error());
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
          return fail_at(*frame, ip - code, make_runtime_error("function index out of range"));
        }
        if (stack.empty()) {
          return fail_at(*frame, ip - code, stack_underflow("call_user_func"));
        }
        Value arg = std::move(stack.back());
        stack.pop_back();

        const auto& function = bytecode_module.functions[fn_idx];
        leave_frame();
        if (const BindFailure failure =
                bind_argument(frames.push(&function.instructions), {}, user_function_parameters(function), std::move(arg));
            failure.has_value()) {
          return fail_at(*frame, ip - code, function_value_argument_error(function.name, user_function_parameters(function), *failure));
        }
        enter_top_frame();
        break;
      }

      case bytecode::Opcode::kCallUserFuncArgs: {
        const auto fn_idx = static_cast<std::size_t>(operand);
        if (fn_idx >= bytecode_module.functions.size()) {
          return fail_at(*frame, ip - code, make_runtime_error("function index out of range"));
        }
        const auto& function = bytecode_module.functions[fn_idx];
        const std::size_t arity = function.arity;
        if (function.has_variadic_tail || arity < 2U) {
          return fail_at(*frame, ip - code,
                         make_runtime_error("call_user_func_args requires a non-variadic function of two or more parameters, not '" +
                                            function.name + "'"));
        }
        if (stack.size() < arity) {
          return fail_at(*frame, ip - code, stack_underflow("call_user_func_args"));
        }
        leave_frame();
        if (const BindFailure failure =
                bind_stack_arguments(frames.push(&function.instructions), {}, user_function_parameters(function), stack, arity);
            failure.has_value()) {
          return fail_at(*frame, ip - code, function_value_argument_error(function.name, user_function_parameters(function), *failure));
        }
        enter_top_frame();
        break;
      }

      case bytecode::Opcode::kCallFunctionValue: {
        auto function = pop_function_value(stack, "call_function_value", 1U);
        if (!function) {
          return fail_at(*frame, ip - code, function.error());
        }
        const runtime::Callable& callable = *function->callable;
        if (callable.owner() != &running_module) {
          auto result = call_function_value_natively(stack.back(), callable, "Std.Apply");
          if (!result) {
            return fail_at(*frame, ip - code, result.error());
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
          return fail_at(*frame, ip - code, compiled.argument_error(*failure));
        }
        enter_top_frame();
        break;
      }

      case bytecode::Opcode::kCallFunctionValueArgs: {
        if (operand < 2) {
          return fail_at(*frame, ip - code, make_runtime_error("call_function_value_args takes two or more arguments"));
        }
        const auto count = static_cast<std::size_t>(operand);
        auto function = pop_function_value(stack, "call_function_value_args", count);
        if (!function) {
          return fail_at(*frame, ip - code, function.error());
        }
        const runtime::Callable& callable = *function->callable;
        if (callable.owner() != &running_module) {
          // Any other function value takes the tuple of the arguments.
          collapse_stack_tail_into_tuple(stack, count);
          auto result = call_function_value_natively(stack.back(), callable, "Std.Apply");
          if (!result) {
            return fail_at(*frame, ip - code, result.error());
          }
          stack.back() = std::move(*result);
          break;
        }
        const auto& compiled = static_cast<const CompiledFunctionValue&>(callable);
        leave_frame();
        if (const auto failure = compiled.bind_from_stack(frames.push(&compiled.function().instructions), stack, count);
            failure.has_value()) {
          return fail_at(*frame, ip - code, compiled.argument_error(*failure));
        }
        enter_top_frame();
        break;
      }

      case bytecode::Opcode::kMakeUserFuncRef: {
        const auto fn_idx = static_cast<std::size_t>(operand);
        if (fn_idx >= bytecode_module.functions.size()) {
          return fail_at(*frame, ip - code, make_runtime_error("function index out of range"));
        }
        stack.push_back(make_function_value(running_module, bytecode_module.functions[fn_idx], output));
        break;
      }

      case bytecode::Opcode::kMakeBuiltinFuncRef: {
        const auto builtin_id = builtin_id_from_operand(operand);
        if (!builtin_id.has_value()) {
          return fail_at(*frame, ip - code, make_runtime_error("builtin index out of range"));
        }
        stack.push_back(make_builtin_function_value(*builtin_id));
        break;
      }

      case bytecode::Opcode::kMakeClosureRef: {
        const auto closure_idx = static_cast<std::size_t>(operand);
        if (closure_idx >= bytecode_module.closures.size()) {
          return fail_at(*frame, ip - code, make_runtime_error("closure index out of range"));
        }

        auto captured_tuple = pop_stack(stack, "make_closure_ref");
        if (!captured_tuple) {
          return fail_at(*frame, ip - code, captured_tuple.error());
        }

        const auto& [function_index, capture_count, declared_arity, declared_has_variadic_tail] =
            bytecode_module.closures[closure_idx];
        if (!captured_tuple->is_tuple() || captured_tuple->tuple().size() != capture_count) {
          return fail_at(*frame, ip - code, make_runtime_error("closure capture tuple size mismatch"));
        }

        // The function value holds the capture tuple itself.
        stack.push_back(runtime::adopt_function(new CompiledFunctionValue(
            running_module, bytecode_module.functions[function_index], std::move(*captured_tuple),
            DeclaredParameters{.arity = declared_arity, .variadic = declared_has_variadic_tail}, output)));
        break;
      }

      case bytecode::Opcode::kReturn: {
        if (stack.empty()) {
          return fail_at(*frame, ip - code, stack_underflow("return"));
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
          return fail_at(*frame, ip - code, make_runtime_error("local slot index out of range"));
        }
        stack.push_back(*local);
        break;
      }

      case bytecode::Opcode::kTakeLocal: {
        Value* local = try_get_local(*frame, static_cast<std::size_t>(operand));
        if (local == nullptr) {
          return fail_at(*frame, ip - code, make_runtime_error("local slot index out of range"));
        }
        stack.push_back(std::move(*local));
        break;
      }

      case bytecode::Opcode::kStoreLocal: {
        if (stack.empty()) {
          return fail_at(*frame, ip - code, stack_underflow("store_local"));
        }
        get_or_create_local(*frame, static_cast<std::size_t>(operand)) = std::move(stack.back());
        stack.pop_back();
        break;
      }

      case bytecode::Opcode::kJump: {
        const auto target = static_cast<std::size_t>(operand);
        if (target > static_cast<std::size_t>(end - code)) {
          return fail_at(*frame, ip - code, make_runtime_error("jump target out of range"));
        }
        ip = code + target;
        break;
      }

      case bytecode::Opcode::kJumpIf: {
        if (stack.empty()) {
          return fail_at(*frame, ip - code, stack_underflow("jump_if"));
        }
        const bool condition = runtime::as_bool(stack.back());
        stack.pop_back();
        const auto target = static_cast<std::size_t>(operand);
        if (target > static_cast<std::size_t>(end - code)) {
          return fail_at(*frame, ip - code, make_runtime_error("jump_if target out of range"));
        }
        if (condition) {
          ip = code + target;
        }
        break;
      }

      case bytecode::Opcode::kJumpIfNot: {
        if (stack.empty()) {
          return fail_at(*frame, ip - code, stack_underflow("jump_if_not"));
        }
        const bool condition = runtime::as_bool(stack.back());
        stack.pop_back();
        const auto target = static_cast<std::size_t>(operand);
        if (target > static_cast<std::size_t>(end - code)) {
          return fail_at(*frame, ip - code, make_runtime_error("jump_if_not target out of range"));
        }
        if (!condition) {
          ip = code + target;
        }
        break;
      }

      case bytecode::Opcode::kAdd:
        if (auto applied =
                apply_binary_op(stack, "Std.Add", [](const Value& lhs, const Value& rhs) -> Value { return runtime::AddBinary(lhs, rhs); });
            !applied) {
          return fail_at(*frame, ip - code, make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kSub:
        if (auto applied = apply_binary_op(stack, "Std.Subtract",
                                           [](const Value& lhs, const Value& rhs) -> Value { return runtime::SubtractBinary(lhs, rhs); });
            !applied) {
          return fail_at(*frame, ip - code, make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kMul:
        if (auto applied = apply_binary_op(stack, "Std.Multiply",
                                           [](const Value& lhs, const Value& rhs) -> Value { return runtime::MultiplyBinary(lhs, rhs); });
            !applied) {
          return fail_at(*frame, ip - code, make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kDiv:
        if (auto applied = apply_binary_op(stack, "Std.Divide",
                                           [](const Value& lhs, const Value& rhs) -> Value { return runtime::DivideBinary(lhs, rhs); });
            !applied) {
          return fail_at(*frame, ip - code, make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kMod:
        if (auto applied =
                apply_binary_op(stack, "Std.Mod", [](const Value& lhs, const Value& rhs) -> Value { return runtime::ModBinary(lhs, rhs); });
            !applied) {
          return fail_at(*frame, ip - code, make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kPow:
        if (auto applied =
                apply_binary_op(stack, "Std.Pow", [](const Value& lhs, const Value& rhs) -> Value { return runtime::PowBinary(lhs, rhs); });
            !applied) {
          return fail_at(*frame, ip - code, make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kNeg:
        if (auto applied =
                apply_unary_op(stack, "Std.UnaryMinus", [](const Value& value) -> Value { return runtime::UnaryMinusValue(value); });
            !applied) {
          return fail_at(*frame, ip - code, make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kCmpEq:
        if (auto applied = apply_binary_op(stack, "Std.Equal",
                                           [](const Value& lhs, const Value& rhs) -> Value { return runtime::make_bool(lhs == rhs); });
            !applied) {
          return fail_at(*frame, ip - code, make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kCmpNe:
        if (auto applied = apply_binary_op(stack, "Std.NotEqual",
                                           [](const Value& lhs, const Value& rhs) -> Value { return runtime::make_bool(lhs != rhs); });
            !applied) {
          return fail_at(*frame, ip - code, make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kCmpLt:
        if (auto applied = apply_binary_op(
                stack, "Std.LessThan",
                [](const Value& lhs, const Value& rhs) -> Value { return runtime::make_bool(runtime::compare_numbers(lhs, rhs) < 0); });
            !applied) {
          return fail_at(*frame, ip - code, make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kCmpGt:
        if (auto applied = apply_binary_op(
                stack, "Std.GreaterThan",
                [](const Value& lhs, const Value& rhs) -> Value { return runtime::make_bool(runtime::compare_numbers(lhs, rhs) > 0); });
            !applied) {
          return fail_at(*frame, ip - code, make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kCmpLe:
        if (auto applied = apply_binary_op(
                stack, "Std.LessOrEqual",
                [](const Value& lhs, const Value& rhs) -> Value { return runtime::make_bool(runtime::compare_numbers(lhs, rhs) <= 0); });
            !applied) {
          return fail_at(*frame, ip - code, make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kCmpGe:
        if (auto applied = apply_binary_op(
                stack, "Std.GreaterOrEqual",
                [](const Value& lhs, const Value& rhs) -> Value { return runtime::make_bool(runtime::compare_numbers(lhs, rhs) >= 0); });
            !applied) {
          return fail_at(*frame, ip - code, make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kAnd:
        if (auto applied = apply_binary_op(stack, "Std.And",
                                           [](const Value& lhs, const Value& rhs) -> Value {
                                             return runtime::make_bool(runtime::as_bool(lhs) && runtime::as_bool(rhs));
                                           });
            !applied) {
          return fail_at(*frame, ip - code, make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kOr:
        if (auto applied = apply_binary_op(stack, "Std.Or",
                                           [](const Value& lhs, const Value& rhs) -> Value {
                                             return runtime::make_bool(runtime::as_bool(lhs) || runtime::as_bool(rhs));
                                           });
            !applied) {
          return fail_at(*frame, ip - code, make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kNot:
        if (auto applied =
                apply_unary_op(stack, "Std.Not", [](const Value& value) -> Value { return runtime::make_bool(!runtime::as_bool(value)); });
            !applied) {
          return fail_at(*frame, ip - code, make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kSelect: {
        if (stack.size() < 3U) {
          return fail_at(*frame, ip - code, stack_underflow("select"));
        }
        const auto base = stack.size() - 3U;
        const bool condition = runtime::as_bool(stack[base]);
        stack[base] = std::move(stack[condition ? base + 1U : base + 2U]);
        stack.resize(base + 1U);
        break;
      }

      case bytecode::Opcode::kBitAnd:
        if (auto applied = apply_binary_op(stack, "Std.Bit.And",
                                           [](const Value& lhs, const Value& rhs) -> Value { return runtime::BitAndBinary(lhs, rhs); });
            !applied) {
          return fail_at(*frame, ip - code, make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kBitOr:
        if (auto applied = apply_binary_op(stack, "Std.Bit.Or",
                                           [](const Value& lhs, const Value& rhs) -> Value { return runtime::BitOrBinary(lhs, rhs); });
            !applied) {
          return fail_at(*frame, ip - code, make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kBitXor:
        if (auto applied = apply_binary_op(stack, "Std.Bit.Xor",
                                           [](const Value& lhs, const Value& rhs) -> Value { return runtime::BitXorBinary(lhs, rhs); });
            !applied) {
          return fail_at(*frame, ip - code, make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kShiftLeft:
        if (auto applied =
                apply_binary_op(stack, "Std.Bit.ShiftLeft",
                                [](const Value& lhs, const Value& rhs) -> Value { return runtime::BitShiftLeftBinary(lhs, rhs); });
            !applied) {
          return fail_at(*frame, ip - code, make_runtime_error(applied.error()));
        }
        break;

      case bytecode::Opcode::kShiftRight:
        if (auto applied =
                apply_binary_op(stack, "Std.Bit.ShiftRight",
                                [](const Value& lhs, const Value& rhs) -> Value { return runtime::BitShiftRightBinary(lhs, rhs); });
            !applied) {
          return fail_at(*frame, ip - code, make_runtime_error(applied.error()));
        }
        break;

      // The item reads and the stack mark sit at the end of the switch: placed next to kTakeLocal, the item reads made
      // other programs 4 to 7% slower, from the layout of run_loop alone.
      case bytecode::Opcode::kLoadLocalItem: {
        const Value* local = try_get_local(*frame, bytecode::local_item_slot(operand));
        const std::size_t position = bytecode::local_item_index(operand);
        if (local == nullptr || !local->is_tuple() || position >= local->tuple().size()) [[unlikely]] {
          return fail_at(*frame, ip - code, local_item_read_error(local, bytecode::local_item_index(operand)));
        }
        stack.push_back(local->tuple()[position]);
        break;
      }

      case bytecode::Opcode::kTakeLocalItem: {
        Value* local = try_get_local(*frame, bytecode::local_item_slot(operand));
        const std::size_t position = bytecode::local_item_index(operand);
        if (local == nullptr || !local->is_tuple() || position >= local->tuple().size()) [[unlikely]] {
          return fail_at(*frame, ip - code, local_item_read_error(local, bytecode::local_item_index(operand)));
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
          return fail_at(*frame, ip - code, make_runtime_error("build_tuple_since_mark expects a stack height in its slot"));
        }
        collapse_stack_tail_into_tuple(stack, stack.size() - runtime::to_index(mark->uint_value()));
        break;
      }

      case bytecode::Opcode::kMatchFailed:
        return fail_at(*frame, ip - code, make_runtime_error(std::format("Std.Match: {}", runtime::k_match_no_case_message)));

      case bytecode::Opcode::kHalt:
        return LoopExit{std::monostate{}};
      default:
        return fail_at(*frame, ip - code, make_runtime_error(std::format("invalid opcode {}", static_cast<int>(opcode))));
    }
  }
}

// run_loop

// Runs the frames on the stack, and gives a failure the calls it happened in. The calls are read here, once, rather
// than at each of run_instructions' failure sites: a call at every site made a loop of arithmetic and item reads 6%
// slower, though none of them runs while the program succeeds (internal_docs/s1_plan_2026-11-25.md).
auto run_loop(const RunningModule& running_module, std::vector<Value>& stack, CallFrameStack& frames, std::ostream& output) -> LoopResult {
  auto result = run_instructions(running_module, stack, frames, output);
  if (!result) {
    append_calls(result.error().calls, running_module.bytecode(), frames);
  }
  return result;
}

// The function of each callable builtin, by BuiltinId. A builtin of one parameter, Std.Println, and Std.Printf take
// their argument as a value; every other builtin takes runtime::Arguments (vm::builtin_takes_arguments). A call is
// one indirect call.
using ValueBuiltin = Value (*)(Value&&);
using ArgumentsBuiltin = Value (*)(const runtime::Arguments&);

struct BuiltinImplementation {
  ValueBuiltin value{nullptr};
  ArgumentsBuiltin arguments{nullptr};
};

constexpr auto kBuiltinImplementations = []() {
  std::array<BuiltinImplementation, builtin_count()> table{};
  const auto takes_value = [&table](const BuiltinId id, const ValueBuiltin function) -> void {
    table[static_cast<std::size_t>(id)].value = function;
  };
  const auto takes_arguments = [&table](const BuiltinId id, const ArgumentsBuiltin function) -> void {
    table[static_cast<std::size_t>(id)].arguments = function;
  };

  takes_value(BuiltinId::UnaryPlus, [](Value&& arg) -> Value { return runtime::UnaryPlus(std::move(arg)); });
  takes_value(BuiltinId::UnaryMinus, [](Value&& arg) -> Value { return runtime::UnaryMinus(std::move(arg)); });
  takes_value(BuiltinId::BitNot, [](Value&& arg) -> Value { return runtime::BitNot(std::move(arg)); });
  takes_value(BuiltinId::Not, [](Value&& arg) -> Value { return runtime::Not(std::move(arg)); });
  takes_value(BuiltinId::Printf, [](Value&& arg) -> Value { return runtime::Printf(std::move(arg)); });
  takes_value(BuiltinId::Println, [](Value&& arg) -> Value { return runtime::Println(std::move(arg)); });
  takes_value(BuiltinId::Type, [](Value&& arg) -> Value { return runtime::Type(std::move(arg)); });
  takes_value(BuiltinId::InputString, [](Value&& arg) -> Value { return runtime::Input_String(std::move(arg)); });
  takes_value(BuiltinId::Help, [](Value&& arg) -> Value { return runtime::Help(std::move(arg)); });
  takes_value(BuiltinId::ExitInt64, [](Value&& arg) -> Value { return runtime::Exit_Int64(std::move(arg)); });
  takes_value(BuiltinId::OSEnv, [](Value&& arg) -> Value { return runtime::OSEnv(std::move(arg)); });
  takes_value(BuiltinId::OSHasEnv, [](Value&& arg) -> Value { return runtime::OSHasEnv(std::move(arg)); });
  takes_value(BuiltinId::OSUnsetEnv, [](Value&& arg) -> Value { return runtime::OSUnsetEnv(std::move(arg)); });
  takes_value(BuiltinId::OSExec, [](Value&& arg) -> Value { return runtime::OSExec(std::move(arg)); });
  takes_value(BuiltinId::PathNormalize, [](Value&& arg) -> Value { return runtime::PathNormalize(std::move(arg)); });
  takes_value(BuiltinId::PathBasename, [](Value&& arg) -> Value { return runtime::PathBasename(std::move(arg)); });
  takes_value(BuiltinId::PathDirname, [](Value&& arg) -> Value { return runtime::PathDirname(std::move(arg)); });
  takes_value(BuiltinId::PathExists, [](Value&& arg) -> Value { return runtime::PathExists(std::move(arg)); });
  takes_value(BuiltinId::PathIsFile, [](Value&& arg) -> Value { return runtime::PathIsFile(std::move(arg)); });
  takes_value(BuiltinId::PathIsDir, [](Value&& arg) -> Value { return runtime::PathIsDir(std::move(arg)); });
  takes_value(BuiltinId::PathAbsolute, [](Value&& arg) -> Value { return runtime::PathAbsolute(std::move(arg)); });
  takes_value(BuiltinId::PathExtension, [](Value&& arg) -> Value { return runtime::PathExtension(std::move(arg)); });
  takes_value(BuiltinId::PathStem, [](Value&& arg) -> Value { return runtime::PathStem(std::move(arg)); });
  takes_value(BuiltinId::FileReadText, [](Value&& arg) -> Value { return runtime::FileReadText(std::move(arg)); });
  takes_value(BuiltinId::FileReadLines, [](Value&& arg) -> Value { return runtime::FileReadLines(std::move(arg)); });
  takes_value(BuiltinId::FileDelete, [](Value&& arg) -> Value { return runtime::FileDelete(std::move(arg)); });
  takes_value(BuiltinId::FileSize, [](Value&& arg) -> Value { return runtime::FileSize(std::move(arg)); });
  takes_value(BuiltinId::FileReadLine, [](Value&& arg) -> Value { return runtime::FileReadLine(std::move(arg)); });
  takes_value(BuiltinId::FileFlush, [](Value&& arg) -> Value { return runtime::FileFlush(std::move(arg)); });
  takes_value(BuiltinId::FileClose, [](Value&& arg) -> Value { return runtime::FileClose(std::move(arg)); });
  takes_value(BuiltinId::DirCreate, [](Value&& arg) -> Value { return runtime::DirCreate(std::move(arg)); });
  takes_value(BuiltinId::DirDelete, [](Value&& arg) -> Value { return runtime::DirDelete(std::move(arg)); });
  takes_value(BuiltinId::DirList, [](Value&& arg) -> Value { return runtime::DirList(std::move(arg)); });
  takes_value(BuiltinId::DirListFull, [](Value&& arg) -> Value { return runtime::DirListFull(std::move(arg)); });
  takes_value(BuiltinId::TupleReverse, [](Value&& arg) -> Value { return runtime::TupleReverse(std::move(arg)); });
  takes_value(BuiltinId::TupleSort, [](Value&& arg) -> Value { return runtime::TupleSort(std::move(arg)); });
  takes_value(BuiltinId::TupleUnique, [](Value&& arg) -> Value { return runtime::TupleUnique(std::move(arg)); });
  takes_value(BuiltinId::TupleMin, [](Value&& arg) -> Value { return runtime::TupleMin(std::move(arg)); });
  takes_value(BuiltinId::TupleMax, [](Value&& arg) -> Value { return runtime::TupleMax(std::move(arg)); });
  takes_value(BuiltinId::TupleRangeInt64, [](Value&& arg) -> Value { return runtime::TupleRangeInt64(std::move(arg)); });
  takes_value(BuiltinId::ArrayTranspose2D, [](Value&& arg) -> Value { return runtime::ArrayTranspose2D(std::move(arg)); });
  takes_value(BuiltinId::ArrayRank, [](Value&& arg) -> Value { return runtime::ArrayRank(std::move(arg)); });
  takes_value(BuiltinId::ArrayShape, [](Value&& arg) -> Value { return runtime::ArrayShape(std::move(arg)); });
  takes_value(BuiltinId::ArrayFlatten, [](Value&& arg) -> Value { return runtime::ArrayFlatten(std::move(arg)); });
  takes_value(BuiltinId::DictCreateDict, [](Value&& arg) -> Value { return runtime::DictCreate_Dict(std::move(arg)); });
  takes_value(BuiltinId::DictKeys, [](Value&& arg) -> Value { return runtime::DictKeys(std::move(arg)); });
  takes_value(BuiltinId::DictValues, [](Value&& arg) -> Value { return runtime::DictValues(std::move(arg)); });
  takes_value(BuiltinId::DictEntries, [](Value&& arg) -> Value { return runtime::DictEntries(std::move(arg)); });
  takes_value(BuiltinId::DictClear, [](Value&& arg) -> Value { return runtime::DictClear(std::move(arg)); });
  takes_value(BuiltinId::DictLength, [](Value&& arg) -> Value { return runtime::DictLength(std::move(arg)); });
  takes_value(BuiltinId::Cast, [](Value&& arg) -> Value { return runtime::Cast(std::move(arg)); });
  takes_value(BuiltinId::TryCast, [](Value&& arg) -> Value { return runtime::TryCast(std::move(arg)); });
  takes_value(BuiltinId::ToInt64, [](Value&& arg) -> Value { return runtime::ToInt64(std::move(arg)); });
  takes_value(BuiltinId::ToUInt64, [](Value&& arg) -> Value { return runtime::ToUInt64(std::move(arg)); });
  takes_value(BuiltinId::ToFloat64, [](Value&& arg) -> Value { return runtime::ToFloat64(std::move(arg)); });
  takes_value(BuiltinId::RandomCreate, [](Value&& arg) -> Value { return runtime::RandomCreate(std::move(arg)); });
  takes_value(BuiltinId::RandomNextUInt64, [](Value&& arg) -> Value { return runtime::RandomNextUInt64(std::move(arg)); });
  takes_value(BuiltinId::RandomNextInt64, [](Value&& arg) -> Value { return runtime::RandomNextInt64(std::move(arg)); });
  takes_value(BuiltinId::RandomNextFloat64, [](Value&& arg) -> Value { return runtime::RandomNextFloat64(std::move(arg)); });
  takes_value(BuiltinId::RandomNextBool, [](Value&& arg) -> Value { return runtime::RandomNextBool(std::move(arg)); });
  takes_value(BuiltinId::RandomSplit, [](Value&& arg) -> Value { return runtime::RandomSplit(std::move(arg)); });
  takes_value(BuiltinId::MathFloor, [](Value&& arg) -> Value { return runtime::MathFloor(std::move(arg)); });
  takes_value(BuiltinId::MathCeil, [](Value&& arg) -> Value { return runtime::MathCeil(std::move(arg)); });
  takes_value(BuiltinId::MathAbs, [](Value&& arg) -> Value { return runtime::MathAbs(std::move(arg)); });
  takes_value(BuiltinId::MathLog, [](Value&& arg) -> Value { return runtime::MathLog(std::move(arg)); });
  takes_value(BuiltinId::Sqrt, [](Value&& arg) -> Value { return runtime::Sqrt(std::move(arg)); });
  takes_value(BuiltinId::Sin, [](Value&& arg) -> Value { return runtime::Sin(std::move(arg)); });
  takes_value(BuiltinId::Cos, [](Value&& arg) -> Value { return runtime::Cos(std::move(arg)); });
  takes_value(BuiltinId::Tan, [](Value&& arg) -> Value { return runtime::Tan(std::move(arg)); });
  takes_value(BuiltinId::ResultOk, [](Value&& arg) -> Value { return runtime::ResultOk(std::move(arg)); });
  takes_value(BuiltinId::ResultErr, [](Value&& arg) -> Value { return runtime::ResultErr(std::move(arg)); });
  takes_value(BuiltinId::ResultPayload, [](Value&& arg) -> Value { return runtime::ResultPayload(std::move(arg)); });
  takes_value(BuiltinId::ResultIsOk, [](Value&& arg) -> Value { return runtime::ResultIsOk(std::move(arg)); });
  takes_value(BuiltinId::ResultIsErr, [](Value&& arg) -> Value { return runtime::ResultIsErr(std::move(arg)); });
  takes_value(BuiltinId::ResultUnwrap, [](Value&& arg) -> Value { return runtime::ResultUnwrap(std::move(arg)); });
  takes_value(BuiltinId::ResultUnwrapErr, [](Value&& arg) -> Value { return runtime::ResultUnwrapErr(std::move(arg)); });
  takes_value(BuiltinId::Panic, [](Value&& arg) -> Value { return runtime::Panic(std::move(arg)); });
  takes_value(BuiltinId::TaskAwait, [](Value&& arg) -> Value { return runtime::TaskAwait(std::move(arg)); });
  takes_value(BuiltinId::TaskAwaitAll, [](Value&& arg) -> Value { return runtime::TaskAwaitAll(std::move(arg)); });
  takes_value(BuiltinId::TaskCancel, [](Value&& arg) -> Value { return runtime::TaskCancel(std::move(arg)); });
  takes_value(BuiltinId::Wrap, [](Value&& arg) -> Value { return runtime::Wrap(std::move(arg)); });
  takes_value(BuiltinId::Unwrap, [](Value&& arg) -> Value { return runtime::Unwrap(std::move(arg)); });
  takes_value(BuiltinId::First, [](Value&& arg) -> Value { return runtime::First(std::move(arg)); });
  takes_value(BuiltinId::Second, [](Value&& arg) -> Value { return runtime::Second(std::move(arg)); });
  takes_value(BuiltinId::Length, [](Value&& arg) -> Value { return runtime::Length(std::move(arg)); });
  takes_value(BuiltinId::ToString, [](Value&& arg) -> Value { return runtime::ToString(std::move(arg)); });
  takes_value(BuiltinId::StringParseInt64, [](Value&& arg) -> Value { return runtime::StringParseInt64(std::move(arg)); });
  takes_value(BuiltinId::StringParseUInt64, [](Value&& arg) -> Value { return runtime::StringParseUInt64(std::move(arg)); });
  takes_value(BuiltinId::StringParseFloat64, [](Value&& arg) -> Value { return runtime::StringParseFloat64(std::move(arg)); });
  takes_value(BuiltinId::StringUpper, [](Value&& arg) -> Value { return runtime::StringUpper(std::move(arg)); });
  takes_value(BuiltinId::StringLower, [](Value&& arg) -> Value { return runtime::StringLower(std::move(arg)); });
  takes_value(BuiltinId::StringTrim, [](Value&& arg) -> Value { return runtime::StringTrim(std::move(arg)); });
  takes_value(BuiltinId::StringTrimStart, [](Value&& arg) -> Value { return runtime::StringTrimStart(std::move(arg)); });
  takes_value(BuiltinId::StringTrimEnd, [](Value&& arg) -> Value { return runtime::StringTrimEnd(std::move(arg)); });
  takes_value(BuiltinId::StringLength, [](Value&& arg) -> Value { return runtime::StringLength(std::move(arg)); });

  takes_arguments(BuiltinId::Add, &runtime::Add);
  takes_arguments(BuiltinId::Subtract, &runtime::Subtract);
  takes_arguments(BuiltinId::Multiply, &runtime::Multiply);
  takes_arguments(BuiltinId::WrappingAdd, &runtime::WrappingAdd);
  takes_arguments(BuiltinId::WrappingSubtract, &runtime::WrappingSubtract);
  takes_arguments(BuiltinId::WrappingMultiply, &runtime::WrappingMultiply);
  takes_arguments(BuiltinId::SaturatingAdd, &runtime::SaturatingAdd);
  takes_arguments(BuiltinId::SaturatingSubtract, &runtime::SaturatingSubtract);
  takes_arguments(BuiltinId::SaturatingMultiply, &runtime::SaturatingMultiply);
  takes_arguments(BuiltinId::Divide, &runtime::Divide);
  takes_arguments(BuiltinId::Mod, &runtime::Mod);
  takes_arguments(BuiltinId::Pow, &runtime::Pow);
  takes_arguments(BuiltinId::BitAnd, &runtime::BitAnd);
  takes_arguments(BuiltinId::BitOr, &runtime::BitOr);
  takes_arguments(BuiltinId::BitXor, &runtime::BitXor);
  takes_arguments(BuiltinId::BitShiftLeft, &runtime::BitShiftLeft);
  takes_arguments(BuiltinId::BitShiftRight, &runtime::BitShiftRight);
  takes_arguments(BuiltinId::Equal, &runtime::Equal);
  takes_arguments(BuiltinId::NotEqual, &runtime::NotEqual);
  takes_arguments(BuiltinId::LessThan, &runtime::LessThan);
  takes_arguments(BuiltinId::GreaterThan, &runtime::GreaterThan);
  takes_arguments(BuiltinId::GreaterOrEqual, &runtime::GreaterOrEqual);
  takes_arguments(BuiltinId::LessOrEqual, &runtime::LessOrEqual);
  takes_arguments(BuiltinId::And, &runtime::And);
  takes_arguments(BuiltinId::Or, &runtime::Or);
  takes_arguments(BuiltinId::Select, &runtime::Select);
  takes_arguments(BuiltinId::Match, &runtime::Match);
  takes_arguments(BuiltinId::Apply, &runtime::Apply);
  takes_arguments(BuiltinId::Branch, &runtime::Branch);
  takes_arguments(BuiltinId::Loop, &runtime::Loop);
  takes_arguments(BuiltinId::LoopN, &runtime::LoopN);
  takes_arguments(BuiltinId::GetArgs, &runtime::GetArgs);
  takes_arguments(BuiltinId::InputVoid, &runtime::Input_Void);
  takes_arguments(BuiltinId::ExitVoid, &runtime::Exit_Void);
  takes_arguments(BuiltinId::Cwd, &runtime::Cwd);
  takes_arguments(BuiltinId::OSSetEnv, &runtime::OSSetEnv);
  takes_arguments(BuiltinId::OSIsWindows, &runtime::OSIsWindows);
  takes_arguments(BuiltinId::OSIsLinux, &runtime::OSIsLinux);
  takes_arguments(BuiltinId::OSIsMacOS, &runtime::OSIsMacOS);
  takes_arguments(BuiltinId::OSHome, &runtime::OSHome);
  takes_arguments(BuiltinId::OSTempDir, &runtime::OSTempDir);
  takes_arguments(BuiltinId::OSMakeTempFile, &runtime::OSMakeTempFile);
  takes_arguments(BuiltinId::OSMakeTempDir, &runtime::OSMakeTempDir);
  takes_arguments(BuiltinId::PathJoin, &runtime::PathJoin);
  takes_arguments(BuiltinId::PathWithExtension, &runtime::PathWithExtension);
  takes_arguments(BuiltinId::PathWithBasename, &runtime::PathWithBasename);
  takes_arguments(BuiltinId::FileWriteText, &runtime::FileWriteText);
  takes_arguments(BuiltinId::FileAppendText, &runtime::FileAppendText);
  takes_arguments(BuiltinId::FileOpen, &runtime::FileOpen);
  takes_arguments(BuiltinId::FileReadChunk, &runtime::FileReadChunk);
  takes_arguments(BuiltinId::FileWriteChunk, &runtime::FileWriteChunk);
  takes_arguments(BuiltinId::FileWithOpen, &runtime::FileWithOpen);
  takes_arguments(BuiltinId::TupleAppend, &runtime::TupleAppend);
  takes_arguments(BuiltinId::TuplePrepend, &runtime::TuplePrepend);
  takes_arguments(BuiltinId::TupleContains, &runtime::TupleContains);
  takes_arguments(BuiltinId::TupleZip, &runtime::TupleZip);
  takes_arguments(BuiltinId::TupleMap, &runtime::TupleMap);
  takes_arguments(BuiltinId::TupleFilter, &runtime::TupleFilter);
  takes_arguments(BuiltinId::TupleReduce, &runtime::TupleReduce);
  takes_arguments(BuiltinId::TupleFindIndex, &runtime::TupleFindIndex);
  takes_arguments(BuiltinId::TupleAny, &runtime::TupleAny);
  takes_arguments(BuiltinId::TupleAll, &runtime::TupleAll);
  takes_arguments(BuiltinId::TupleRangeInt64Int64, &runtime::TupleRangeInt64Int64);
  takes_arguments(BuiltinId::TupleRangeInt64Int64Int64, &runtime::TupleRangeInt64Int64Int64);
  takes_arguments(BuiltinId::TupleSetAt, &runtime::TupleSetAt);
  takes_arguments(BuiltinId::TupleInsertAt, &runtime::TupleInsertAt);
  takes_arguments(BuiltinId::TupleRemoveAt, &runtime::TupleRemoveAt);
  takes_arguments(BuiltinId::TupleConcat, &runtime::TupleConcat);
  takes_arguments(BuiltinId::ArraySetAt2D, &runtime::ArraySetAt2D);
  takes_arguments(BuiltinId::ArrayFill, &runtime::ArrayFill);
  takes_arguments(BuiltinId::ArraySlice2D, &runtime::ArraySlice2D);
  takes_arguments(BuiltinId::ArrayReshape, &runtime::ArrayReshape);
  takes_arguments(BuiltinId::ArrayGetAtND, &runtime::ArrayGetAtND);
  takes_arguments(BuiltinId::ArraySetAtND, &runtime::ArraySetAtND);
  takes_arguments(BuiltinId::ArrayReshapeND, &runtime::ArrayReshapeND);
  takes_arguments(BuiltinId::DictCreateVoid, &runtime::DictCreate_Void);
  takes_arguments(BuiltinId::DictSet, &runtime::DictSet);
  takes_arguments(BuiltinId::DictGet, &runtime::DictGet);
  takes_arguments(BuiltinId::DictGetDefault, &runtime::DictGetDefault);
  takes_arguments(BuiltinId::DictContains, &runtime::DictContains);
  takes_arguments(BuiltinId::DictDelete, &runtime::DictDelete);
  takes_arguments(BuiltinId::DictMerge, &runtime::DictMerge);
  takes_arguments(BuiltinId::MathClamp, &runtime::MathClamp);
  takes_arguments(BuiltinId::Try, &runtime::Try);
  takes_arguments(BuiltinId::ParallelMap, &runtime::ParallelMap);
  takes_arguments(BuiltinId::ParallelWithOptions, &runtime::ParallelWithOptions);
  takes_arguments(BuiltinId::ParallelForEach, &runtime::ParallelForEach);
  takes_arguments(BuiltinId::ParallelReduce, &runtime::ParallelReduce);
  takes_arguments(BuiltinId::TaskSpawn, &runtime::TaskSpawn);
  takes_arguments(BuiltinId::TaskWithTimeout, &runtime::TaskWithTimeout);
  takes_arguments(BuiltinId::ElementAt, &runtime::ElementAt);
  takes_arguments(BuiltinId::Take, &runtime::Take);
  takes_arguments(BuiltinId::Drop, &runtime::Drop);
  takes_arguments(BuiltinId::SliceTupleInt64, &runtime::SliceTupleInt64);
  takes_arguments(BuiltinId::SliceTupleInt64Int64, &runtime::SliceTupleInt64Int64);
  takes_arguments(BuiltinId::SliceTupleInt64Int64Int64, &runtime::SliceTupleInt64Int64Int64);
  takes_arguments(BuiltinId::StringSplit, &runtime::StringSplit);
  takes_arguments(BuiltinId::StringJoin, &runtime::StringJoin);
  takes_arguments(BuiltinId::StringReplace, &runtime::StringReplace);
  takes_arguments(BuiltinId::StringContains, &runtime::StringContains);
  takes_arguments(BuiltinId::StringStartsWith, &runtime::StringStartsWith);
  takes_arguments(BuiltinId::StringEndsWith, &runtime::StringEndsWith);
  takes_arguments(BuiltinId::StringCharAt, &runtime::StringCharAt);
  takes_arguments(BuiltinId::StringSliceStringInt64, &runtime::StringSliceStringInt64);
  takes_arguments(BuiltinId::StringSliceStringInt64Int64, &runtime::StringSliceStringInt64Int64);
  takes_arguments(BuiltinId::StringFindStringString, &runtime::StringFindStringString);
  takes_arguments(BuiltinId::StringFindStringStringInt64, &runtime::StringFindStringStringInt64);
  takes_arguments(BuiltinId::StringFormat, &runtime::StringFormat);
  takes_arguments(BuiltinId::StringRegexIsMatch, &runtime::StringRegexIsMatch);
  takes_arguments(BuiltinId::StringRegexFind, &runtime::StringRegexFind);
  takes_arguments(BuiltinId::StringRegexReplace, &runtime::StringRegexReplace);
  takes_arguments(BuiltinId::StringRegexSplit, &runtime::StringRegexSplit);
  takes_arguments(BuiltinId::ResultMap, &runtime::ResultMap);
  takes_arguments(BuiltinId::ResultMapErr, &runtime::ResultMapErr);
  takes_arguments(BuiltinId::ResultAndThen, &runtime::ResultAndThen);
  takes_arguments(BuiltinId::ResultOrElse, &runtime::ResultOrElse);
  takes_arguments(BuiltinId::ResultUnwrapOr, &runtime::ResultUnwrapOr);
  takes_arguments(BuiltinId::Assert, &runtime::Assert);
  takes_arguments(BuiltinId::AssertEqual, &runtime::AssertEqual);
  takes_value(BuiltinId::MathExp, [](Value&& arg) -> Value { return runtime::MathExp(std::move(arg)); });
  takes_value(BuiltinId::MathLog2, [](Value&& arg) -> Value { return runtime::MathLog2(std::move(arg)); });
  takes_value(BuiltinId::MathLog10, [](Value&& arg) -> Value { return runtime::MathLog10(std::move(arg)); });
  takes_value(BuiltinId::MathRound, [](Value&& arg) -> Value { return runtime::MathRound(std::move(arg)); });
  takes_value(BuiltinId::MathTrunc, [](Value&& arg) -> Value { return runtime::MathTrunc(std::move(arg)); });
  takes_value(BuiltinId::MathAsin, [](Value&& arg) -> Value { return runtime::MathAsin(std::move(arg)); });
  takes_value(BuiltinId::MathAcos, [](Value&& arg) -> Value { return runtime::MathAcos(std::move(arg)); });
  takes_value(BuiltinId::MathAtan, [](Value&& arg) -> Value { return runtime::MathAtan(std::move(arg)); });
  takes_arguments(BuiltinId::MathAtan2, &runtime::MathAtan2);
  takes_arguments(BuiltinId::Min, &runtime::Min);
  takes_arguments(BuiltinId::Max, &runtime::Max);
  takes_value(BuiltinId::TupleEnumerate, [](Value&& arg) -> Value { return runtime::TupleEnumerate(std::move(arg)); });
  takes_arguments(BuiltinId::TupleSortBy, &runtime::TupleSortBy);
  takes_arguments(BuiltinId::TupleGroupBy, &runtime::TupleGroupBy);
  takes_arguments(BuiltinId::TuplePartition, &runtime::TuplePartition);
  takes_arguments(BuiltinId::TupleFlatMap, &runtime::TupleFlatMap);
  takes_arguments(BuiltinId::TupleScan, &runtime::TupleScan);
  takes_arguments(BuiltinId::TupleTakeWhile, &runtime::TupleTakeWhile);
  takes_arguments(BuiltinId::TupleDropWhile, &runtime::TupleDropWhile);
  takes_arguments(BuiltinId::DictMap, &runtime::DictMap);
  takes_arguments(BuiltinId::DictFilter, &runtime::DictFilter);
  takes_arguments(BuiltinId::DictUpdate, &runtime::DictUpdate);
  takes_arguments(BuiltinId::StringRepeat, &runtime::StringRepeat);
  takes_value(BuiltinId::StringReverse, [](Value&& arg) -> Value { return runtime::StringReverse(std::move(arg)); });
  takes_value(BuiltinId::StringChars, [](Value&& arg) -> Value { return runtime::StringChars(std::move(arg)); });
  takes_arguments(BuiltinId::TimeNow, &runtime::TimeNow);
  takes_arguments(BuiltinId::TimeMonotonic, &runtime::TimeMonotonic);
  takes_value(BuiltinId::TimeSleep, [](Value&& arg) -> Value { return runtime::TimeSleep(std::move(arg)); });
  return table;
}();

// Every builtin has the one function its kind takes, so a builtin missing from the table is a compile error.
[[nodiscard]] consteval auto builtin_implementations_are_complete() -> bool {
  for (std::size_t index = 0; index < builtin_count(); ++index) {
    const BuiltinImplementation& implementation = kBuiltinImplementations[index];
    const bool takes_arguments = builtin_takes_arguments(static_cast<BuiltinId>(index));
    if ((implementation.arguments != nullptr) != takes_arguments || (implementation.value != nullptr) == takes_arguments) {
      return false;
    }
  }
  return true;
}
static_assert(builtin_implementations_are_complete(), "every builtin has the one function its kind takes");

auto dispatch_builtin(const BuiltinId builtin_id, Value arg) -> tl::expected<Value, RuntimeError> {
  try {
    const BuiltinImplementation& implementation = kBuiltinImplementations[static_cast<std::size_t>(builtin_id)];
    if (implementation.value != nullptr) {
      return implementation.value(std::move(arg));
    }
    // An argument tuple, built at run time (a spread) or by a call through a function value.
    return implementation.arguments(runtime::Arguments{std::move(arg)});
  } catch (const std::exception& ex) {
    return tl::unexpected(error_from_exception(ex, builtin_name(builtin_id)));
  }
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
    return kBuiltinImplementations[static_cast<std::size_t>(builtin_id)].arguments(runtime::Arguments{values});
  } catch (const std::exception& ex) {
    return tl::unexpected(error_from_exception(ex, builtin_name(builtin_id)));
  }
}

}  // namespace

struct RuntimeSession::Impl {
  explicit Impl(const std::vector<std::string>& initial_process_args)
      : process_args(initial_process_args), source_path((std::filesystem::current_path() / "__repl__.fleaux").lexically_normal()) {}

  std::vector<std::string> process_args;
  std::filesystem::path source_path;
  std::vector<frontend::ir::IRLet> lets;
  std::vector<frontend::ir::IRTypeDecl> type_decls;
  std::vector<frontend::ir::IRAliasDecl> alias_decls;
};

RuntimeSession::RuntimeSession(const std::vector<std::string>& process_args) : impl_(std::in_place, process_args) {}

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

auto Runtime::create_session(const std::vector<std::string>& process_args) const -> RuntimeSession { return RuntimeSession(process_args); }

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
      arguments.push(make_builtin_function_value(*builtin_id));
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
    auto dispatched = dispatch_builtin(*builtin_id, std::move(arg));
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
