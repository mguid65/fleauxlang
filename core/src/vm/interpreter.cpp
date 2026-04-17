#include "fleaux/vm/interpreter.hpp"

#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>

#include "fleaux/common/overloaded.hpp"
#include "fleaux/frontend/ast.hpp"
#include "fleaux/frontend/analysis.hpp"
#include "fleaux/frontend/source_loader.hpp"
#include "fleaux/runtime/runtime_support.hpp"

#include "builtin_map.hpp"

namespace fleaux::vm {
namespace {

using fleaux::frontend::diag::SourceSpan;
using fleaux::frontend::ir::IRCallTarget;
using fleaux::frontend::ir::IRClosureExprBox;
using fleaux::frontend::ir::IRExpr;
using fleaux::frontend::ir::IRFlowExpr;
using fleaux::frontend::ir::IRNameRef;
using fleaux::frontend::ir::IRProgram;
using fleaux::frontend::ir::IRTupleExpr;
using fleaux::runtime::RuntimeCallable;
using fleaux::runtime::Value;

auto let_key(const std::optional<std::string>& qualifier, const std::string& name) -> std::string {
  return frontend::source_loader::symbol_key(qualifier, name);
}

enum class OperatorDispatchKey {
  kAdd,
  kSubtract,
  kMultiply,
  kDivide,
  kMod,
  kPow,
  kEqual,
  kNotEqual,
  kLessThan,
  kGreaterThan,
  kGreaterOrEqual,
  kLessOrEqual,
  kNot,
  kAnd,
  kOr,
};

auto operator_dispatch_key(const std::string& op) -> std::optional<OperatorDispatchKey> {
  static const std::unordered_map<std::string, OperatorDispatchKey> table = {
      {"+", OperatorDispatchKey::kAdd},
      {"-", OperatorDispatchKey::kSubtract},
      {"*", OperatorDispatchKey::kMultiply},
      {"/", OperatorDispatchKey::kDivide},
      {"%", OperatorDispatchKey::kMod},
      {"^", OperatorDispatchKey::kPow},
      {"==", OperatorDispatchKey::kEqual},
      {"!=", OperatorDispatchKey::kNotEqual},
      {"<", OperatorDispatchKey::kLessThan},
      {">", OperatorDispatchKey::kGreaterThan},
      {">=", OperatorDispatchKey::kGreaterOrEqual},
      {"<=", OperatorDispatchKey::kLessOrEqual},
      {"!", OperatorDispatchKey::kNot},
      {"&&", OperatorDispatchKey::kAnd},
      {"||", OperatorDispatchKey::kOr},
  };
  if (const auto it = table.find(op); it != table.end()) { return it->second; }
  return std::nullopt;
}

auto make_error(const std::string& message, const std::optional<std::string>& hint = std::nullopt,
                const std::optional<SourceSpan>& span = std::nullopt) -> InterpretError {
  return InterpretError{
      .message = message,
      .hint = hint,
      .span = span,
  };
}

auto resolve_std_callable_or_throw(const std::string& key, const std::optional<SourceSpan>& span) -> RuntimeCallable {
  const auto& map = vm_builtin_callables();
  const auto it = map.find(key);
  if (it == map.end()) { throw std::runtime_error("Unsupported VM builtin target: '" + key + "'."); }
  (void)span;
  return it->second;
}

auto parse_and_analyze_text(const std::string& source_text, const std::string& source_name)
    -> tl::expected<IRProgram, InterpretError> {
  return frontend::source_loader::parse_text_to_ir<InterpretError>(source_text, source_name, make_error);
}

auto ensure_repl_imports_supported(const IRProgram& program) -> tl::expected<void, InterpretError> {
  for (const auto& [module_name, span] : program.imports) {
    if (module_name == "Std" || module_name == "StdBuiltins") { continue; }
    return tl::unexpected(make_error("REPL only supports symbolic imports: Std, StdBuiltins.",
                                     "Define helper lets inline, or run a file for module imports.", span));
  }
  return {};
}

auto load_ir_program(const std::filesystem::path& source_file) -> tl::expected<IRProgram, InterpretError> {
  return frontend::source_loader::load_ir_program<InterpretError>(
      source_file, make_error, "Cyclic import detected while executing in VM mode.",
      "Break the cycle by moving shared definitions into a third module.");
}

struct EvalState {
  IRProgram program;
  std::unordered_map<std::string, std::size_t> let_indices;
  std::unordered_map<std::string, RuntimeCallable> functions_qualified;
  std::unordered_map<std::string, RuntimeCallable> functions_unqualified;

