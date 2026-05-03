#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "fleaux/frontend/box.hpp"
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
using TypeBox = Box<TypeNode>;

struct NamedTarget;
using NamedTargetBox = Box<NamedTarget>;

struct TypeList {
  std::vector<TypeBox> types;
  std::optional<diag::SourceSpan> span;
};

struct UnionTypeList {
  std::vector<TypeBox> alternatives;
  std::optional<diag::SourceSpan> span;
};

// Named type with type arguments, e.g. Dict(String, Any) or Result(Bool, Any).
struct AppliedTypeNode {
  std::string name;
  TypeList args;
  std::optional<diag::SourceSpan> span;
};

// Function type, e.g. (Any, String) => Bool or () => Any
struct FunctionTypeNode {
  TypeList params;
  TypeBox return_type;
  std::optional<diag::SourceSpan> span;
};

struct TypeNode {
  std::variant<std::string, QualifiedId, Box<TypeList>, Box<UnionTypeList>, Box<AppliedTypeNode>, Box<FunctionTypeNode>>
      value;
  bool variadic = false;
  std::optional<diag::SourceSpan> span;
};

struct NamedTarget {
  std::variant<std::string, QualifiedId> target;
  std::vector<TypeBox> explicit_type_args;
  std::optional<diag::SourceSpan> span;
};

struct Parameter {
  std::string param_name;
  TypeNode type;
  std::optional<diag::SourceSpan> span;
};

struct ParameterDeclList {
  std::vector<Parameter> params;
  std::optional<diag::SourceSpan> span;
};

struct Constant {
  std::variant<std::int64_t, std::uint64_t, double, bool, std::string, std::monostate> val;
  std::optional<diag::SourceSpan> span;
};

struct Expression;
using ExpressionBox = Box<Expression>;

struct ClosureExpression;
using ClosureExpressionBox = Box<ClosureExpression>;

struct DelimitedExpression {
  std::vector<ExpressionBox> items;
  std::optional<diag::SourceSpan> span;
};

struct Atom {
  std::variant<std::monostate, Box<DelimitedExpression>, ClosureExpressionBox, Constant, QualifiedId, std::string,
               NamedTargetBox>
      value;
  std::optional<diag::SourceSpan> span;
};

struct ClosureExpression {
  std::vector<std::string> generic_params;
  ParameterDeclList params;
  TypeNode rtype;
  ExpressionBox body;
  std::optional<diag::SourceSpan> span;
};

struct Primary {
  Atom base;
  std::vector<std::string> extra;
  std::optional<diag::SourceSpan> span;
};

struct FlowExpression {
  Primary lhs;
  std::vector<Primary> rhs;
  std::optional<diag::SourceSpan> span;
};

struct Expression {
  FlowExpression expr;
  std::optional<diag::SourceSpan> span;
};

struct ImportStatement {
  std::string module_name;
  std::optional<diag::SourceSpan> span;
};

struct LetStatement {
  std::variant<std::string, QualifiedId> id;
  std::vector<std::string> generic_params;
  ParameterDeclList params;
  TypeNode rtype;
  std::vector<std::string> doc_comments;
  std::optional<Expression> expr;
  bool is_builtin = false;
  std::optional<diag::SourceSpan> span;
};

struct TypeStatement {
  std::string name;
  TypeNode target;
  std::optional<diag::SourceSpan> span;
};

struct AliasStatement {
  std::string name;
  TypeNode target;
  std::optional<diag::SourceSpan> span;
};

struct ExpressionStatement {
  Expression expr;
  std::optional<diag::SourceSpan> span;
};

using Statement = std::variant<ImportStatement, TypeStatement, AliasStatement, LetStatement, ExpressionStatement>;

struct Program {
  std::string source_name;
  std::string source_text;
  std::vector<Statement> statements;
  std::optional<diag::SourceSpan> span;
};

}  // namespace fleaux::frontend::model

