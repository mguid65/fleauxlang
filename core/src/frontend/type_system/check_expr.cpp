#include "fleaux/frontend/type_system/detail/checker_internal.hpp"

#include <format>

#include "fleaux/common/overloaded.hpp"

namespace fleaux::frontend::type_system::detail {

namespace {

auto instantiate_type_preserving_unbound(const Type& type, const TypeBindings& bindings) -> Type {
  if (type.kind == TypeKind::kTypeVar) {
    if (const auto it = bindings.find(type.nominal_name); it != bindings.end()) {
      return normalize_type(it->second);
    }
    return type;
  }

  Type out = type;
  for (auto& item : out.items) {
    const bool variadic = item.variadic;
    item = instantiate_type_preserving_unbound(item, bindings);
    item.variadic = variadic;
  }
  for (auto& member : out.union_members) { member = instantiate_type_preserving_unbound(member, bindings); }
  for (auto& arg : out.applied_args) { arg = instantiate_type_preserving_unbound(arg, bindings); }
  for (auto& param : out.function_params) {
    const bool variadic = param.variadic;
    param = instantiate_type_preserving_unbound(param, bindings);
    param.variadic = variadic;
  }
  if (out.function_return.has_value()) {
    out.function_return = make_box<Type>(instantiate_type_preserving_unbound(**out.function_return, bindings));
  }
  return normalize_type(std::move(out));
}

auto instantiate_function_sig(const FunctionSig& sig, const TypeBindings& bindings) -> FunctionSig {
  FunctionSig instantiated = sig;
  for (auto& param : instantiated.params) {
    param.type = instantiate_type_preserving_unbound(param.type, bindings);
  }
  instantiated.return_type = instantiate_type_preserving_unbound(instantiated.return_type, bindings);
  return instantiated;
}

}  // namespace

auto infer_expr(ir::IRExpr& expr, const FunctionIndex& index, const StrongTypeIndex& type_index, const LocalTypes& locals,
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
            if (tuple.items.size() == 1U) { return infer_expr(*tuple.items[0], index, type_index, locals, generic_params); }

            Type out;
            out.kind = TypeKind::kTuple;
            out.items.reserve(tuple.items.size());
            for (auto& item : tuple.items) {
              auto item_type = infer_expr(*item, index, type_index, locals, generic_params);
              if (!item_type) { return tl::unexpected(item_type.error()); }
              out.items.push_back(*item_type);
            }
            return out;
          },
          [&](const ir::IRNameRef& name_ref) -> tl::expected<Type, type_check::AnalysisError> {
            auto explicit_type_args =
                resolve_explicit_type_args(name_ref.explicit_type_args, type_index, generic_params, name_ref.span);
            if (!explicit_type_args) { return tl::unexpected(explicit_type_args.error()); }

            if (!name_ref.qualifier.has_value()) {
              if (const auto it = locals.find(name_ref.name); it != locals.end()) {
                if (!explicit_type_args->empty()) {
                  return tl::unexpected(make_error(
                      "Invalid explicit type argument application.",
                      std::format("{} is a local value and does not accept explicit type arguments.", name_ref.name),
                      name_ref.span));
                }
                return it->second;
              }
            }

            if (const auto* overloads = resolve_name_or_symbolic_builtin(index, name_ref.qualifier, name_ref.name);
                overloads != nullptr) {
              const auto full_name = qualified_symbol_name(name_ref.qualifier, name_ref.name);
              const auto filtered =
                  filter_overloads_for_explicit_type_args(full_name, name_ref.span, *overloads, *explicit_type_args);
              if (!filtered) { return tl::unexpected(filtered.error()); }

              if (filtered->size() > 1U) {
                return tl::unexpected(
                    make_error("Ambiguous overloaded function reference.",
                               std::format("{} has multiple overloads. Use it in direct call position or wrap the "
                                           "desired overload in an explicit closure. Candidates: {}.",
                                           full_name, overload_candidate_list(full_name, *overloads)),
                               name_ref.span));
              }

              const auto& sig = *filtered->front();
              const auto bindings = explicit_type_arg_bindings_for_sig(full_name, name_ref.span, sig, *explicit_type_args);
              if (!bindings) { return tl::unexpected(bindings.error()); }

              const auto instantiated = instantiate_function_sig(sig, *bindings);
              if (instantiated.params.empty()) { return instantiated.return_type; }
              return function_type_from_sig(instantiated);
            }

            if (name_ref.qualifier.has_value()) {
              if (is_removed_symbolic_alias(name_ref.qualifier, name_ref.name)) {
                return tl::unexpected(make_unresolved_symbol_error(
                    qualified_symbol_name(name_ref.qualifier, name_ref.name), name_ref.span));
              }
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
            std::unordered_set<std::string> closure_generic_params = generic_params;
            for (const auto& generic_param : closure.generic_params) {
              closure_generic_params.insert(generic_param);
            }

            FunctionSig closure_sig;
            closure_sig.params.reserve(closure.params.size());
            for (const auto& param : closure.params) {
              Type param_type = rewrite_generic_type(from_ir_type(param.type), closure_generic_params);
              if (auto validated = validate_declared_type(param_type, type_index, closure_generic_params, param.span);
                  !validated) {
                return tl::unexpected(validated.error());
              }
              closure_locals.insert_or_assign(param.name, param_type);
              closure_sig.params.push_back(ParamSig{
                  .name = param.name,
                  .type = std::move(param_type),
                  .variadic = param.type.variadic,
              });
            }
            closure_sig.return_type = rewrite_generic_type(from_ir_type(closure.return_type), closure_generic_params);
            if (auto validated =
                    validate_declared_type(closure_sig.return_type, type_index, closure_generic_params, closure.span);
                !validated) {
              return tl::unexpected(validated.error());
            }

            auto inferred_body = infer_expr(*closure.body, index, type_index, closure_locals, closure_generic_params);
            if (!inferred_body) { return tl::unexpected(inferred_body.error()); }
            if (!is_consistent(closure_sig.return_type, *inferred_body)) {
              return tl::unexpected(make_error("Type mismatch in function return.",
                                               "Closure body type does not match declared return type.", closure.span));
            }

            return function_type_from_sig(closure_sig);
          },
          [&](ir::IRFlowExpr& flow) -> tl::expected<Type, type_check::AnalysisError> {
            return infer_flow_expr(flow, index, type_index, locals, generic_params);
          },
      },
      expr.node);
}

}  // namespace fleaux::frontend::type_system::detail
