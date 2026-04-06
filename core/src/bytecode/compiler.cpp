#include "fleaux/bytecode/compiler.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <variant>

namespace fleaux::bytecode {
namespace {

using namespace frontend::ir;

// ── Operator → builtin name mapping ──────────────────────────────────────────

const std::unordered_map<std::string, std::string>& operator_to_builtin() {
  static const std::unordered_map<std::string, std::string> map = {
      {"+",  "Std.Add"},           {"-",  "Std.Subtract"},
      {"*",  "Std.Multiply"},      {"/",  "Std.Divide"},
      {"%",  "Std.Mod"},           {"^",  "Std.Pow"},
      {"==", "Std.Equal"},         {"!=", "Std.NotEqual"},
      {"<",  "Std.LessThan"},      {">",  "Std.GreaterThan"},
      {">=", "Std.GreaterOrEqual"},{"<=", "Std.LessOrEqual"},
      {"!",  "Std.Not"},           {"&&", "Std.And"},
      {"||", "Std.Or"},
  };
  return map;
}

// ── Compile state ─────────────────────────────────────────────────────────────

struct CompileState {
  // Builtin name → index into Module::builtin_names (deduplicated).
  std::unordered_map<std::string, std::uint32_t> builtin_idx;
  // Fully-qualified function name → index into Module::functions.
  std::unordered_map<std::string, std::uint32_t> function_idx;
  // Maps a short/full name to the stdlib builtin it aliases (from is_builtin lets).
  std::unordered_map<std::string, std::string> builtin_alias;

  std::uint32_t intern_builtin(Module& module, const std::string& name) {
    auto it = builtin_idx.find(name);
    if (it != builtin_idx.end()) return it->second;
    const auto idx = static_cast<std::uint32_t>(module.builtin_names.size());
    module.builtin_names.push_back(name);
    builtin_idx[name] = idx;
    return idx;
  }

