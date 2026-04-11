#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "fleaux/frontend/diagnostics.hpp"

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
using TypeRef = std::shared_ptr<TypeNode>;

struct TypeList {
  std::vector<TypeRef> types;
  std::optional<diag::SourceSpan> span;
};

struct TypeNode {
  std::variant<std::string, QualifiedId, std::shared_ptr<TypeList>> value;
  std::optional<diag::SourceSpan> span;
};

struct Parameter {
  std::string param_name;
  TypeRef type;
  std::optional<diag::SourceSpan> span;
};

struct ParameterDeclList {
  std::vector<Parameter> params;
  std::optional<diag::SourceSpan> span;
};

struct Constant {
  std::variant<std::int64_t, double, bool, std::string, std::monostate> val;
  std::optional<diag::SourceSpan> span;
};

struct Expression;
using ExpressionPtr = std::shared_ptr<Expression>;

struct ClosureExpression;
using ClosureExpressionPtr = std::shared_ptr<ClosureExpression>;

struct DelimitedExpression {
  std::vector<ExpressionPtr> items;
  std::optional<diag::SourceSpan> span;
};

struct Atom {
  std::shared_ptr<DelimitedExpression> inner;
  ClosureExpressionPtr closure;
  std::optional<Constant> constant;
  std::optional<QualifiedId> qualified_var;
  std::optional<std::string> var;
  std::optional<diag::SourceSpan> span;
};

struct ClosureExpression {
  ParameterDeclList params;
  TypeRef rtype;
  ExpressionPtr body;
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
  ParameterDeclList params;
  TypeRef rtype;
  std::variant<std::string, ExpressionPtr> expr;
  std::optional<diag::SourceSpan> span;
};

struct ExpressionStatement {
  ExpressionPtr expr;
  std::optional<diag::SourceSpan> span;
};

using Statement = std::variant<ImportStatement, LetStatement, ExpressionStatement>;

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
using IRExprPtr = std::shared_ptr<IRExpr>;

struct IRClosureExpr;
using IRClosureExprPtr = std::shared_ptr<IRClosureExpr>;

struct IRConstant {
  std::variant<std::int64_t, double, bool, std::string, std::monostate> val;
  std::optional<diag::SourceSpan> span;
};

struct IRNameRef {
  std::optional<std::string> qualifier;
  std::string name;
  std::optional<diag::SourceSpan> span;
};

struct IROperatorRef {
  std::string op;
  std::optional<diag::SourceSpan> span;
};

using IRCallTarget = std::variant<IRNameRef, IROperatorRef>;

struct IRTupleExpr {
  std::vector<IRExprPtr> items;
  std::optional<diag::SourceSpan> span;
};

struct IRFlowExpr {
  IRExprPtr lhs;
  IRCallTarget rhs;
  std::optional<diag::SourceSpan> span;
};

struct IRExpr {
  std::variant<IRFlowExpr, IRTupleExpr, IRConstant, IRNameRef, IRClosureExprPtr> node;
  std::optional<diag::SourceSpan> span;
};

struct IRClosureExpr {
  std::vector<IRParam> params;
  IRSimpleType return_type;
  IRExprPtr body;
  std::vector<std::string> captures;
  std::optional<diag::SourceSpan> span;
};

struct IRLet {
  std::optional<std::string> qualifier;
  std::string name;
  std::vector<IRParam> params;
  IRSimpleType return_type;
  IRExprPtr body;
  bool is_builtin = false;
  std::optional<diag::SourceSpan> span;
};

struct IRExprStatement {
  IRExprPtr expr;
  std::optional<diag::SourceSpan> span;
};

struct IRProgram {
  std::vector<IRImport> imports;
  std::vector<IRLet> lets;
  std::vector<IRExprStatement> expressions;
  std::optional<diag::SourceSpan> span;
};

}  // namespace fleaux::frontend::ir

