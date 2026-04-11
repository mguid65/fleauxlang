#include "fleaux/vm/interpreter.hpp"

#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>

#include "fleaux/frontend/ast.hpp"
#include "fleaux/frontend/lowering.hpp"
#include "fleaux/frontend/parser.hpp"
#include "fleaux/runtime/fleaux_runtime.hpp"

#include "builtin_map.hpp"

namespace fleaux::vm {
namespace {

using fleaux::frontend::diag::SourceSpan;
using fleaux::frontend::ir::IRCallTarget;
using fleaux::frontend::ir::IRExprPtr;
using fleaux::frontend::ir::IRFlowExpr;
using fleaux::frontend::ir::IRNameRef;
using fleaux::frontend::ir::IRProgram;
using fleaux::frontend::ir::IRTupleExpr;
using fleaux::runtime::RuntimeCallable;
using fleaux::runtime::Value;

std::string read_text_file(const std::filesystem::path& file) {
  std::ifstream in(file);
  if (!in) {
    return {};
  }
  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

std::string let_key(const std::optional<std::string>& qualifier, const std::string& name) {
  if (!qualifier.has_value()) {
    return name;
  }
  return *qualifier + "." + name;
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

std::optional<OperatorDispatchKey> operator_dispatch_key(const std::string& op) {
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
  if (const auto it = table.find(op); it != table.end()) {
    return it->second;
  }
  return std::nullopt;
}

InterpretError make_error(const std::string& message,
                          const std::optional<std::string>& hint = std::nullopt,
                          const std::optional<SourceSpan>& span = std::nullopt) {
  return InterpretError{
      .message = message,
      .hint = hint,
      .span = span,
  };
}

RuntimeCallable resolve_std_callable_or_throw(const std::string& key,
                                              const std::optional<SourceSpan>& span) {
  const auto& map = vm_builtin_callables();
  const auto it = map.find(key);
  if (it == map.end()) {
    throw std::runtime_error("Unsupported VM builtin target: '" + key + "'.");
  }
  (void)span;
  return it->second;
}

std::filesystem::path resolve_import_source(const std::filesystem::path& current_source,
                                            const std::string& module_name) {
  if (module_name == "Std" || module_name == "StdBuiltins") {
    return {};
  }

  if (const auto local = current_source.parent_path() / (module_name + ".fleaux"); std::filesystem::exists(local)) {
    return std::filesystem::weakly_canonical(local);
  }

  return {};
}

tl::expected<IRProgram, InterpretError> parse_and_lower(const std::filesystem::path& source_file) {
  const auto source_text = read_text_file(source_file);
  if (source_text.empty()) {
    return tl::unexpected(make_error(
        "Failed to read source file.",
        "Check the file path and ensure it is not empty."));
  }

  constexpr frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(source_text, source_file.string());
  if (!parsed) {
    return tl::unexpected(make_error(parsed.error().message, parsed.error().hint, parsed.error().span));
  }

  constexpr frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  if (!lowered) {
    return tl::unexpected(make_error(lowered.error().message, lowered.error().hint, lowered.error().span));
  }

  return lowered.value();
}

tl::expected<IRProgram, InterpretError> parse_and_lower_text(const std::string& source_text,
                                                             const std::string& source_name) {
  constexpr frontend::parse::Parser parser;
  const auto parsed = parser.parse_program(source_text, source_name);
  if (!parsed) {
    return tl::unexpected(make_error(parsed.error().message, parsed.error().hint, parsed.error().span));
  }

  constexpr frontend::lowering::Lowerer lowerer;
  const auto lowered = lowerer.lower(parsed.value());
  if (!lowered) {
    return tl::unexpected(make_error(lowered.error().message, lowered.error().hint, lowered.error().span));
  }
  return lowered.value();
}

tl::expected<void, InterpretError> ensure_repl_imports_supported(const IRProgram& program) {
  for (const auto& [module_name, span] : program.imports) {
    if (module_name == "Std" || module_name == "StdBuiltins") {
      continue;
    }
    return tl::unexpected(make_error("REPL only supports symbolic imports: Std, StdBuiltins.",
                                     "Define helper lets inline, or run a file for module imports.", span));
  }
  return {};
}

tl::expected<IRProgram, InterpretError> collect_program(
    const std::filesystem::path& source_file,
    std::unordered_map<std::string, IRProgram>& cache,
    std::unordered_set<std::string>& in_progress) {
  const std::string key = std::filesystem::weakly_canonical(source_file).string();
  if (cache.contains(key)) {
    return cache[key];
  }

  if (in_progress.contains(key)) {
    return tl::unexpected(make_error(
        "Cyclic import detected while executing in VM mode.",
        "Break the cycle by moving shared definitions into a third module."));
  }

  in_progress.insert(key);
  auto current_result = parse_and_lower(source_file);
  if (!current_result) {
    in_progress.erase(key);
    return tl::unexpected(current_result.error());
  }

  IRProgram merged = current_result.value();
  std::unordered_set<std::string> seen_symbols;
  for (const auto& let : merged.lets) {
    seen_symbols.insert(let_key(let.qualifier, let.name));
  }

  std::vector<frontend::ir::IRExprStatement> imported_exprs;
  std::vector<frontend::ir::IRLet> imported_lets;

  for (const auto&[module_name, span] : current_result->imports) {
    const auto import_source = resolve_import_source(source_file, module_name);
    if (import_source.empty()) {
      continue;
    }

    auto imported_result = collect_program(import_source, cache, in_progress);
    if (!imported_result) {
      in_progress.erase(key);
      return tl::unexpected(imported_result.error());
    }

    imported_exprs.insert(imported_exprs.end(), imported_result->expressions.begin(),
                          imported_result->expressions.end());

    for (const auto& imported_let : imported_result->lets) {
      if (const std::string sym = let_key(imported_let.qualifier, imported_let.name); seen_symbols.insert(sym).second) {
        imported_lets.push_back(imported_let);
      }
    }
  }

  merged.lets.insert(merged.lets.begin(), imported_lets.begin(), imported_lets.end());
  merged.expressions.insert(merged.expressions.begin(), imported_exprs.begin(), imported_exprs.end());

  cache[key] = merged;
  in_progress.erase(key);
  return merged;
}

template <class... Ts>
struct overloaded : Ts... { using Ts::operator()...; };
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

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
    if (!let.qualifier.has_value()) {
      functions_unqualified[let.name] = callable;
    }
  }

  Value invoke_let_by_key(const std::string& full_name, Value arg) const {
    const auto idx_it = let_indices.find(full_name);
    if (idx_it == let_indices.end()) {
      throw std::runtime_error("Unknown let in VM interpreter state: '" + full_name + "'.");
    }
    return invoke_let(program.lets[idx_it->second], std::move(arg));
  }

  Value invoke_closure(const frontend::ir::IRClosureExpr& closure,
                       const std::unordered_map<std::string, Value>& captured_locals,
                       Value arg) const {
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
        if (arr.Size() < fixed_count) {
          throw std::runtime_error("too few arguments for inline closure");
        }
        for (std::size_t idx = 0; idx < fixed_count; ++idx) {
          locals[closure.params[idx].name] = *arr.TryGet(idx);
        }

        fleaux::runtime::Array tail;
        tail.Reserve(arr.Size() - fixed_count);
        for (std::size_t idx = fixed_count; idx < arr.Size(); ++idx) {
          tail.PushBack(*arr.TryGet(idx));
        }
        locals[closure.params.back().name] = Value{std::move(tail)};
      }
    }