  static std::uint32_t intern_const(Module& module, ConstValue c) {
    const auto idx = static_cast<std::uint32_t>(module.constants.size());
    module.constants.push_back(std::move(c));
    return idx;
  }
};

CompileError make_err(const std::string& msg) {
  return CompileError{.message = msg};
}

using EmitResult = tl::expected<void, CompileError>;
using LocalSlots = std::unordered_map<std::string, std::uint32_t>;

// Forward declaration.
EmitResult emit_expr(const IRExprPtr& expr,
                     std::vector<Instruction>& out,
                     const LocalSlots& locals,
                     CompileState& state,
                     Module& module);

// ── Call-target emission ──────────────────────────────────────────────────────

EmitResult emit_call_target(const IRCallTarget& target,
                            std::vector<Instruction>& out,
                            const LocalSlots& locals,
                            CompileState& state,
                            Module& module) {
  // Helper: emit kCallBuiltin with an interned name.
  auto emit_builtin = [&](const std::string& name) -> EmitResult {
    const auto idx = state.intern_builtin(module, name);
    out.push_back(Instruction{Opcode::kCallBuiltin, static_cast<std::int64_t>(idx)});
    return {};
  };

  // Operator shorthand (-> +, -> *, etc.)
  if (std::holds_alternative<IROperatorRef>(target)) {
    const auto& op = std::get<IROperatorRef>(target).op;
    const auto& opmap = operator_to_builtin();
    const auto it = opmap.find(op);
    if (it == opmap.end()) {
      return tl::unexpected(make_err(
          "Unsupported operator in bytecode compiler: '" + op + "'."));
    }
    return emit_builtin(it->second);
  }

  const auto& name_ref = std::get<IRNameRef>(target);
  const std::string full_name = name_ref.qualifier.has_value()
      ? (*name_ref.qualifier + "." + name_ref.name)
      : name_ref.name;

  // Qualified Std.* builtin (e.g. Std.Add, Std.Math.Floor).
  if (name_ref.qualifier.has_value() &&
      (*name_ref.qualifier == "Std" ||
       name_ref.qualifier->rfind("Std.", 0) == 0)) {
    return emit_builtin(full_name);
  }

  // User-defined function (check by full name first, then short name).
  {
    auto fn_it = state.function_idx.find(full_name);
    if (fn_it != state.function_idx.end()) {
      out.push_back(Instruction{Opcode::kCallUserFunc,
                                static_cast<std::int64_t>(fn_it->second)});
      return {};
    }
  }

  // Builtin alias introduced by an is_builtin let (unqualified import).
  {
    auto alias_it = state.builtin_alias.find(full_name);
    if (alias_it != state.builtin_alias.end()) {
      return emit_builtin(alias_it->second);
    }
  }

  return tl::unexpected(make_err(
      "Unsupported call target in bytecode compiler: '" + full_name + "'."));
}

// ── Expression emission ───────────────────────────────────────────────────────

EmitResult emit_expr(const IRExprPtr& expr,
                     std::vector<Instruction>& out,
                     const LocalSlots& locals,
                     CompileState& state,
                     Module& module) {
  if (!expr) {
    return tl::unexpected(make_err("Cannot compile null IR expression."));
  }

  // ── Constant ──────────────────────────────────────────────────────────────
  if (std::holds_alternative<IRConstant>(expr->node)) {
    const auto& c = std::get<IRConstant>(expr->node);
    if (std::holds_alternative<std::int64_t>(c.val)) {
      out.push_back(
          Instruction{Opcode::kPushConstI64, std::get<std::int64_t>(c.val)});
      return {};
    }
    const auto idx = CompileState::intern_const(module, ConstValue{c.val});
    out.push_back(
        Instruction{Opcode::kPushConst, static_cast<std::int64_t>(idx)});
    return {};
  }

  // ── Name reference (local variable or first-class function ref) ───────────
  if (std::holds_alternative<IRNameRef>(expr->node)) {
    const auto& name_ref = std::get<IRNameRef>(expr->node);
    const std::string full_name = name_ref.qualifier.has_value()
        ? (*name_ref.qualifier + "." + name_ref.name)
        : name_ref.name;

    // 1. Local parameter slot (unqualified names only).
    if (!name_ref.qualifier.has_value()) {
      const auto it = locals.find(name_ref.name);
      if (it != locals.end()) {
        out.push_back(Instruction{Opcode::kLoadLocal,
                                  static_cast<std::int64_t>(it->second)});
        return {};
      }
    }

    // 2. User-defined function used as a first-class value.
    {
      auto fn_it = state.function_idx.find(full_name);
      if (fn_it != state.function_idx.end()) {
        out.push_back(Instruction{Opcode::kMakeUserFuncRef,
                                  static_cast<std::int64_t>(fn_it->second)});
        return {};
      }
    }

    // 3. Unsupported as a value expression.
    return tl::unexpected(make_err(
        "Name '" + full_name +
        "' used as a value is not supported in the bytecode compiler."));
  }

  // ── Tuple ─────────────────────────────────────────────────────────────────
  if (std::holds_alternative<IRTupleExpr>(expr->node)) {
    const auto& tuple = std::get<IRTupleExpr>(expr->node);
    for (const auto& item : tuple.items) {
      if (auto r = emit_expr(item, out, locals, state, module); !r) return r;
    }
    out.push_back(Instruction{Opcode::kBuildTuple,
                              static_cast<std::int64_t>(tuple.items.size())});
    return {};
  }

  // ── Flow expression ────────────────────────────────────────────────────────
  if (std::holds_alternative<IRFlowExpr>(expr->node)) {
    const auto& flow = std::get<IRFlowExpr>(expr->node);
    if (auto r = emit_expr(flow.lhs, out, locals, state, module); !r) return r;
    return emit_call_target(flow.rhs, out, locals, state, module);
  }

  return tl::unexpected(
      make_err("Unsupported IR expression type in bytecode compiler."));
}

}  // namespace

// ── BytecodeCompiler::compile ─────────────────────────────────────────────────

CompileResult BytecodeCompiler::compile(const IRProgram& program) const {
  Module module;
  CompileState state;

  // ── Pass 1: register all let definitions ──────────────────────────────────
  // is_builtin lets become aliases; user-defined lets populate the function table.
  for (const auto& let : program.lets) {
    const std::string full_name = let.qualifier.has_value()
        ? (*let.qualifier + "." + let.name)
        : let.name;

    if (let.is_builtin) {
      // Register both the short name and the qualified name as aliases.
      state.builtin_alias[let.name]  = full_name;
      state.builtin_alias[full_name] = full_name;
      continue;
    }

    const auto fn_idx = static_cast<std::uint32_t>(module.functions.size());
    FunctionDef fn_def;
    fn_def.name  = full_name;
    fn_def.arity = static_cast<std::uint32_t>(let.params.size());
    module.functions.push_back(std::move(fn_def));

    // Register by full name (always).
    state.function_idx[full_name] = fn_idx;
    // Register by short name; later definitions with the same short name win.
    state.function_idx[let.name] = fn_idx;
  }

  // ── Pass 2: compile function bodies ───────────────────────────────────────
  std::size_t fn_slot = 0;
  for (const auto& let : program.lets) {
    if (let.is_builtin) continue;

    const std::string full_name = let.qualifier.has_value()
        ? (*let.qualifier + "." + let.name)
        : let.name;

    // Map parameter names to local slots.
    LocalSlots local_slots;
    for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(let.params.size()); ++i) {
      local_slots[let.params[i].name] = i;
    }

    const auto current_fn_idx = state.function_idx.at(full_name);
    auto& fn_def = module.functions[current_fn_idx];

    if (auto r = emit_expr(let.body, fn_def.instructions, local_slots, state, module); !r) {
      return tl::unexpected(r.error());
    }
    fn_def.instructions.push_back(Instruction{Opcode::kReturn, 0});
    ++fn_slot;
  }

  // ── Pass 3: compile top-level expression statements ───────────────────────
  for (const auto& expr_stmt : program.expressions) {
    if (auto r = emit_expr(expr_stmt.expr, module.instructions, {}, state, module); !r) {
      return tl::unexpected(r.error());
    }
    // Expression statements discard their result.
    module.instructions.push_back(Instruction{Opcode::kPop, 0});
  }

  module.instructions.push_back(Instruction{Opcode::kHalt, 0});
  return module;
}

}  // namespace fleaux::bytecode
