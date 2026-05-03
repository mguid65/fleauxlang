#include "fleaux/bytecode/escape_analyzer.hpp"

#include <algorithm>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "fleaux/common/overloaded.hpp"

namespace fleaux::bytecode {
namespace {

using namespace frontend::ir;

struct CallSite {
  std::string target_name;
  const IRExpr* lhs{nullptr};
};

struct LetBodySummary {
  std::unordered_set<std::string> captured_by_nested_closure;
  std::unordered_set<std::string> reaches_escape_builtin;
};

auto escape_builtins() -> const std::unordered_set<std::string>& {
  static const std::unordered_set<std::string> names = {
      "Std.Apply",
      "Std.Branch",
      "Std.Loop",
      "Std.LoopN",
      "Std.Match",
      "Std.Parallel.Map",
      "Std.Parallel.WithOptions",
      "Std.Parallel.ForEach",
      "Std.Parallel.Reduce",
      "Std.Task.Spawn",
      "Std.Task.Await",
      "Std.Task.AwaitAll",
      "Std.Task.Cancel",
      "Std.Task.WithTimeout",
  };
  return names;
}

auto full_symbol_name(const std::optional<std::string>& qualifier, const std::string& name) -> std::string {
  return qualifier.has_value() ? (*qualifier + "." + name) : name;
}

auto let_identity_key(const IRLet& let) -> std::string {
  if (!let.symbol_key.empty()) {
    return let.symbol_key;
  }
  return full_symbol_name(let.qualifier, let.name);
}

auto target_identity_key(const IRNameRef& name_ref) -> std::string {
  if (name_ref.resolved_symbol_key.has_value()) {
    return *name_ref.resolved_symbol_key;
  }
  return full_symbol_name(name_ref.qualifier, name_ref.name);
}

auto saturating_add(const std::size_t lhs, const std::size_t rhs) -> std::size_t {
  if (lhs > std::numeric_limits<std::size_t>::max() - rhs) {
    return std::numeric_limits<std::size_t>::max();
  }
  return lhs + rhs;
}

auto estimate_expr_size_bytes(const IRExpr& expr) -> std::optional<std::size_t> {
  return std::visit(
      common::overloaded{[](const IRConstant& c) -> std::optional<std::size_t> {
                           return std::visit(
                               common::overloaded{[](std::int64_t) -> std::size_t { return 8; },
                                                  [](std::uint64_t) -> std::size_t { return 8; },
                                                  [](double) -> std::size_t { return 8; },
                                                  [](bool) -> std::size_t { return 1; },
                                                  [](const std::string& s) -> std::size_t { return s.size(); },
                                                  [](std::monostate) -> std::size_t { return 0; }},
                               c.val);
                         },
                         [](const IRTupleExpr& tuple) -> std::optional<std::size_t> {
                           if (tuple.items.size() == 1) {
                             return estimate_expr_size_bytes(*tuple.items.front());
                           }
                           std::size_t total = 16;
                           for (const auto& item : tuple.items) {
                             const auto item_size = estimate_expr_size_bytes(*item);
                             if (!item_size.has_value()) {
                               return std::nullopt;
                             }
                             total = saturating_add(total, *item_size);
                           }
                           return total;
                         },
                         [](const auto&) -> std::optional<std::size_t> { return std::nullopt; }},
      expr.node);
}

void collect_unqualified_name_refs(const IRExpr& expr, std::unordered_set<std::string>& out) {
  std::visit(common::overloaded{[&](const IRNameRef& name_ref) -> void {
                                  if (!name_ref.qualifier.has_value()) {
                                    out.insert(name_ref.name);
                                  }
                                },
                                [&](const IRFlowExpr& flow) -> void { collect_unqualified_name_refs(*flow.lhs, out); },
                                [&](const IRTupleExpr& tuple) -> void {
                                  for (const auto& item : tuple.items) {
                                    collect_unqualified_name_refs(*item, out);
                                  }
                                },
                                [&](const IRClosureExprBox& closure_ptr) -> void {
                                  collect_unqualified_name_refs(*closure_ptr->body, out);
                                },
                                [](const auto&) -> void {}},
             expr.node);
}

void summarize_let_body(const IRExpr& expr, LetBodySummary& summary) {
  std::visit(common::overloaded{[&](const IRFlowExpr& flow) -> void {
                                  std::visit(common::overloaded{[&](const IRNameRef& name_ref) -> void {
                                                                  if (escape_builtins().contains(full_symbol_name(
                                                                          name_ref.qualifier, name_ref.name))) {
                                                                    collect_unqualified_name_refs(
                                                                        *flow.lhs, summary.reaches_escape_builtin);
                                                                  }
                                                                },
                                                                [](const IROperatorRef&) -> void {}},
                                             flow.rhs);
                                  summarize_let_body(*flow.lhs, summary);
                                },
                                [&](const IRTupleExpr& tuple) -> void {
                                  for (const auto& item : tuple.items) {
                                    summarize_let_body(*item, summary);
                                  }
                                },
                                [&](const IRClosureExprBox& closure_ptr) -> void {
                                  for (const auto& capture : closure_ptr->captures) {
                                    summary.captured_by_nested_closure.insert(capture);
                                  }
                                  summarize_let_body(*closure_ptr->body, summary);
                                },
                                [](const auto&) -> void {}},
             expr.node);
}

void collect_call_sites(const IRExpr& expr, std::vector<CallSite>& out) {
  std::visit(common::overloaded{
                 [&](const IRFlowExpr& flow) -> void {
                   collect_call_sites(*flow.lhs, out);
                   std::visit(common::overloaded{[&](const IRNameRef& name_ref) -> void {
                                                   out.push_back(CallSite{.target_name = target_identity_key(name_ref),
                                                                          .lhs = &*flow.lhs});
                                                 },
                                                 [](const IROperatorRef&) -> void {}},
                              flow.rhs);
                 },
                 [&](const IRTupleExpr& tuple) -> void {
                   for (const auto& item : tuple.items) {
                     collect_call_sites(*item, out);
                   }
                 },
                 [&](const IRClosureExprBox& closure_ptr) -> void { collect_call_sites(*closure_ptr->body, out); },
                 [](const auto&) -> auto {}},
             expr.node);
}

void collect_program_call_sites(const IRProgram& program, std::vector<CallSite>& out) {
  for (const auto& let : program.lets) {
    if (let.is_builtin || !let.body.has_value()) {
      continue;
    }
    collect_call_sites(*let.body, out);
  }
  for (const auto& [expr, span] : program.expressions) {
    collect_call_sites(expr, out);
  }
}

auto call_arg_for_param(const IRExpr& lhs, const std::size_t arity, const std::size_t param_index) -> const IRExpr* {
  const auto* tuple_expr = std::get_if<IRTupleExpr>(&lhs.node);
  if (arity == 1) {
    if (tuple_expr != nullptr && tuple_expr->items.size() == 1) {
      return &*tuple_expr->items.front();
    }
    return &lhs;
  }

  if (tuple_expr == nullptr) {
    return nullptr;
  }
  if (tuple_expr->items.size() != arity) {
    return nullptr;
  }
  if (param_index >= tuple_expr->items.size()) {
    return nullptr;
  }
  return &*tuple_expr->items[param_index];
}

}  // namespace

auto analyze_auto_value_ref_params(const IRProgram& program, const AutoValueRefAnalysisOptions& options)
    -> AutoValueRefParamSlots {
  AutoValueRefParamSlots result;
  if (!options.enabled) {
    return result;
  }

  std::unordered_map<std::string, const IRLet*> lets_by_name;
  std::unordered_map<std::string, LetBodySummary> let_body_summaries;
  std::unordered_map<std::string, std::string> short_to_full_name;
  std::unordered_set<std::string> ambiguous_short_names;

  for (const auto& let : program.lets) {
    if (let.is_builtin || !let.body.has_value()) {
      continue;
    }
    const auto full_name = let_identity_key(let);
    lets_by_name.emplace(full_name, &let);
    auto& summary = let_body_summaries[full_name];
    summarize_let_body(*let.body, summary);
    if (!let.qualifier.has_value()) {
      if (!short_to_full_name.contains(let.name) && !ambiguous_short_names.contains(let.name)) {
        short_to_full_name.emplace(let.name, full_name);
      } else {
        short_to_full_name.erase(let.name);
        ambiguous_short_names.insert(let.name);
      }
    }
  }

  std::vector<CallSite> call_sites;
  collect_program_call_sites(program, call_sites);

  std::unordered_map<std::string, std::vector<const CallSite*>> calls_by_target;
  for (const auto& call_site : call_sites) {
    auto resolved_name = call_site.target_name;
    if (!lets_by_name.contains(resolved_name)) {
      if (const auto short_it = short_to_full_name.find(call_site.target_name); short_it != short_to_full_name.end()) {
        resolved_name = short_it->second;
      } else {
        continue;
      }
    }
    calls_by_target[resolved_name].push_back(&call_site);
  }

  for (const auto& [full_name, let_ptr] : lets_by_name) {
    const auto callsite_it = calls_by_target.find(full_name);
    if (callsite_it == calls_by_target.end()) {
      continue;
    }

    const auto& let = *let_ptr;
    const auto arity = let.params.size();
    if (arity == 0) {
      continue;
    }

    std::unordered_set<std::uint32_t> marked_params;

    for (std::size_t param_idx = 0; param_idx < arity; ++param_idx) {
      const auto& param = let.params[param_idx];
      if (param.type.variadic) {
        continue;
      }
      if (param.type.name == "Any") {
        continue;
      }

      if (const auto summary_it = let_body_summaries.find(full_name); summary_it != let_body_summaries.end()) {
        if (summary_it->second.captured_by_nested_closure.contains(param.name)) {
          continue;
        }
        if (summary_it->second.reaches_escape_builtin.contains(param.name)) {
          continue;
        }
      }

      bool all_calls_large = true;
      bool saw_call = false;

      for (const auto* call_site : callsite_it->second) {
        const auto* arg_expr = call_arg_for_param(*call_site->lhs, arity, param_idx);
        if (arg_expr == nullptr) {
          all_calls_large = false;
          break;
        }

        if (const auto estimate = estimate_expr_size_bytes(*arg_expr);
            !estimate.has_value() || *estimate < options.byte_cutoff) {
          all_calls_large = false;
          break;
        }
        saw_call = true;
      }

      if (saw_call && all_calls_large) {
        marked_params.insert(static_cast<std::uint32_t>(param_idx));
      }
    }

    if (!marked_params.empty()) {
      result.emplace(full_name, std::move(marked_params));
    }
  }

  return result;
}

}  // namespace fleaux::bytecode
