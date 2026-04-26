#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>

#include "fleaux/bytecode/compiler.hpp"
#include "fleaux/bytecode/escape_analyzer.hpp"
#include "fleaux/common/overloaded.hpp"
#include "fleaux/vm/builtin_catalog.hpp"

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
  // Exact internal function key -> index into Module::functions.
  std::unordered_map<std::string, std::uint32_t> function_idx;
  std::unordered_set<std::string> ambiguous_function_names;
  // Maps a short/full name to the stdlib builtin it aliases (from is_builtin lets).
  std::unordered_map<std::string, std::string> builtin_alias;
  bool enable_auto_value_ref{false};
  std::unordered_map<std::uint32_t, std::unordered_set<std::uint32_t>> by_ref_param_slots;
  std::optional<std::uint32_t> current_function_idx;

  static auto intern_const(Module& bytecode_module, ConstValue c) -> std::uint32_t {
    const auto idx = static_cast<std::uint32_t>(bytecode_module.constants.size());
    bytecode_module.constants.push_back(std::move(c));
    return idx;
  }

  auto register_public_function_name(const std::string& name, const std::uint32_t fn_idx) -> void {
    if (ambiguous_function_names.contains(name)) { return; }
    if (const auto existing = function_idx.find(name); existing != function_idx.end()) {
      function_idx.erase(existing);
      ambiguous_function_names.insert(name);
      return;
    }
    function_idx[name] = fn_idx;
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

auto internal_symbol_name(const IRLet& let) -> std::string {
  if (!let.symbol_key.empty()) { return let.symbol_key; }
  return full_symbol_name(let.qualifier, let.name);
}

auto target_symbol_name(const IRNameRef& name_ref) -> std::string {
  if (name_ref.resolved_symbol_key.has_value()) { return *name_ref.resolved_symbol_key; }
  return full_symbol_name(name_ref.qualifier, name_ref.name);
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

      const auto link_name = symbol.link_name.empty() ? symbol.name : symbol.link_name;
      if (state.function_idx.contains(link_name)) { continue; }
      if (symbol.index >= imported_module->functions.size()) { continue; }

      const auto& imported_fn = imported_module->functions[symbol.index];
      const auto fn_idx = static_cast<std::uint32_t>(bytecode_module.functions.size());
      bytecode_module.functions.push_back(FunctionDef{
          .name = link_name,
          .arity = imported_fn.arity,
          .has_variadic_tail = imported_fn.has_variadic_tail,
          .is_import_placeholder = true,
          .instructions = {},
      });
      state.function_idx[link_name] = fn_idx;
      state.register_public_function_name(symbol.name, fn_idx);
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
  const auto emit_builtin_call = [&]() -> EmitResult {
    if (auto emit_result = emit_expr(lhs, out, locals, state, bytecode_module); !emit_result) return emit_result;

    const auto& opmap = operator_to_builtin();
    const auto it = opmap.find(op_ref.op);
    if (it == opmap.end()) {
      return tl::unexpected(make_err("Unsupported operator in bytecode compiler: '" + op_ref.op + "'."));
    }
    const auto builtin_id = fleaux::vm::builtin_id_from_name(it->second);
    if (!builtin_id.has_value()) {
      return tl::unexpected(make_err("Unknown builtin in bytecode compiler: '" + it->second + "'."));
    }
    out.push_back(Instruction{.opcode = Opcode::kCallBuiltin, .operand = fleaux::vm::builtin_operand(*builtin_id)});
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

  return emit_builtin_call();
}

auto expr_is_known_user_function_ref(const IRExpr& expr, const CompileState& state) -> bool {
  return std::visit(common::overloaded{[&](const IRNameRef& name_ref) -> bool {
                                         return state.function_idx.contains(target_symbol_name(name_ref));
                                       },
                                       [](const auto&) -> bool { return false; }},
                    expr.node);
}

auto expr_is_known_builtin_function_ref(const IRExpr& expr, const CompileState& state) -> bool {
  return std::visit(
      common::overloaded{[&](const IRNameRef& name_ref) -> bool {
                           const std::string full_name = name_ref.qualifier.has_value()
                                                             ? (*name_ref.qualifier + "." + name_ref.name)
                                                             : name_ref.name;

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

auto user_function_index_from_target(const IRCallTarget& target, const CompileState& state)
    -> std::optional<std::uint32_t> {
  return std::visit(
      common::overloaded{[&](const IRNameRef& name_ref) -> std::optional<std::uint32_t> {
                           if (const auto it = state.function_idx.find(target_symbol_name(name_ref));
                               it != state.function_idx.end()) {
                             return it->second;
                           }
                           return std::nullopt;
                         },
                         [](const IROperatorRef&) -> std::optional<std::uint32_t> { return std::nullopt; }},
      target);
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
  // Helper: emit kCallBuiltin with a resolved BuiltinId.
  auto emit_builtin = [&](const std::string& name) -> EmitResult {
    const auto builtin_id = fleaux::vm::builtin_id_from_name(name);
    if (!builtin_id.has_value()) {
      return tl::unexpected(make_err("Unknown builtin in bytecode compiler: '" + name + "'."));
    }
    out.push_back(Instruction{.opcode = Opcode::kCallBuiltin, .operand = fleaux::vm::builtin_operand(*builtin_id)});
    return {};
  };

  return std::visit(
      common::overloaded{
          [&](const IROperatorRef& op_ref) -> EmitResult {
            // Operator shorthand routes through builtin dispatch here when a native opcode
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
            const std::string target_name = target_symbol_name(name_ref);

            // Qualified Std.* builtin (e.g. Std.Add, Std.Math.Floor).
            if (name_ref.qualifier.has_value() &&
                (*name_ref.qualifier == "Std" || name_ref.qualifier->starts_with("Std."))) {
              return emit_builtin(full_name);
            }

            // User-defined function (check by full name first, then short name).
            if (const auto fn_it = state.function_idx.find(target_name); fn_it != state.function_idx.end()) {
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

auto emit_lhs_for_user_call_with_auto_value_ref(const IRExpr& lhs, const std::uint32_t function_index,
                                                std::vector<Instruction>& out, const LocalSlots& locals,
                                                CompileState& state, Module& bytecode_module)
    -> tl::expected<bool, CompileError> {
  if (!state.enable_auto_value_ref) { return false; }

  const auto by_ref_it = state.by_ref_param_slots.find(function_index);
  if (by_ref_it == state.by_ref_param_slots.end() || by_ref_it->second.empty()) { return false; }

  if (function_index >= bytecode_module.functions.size()) { return false; }
  const auto& fn_def = bytecode_module.functions[function_index];
  if (fn_def.is_import_placeholder || fn_def.has_variadic_tail) { return false; }

  if (fn_def.arity == 1) {
    if (auto emit_result = emit_expr(lhs, out, locals, state, bytecode_module); !emit_result) {
      return tl::unexpected(emit_result.error());
    }
    if (by_ref_it->second.contains(0)) { out.push_back(Instruction{.opcode = Opcode::kMakeValueRef, .operand = 0}); }
    return true;
  }

  const auto* tuple_expr = std::get_if<IRTupleExpr>(&lhs.node);
  if (tuple_expr == nullptr || tuple_expr->items.size() != fn_def.arity) { return false; }

  bool emitted_any = false;
  for (std::uint32_t param_index = 0; param_index < fn_def.arity; ++param_index) {
    if (auto emit_result = emit_expr(*tuple_expr->items[param_index], out, locals, state, bytecode_module);
        !emit_result) {
      return tl::unexpected(emit_result.error());
    }
    if (by_ref_it->second.contains(param_index)) {
      out.push_back(Instruction{.opcode = Opcode::kMakeValueRef, .operand = 0});
      emitted_any = true;
    }
  }

  out.push_back(
      Instruction{.opcode = Opcode::kBuildTuple, .operand = static_cast<std::int64_t>(tuple_expr->items.size())});
  return emitted_any;
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
            const std::string target_name = target_symbol_name(name_ref);

            if (!name_ref.qualifier.has_value()) {
              if (const auto it = locals.find(name_ref.name); it != locals.end()) {
                out.push_back(
                    Instruction{.opcode = Opcode::kLoadLocal, .operand = static_cast<std::int64_t>(it->second)});
                if (state.current_function_idx.has_value()) {
                  if (const auto by_ref_it = state.by_ref_param_slots.find(*state.current_function_idx);
                      by_ref_it != state.by_ref_param_slots.end() && by_ref_it->second.contains(it->second)) {
                    out.push_back(Instruction{.opcode = Opcode::kDerefValueRef, .operand = 0});
                  }
                }
                return {};
              }
            }

            if (const auto fn_it = state.function_idx.find(target_name); fn_it != state.function_idx.end()) {
              out.push_back(
                  Instruction{.opcode = Opcode::kMakeUserFuncRef, .operand = static_cast<std::int64_t>(fn_it->second)});
              return {};
            }

            if (name_ref.qualifier.has_value() &&
                (*name_ref.qualifier == "Std" || name_ref.qualifier->starts_with("Std."))) {
              const auto builtin_id = fleaux::vm::builtin_id_from_name(full_name);
              if (!builtin_id.has_value()) {
                return tl::unexpected(make_err("Unknown builtin in bytecode compiler: '" + full_name + "'."));
              }
              out.push_back(
                  Instruction{.opcode = Opcode::kMakeBuiltinFuncRef, .operand = fleaux::vm::builtin_operand(*builtin_id)});
              return {};
            }

            if (const auto alias_it = state.builtin_alias.find(full_name); alias_it != state.builtin_alias.end()) {
              const auto builtin_id = fleaux::vm::builtin_id_from_name(alias_it->second);
              if (!builtin_id.has_value()) {
                return tl::unexpected(make_err("Unknown builtin in bytecode compiler: '" + alias_it->second + "'."));
              }
              out.push_back(
                  Instruction{.opcode = Opcode::kMakeBuiltinFuncRef, .operand = fleaux::vm::builtin_operand(*builtin_id)});
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

            const auto previous_fn_idx = state.current_function_idx;
            state.current_function_idx = fn_idx;
            if (auto emit_result =
                    emit_expr(*closure.body, fn_def.instructions, closure_locals, state, bytecode_module);
                !emit_result) {
              state.current_function_idx = previous_fn_idx;
              return tl::unexpected(emit_result.error());
            }
            state.current_function_idx = previous_fn_idx;
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
            out.push_back(Instruction{.opcode = Opcode::kBuildTuple,
                                      .operand = static_cast<std::int64_t>(closure.captures.size())});
            out.push_back(
                Instruction{.opcode = Opcode::kMakeClosureRef, .operand = static_cast<std::int64_t>(closure_idx)});
            return {};
          },
          [&](const IRTupleExpr& tuple_expr) -> EmitResult {
            if (tuple_expr.items.size() == 1) {
              return emit_expr(*tuple_expr.items[0], out, locals, state, bytecode_module);
            }
            if (auto emit_result = emit_tuple_items(tuple_expr); !emit_result) { return emit_result; }
            out.push_back(Instruction{.opcode = Opcode::kBuildTuple,
                                      .operand = static_cast<std::int64_t>(tuple_expr.items.size())});
            return {};
          },
          [&](const IRFlowExpr& flow) -> EmitResult {
            auto handled = std::visit(
                common::overloaded{
                    [&](const IROperatorRef& op_ref) -> tl::expected<bool, CompileError> {
                      if (auto emit_result =
                              emit_operator_flow_expr(*flow.lhs, op_ref, out, locals, state, bytecode_module);
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

                        if (auto fast_path =
                                emit_fast_path(full_name == "Std.Select" && tuple->items.size() == 3 &&
                                                   !expr_is_known_user_function_ref(*tuple->items[1], state) &&
                                                   !expr_is_known_user_function_ref(*tuple->items[2], state),
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

                        if (auto fast_path =
                                emit_fast_path(full_name == "Std.Loop" && tuple->items.size() == 3, Opcode::kLoopCall);
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

            if (!handled) { return tl::unexpected(handled.error()); }
            if (*handled) { return {}; }

            bool emitted_lhs = false;
            if (const auto user_fn_idx = user_function_index_from_target(flow.rhs, state); user_fn_idx.has_value()) {
              if (auto wrapped_emit = emit_lhs_for_user_call_with_auto_value_ref(*flow.lhs, *user_fn_idx, out, locals,
                                                                                 state, bytecode_module);
                  !wrapped_emit) {
                return tl::unexpected(wrapped_emit.error());
              } else {
                emitted_lhs = *wrapped_emit;
              }
            }

            if (!emitted_lhs) {
              if (auto emit_result = emit_expr(*flow.lhs, out, locals, state, bytecode_module); !emit_result) {
                return emit_result;
              }
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
  if (options.enable_auto_value_ref && !options.enable_value_ref_gate) {
    return tl::unexpected(
        make_err("Invalid compile options: enable_auto_value_ref requires enable_value_ref_gate=true."));
  }
  if (options.enable_auto_value_ref && options.value_ref_byte_cutoff == 0) {
    return tl::unexpected(
        make_err("Invalid compile options: value_ref_byte_cutoff must be > 0 when auto value-ref is enabled."));
  }

  Module bytecode_module;
  CompileState state;
  state.enable_auto_value_ref = options.enable_auto_value_ref;

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
    const std::string link_name = internal_symbol_name(let);

    if (let.is_builtin) {
      // Builtins are addressable by their declared symbol only.
      // Qualified builtins (e.g. Std.Println) must remain qualified.
      state.builtin_alias[full_name] = full_name;
      if (let_belongs_to_module(let, options)) {
        bytecode_module.exports.push_back(ExportedSymbol{
            .name = full_name,
            .link_name = full_name,
            .kind = ExportKind::kBuiltinAlias,
            .index = 0,
            .builtin_name = full_name,
        });
      }
      continue;
    }

    const auto fn_idx = static_cast<std::uint32_t>(bytecode_module.functions.size());
    FunctionDef fn_def;
    fn_def.name = link_name;
    fn_def.arity = static_cast<std::uint32_t>(let.params.size());
    fn_def.has_variadic_tail = !let.params.empty() && let.params.back().type.variadic;
    bytecode_module.functions.push_back(std::move(fn_def));

    // Register by full name (always).
    state.function_idx[link_name] = fn_idx;
    state.register_public_function_name(full_name, fn_idx);
    if (let_belongs_to_module(let, options)) {
      bytecode_module.exports.push_back(ExportedSymbol{
          .name = full_name,
          .link_name = link_name,
          .kind = ExportKind::kFunction,
          .index = fn_idx,
          .builtin_name = {},
      });
    }
  }

  const auto by_ref_analysis = analyze_auto_value_ref_params(
      program, AutoValueRefAnalysisOptions{.enabled = options.enable_auto_value_ref && options.enable_value_ref_gate,
                                           .byte_cutoff = options.value_ref_byte_cutoff});
  for (const auto& [full_name, param_slots] : by_ref_analysis) {
    const auto fn_it = state.function_idx.find(full_name);
    if (fn_it == state.function_idx.end()) { continue; }
    if (fn_it->second >= bytecode_module.functions.size()) { continue; }
    if (bytecode_module.functions[fn_it->second].is_import_placeholder) { continue; }
    state.by_ref_param_slots.emplace(fn_it->second, param_slots);
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

    const std::string full_name = internal_symbol_name(let);

    // Map parameter names to local slots.
    LocalSlots local_slots;
    for (std::uint32_t param_index = 0; param_index < static_cast<std::uint32_t>(let.params.size()); ++param_index) {
      local_slots[let.params[param_index].name] = param_index;
    }

    const auto current_fn_idx = state.function_idx.at(full_name);
    const auto previous_fn_idx = state.current_function_idx;
    state.current_function_idx = current_fn_idx;
    if (auto emit_result = emit_expr(*let.body, bytecode_module.functions[current_fn_idx].instructions, local_slots,
                                     state, bytecode_module);
        !emit_result) {
      state.current_function_idx = previous_fn_idx;
      return tl::unexpected(emit_result.error());
    }
    state.current_function_idx = previous_fn_idx;
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