namespace fleaux::frontend::ir {

struct IRSimpleType {
  std::string name;
  bool variadic = false;
  // Non-empty when this is a union type (e.g. Float64 | Int64 | UInt64).
  // `name` holds the first alternative; `alternatives` holds all of them.
  std::vector<std::string> alternatives;
  // Structured union alternatives preserved for Phase 2 migration.
  // Compatibility note: current type checking still reads `alternatives` strings.
  std::vector<IRSimpleType> alternative_types;
  // Non-empty when this is a tuple type and we have preserved its element structure.
  // Phase 2 compatibility note: current type checking still treats tuple compatibility
  // coarsely; this field exists so later phases can stop erasing tuple shape in IR.
  std::vector<IRSimpleType> tuple_items;
  // Non-empty when this is an applied named type, e.g. Dict(String, Any).
  // `name` holds the outer type name; `type_args` holds the argument types.
  std::vector<IRSimpleType> type_args;
  // When this is a function type, holds the parameter types and return type.
  // A non-nullopt function_sig means this IRSimpleType represents a callable.
  struct FunctionSignature {
    std::vector<IRSimpleType> param_types;
    Box<IRSimpleType> return_type;
  };
  std::optional<FunctionSignature> function_sig;
  // Phase 2 bridge artifact: structured type node preserved from lowering.
  // Kept in parallel with existing fields to avoid behavior changes during migration.
  std::optional<types::TypeNode> bridge_type_node;
  std::optional<diag::SourceSpan> span;
};

struct IRParam {
  std::string name;
  IRSimpleType type;
  std::optional<diag::SourceSpan> span;
};

struct IRImport {
  std::string module_name;
  std::optional<diag::SourceSpan> span;
};

struct IRExpr;
using IRExprBox = Box<IRExpr>;

struct IRClosureExpr;
using IRClosureExprBox = Box<IRClosureExpr>;

struct IRConstant {
  std::variant<std::int64_t, std::uint64_t, double, bool, std::string, std::monostate> val;
  std::optional<diag::SourceSpan> span;
};

struct IRNameRef {
  std::optional<std::string> qualifier;
  std::string name;
  std::vector<IRSimpleType> explicit_type_args;
  std::optional<std::string> resolved_symbol_key;
  std::optional<diag::SourceSpan> span;
};

struct IROperatorRef {
  std::string op;
  std::optional<diag::SourceSpan> span;
};

using IRCallTarget = std::variant<IRNameRef, IROperatorRef>;

struct IRTupleExpr {
  std::vector<IRExprBox> items;
  std::optional<diag::SourceSpan> span;
};

struct IRFlowExpr {
  IRExprBox lhs;
  IRCallTarget rhs;
  std::optional<diag::SourceSpan> span;
};

struct IRExpr {
  std::variant<IRFlowExpr, IRTupleExpr, IRConstant, IRNameRef, IRClosureExprBox> node;
  std::optional<diag::SourceSpan> span;
};

struct IRClosureExpr {
  std::vector<std::string> generic_params;
  std::vector<IRParam> params;
  IRSimpleType return_type;
  IRExprBox body;
  std::vector<std::string> captures;
  std::optional<diag::SourceSpan> span;
};

struct IRLet {
  std::optional<std::string> qualifier;
  std::string name;
  std::string symbol_key;
  std::vector<std::string> generic_params;
  std::vector<IRParam> params;
  IRSimpleType return_type;
  std::vector<std::string> doc_comments;
  std::optional<IRExpr> body;
  bool is_builtin = false;
  std::optional<diag::SourceSpan> span;
};

struct IRTypeDecl {
  std::string name;
  IRSimpleType target;
  std::optional<diag::SourceSpan> span;
};

struct IRAliasDecl {
  std::string name;
  IRSimpleType target;
  std::optional<diag::SourceSpan> span;
};

struct IRExprStatement {
  IRExpr expr;
  std::optional<diag::SourceSpan> span;
};

struct IRProgram {
  std::vector<IRImport> imports;
  std::vector<IRTypeDecl> type_decls;
  std::vector<IRLet> lets;
  std::vector<IRExprStatement> expressions;
  std::vector<IRAliasDecl> alias_decls;
  std::optional<diag::SourceSpan> span;
};

}  // namespace fleaux::frontend::ir