    return eval_expr(closure.body, locals);
  }

  Value eval_expr(const IRExprPtr& expr, const std::unordered_map<std::string, Value>& locals) const {
    if (!expr) {
      return fleaux::runtime::make_null();
    }

    return std::visit(overloaded{
      [&](const IRFlowExpr& flow) -> Value {
        const Value lhs = eval_expr(flow.lhs, locals);
        const auto target = resolve_call_target(flow.rhs, locals);
        return target(lhs);
      },
      [&](const IRTupleExpr& tuple) -> Value {
        const auto& items = tuple.items;
        // Grouping semantics (Option B): single-element tuple collapses to its
        // value.  The lowerer should already have handled this, but guard here
        // defensively so the interpreter is self-consistent.
        if (items.size() == 1) {
          return eval_expr(items[0], locals);
        }
        fleaux::runtime::Array arr;
        arr.Reserve(items.size());
        for (const auto& item : items) {
          if (item) {
            if (const auto* name_ref = std::get_if<IRNameRef>(&item->node);
                name_ref != nullptr && (name_ref->qualifier.has_value() || !locals.contains(name_ref->name))) {
              arr.PushBack(fleaux::runtime::make_callable_ref(resolve_name_callable(*name_ref, locals)));
              continue;
            }
          }
          arr.PushBack(eval_expr(item, locals));
        }
        return Value{std::move(arr)};
      },
      [&](const frontend::ir::IRConstant& constant) -> Value {
        return std::visit(overloaded{
          [](std::monostate) -> Value { return fleaux::runtime::make_null(); },
          [](bool b) -> Value { return fleaux::runtime::make_bool(b); },
          [](std::int64_t i) -> Value { return fleaux::runtime::make_int(i); },
          [](double d) -> Value { return fleaux::runtime::make_float(d); },
          [](const std::string& s) -> Value { return fleaux::runtime::make_string(s); },
        }, constant.val);
      },
      [&](const IRNameRef& name_ref) -> Value {
        if (!name_ref.qualifier.has_value()) {
          if (const auto local_it = locals.find(name_ref.name); local_it != locals.end()) {
            return local_it->second;
          }
          if (const auto fn_it = functions_unqualified.find(name_ref.name); fn_it != functions_unqualified.end()) {
            return fleaux::runtime::make_callable_ref(fn_it->second);
          }
          throw std::runtime_error("Unresolved name in VM interpreter: '" + name_ref.name + "'.");
        }
        const auto full_name = *name_ref.qualifier + "." + name_ref.name;
        if (name_ref.qualifier.value() == "Std" || name_ref.qualifier->rfind("Std.", 0) == 0) {
          return resolve_std_callable_or_throw(full_name, name_ref.span)(fleaux::runtime::make_tuple());
        }
        if (const auto fn_it = functions_qualified.find(full_name); fn_it != functions_qualified.end()) {
          return fleaux::runtime::make_callable_ref(fn_it->second);
        }
        throw std::runtime_error("Unresolved qualified name in VM interpreter: '" + full_name + "'.");
      },
      [&](const frontend::ir::IRClosureExprPtr& closure_ptr) -> Value {
        if (!closure_ptr) {
          throw std::runtime_error("Internal error: null closure expression");
        }

        std::unordered_map<std::string, Value> captured;
        for (const auto& capture_name : closure_ptr->captures) {
          const auto capture_it = locals.find(capture_name);
          if (capture_it == locals.end()) {
            throw std::runtime_error("Closure capture not found in VM interpreter: '" + capture_name + "'.");
          }
          captured[capture_name] = capture_it->second;
        }

        auto callable = [state = this, closure = *closure_ptr, captured = std::move(captured)](Value arg) -> Value {
          return state->invoke_closure(closure, captured, std::move(arg));
        };
        return fleaux::runtime::make_callable_ref(std::move(callable));
      },
    }, expr->node);
  }

