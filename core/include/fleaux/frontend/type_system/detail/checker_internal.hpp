#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <tl/expected.hpp>

#include "fleaux/frontend/type_system/checker.hpp"
#include "fleaux/frontend/type_system/function_index.hpp"

namespace fleaux::frontend::type_system::detail {

using LocalTypes = std::unordered_map<std::string, Type>;
using TypeBindings = std::unordered_map<std::string, Type>;
using TypeNameSet = std::unordered_set<std::string>;

struct ResolvedInvocation {
  Type return_type;
  std::string resolved_symbol_key;
};

struct BindingQuality {
  std::size_t resolved_bindings = 0U;
  std::size_t total_bindings = 0U;
  std::size_t complexity = 0U;
};

inline constexpr std::string_view kMatchWildcardSentinel = "__fleaux_match_wildcard__";

[[nodiscard]] auto make_error(const std::string& message, const std::optional<std::string>& hint,
                              const std::optional<diag::SourceSpan>& span) -> type_check::AnalysisError;
[[nodiscard]] auto make_unresolved_symbol_error(const std::string& symbol, const std::optional<diag::SourceSpan>& span)
    -> type_check::AnalysisError;
[[nodiscard]] auto qualified_symbol_name(const std::optional<std::string>& qualifier, const std::string& name)
    -> std::string;
[[nodiscard]] auto is_symbolic_qualifier(const std::optional<std::string>& qualifier) -> bool;
[[nodiscard]] auto is_removed_symbolic_alias(const std::optional<std::string>& qualifier, const std::string& name)
    -> bool;
[[nodiscard]] auto resolve_name_or_symbolic_builtin(const FunctionIndex& index,
                                                    const std::optional<std::string>& qualifier,
                                                    const std::string& name) -> const FunctionOverloadSet*;
[[nodiscard]] auto target_name(const ir::IRCallTarget& target) -> std::optional<std::string>;
[[nodiscard]] auto resolve_signature(const FunctionIndex& index, const ir::IRCallTarget& target)
    -> const FunctionOverloadSet*;
[[nodiscard]] auto function_type_from_sig(const FunctionSig& sig) -> Type;
auto resolve_explicit_type_args(const std::vector<ir::IRSimpleType>& explicit_type_args,
                                const StrongTypeIndex& type_index,
                                const AliasIndex& alias_index,
                                const std::unordered_set<std::string>& generic_params,
                                const std::optional<diag::SourceSpan>& span)
    -> tl::expected<std::vector<Type>, type_check::AnalysisError>;
auto explicit_type_arg_bindings_for_sig(const std::string& full_name, const std::optional<diag::SourceSpan>& span,
                                        const FunctionSig& sig, const std::vector<Type>& explicit_type_args)
    -> tl::expected<TypeBindings, type_check::AnalysisError>;
auto filter_overloads_for_explicit_type_args(const std::string& full_name, const std::optional<diag::SourceSpan>& span,
                                             const FunctionOverloadSet& overloads,
                                             const std::vector<Type>& explicit_type_args)
    -> tl::expected<std::vector<const FunctionSig*>, type_check::AnalysisError>;

auto collect_type_vars(const Type& type, std::unordered_set<std::string>& out) -> void;
auto is_type_resolved(const Type& type, const TypeBindings& bindings,
                      const std::unordered_set<std::string>& allowed_unbound, std::unordered_set<std::string>& visiting,
                      std::unordered_map<std::string, bool>& resolved_cache) -> bool;
auto is_type_var_resolved(const std::string& type_var, const TypeBindings& bindings,
                          const std::unordered_set<std::string>& allowed_unbound,
                          std::unordered_set<std::string>& visiting,
                          std::unordered_map<std::string, bool>& resolved_cache) -> bool;
auto collect_unresolved_return_type_vars(const Type& return_type, const TypeBindings& bindings,
                                         const std::unordered_set<std::string>& allowed_unbound,
                                         std::unordered_set<std::string>& out) -> void;
auto collect_unbound_type_vars(const Type& type, const TypeBindings& bindings, std::unordered_set<std::string>& out)
    -> void;
auto join_sorted_type_var_names(const std::unordered_set<std::string>& names) -> std::string;
auto generic_var_suffix_for_type(const Type& type) -> std::string;
auto type_debug_name(const Type& type) -> std::string;
auto generic_binding_detail_for_type(const Type& expected_type, const TypeBindings& bindings, const Type& actual)
    -> std::string;
auto generic_arg_mismatch_hint(const std::string& full_name, std::size_t arg_index, const Type& expected_type,
                               bool variadic, const TypeBindings& bindings, const Type& actual) -> std::string;
auto format_signature_debug_name(const std::string& full_name, const FunctionSig& sig) -> std::string;
auto overload_candidate_list(const std::string& full_name, const FunctionOverloadSet& overloads) -> std::string;
auto overload_candidate_list(const std::string& full_name, const std::vector<const FunctionSig*>& overloads)
    -> std::string;
auto call_shape_matches(const FunctionSig& sig, std::size_t arg_count) -> bool;
auto validate_supported_overload_sets(const ir::IRProgram& program, const std::vector<ir::IRLet>& imported_typed_lets)
    -> tl::expected<void, type_check::AnalysisError>;
auto collect_known_strong_type_names(const ir::IRProgram& program,
                                     const std::vector<ir::IRTypeDecl>& imported_type_decls) -> TypeNameSet;
auto validate_alias_declarations(const ir::IRProgram& program, const std::vector<ir::IRTypeDecl>& imported_type_decls,
                                 const std::vector<ir::IRAliasDecl>& imported_alias_decls)
    -> tl::expected<AliasIndex, type_check::AnalysisError>;
auto validate_strong_type_declarations(const ir::IRProgram& program,
                                       const std::vector<ir::IRTypeDecl>& imported_type_decls,
                                       const AliasIndex& alias_index)
    -> tl::expected<void, type_check::AnalysisError>;
auto expand_aliases_in_type(const Type& type, const TypeNameSet& known_strong_type_names, const AliasIndex& alias_index,
                            const std::unordered_set<std::string>& generic_params,
                            const std::optional<diag::SourceSpan>& span)
    -> tl::expected<Type, type_check::AnalysisError>;
auto expand_aliases_in_type(const Type& type, const StrongTypeIndex& type_index, const AliasIndex& alias_index,
                            const std::unordered_set<std::string>& generic_params,
                            const std::optional<diag::SourceSpan>& span)
    -> tl::expected<Type, type_check::AnalysisError>;
auto validate_declared_type(const Type& type, const StrongTypeIndex& type_index,
                            const AliasIndex& alias_index,
                            const std::unordered_set<std::string>& generic_params,
                            const std::optional<diag::SourceSpan>& span)
    -> tl::expected<void, type_check::AnalysisError>;

auto rewrite_generic_type(const Type& type, const std::unordered_set<std::string>& generic_params) -> Type;
auto type_complexity(const Type& type) -> std::size_t;
auto binding_quality(const TypeBindings& bindings) -> BindingQuality;
auto is_better_binding_quality(const TypeBindings& candidate, const TypeBindings& incumbent) -> bool;
auto merge_binding_types(const Type& lhs, const Type& rhs) -> Type;
auto union_member_requires_coverage(const Type& member) -> bool;
auto fixed_union_members_are_covered(const std::vector<Type>& expected_members, const std::vector<Type>& actual_members,
                                     const TypeBindings& bindings) -> bool;
auto bind_union_members(const std::vector<Type>& expected_members, const std::vector<Type>& actual_members,
                        std::size_t actual_index, const TypeBindings& base_bindings, TypeBindings& bindings) -> bool;
auto instantiate_generic_type(const Type& type, const TypeBindings& bindings) -> Type;
auto bind_function_type(const Type& expected, const Type& actual, TypeBindings& bindings) -> bool;
auto bind_generic_type(const Type& expected, const Type& actual, TypeBindings& bindings) -> bool;
auto try_bind_union_option(const Type& expected_option, const Type& actual_member, const TypeBindings& base_bindings,
                           TypeBindings& trial_bindings) -> bool;

auto check_invocation(const std::string& full_name, const std::optional<diag::SourceSpan>& span, const FunctionSig& sig,
                      const std::vector<Type>& args, const StrongTypeIndex& type_index,
                      const std::unordered_set<std::string>& allowed_unbound,
                      const std::vector<Type>& explicit_type_args = {})
    -> tl::expected<Type, type_check::AnalysisError>;
auto resolve_overload_invocation(const std::string& full_name, const std::optional<diag::SourceSpan>& span,
                                 const FunctionOverloadSet& overloads, const std::vector<Type>& args,
                                 const StrongTypeIndex& type_index,
                                 const std::unordered_set<std::string>& allowed_unbound,
                                 const std::vector<Type>& explicit_type_args = {})
    -> tl::expected<ResolvedInvocation, type_check::AnalysisError>;

auto is_std_match_target(const ir::IRCallTarget& target) -> bool;
auto is_match_wildcard_pattern(const ir::IRExpr& expr) -> bool;
auto merge_match_result_types(const Type& lhs, const Type& rhs) -> std::optional<Type>;
auto match_handler_return_type(const Type& handler_type, const Type& subject_type,
                               const std::optional<diag::SourceSpan>& span)
    -> tl::expected<Type, type_check::AnalysisError>;
auto validate_match_pattern_type(const Type& pattern_type, const Type& subject_type,
                                 const std::optional<diag::SourceSpan>& span)
    -> tl::expected<void, type_check::AnalysisError>;
auto infer_std_match_expr(ir::IRFlowExpr& flow, const FunctionIndex& index, const StrongTypeIndex& type_index,
                          const AliasIndex& alias_index, const LocalTypes& locals,
                          const std::unordered_set<std::string>& generic_params)
    -> tl::expected<Type, type_check::AnalysisError>;
auto infer_flow_expr(ir::IRFlowExpr& flow, const FunctionIndex& index, const StrongTypeIndex& type_index,
                     const AliasIndex& alias_index, const LocalTypes& locals,
                     const std::unordered_set<std::string>& generic_params)
    -> tl::expected<Type, type_check::AnalysisError>;
auto infer_expr(ir::IRExpr& expr, const FunctionIndex& index, const StrongTypeIndex& type_index,
                const AliasIndex& alias_index, const LocalTypes& locals,
                const std::unordered_set<std::string>& generic_params)
    -> tl::expected<Type, type_check::AnalysisError>;

}  // namespace fleaux::frontend::type_system::detail