  void register_let(const frontend::ir::IRLet& let, const std::shared_ptr<EvalState>& self) {
    const std::string full_name = let_key(let.qualifier, let.name);

    if (const auto idx_it = let_indices.find(full_name); idx_it != let_indices.end()) {
      program.lets[idx_it->second] = let;
    } else {
      const auto idx = program.lets.size();
      program.lets.push_back(let);
      let_indices[full_name] = idx;
    }

    const RuntimeCallable callable = [state = self, full_name](Value arg) -> Value {
      return state->invoke_let_by_key(full_name, std::move(arg));
    };

    functions_qualified[full_name] = callable;
    if (!let.qualifier.has_value()) { functions_unqualified[let.name] = callable; }
  }

  auto invoke_let_by_key(const std::string& full_name, Value arg) const -> Value {
    const auto idx_it = let_indices.find(full_name);
    if (idx_it == let_indices.end()) {
      throw std::runtime_error("Unknown let in VM interpreter state: '" + full_name + "'.");
    }
    return invoke_let(program.lets[idx_it->second], std::move(arg));
  }

  auto invoke_closure(const frontend::ir::IRClosureExpr& closure,
                      const std::unordered_map<std::string, Value>& captured_locals, Value arg) const -> Value {
    std::unordered_map<std::string, Value> locals = captured_locals;
    for (const auto& [name, callable] : functions_unqualified) {
      locals.try_emplace(name, fleaux::runtime::make_callable_ref(callable));
    }

    if (!closure.params.empty()) {
      if (const bool has_variadic_tail = closure.params.back().type.variadic; !has_variadic_tail) {
        if (closure.params.size() == 1U) {
          locals[closure.params[0].name] = fleaux::runtime::unwrap_singleton_arg(std::move(arg));
        } else {
          for (std::size_t idx = 0; idx < closure.params.size(); ++idx) {
            locals[closure.params[idx].name] = fleaux::runtime::array_at(arg, idx);
          }
        }
      } else if (closure.params.size() == 1U) {
        locals[closure.params[0].name] = arg.HasArray() ? std::move(arg) : fleaux::runtime::make_tuple(std::move(arg));
      } else {
        const auto fixed_count = closure.params.size() - 1U;
        const auto& arr = fleaux::runtime::as_array(arg);
        if (arr.Size() < fixed_count) { throw std::runtime_error("too few arguments for inline closure"); }
        for (std::size_t idx = 0; idx < fixed_count; ++idx) { locals[closure.params[idx].name] = *arr.TryGet(idx); }

        fleaux::runtime::Array tail;
        tail.Reserve(arr.Size() - fixed_count);
        for (std::size_t idx = fixed_count; idx < arr.Size(); ++idx) { tail.PushBack(*arr.TryGet(idx)); }
        locals[closure.params.back().name] = Value{std::move(tail)};
      }
    }

    return eval_expr(*closure.body, locals);
  }

