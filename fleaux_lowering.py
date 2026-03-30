"""
fleaux_lowering.py – Lower a textX Program model to an IRProgram.

This is the only module that knows about raw textX object shapes.
Everything downstream (transpiler, future passes) operates on the IR.
"""
from __future__ import annotations

from fleaux_ast import (
    IRProgram, IRImport, IRLet, IRExprStatement,
    IRParam, IRSimpleType, IRTupleType, IRType,
    IRConstant, IRNameRef, IROperatorRef,
    IRTupleExpr, IRFlowExpr,
    IRExpr, IRCallTarget, IRStatement,
)


def lower(textx_model) -> IRProgram:
    """Lower a textX Program model into a stable IRProgram."""
    return IRProgram(
        statements=[_lower_statement(s) for s in textx_model.statements]
    )


# ── Statements ────────────────────────────────────────────────────────────────

def _lower_statement(stmt) -> IRStatement:
    t = type(stmt).__name__
    if t == "ImportStatement":
        return IRImport(module_name=stmt.module_name)
    if t == "LetStatement":
        return _lower_let(stmt)
    if t == "ExpressionStatement":
        return IRExprStatement(expr=_lower_expr(stmt.expr))
    raise NotImplementedError(f"Cannot lower statement type '{t}'.")


def _lower_let(stmt) -> IRLet:
    qualifier, name = _split_id(stmt.id)
    params = _lower_param_list(stmt.params)
    return_type = _lower_type(stmt.rtype)
    is_builtin = isinstance(stmt.expr, str) and stmt.expr == "__builtin__"
    body = None if is_builtin else _lower_expr(stmt.expr)
    return IRLet(
        qualifier=qualifier,
        name=name,
        params=params,
        return_type=return_type,
        body=body,
        is_builtin=is_builtin,
    )


def _lower_param_list(params_node) -> list[IRParam]:
    if params_node is None:
        return []
    return [
        IRParam(name=p.param_name, type=_lower_type(p.type))
        for p in params_node.params
    ]


# ── Types ─────────────────────────────────────────────────────────────────────

def _lower_type(t) -> IRType:
    if isinstance(t, str):
        variadic = t.endswith("...")
        name = t.removesuffix("...").strip()
        return IRSimpleType(name=name, variadic=variadic)
    t_class = type(t).__name__
    if t_class == "TypeList":
        # This is the inner types list of a Tuple(...) type.
        types = [_lower_type(inner) for inner in t.types]
        return IRTupleType(types=types, variadic=False)
    raise NotImplementedError(f"Cannot lower type node class '{t_class}'.")


# ── Expressions ───────────────────────────────────────────────────────────────

def _lower_expr(expr) -> IRExpr:
    node_type = type(expr).__name__

    # Transparent wrapper nodes – recurse through them.
    if node_type in ("Expression", "TermExpression"):
        return _lower_expr(expr.expr)

    if node_type == "FlowExpression":
        lhs = _lower_parenthesized(expr.lhs)
        rhs = _lower_call_target(expr.rhs)
        return IRFlowExpr(lhs=lhs, rhs=rhs)

    if node_type == "ParenthesizedExpression":
        return _lower_parenthesized(expr)

    if node_type == "DelimitedExpression":
        return IRTupleExpr(items=[_lower_expr(e) for e in expr.exprs])

    if node_type == "Term":
        if expr.constant is not None:
            return IRConstant(val=expr.constant.val)
        if expr.qualified_var is not None:
            q, n = _split_id(expr.qualified_var)
            return IRNameRef(qualifier=q, name=n)
        if expr.var is not None:
            return IRNameRef(qualifier=None, name=expr.var)
        raise NotImplementedError("Encountered a Term with no resolvable variant.")

    # Bare Python primitives (textX sometimes produces these for constant matches).
    if isinstance(expr, (int, float, bool)):
        return IRConstant(val=expr)
    if isinstance(expr, str):
        return IRConstant(val=expr)

    raise NotImplementedError(
        f"Cannot lower expression node type '{node_type}'."
    )


def _lower_parenthesized(node) -> IRTupleExpr:
    if node.expr is None:
        return IRTupleExpr(items=[])
    return IRTupleExpr(items=[_lower_expr(e) for e in node.expr.exprs])


def _lower_call_target(rhs) -> IRCallTarget:
    if rhs.qualified_var is not None:
        q, n = _split_id(rhs.qualified_var)
        return IRNameRef(qualifier=q, name=n)
    if rhs.var is not None:
        return IRNameRef(qualifier=None, name=rhs.var)
    if rhs.op is not None:
        return IROperatorRef(op=rhs.op)
    raise NotImplementedError("Cannot lower OpOrGraphId: no variant matched.")


# ── Helpers ───────────────────────────────────────────────────────────────────

def _split_id(id_node) -> tuple[str | None, str]:
    """Return (qualifier, name) from a QualifiedId object or a plain Identifier string."""
    if isinstance(id_node, str):
        return None, id_node
    # QualifiedId: .qualifier.qualifier (str) and .id (str)
    return id_node.qualifier.qualifier, id_node.id

