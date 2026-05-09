#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "fleaux/common/indirect_optional.hpp"
#include "fleaux/frontend/diagnostics.hpp"
#include "fleaux/frontend/type_node.hpp"

namespace fleaux::frontend::model {

struct Qualifier {
  std::string qualifier;
  std::optional<diag::SourceSpan> span;
};

struct QualifiedId {
  Qualifier qualifier;
  std::string id;
  std::optional<diag::SourceSpan> span;
};

struct TypeNode;
struct TypeList;
struct UnionTypeList;
struct AppliedTypeNode;
struct FunctionTypeNode;
struct NamedTarget;
struct Expression;
struct ClosureExpression;
struct DelimitedExpression;

using TypeBox = common::IndirectOptional<TypeNode>;
using TypeListBox = common::IndirectOptional<TypeList>;
using UnionTypeListBox = common::IndirectOptional<UnionTypeList>;
using AppliedTypeNodeBox = common::IndirectOptional<AppliedTypeNode>;
using FunctionTypeNodeBox = common::IndirectOptional<FunctionTypeNode>;
using NamedTargetBox = common::IndirectOptional<NamedTarget>;
using ExpressionBox = common::IndirectOptional<Expression>;
using ClosureExpressionBox = common::IndirectOptional<ClosureExpression>;
using DelimitedExpressionBox = common::IndirectOptional<DelimitedExpression>;

struct TypeList {
  std::vector<TypeBox> types{};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

struct UnionTypeList {
  std::vector<TypeBox> alternatives{};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

// Named type with type arguments, e.g. Dict(String, Any) or Result(Bool, Any).
struct AppliedTypeNode {
  std::string name{};
  TypeList args{};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

// Function type, e.g. (Any, String) => Bool or () => Any
struct FunctionTypeNode {
  TypeList params{};
  TypeBox return_type{std::nullopt};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

struct TypeNode {
  std::variant<std::string, QualifiedId, TypeListBox, UnionTypeListBox, AppliedTypeNodeBox, FunctionTypeNodeBox> value;
  bool variadic{false};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

struct NamedTarget {
  std::variant<std::string, QualifiedId> target;
  std::vector<TypeBox> explicit_type_args{};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

struct Parameter {
  std::string param_name{};
  TypeNode type{};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

struct ParameterDeclList {
  std::vector<Parameter> params{};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

struct Constant {
  std::variant<std::int64_t, std::uint64_t, double, bool, std::string, std::monostate> val;
  std::optional<diag::SourceSpan> span{std::nullopt};
};

struct DelimitedExpression {
  std::vector<ExpressionBox> items{};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

struct Atom {
  std::variant<std::monostate, DelimitedExpressionBox, ClosureExpressionBox, Constant, QualifiedId, std::string,
               NamedTargetBox>
      value;
  std::optional<diag::SourceSpan> span{std::nullopt};
};

struct ClosureExpression {
  std::vector<std::string> generic_params{};
  ParameterDeclList params{};
  TypeNode rtype{};
  ExpressionBox body{std::nullopt};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

[[nodiscard]] inline auto make_closure_expression_box(ClosureExpression value) -> ClosureExpressionBox {
  return ClosureExpressionBox(std::in_place, std::move(value));
}

struct Primary {
  Atom base{};
  std::vector<std::string> extra{};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

struct FlowExpression {
  Primary lhs{};
  std::vector<Primary> rhs{};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

struct Expression {
  FlowExpression expr{};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

struct ImportStatement {
  std::string module_name{};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

struct LetStatement {
  std::variant<std::string, QualifiedId> id;
  std::vector<std::string> generic_params{};
  ParameterDeclList params{};
  TypeNode rtype{};
  std::vector<std::string> doc_comments{};
  ExpressionBox expr{std::nullopt};
  bool is_builtin{false};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

struct TypeStatement {
  std::string name{};
  TypeNode target{};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

struct AliasStatement {
  std::string name{};
  TypeNode target{};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

struct ExpressionStatement {
  Expression expr{};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

using Statement = std::variant<ImportStatement, TypeStatement, AliasStatement, LetStatement, ExpressionStatement>;

struct Program {
  std::string source_name{};
  std::string source_text{};
  std::vector<Statement> statements{};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

}  // namespace fleaux::frontend::model

namespace fleaux::frontend::ir {

struct IRSimpleType;
using IRSimpleTypeBox = common::IndirectOptional<IRSimpleType>;

struct IRSimpleType {
  std::string name{};
  bool variadic{false};
  // Non-empty when this is a union type (e.g. Float64 | Int64 | UInt64).
  // `name` holds the first alternative; `alternatives` holds all of them.
  std::vector<std::string> alternatives{};
  // Structured union alternatives preserved for Phase 2 migration.
  // Compatibility note: current type checking still reads `alternatives` strings.
  std::vector<IRSimpleType> alternative_types{};
  // Non-empty when this is a tuple type and we have preserved its element structure.
  // Phase 2 compatibility note: current type checking still treats tuple compatibility
  // coarsely; this field exists so later phases can stop erasing tuple shape in IR.
  std::vector<IRSimpleType> tuple_items{};
  // Non-empty when this is an applied named type, e.g. Dict(String, Any).
  // `name` holds the outer type name; `type_args` holds the argument types.
  std::vector<IRSimpleType> type_args{};
  // When this is a function type, holds the parameter types and return type.
  // A non-nullopt function_sig means this IRSimpleType represents a callable.
  struct FunctionSignature {
    std::vector<IRSimpleType> param_types{};
    IRSimpleTypeBox return_type{std::nullopt};
  };
  std::optional<FunctionSignature> function_sig{std::nullopt};
  // Phase 2 bridge artifact: structured type node preserved from lowering.
  // Kept in parallel with existing fields to avoid behavior changes during migration.
  std::optional<types::TypeNode> bridge_type_node{std::nullopt};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

struct IRParam {
  std::string name{};
  IRSimpleType type{};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

struct IRImport {
  std::string module_name{};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

struct IRExpr;
struct IRClosureExpr;
using IRExprBox = common::IndirectOptional<IRExpr>;
using IRClosureExprBox = common::IndirectOptional<IRClosureExpr>;

struct IRConstant {
  std::variant<std::int64_t, std::uint64_t, double, bool, std::string, std::monostate> val;
  std::optional<diag::SourceSpan> span;
};

struct IRNameRef {
  std::optional<std::string> qualifier{std::nullopt};
  std::string name{};
  std::vector<IRSimpleType> explicit_type_args{};
  std::optional<std::string> resolved_symbol_key{std::nullopt};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

struct IROperatorRef {
  std::string op{};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

using IRCallTarget = std::variant<IRNameRef, IROperatorRef>;

struct IRTupleExpr {
  std::vector<IRExprBox> items{};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

struct IRFlowExpr {
  IRExprBox lhs{std::nullopt};
  IRCallTarget rhs{};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

struct IRExpr {
  std::variant<IRFlowExpr, IRTupleExpr, IRConstant, IRNameRef, IRClosureExprBox> node;
  std::optional<diag::SourceSpan> span{std::nullopt};
};

struct IRClosureExpr {
  std::vector<std::string> generic_params{};
  std::vector<IRParam> params{};
  IRSimpleType return_type{};
  IRExprBox body{std::nullopt};
  std::vector<std::string> captures{};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

[[nodiscard]] inline auto make_ir_closure_expr_box(IRClosureExpr value) -> IRClosureExprBox {
  return IRClosureExprBox(std::in_place, std::move(value));
}

struct IRLet {
  std::optional<std::string> qualifier{std::nullopt};
  std::string name{};
  std::string symbol_key{};
  std::vector<std::string> generic_params{};
  std::vector<IRParam> params{};
  IRSimpleType return_type{};
  std::vector<std::string> doc_comments{};
  IRExprBox body{std::nullopt};
  bool is_builtin{false};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

struct IRTypeDecl {
  std::string name{};
  IRSimpleType target{};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

struct IRAliasDecl {
  std::string name{};
  IRSimpleType target{};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

struct IRExprStatement {
  IRExpr expr{};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

struct IRProgram {
  std::vector<IRImport> imports{};
  std::vector<IRTypeDecl> type_decls{};
  std::vector<IRLet> lets{};
  std::vector<IRExprStatement> expressions{};
  std::vector<IRAliasDecl> alias_decls{};
  std::optional<diag::SourceSpan> span{std::nullopt};
};

}  // namespace fleaux::frontend::ir