  auto eval_expr(const IRExpr& expr, const std::unordered_map<std::string, Value>& locals) const -> Value {
    return std::visit(
        common::overloaded{
            [&](const IRFlowExpr& flow) -> Value {
              const Value lhs = eval_expr(*flow.lhs, locals);
              const auto target = resolve_call_target(flow.rhs, locals);
              return target(lhs);
            },
            [&](const IRTupleExpr& tuple) -> Value {
              const auto& items = tuple.items;
              // Grouping semantics (Option B): single-element tuple collapses to its
              // value.  The lowerer should already have handled this, but guard here
              // defensively so the interpreter is self-consistent.
              if (items.size() == 1) { return eval_expr(*items[0], locals); }
              fleaux::runtime::Array arr;
              arr.Reserve(items.size());
              for (const auto& item : items) {
                if (const auto* name_ref = std::get_if<IRNameRef>(&item->node);
                    name_ref != nullptr && (name_ref->qualifier.has_value() || !locals.contains(name_ref->name))) {
                  arr.PushBack(fleaux::runtime::make_callable_ref(resolve_name_callable(*name_ref, locals)));
                  continue;
                }
                arr.PushBack(eval_expr(*item, locals));
              }
              return Value{std::move(arr)};
            },
            [&](const frontend::ir::IRConstant& constant) -> Value {
              return std::visit(common::overloaded{
                                    [](std::monostate) -> Value { return fleaux::runtime::make_null(); },
                                    [](bool b) -> Value { return fleaux::runtime::make_bool(b); },
                                    [](std::int64_t i) -> Value { return fleaux::runtime::make_int(i); },
                                    [](double d) -> Value { return fleaux::runtime::make_float(d); },
                                    [](const std::string& s) -> Value { return fleaux::runtime::make_string(s); },
                                },
                                constant.val);
            },
            [&](const IRNameRef& name_ref) -> Value {
              if (!name_ref.qualifier.has_value()) {
                if (const auto local_it = locals.find(name_ref.name); local_it != locals.end()) {
                  return local_it->second;
                }
                if (const auto fn_it = functions_unqualified.find(name_ref.name);
                    fn_it != functions_unqualified.end()) {
                  return fleaux::runtime::make_callable_ref(fn_it->second);
                }
                throw std::runtime_error("Unresolved name in VM interpreter: '" + name_ref.name + "'.");
              }
              const auto full_name = *name_ref.qualifier + "." + name_ref.name;
              if (name_ref.qualifier.value() == "Std" || name_ref.qualifier->starts_with("Std.")) {
                return resolve_std_callable_or_throw(full_name, name_ref.span)(fleaux::runtime::make_tuple());
              }
              if (const auto fn_it = functions_qualified.find(full_name); fn_it != functions_qualified.end()) {
                return fleaux::runtime::make_callable_ref(fn_it->second);
              }
              throw std::runtime_error("Unresolved qualified name in VM interpreter: '" + full_name + "'.");
            },
            [&](const IRClosureExprBox& closure_ptr) -> Value {
              std::unordered_map<std::string, Value> captured;
              for (const auto& capture_name : closure_ptr->captures) {
                const auto capture_it = locals.find(capture_name);
                if (capture_it == locals.end()) {
                  throw std::runtime_error("Closure capture not found in VM interpreter: '" + capture_name + "'.");
                }
                captured[capture_name] = capture_it->second;
              }

              auto callable = [state = this, closure = *closure_ptr,
                               captured = std::move(captured)](Value arg) -> Value {
                return state->invoke_closure(closure, captured, std::move(arg));
              };
              return fleaux::runtime::make_callable_ref(std::move(callable));
            },
        },
        expr.node);
  }

  auto resolve_name_callable(const IRNameRef& name, const std::unordered_map<std::string, Value>& locals) const
      -> RuntimeCallable {
    if (name.qualifier.has_value()) {
      const std::string full_name = *name.qualifier + "." + name.name;
      if (*name.qualifier == "Std" || name.qualifier->starts_with("Std.")) {
        return resolve_std_callable_or_throw(full_name, name.span);
      }

      if (const auto fn_it = functions_qualified.find(full_name); fn_it != functions_qualified.end()) {
        return fn_it->second;
      }

      throw std::runtime_error("Unknown callable target in VM interpreter: '" + full_name + "'.");
    }

    if (const auto local_it = locals.find(name.name); local_it != locals.end()) {
      const Value callable_ref = local_it->second;
      return [callable_ref](Value arg) -> Value {
        return fleaux::runtime::invoke_callable_ref(callable_ref, std::move(arg));
      };
    }

    if (const auto fn_it = functions_unqualified.find(name.name); fn_it != functions_unqualified.end()) {
      return fn_it->second;
    }

    throw std::runtime_error("Unknown unqualified callable in VM interpreter: '" + name.name + "'.");
  }

