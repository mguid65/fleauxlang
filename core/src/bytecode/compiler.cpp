#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>

#include "fleaux/bytecode/compiler.hpp"
#include "fleaux/common/overloaded.hpp"

namespace fleaux::bytecode {
namespace {

using namespace frontend::ir;

// Operator helpers

auto operator_to_builtin() -> const std::unordered_map<std::string, std::string>& {
  static const std::unordered_map<std::string, std::string> map = {
      {"+", "Std.Add"},
      {"-", "Std.Subtract"},
      {"*", "Std.Multiply"},
      {"/", "Std.Divide"},
      {"%", "Std.Mod"},
      {"^", "Std.Pow"},
      {"==", "Std.Equal"},
      {"!=", "Std.NotEqual"},
      {"<", "Std.LessThan"},
      {">", "Std.GreaterThan"},
      {">=", "Std.GreaterOrEqual"},
      {"<=", "Std.LessOrEqual"},
      {"!", "Std.Not"},
      {"&&", "Std.And"},
      {"||", "Std.Or"},
  };
  return map;
}

auto unary_operator_to_opcode(const std::string& op) -> std::optional<Opcode> {
  if (op == "!") return Opcode::kNot;
  if (op == "-") return Opcode::kNeg;
  return std::nullopt;
}

auto binary_operator_to_opcode(const std::string& op) -> std::optional<Opcode> {
  if (op == "+") return Opcode::kAdd;
  if (op == "-") return Opcode::kSub;
  if (op == "*") return Opcode::kMul;
  if (op == "/") return Opcode::kDiv;
  if (op == "%") return Opcode::kMod;
  if (op == "^") return Opcode::kPow;
  if (op == "==") return Opcode::kCmpEq;
  if (op == "!=") return Opcode::kCmpNe;
  if (op == "<") return Opcode::kCmpLt;
  if (op == ">") return Opcode::kCmpGt;
  if (op == "<=") return Opcode::kCmpLe;
  if (op == ">=") return Opcode::kCmpGe;
  if (op == "&&") return Opcode::kAnd;
  if (op == "||") return Opcode::kOr;
  return std::nullopt;
}

// Compile state

struct CompileState {
  // Builtin name -> index into Module::builtin_names (deduplicated).
  std::unordered_map<std::string, std::uint32_t> builtin_idx;
  // Fully-qualified function name -> index into Module::functions.
  std::unordered_map<std::string, std::uint32_t> function_idx;
  // Maps a short/full name to the stdlib builtin it aliases (from is_builtin lets).
  std::unordered_map<std::string, std::string> builtin_alias;

  auto intern_builtin(Module& bytecode_module, const std::string& name) -> std::uint32_t {
    if (const auto it = builtin_idx.find(name); it != builtin_idx.end()) return it->second;
    const auto idx = static_cast<std::uint32_t>(bytecode_module.builtin_names.size());
    bytecode_module.builtin_names.push_back(name);
    builtin_idx[name] = idx;
    return idx;
  }