  RuntimeCallable resolve_name_callable(const IRNameRef& name,
                                        const std::unordered_map<std::string, Value>& locals) const {
    if (name.qualifier.has_value()) {
      const std::string full_name = *name.qualifier + "." + name.name;
      if (*name.qualifier == "Std" || name.qualifier->rfind("Std.", 0) == 0) {
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

  RuntimeCallable resolve_call_target(const IRCallTarget& target,
                                      const std::unordered_map<std::string, Value>& locals) const {
    return std::visit(overloaded{
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
      [&](const IRNameRef& name_ref) -> RuntimeCallable {
        return resolve_name_callable(name_ref, locals);
      },
    }, target);
  }

  Value invoke_let(const frontend::ir::IRLet& let, Value arg) const {
    if (let.is_builtin) {
      return resolve_std_callable_or_throw(let_key(let.qualifier, let.name), let.span)(std::move(arg));
    }

    std::unordered_map<std::string, Value> locals;
    for (const auto&[fst, snd] : functions_unqualified) {
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
        for (std::size_t idx = 0; idx < fixed_count; ++idx) {
          locals[let.params[idx].name] = *arr.TryGet(idx);
        }

        fleaux::runtime::Array tail;
        tail.Reserve(arr.Size() - fixed_count);
        for (std::size_t idx = fixed_count; idx < arr.Size(); ++idx) {
          tail.PushBack(*arr.TryGet(idx));
        }
        locals[let.params.back().name] = Value{std::move(tail)};
      }
    }

    return eval_expr(let.body, locals);
  }
};

InterpretResult execute_program_in_state(const std::shared_ptr<EvalState>& state, const IRProgram& program_slice) {
  for (const auto& let : program_slice.lets) {
    state->register_let(let, state);
  }

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
  explicit Impl(const std::vector<std::string>& process_args)
      : state(std::make_shared<EvalState>()) {
    std::vector<std::string> args_storage;
    args_storage.reserve(process_args.size() + 1U);
    args_storage.push_back("<repl>");
    args_storage.insert(args_storage.end(), process_args.begin(), process_args.end());

    std::vector<char*> argv_ptrs;
    argv_ptrs.reserve(args_storage.size());
    for (auto& arg : args_storage) {
      argv_ptrs.push_back(arg.data());
    }
    fleaux::runtime::set_process_args(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());
  }

  std::shared_ptr<EvalState> state;
};

InterpreterSession::InterpreterSession(const std::vector<std::string>& process_args)
    : impl_(std::make_shared<Impl>(process_args)) {}

InterpretResult InterpreterSession::run_snippet(const std::string& snippet_text) const {
  auto lowered = parse_and_lower_text(snippet_text, "<repl>");
  if (!lowered) {
    return tl::unexpected(lowered.error());
  }

  if (auto imports_ok = ensure_repl_imports_supported(lowered.value()); !imports_ok) {
    return tl::unexpected(imports_ok.error());
  }

  return execute_program_in_state(impl_->state, lowered.value());
}

InterpretResult Interpreter::run_file(const std::filesystem::path& source_file,
                                      const std::vector<std::string>& process_args) const {
  if (!std::filesystem::exists(source_file)) {
    return tl::unexpected(make_error("Input file does not exist.", std::nullopt));
  }

  std::unordered_map<std::string, IRProgram> cache;
  std::unordered_set<std::string> in_progress;
  auto merged_result = collect_program(source_file, cache, in_progress);
  if (!merged_result) {
    return tl::unexpected(merged_result.error());
  }

  std::vector<std::string> args_storage;
  args_storage.reserve(process_args.size() + 1U);
  args_storage.push_back(source_file.string());
  args_storage.insert(args_storage.end(), process_args.begin(), process_args.end());
  std::vector<char*> argv_ptrs;
  argv_ptrs.reserve(args_storage.size());
  for (auto& arg : args_storage) {
    argv_ptrs.push_back(arg.data());
  }
  fleaux::runtime::set_process_args(static_cast<int>(argv_ptrs.size()), argv_ptrs.data());

  const auto state = std::make_shared<EvalState>();
  return execute_program_in_state(state, merged_result.value());
}

InterpreterSession Interpreter::create_session(const std::vector<std::string>& process_args) const {
  return InterpreterSession(process_args);
}

}  // namespace fleaux::vm