  auto resolve_call_target(const IRCallTarget& target, const std::unordered_map<std::string, Value>& locals) const
      -> RuntimeCallable {
    return std::visit(
        common::overloaded{
            [&](const frontend::ir::IROperatorRef& op_ref) -> RuntimeCallable {
              const auto& op = op_ref.op;
              const auto dispatch = operator_dispatch_key(op);
              if (!dispatch.has_value()) {
                throw std::runtime_error("Unsupported operator in VM interpreter: '" + op + "'.");
              }
              switch (*dispatch) {
                case OperatorDispatchKey::kAdd:
                  return resolve_std_callable_or_throw("Std.Add", std::nullopt);
                case OperatorDispatchKey::kSubtract:
                  return resolve_std_callable_or_throw("Std.Subtract", std::nullopt);
                case OperatorDispatchKey::kMultiply:
                  return resolve_std_callable_or_throw("Std.Multiply", std::nullopt);
                case OperatorDispatchKey::kDivide:
                  return resolve_std_callable_or_throw("Std.Divide", std::nullopt);
                case OperatorDispatchKey::kMod:
                  return resolve_std_callable_or_throw("Std.Mod", std::nullopt);
                case OperatorDispatchKey::kPow:
                  return resolve_std_callable_or_throw("Std.Pow", std::nullopt);
                case OperatorDispatchKey::kEqual:
                  return resolve_std_callable_or_throw("Std.Equal", std::nullopt);
                case OperatorDispatchKey::kNotEqual:
                  return resolve_std_callable_or_throw("Std.NotEqual", std::nullopt);
                case OperatorDispatchKey::kLessThan:
                  return resolve_std_callable_or_throw("Std.LessThan", std::nullopt);
                case OperatorDispatchKey::kGreaterThan:
                  return resolve_std_callable_or_throw("Std.GreaterThan", std::nullopt);
                case OperatorDispatchKey::kGreaterOrEqual:
                  return resolve_std_callable_or_throw("Std.GreaterOrEqual", std::nullopt);
                case OperatorDispatchKey::kLessOrEqual:
                  return resolve_std_callable_or_throw("Std.LessOrEqual", std::nullopt);
                case OperatorDispatchKey::kNot:
                  return resolve_std_callable_or_throw("Std.Not", std::nullopt);
                case OperatorDispatchKey::kAnd:
                  return resolve_std_callable_or_throw("Std.And", std::nullopt);
                case OperatorDispatchKey::kOr:
                  return resolve_std_callable_or_throw("Std.Or", std::nullopt);
              }
              throw std::runtime_error("Unsupported operator in VM interpreter: '" + op + "'.");
            },
            [&](const IRNameRef& name_ref) -> RuntimeCallable { return resolve_name_callable(name_ref, locals); },
        },
        target);
  }