  static auto intern_const(Module& bytecode_module, ConstValue c) -> std::uint32_t {
    const auto idx = static_cast<std::uint32_t>(bytecode_module.constants.size());
    bytecode_module.constants.push_back(std::move(c));
    return idx;
  }
};

auto make_err(const std::string& msg) -> CompileError { return CompileError{.message = msg}; }

using EmitResult = tl::expected<void, CompileError>;
using LocalSlots = std::unordered_map<std::string, std::uint32_t>;

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

auto hash_text(const std::string& text) -> std::uint64_t {
  std::uint64_t hash = kFnvOffsetBasis;
  for (const unsigned char ch : text) {
    hash ^= static_cast<std::uint64_t>(ch);
    hash *= kFnvPrime;
  }
  return hash;
}

auto full_symbol_name(const std::optional<std::string>& qualifier, const std::string& name) -> std::string {
  return qualifier.has_value() ? (*qualifier + "." + name) : name;
}

auto is_symbolic_import(const std::string& module_name) -> bool {
  return module_name == "Std" || module_name == "StdBuiltins";
}

auto let_belongs_to_module(const IRLet& let, const CompileOptions& options) -> bool {
  if (!options.source_path.has_value()) { return true; }
  if (!let.span.has_value()) { return true; }
  return let.span->source_name == options.source_path->string();
}

void seed_imported_exports(Module& bytecode_module, CompileState& state, const CompileOptions& options) {
  for (const Module* imported_module : options.imported_modules) {
    if (imported_module == nullptr) { continue; }
    for (const auto& symbol : imported_module->exports) {
      if (symbol.kind == ExportKind::kBuiltinAlias) {
        state.builtin_alias.emplace(symbol.name, symbol.builtin_name);
        continue;
      }

      if (state.function_idx.contains(symbol.name)) { continue; }
      if (symbol.index >= imported_module->functions.size()) { continue; }

      const auto& imported_fn = imported_module->functions[symbol.index];
      const auto fn_idx = static_cast<std::uint32_t>(bytecode_module.functions.size());
      bytecode_module.functions.push_back(FunctionDef{
          .name = symbol.name,
          .arity = imported_fn.arity,
          .has_variadic_tail = imported_fn.has_variadic_tail,
          .is_import_placeholder = true,
          .instructions = {},
      });
      state.function_idx[symbol.name] = fn_idx;
    }
  }
}

// Forward declaration.
auto emit_expr(const IRExpr& expr, std::vector<Instruction>& out, const LocalSlots& locals, CompileState& state,
               Module& bytecode_module) -> EmitResult;

auto count_inline_closures(const IRExpr& expr) -> std::size_t {
  return std::visit(
      common::overloaded{[](const IRFlowExpr& flow) -> std::size_t { return count_inline_closures(*flow.lhs); },
                         [](const IRTupleExpr& tuple) -> std::size_t {
                           std::size_t total = 0;
                           for (const auto& item : tuple.items) { total += count_inline_closures(*item); }
                           return total;
                         },
                         [](const IRClosureExprBox& closure_ptr) -> std::size_t {
                           return 1U + count_inline_closures(*closure_ptr->body);
                         },
                         [](auto&&) -> std::size_t { return 0; }},
      expr.node);
}

auto emit_operator_flow_expr(const IRExpr& lhs, const IROperatorRef& op_ref, std::vector<Instruction>& out,
                             const LocalSlots& locals, CompileState& state, Module& bytecode_module) -> EmitResult {
  const auto emit_builtin_fallback = [&]() -> EmitResult {
    if (auto emit_result = emit_expr(lhs, out, locals, state, bytecode_module); !emit_result) return emit_result;

    const auto& opmap = operator_to_builtin();
    const auto it = opmap.find(op_ref.op);
    if (it == opmap.end()) {
      return tl::unexpected(make_err("Unsupported operator in bytecode compiler: '" + op_ref.op + "'."));
    }
    const auto idx = state.intern_builtin(bytecode_module, it->second);
    out.push_back(Instruction{.opcode = Opcode::kCallBuiltin, .operand = static_cast<std::int64_t>(idx)});
    return {};
  };

  if (const auto* tuple = std::get_if<IRTupleExpr>(&lhs.node); tuple != nullptr) {
    if (tuple->items.size() == 2) {
      if (const auto opcode = binary_operator_to_opcode(op_ref.op)) {
        if (auto emit_result = emit_expr(*tuple->items[0], out, locals, state, bytecode_module); !emit_result) {
          return emit_result;
        }
        if (auto emit_result = emit_expr(*tuple->items[1], out, locals, state, bytecode_module); !emit_result) {
          return emit_result;
        }
        out.push_back(Instruction{.opcode = *opcode, .operand = 0});
        return {};
      }
    }

    if (tuple->items.size() == 1) {
      if (const auto opcode = unary_operator_to_opcode(op_ref.op)) {
        if (auto emit_result = emit_expr(*tuple->items[0], out, locals, state, bytecode_module); !emit_result) {
          return emit_result;
        }
        out.push_back(Instruction{.opcode = *opcode, .operand = 0});
        return {};
      }
    }
  } else if (const auto opcode = unary_operator_to_opcode(op_ref.op)) {
    if (auto emit_result = emit_expr(lhs, out, locals, state, bytecode_module); !emit_result) { return emit_result; }
    out.push_back(Instruction{.opcode = *opcode, .operand = 0});
    return {};
  }

  return emit_builtin_fallback();
}

auto expr_is_known_user_function_ref(const IRExpr& expr, const CompileState& state) -> bool {
  return std::visit(
      common::overloaded{
          [&](const IRNameRef& name_ref) -> bool {
            const std::string full_name =
                name_ref.qualifier.has_value() ? (*name_ref.qualifier + "." + name_ref.name) : name_ref.name;
            return state.function_idx.contains(full_name);
          },
          [](const auto&) -> bool { return false; }},
      expr.node);
}

auto expr_is_known_builtin_function_ref(const IRExpr& expr, const CompileState& state) -> bool {
  return std::visit(
      common::overloaded{
          [&](const IRNameRef& name_ref) -> bool {
            const std::string full_name =
                name_ref.qualifier.has_value() ? (*name_ref.qualifier + "." + name_ref.name) : name_ref.name;

            if (name_ref.qualifier.has_value() &&
                (*name_ref.qualifier == "Std" || name_ref.qualifier->starts_with("Std."))) {
              return true;
            }
            return state.builtin_alias.contains(full_name);
          },
          [](const auto&) -> bool { return false; }},
      expr.node);
}

auto expr_is_known_callable_ref(const IRExpr& expr, const CompileState& state) -> bool {
  return expr_is_known_user_function_ref(expr, state) || expr_is_known_builtin_function_ref(expr, state);
}

auto can_emit_native_branch_call(const std::string& full_name, const IRTupleExpr& tuple, const CompileState& state)
    -> bool {
  if (full_name != "Std.Branch" || tuple.items.size() != 4) { return false; }
  // Keep this strict: only lower when both function positions are known
  // callable symbols. Callable locals still use builtin dispatch.
  return expr_is_known_callable_ref(*tuple.items[2], state) && expr_is_known_callable_ref(*tuple.items[3], state);
}

// Call-target emission

auto emit_call_target(const IRCallTarget& target, std::vector<Instruction>& out, const LocalSlots& locals,
                      CompileState& state, Module& bytecode_module) -> EmitResult {
  (void)locals;
  // Helper: emit kCallBuiltin with an interned name.
  auto emit_builtin = [&](const std::string& name) -> EmitResult {
    const auto idx = state.intern_builtin(bytecode_module, name);
    out.push_back(Instruction{.opcode = Opcode::kCallBuiltin, .operand = static_cast<std::int64_t>(idx)});
    return {};
  };

  return std::visit(
      common::overloaded{
          [&](const IROperatorRef& op_ref) -> EmitResult {
            // Operator shorthand falls back to builtin dispatch here when a native opcode
            // form is not selected by emit_operator_flow_expr().
            const auto& opmap = operator_to_builtin();
            const auto it = opmap.find(op_ref.op);
            if (it == opmap.end()) {
              return tl::unexpected(make_err("Unsupported operator in bytecode compiler: '" + op_ref.op + "'."));
            }
            return emit_builtin(it->second);
          },
          [&](const IRNameRef& name_ref) -> EmitResult {
            const std::string full_name =
                name_ref.qualifier.has_value() ? (*name_ref.qualifier + "." + name_ref.name) : name_ref.name;

            // Qualified Std.* builtin (e.g. Std.Add, Std.Math.Floor).
            if (name_ref.qualifier.has_value() &&
                (*name_ref.qualifier == "Std" || name_ref.qualifier->starts_with("Std."))) {
              return emit_builtin(full_name);
            }

            // User-defined function (check by full name first, then short name).
            if (const auto fn_it = state.function_idx.find(full_name); fn_it != state.function_idx.end()) {
              out.push_back(
                  Instruction{.opcode = Opcode::kCallUserFunc, .operand = static_cast<std::int64_t>(fn_it->second)});
              return {};
            }

            // Builtin alias introduced by an is_builtin let (unqualified import).
            if (const auto alias_it = state.builtin_alias.find(full_name); alias_it != state.builtin_alias.end()) {
              return emit_builtin(alias_it->second);
            }

            return tl::unexpected(make_err("Unsupported call target in bytecode compiler: '" + full_name + "'."));
          }},
      target);
}

// Expression emission

auto emit_expr(const IRExpr& expr, std::vector<Instruction>& out, const LocalSlots& locals, CompileState& state,
               Module& bytecode_module) -> EmitResult {
  const auto emit_tuple_items = [&](const IRTupleExpr& tuple_expr) -> EmitResult {
    for (const auto& item : tuple_expr.items) {
      if (auto emit_result = emit_expr(*item, out, locals, state, bytecode_module); !emit_result) {
        return emit_result;
      }
    }
    return {};
  };

  return std::visit(
      common::overloaded{
          [&](const IRConstant& constant) -> EmitResult {
            const auto idx = CompileState::intern_const(bytecode_module, ConstValue{constant.val});
            out.push_back(Instruction{.opcode = Opcode::kPushConst, .operand = static_cast<std::int64_t>(idx)});
            return {};
          },
          [&](const IRNameRef& name_ref) -> EmitResult {
            const std::string full_name =
                name_ref.qualifier.has_value() ? (*name_ref.qualifier + "." + name_ref.name) : name_ref.name;

            if (!name_ref.qualifier.has_value()) {
              if (const auto it = locals.find(name_ref.name); it != locals.end()) {
                out.push_back(Instruction{.opcode = Opcode::kLoadLocal, .operand = static_cast<std::int64_t>(it->second)});
                return {};
              }
            }

            if (const auto fn_it = state.function_idx.find(full_name); fn_it != state.function_idx.end()) {
              out.push_back(
                  Instruction{.opcode = Opcode::kMakeUserFuncRef, .operand = static_cast<std::int64_t>(fn_it->second)});
              return {};
            }

            if (name_ref.qualifier.has_value() &&
                (*name_ref.qualifier == "Std" || name_ref.qualifier->starts_with("Std."))) {
              const auto idx = state.intern_builtin(bytecode_module, full_name);
              out.push_back(
                  Instruction{.opcode = Opcode::kMakeBuiltinFuncRef, .operand = static_cast<std::int64_t>(idx)});
              return {};
            }

            if (const auto alias_it = state.builtin_alias.find(full_name); alias_it != state.builtin_alias.end()) {
              const auto idx = state.intern_builtin(bytecode_module, alias_it->second);
              out.push_back(
                  Instruction{.opcode = Opcode::kMakeBuiltinFuncRef, .operand = static_cast<std::int64_t>(idx)});
              return {};
            }

            return tl::unexpected(
                make_err("Name '" + full_name + "' used as a value is not supported in the bytecode compiler."));
          },
          [&](const IRClosureExprBox& closure_ptr) -> EmitResult {
            const auto& closure = *closure_ptr;

            const auto fn_idx = static_cast<std::uint32_t>(bytecode_module.functions.size());
            FunctionDef fn_def;
            fn_def.name = "__closure_" + std::to_string(fn_idx);
            fn_def.arity = static_cast<std::uint32_t>(closure.captures.size() + closure.params.size());
            fn_def.has_variadic_tail = !closure.params.empty() && closure.params.back().type.variadic;

            LocalSlots closure_locals;
            std::uint32_t slot = 0;
            for (const auto& capture_name : closure.captures) { closure_locals[capture_name] = slot++; }
            for (const auto& param : closure.params) { closure_locals[param.name] = slot++; }

            if (auto emit_result = emit_expr(*closure.body, fn_def.instructions, closure_locals, state, bytecode_module);
                !emit_result) {
              return tl::unexpected(emit_result.error());
            }
            fn_def.instructions.push_back(Instruction{.opcode = Opcode::kReturn, .operand = 0});
            bytecode_module.functions.push_back(std::move(fn_def));

            const auto closure_idx = static_cast<std::uint32_t>(bytecode_module.closures.size());
            bytecode_module.closures.push_back(ClosureDef{
                .function_index = fn_idx,
                .capture_count = static_cast<std::uint32_t>(closure.captures.size()),
                .declared_arity = static_cast<std::uint32_t>(closure.params.size()),
                .declared_has_variadic_tail = !closure.params.empty() && closure.params.back().type.variadic,
            });

            for (const auto& capture_name : closure.captures) {
              const auto local_it = locals.find(capture_name);
              if (local_it == locals.end()) {
                return tl::unexpected(
                    make_err("Unsupported closure capture in bytecode compiler: '" + capture_name + "'."));
              }
              out.push_back(
                  Instruction{.opcode = Opcode::kLoadLocal, .operand = static_cast<std::int64_t>(local_it->second)});
            }
            out.push_back(
                Instruction{.opcode = Opcode::kBuildTuple, .operand = static_cast<std::int64_t>(closure.captures.size())});
            out.push_back(
                Instruction{.opcode = Opcode::kMakeClosureRef, .operand = static_cast<std::int64_t>(closure_idx)});
            return {};
          },
          [&](const IRTupleExpr& tuple_expr) -> EmitResult {
            if (tuple_expr.items.size() == 1) {
              return emit_expr(*tuple_expr.items[0], out, locals, state, bytecode_module);
            }
            if (auto emit_result = emit_tuple_items(tuple_expr); !emit_result) { return emit_result; }
            out.push_back(
                Instruction{.opcode = Opcode::kBuildTuple, .operand = static_cast<std::int64_t>(tuple_expr.items.size())});
            return {};
          },
          [&](const IRFlowExpr& flow) -> EmitResult {
            auto handled = std::visit(
                common::overloaded{
                    [&](const IROperatorRef& op_ref) -> tl::expected<bool, CompileError> {
                      if (auto emit_result = emit_operator_flow_expr(*flow.lhs, op_ref, out, locals, state,
                                                                     bytecode_module);
                          !emit_result) {
                        return tl::unexpected(emit_result.error());
                      }
                      return true;
                    },
                    [&](const IRNameRef& name_ref) -> tl::expected<bool, CompileError> {
                      if (const auto* tuple = std::get_if<IRTupleExpr>(&flow.lhs->node); tuple != nullptr) {
                        const std::string full_name = name_ref.qualifier.has_value()
                                                          ? (*name_ref.qualifier + "." + name_ref.name)
                                                          : name_ref.name;

                        const auto emit_fast_path = [&](const bool matched,
                                                        const Opcode opcode) -> tl::expected<bool, CompileError> {
                          if (!matched) { return false; }
                          if (auto emit_result = emit_tuple_items(*tuple); !emit_result) {
                            return tl::unexpected(emit_result.error());
                          }
                          out.push_back(Instruction{.opcode = opcode, .operand = 0});
                          return true;
                        };

                        if (auto fast_path = emit_fast_path(full_name == "Std.Select" && tuple->items.size() == 3 &&
                                                                !expr_is_known_user_function_ref(*tuple->items[1],
                                                                                                state) &&
                                                                !expr_is_known_user_function_ref(*tuple->items[2],
                                                                                                state),
                                                            Opcode::kSelect);
                            !fast_path) {
                          return tl::unexpected(fast_path.error());
                        } else if (*fast_path) {
                          return true;
                        }

                        if (auto fast_path = emit_fast_path(can_emit_native_branch_call(full_name, *tuple, state),
                                                            Opcode::kBranchCall);
                            !fast_path) {
                          return tl::unexpected(fast_path.error());
                        } else if (*fast_path) {
                          return true;
                        }

                        if (auto fast_path = emit_fast_path(full_name == "Std.Loop" && tuple->items.size() == 3,
                                                            Opcode::kLoopCall);
                            !fast_path) {
                          return tl::unexpected(fast_path.error());
                        } else if (*fast_path) {
                          return true;
                        }

                        if (auto fast_path = emit_fast_path(full_name == "Std.LoopN" && tuple->items.size() == 4,
                                                            Opcode::kLoopNCall);
                            !fast_path) {
                          return tl::unexpected(fast_path.error());
                        } else if (*fast_path) {
                          return true;
                        }
                      }
                      return false;
                    }},
                flow.rhs);

            if (!handled) {
              return tl::unexpected(handled.error());
            }
            if (*handled) {
              return {};
            }

            if (auto emit_result = emit_expr(*flow.lhs, out, locals, state, bytecode_module); !emit_result) {
              return emit_result;
            }
            return emit_call_target(flow.rhs, out, locals, state, bytecode_module);
          },
          [&](auto&&) -> EmitResult {
            return tl::unexpected(make_err("Unsupported IR expression type in bytecode compiler."));
          }},
      expr.node);
}

}  // namespace

// BytecodeCompiler::compile

auto BytecodeCompiler::compile(const IRProgram& program, const CompileOptions& options) const -> CompileResult {
  Module bytecode_module;
  CompileState state;

  bytecode_module.header.module_name = options.module_name.value_or(
      options.source_path.has_value() ? options.source_path->stem().string() : std::string{});
  bytecode_module.header.source_path = options.source_path.has_value() ? options.source_path->string() : std::string{};
  bytecode_module.header.source_hash = options.source_text.has_value() ? hash_text(*options.source_text) : 0;

  std::unordered_set<std::string> seen_dependencies;
  for (const auto& [module_name, _span] : program.imports) {
    if (!seen_dependencies.insert(module_name).second) { continue; }
    bytecode_module.dependencies.push_back(ModuleDependency{
        .module_name = module_name,
        .is_symbolic = is_symbolic_import(module_name),
    });
  }

  seed_imported_exports(bytecode_module, state, options);

  // Pass 1: register all let definitions
  // is_builtin lets become aliases; user-defined lets populate the function table.
  for (const auto& let : program.lets) {
    const std::string full_name = full_symbol_name(let.qualifier, let.name);

    if (let.is_builtin) {
      // Builtins are addressable by their declared symbol only.
      // Qualified builtins (e.g. Std.Println) must remain qualified.
      state.builtin_alias[full_name] = full_name;
      if (let_belongs_to_module(let, options)) {
        bytecode_module.exports.push_back(ExportedSymbol{
            .name = full_name,
            .kind = ExportKind::kBuiltinAlias,
            .index = 0,
            .builtin_name = full_name,
        });
      }
      continue;
    }

    const auto fn_idx = static_cast<std::uint32_t>(bytecode_module.functions.size());
    FunctionDef fn_def;
    fn_def.name = full_name;
    fn_def.arity = static_cast<std::uint32_t>(let.params.size());
    fn_def.has_variadic_tail = !let.params.empty() && let.params.back().type.variadic;
    bytecode_module.functions.push_back(std::move(fn_def));

    // Register by full name (always).
    state.function_idx[full_name] = fn_idx;
    // Only unqualified declarations are addressable by short name.
    if (!let.qualifier.has_value()) { state.function_idx[let.name] = fn_idx; }
    if (let_belongs_to_module(let, options)) {
      bytecode_module.exports.push_back(ExportedSymbol{
          .name = full_name,
          .kind = ExportKind::kFunction,
          .index = fn_idx,
          .builtin_name = {},
      });
    }
  }

  // Inline closures compile to synthetic functions; reserve once so references
  // to existing function instruction vectors stay valid while emitting code.
  std::size_t closure_count = 0;
  for (const auto& let : program.lets) {
    if (!let.is_builtin && let.body.has_value()) { closure_count += count_inline_closures(*let.body); }
  }
  for (const auto& [expr, span] : program.expressions) {
    (void)span;
    closure_count += count_inline_closures(expr);
  }
  bytecode_module.functions.reserve(bytecode_module.functions.size() + closure_count);
  bytecode_module.closures.reserve(closure_count);

  // Pass 2: compile function bodies
  std::size_t fn_slot = 0;
  for (const auto& let : program.lets) {
    if (let.is_builtin) continue;

    const std::string full_name = full_symbol_name(let.qualifier, let.name);

    // Map parameter names to local slots.
    LocalSlots local_slots;
    for (std::uint32_t param_index = 0; param_index < static_cast<std::uint32_t>(let.params.size()); ++param_index) {
      local_slots[let.params[param_index].name] = param_index;
    }

    const auto current_fn_idx = state.function_idx.at(full_name);
    if (auto emit_result = emit_expr(*let.body, bytecode_module.functions[current_fn_idx].instructions, local_slots,
                                     state, bytecode_module);
        !emit_result) {
      return tl::unexpected(emit_result.error());
    }
    bytecode_module.functions[current_fn_idx].instructions.push_back(
        Instruction{.opcode = Opcode::kReturn, .operand = 0});
    ++fn_slot;
  }

  // Pass 3: compile top-level expression statements
  for (const auto& [expr, span] : program.expressions) {
    if (auto emit_result = emit_expr(expr, bytecode_module.instructions, {}, state, bytecode_module); !emit_result) {
      return tl::unexpected(emit_result.error());
    }
    // Expression statements discard their result.
    bytecode_module.instructions.push_back(Instruction{.opcode = Opcode::kPop, .operand = 0});
  }

  bytecode_module.instructions.push_back(Instruction{.opcode = Opcode::kHalt, .operand = 0});
  return bytecode_module;
}

}  // namespace fleaux::bytecode
