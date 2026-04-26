#include "fleaux/frontend/type_system/detail/checker_internal.hpp"

#include <format>

#include "fleaux/common/overloaded.hpp"

namespace fleaux::frontend::type_system::detail {

auto infer_expr(ir::IRExpr& expr, const FunctionIndex& index, const LocalTypes& locals,
                const std::unordered_set<std::string>& generic_params)
    -> tl::expected<Type, type_check::AnalysisError> {
  return std::visit(
      common::overloaded{
          [&](const ir::IRConstant& constant) -> tl::expected<Type, type_check::AnalysisError> {
            return std::visit(
                common::overloaded{[](std::int64_t) -> Type { return Type{.kind = TypeKind::kInt64}; },
                                   [](std::uint64_t) -> Type { return Type{.kind = TypeKind::kUInt64}; },
                                   [](double) -> Type { return Type{.kind = TypeKind::kFloat64}; },
                                   [](bool) -> Type { return Type{.kind = TypeKind::kBool}; },
                                   [](const std::string&) -> Type { return Type{.kind = TypeKind::kString}; },
                                   [](std::monostate) -> Type { return Type{.kind = TypeKind::kNull}; }},
                constant.val);
          },
          [&](ir::IRTupleExpr& tuple) -> tl::expected<Type, type_check::AnalysisError> {
            if (tuple.items.size() == 1U) { return infer_expr(*tuple.items[0], index, locals, generic_params); }

            Type out;
            out.kind = TypeKind::kTuple;
            out.items.reserve(tuple.items.size());
            for (auto& item : tuple.items) {
              auto item_type = infer_expr(*item, index, locals, generic_params);
              if (!item_type) { return tl::unexpected(item_type.error()); }
              out.items.push_back(*item_type);
            }
            return out;
          },
          [&](const ir::IRNameRef& name_ref) -> tl::expected<Type, type_check::AnalysisError> {
            if (!name_ref.qualifier.has_value()) {
              if (const auto it = locals.find(name_ref.name); it != locals.end()) { return it->second; }
            }

            if (const auto* overloads = resolve_name_or_symbolic_builtin(index, name_ref.qualifier, name_ref.name);
                overloads != nullptr) {
              const auto full_name = qualified_symbol_name(name_ref.qualifier, name_ref.name);
              if (overloads->size() > 1U) {
                return tl::unexpected(
                    make_error("Ambiguous overloaded function reference.",
                               std::format("{} has multiple overloads. Use it in direct call position or wrap the "
                                           "desired overload in an explicit closure. Candidates: {}.",
                                           full_name, overload_candidate_list(full_name, *overloads)),
                               name_ref.span));
              }

              const auto& sig = overloads->front();
              if (sig.params.empty()) { return sig.return_type; }
              return function_type_from_sig(sig);
            }

            if (name_ref.qualifier.has_value()) {
              if (is_removed_symbolic_alias(name_ref.qualifier, name_ref.name)) {
                return tl::unexpected(make_unresolved_symbol_error(
                    qualified_symbol_name(name_ref.qualifier, name_ref.name), name_ref.span));
              }
              if (is_symbolic_qualifier(name_ref.qualifier)) { return Type{.kind = TypeKind::kAny}; }
              if (index.has_qualified_symbol(name_ref.qualifier, name_ref.name)) {
                return Type{.kind = TypeKind::kAny};
              }
              return tl::unexpected(make_unresolved_symbol_error(
                  qualified_symbol_name(name_ref.qualifier, name_ref.name), name_ref.span));
            }
            if (index.has_unqualified_symbol(name_ref.name)) { return Type{.kind = TypeKind::kAny}; }
            return tl::unexpected(make_unresolved_symbol_error(name_ref.name, name_ref.span));
          },
          [&](ir::IRClosureExprBox& closure_box) -> tl::expected<Type, type_check::AnalysisError> {
            auto& closure = *closure_box;

            LocalTypes closure_locals = locals;
            FunctionSig closure_sig;
            closure_sig.params.reserve(closure.params.size());
            for (const auto& param : closure.params) {
              Type param_type = rewrite_generic_type(from_ir_type(param.type), generic_params);
              closure_locals.insert_or_assign(param.name, param_type);
              closure_sig.params.push_back(ParamSig{
                  .name = param.name,
                  .type = std::move(param_type),
                  .variadic = param.type.variadic,
              });
            }
            closure_sig.return_type = rewrite_generic_type(from_ir_type(closure.return_type), generic_params);

            auto inferred_body = infer_expr(*closure.body, index, closure_locals, generic_params);
            if (!inferred_body) { return tl::unexpected(inferred_body.error()); }
            if (!is_consistent(closure_sig.return_type, *inferred_body)) {
              return tl::unexpected(make_error("Type mismatch in function return.",
                                               "Closure body type does not match declared return type.", closure.span));
            }

            return function_type_from_sig(closure_sig);
          },
          [&](ir::IRFlowExpr& flow) -> tl::expected<Type, type_check::AnalysisError> {
            return infer_flow_expr(flow, index, locals, generic_params);
          },
      },
      expr.node);
}

}  // namespace fleaux::frontend::type_system::detail