  auto invoke_let(const frontend::ir::IRLet& let, Value arg) const -> Value {
    if (let.is_builtin) {
      return resolve_std_callable_or_throw(let_key(let.qualifier, let.name), let.span)(std::move(arg));
    }

    std::unordered_map<std::string, Value> locals;
    for (const auto& [fst, snd] : functions_unqualified) {
      locals.emplace(fst, fleaux::runtime::make_callable_ref(snd));
    }

    if (!let.params.empty()) {
      if (const bool has_variadic_tail = let.params.back().type.variadic; !has_variadic_tail) {
        if (let.params.size() == 1U) {
          locals[let.params[0].name] = fleaux::runtime::unwrap_singleton_arg(std::move(arg));
        } else {
          for (std::size_t idx = 0; idx < let.params.size(); ++idx) {
            locals[let.params[idx].name] = fleaux::runtime::array_at(arg, idx);
          }
        }
      } else if (let.params.size() == 1U) {
        locals[let.params[0].name] = arg.HasArray() ? std::move(arg) : fleaux::runtime::make_tuple(std::move(arg));
      } else {
        const auto fixed_count = let.params.size() - 1U;
        const auto& arr = fleaux::runtime::as_array(arg);
        if (arr.Size() < fixed_count) {
          throw std::runtime_error("too few arguments for '" + let_key(let.qualifier, let.name) + "'");
        }
        for (std::size_t idx = 0; idx < fixed_count; ++idx) { locals[let.params[idx].name] = *arr.TryGet(idx); }

        fleaux::runtime::Array tail;
        tail.Reserve(arr.Size() - fixed_count);
        for (std::size_t idx = fixed_count; idx < arr.Size(); ++idx) { tail.PushBack(*arr.TryGet(idx)); }
        locals[let.params.back().name] = Value{std::move(tail)};
      }
    }

    return eval_expr(*let.body, locals);
  }
};

auto execute_program_in_state(const std::shared_ptr<EvalState>& state, const IRProgram& program_slice)
    -> InterpretResult {
  for (const auto& let : program_slice.lets) { state->register_let(let, state); }

  try {
    for (const auto& [expr, span] : program_slice.expressions) {
      (void)span;
      (void)state->eval_expr(expr, {});
    }
  } catch (const std::exception& ex) {
    return tl::unexpected(make_error(ex.what(), "Unsupported VM coverage in current interpreter slice."));
  }

  return {};
}

}  // namespace

struct InterpreterSession::Impl {
  explicit Impl(const std::vector<std::string>& process_args) : state(std::make_shared<EvalState>()) {
    std::vector<std::string> args_storage;
    args_storage.reserve(process_args.size() + 1U);
    args_storage.emplace_back("<repl>");
    args_storage.insert(args_storage.end(), process_args.begin(), process_args.end());

    std::vector<char*> argv_ptrs;
    argv_ptrs.reserve(args_storage.size());
    for (auto& arg : args_storage) { argv_ptrs.push_back(arg.data()); }
    fleaux::runtime::set_process_args(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());
  }

  std::shared_ptr<EvalState> state;
};

InterpreterSession::InterpreterSession(const std::vector<std::string>& process_args)
    : impl_(std::make_shared<Impl>(process_args)) {}

auto InterpreterSession::run_snippet(const std::string& snippet_text) const -> InterpretResult {
  auto analyzed = parse_and_analyze_text(snippet_text, "<repl>");
  if (!analyzed) { return tl::unexpected(analyzed.error()); }

  if (auto imports_ok = ensure_repl_imports_supported(analyzed.value()); !imports_ok) {
    return tl::unexpected(imports_ok.error());
  }

  return execute_program_in_state(impl_->state, analyzed.value());
}

auto Interpreter::run_file(const std::filesystem::path& source_file, const std::vector<std::string>& process_args) const
    -> InterpretResult {
  if (!std::filesystem::exists(source_file)) {
    return tl::unexpected(make_error("Input file does not exist.", std::nullopt));
  }

  auto merged_result = load_ir_program(source_file);
  if (!merged_result) { return tl::unexpected(merged_result.error()); }

  std::vector<std::string> args_storage;
  args_storage.reserve(process_args.size() + 1U);
  args_storage.push_back(source_file.string());
  args_storage.insert(args_storage.end(), process_args.begin(), process_args.end());
  std::vector<char*> argv_ptrs;
  argv_ptrs.reserve(args_storage.size());
  for (auto& arg : args_storage) { argv_ptrs.push_back(arg.data()); }
  fleaux::runtime::set_process_args(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());

  const auto state = std::make_shared<EvalState>();
  return execute_program_in_state(state, merged_result.value());
}

auto Interpreter::create_session(const std::vector<std::string>& process_args) const -> InterpreterSession {
  return InterpreterSession(process_args);
}

}  // namespace fleaux::vm
