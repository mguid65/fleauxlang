"""
fleaux_lowering.py – Lower a textX Program model to an IRProgram.

This is the only module that knows about raw textX object shapes.
Everything downstream (transpiler, future passes) operates on the IR.
"""
from __future__ import annotations

from .fleaux_ast import (
    IRProgram, IRImport, IRLet, IRExprStatement,
    IRParam, IRSimpleType, IRTupleType, IRType,
    IRConstant, IRNameRef, IROperatorRef,
    IRTupleExpr, IRFlowExpr,
    IRExpr, IRCallTarget, IRStatement,
)
from .fleaux_diagnostics import format_diagnostic


class FleauxLoweringError(Exception):
    def __init__(
        self,
        message: str,
        *,
        stage: str = "lowering",
        stage_index: int | None = None,
        hint: str | None = None,
        span=None,
    ):
        self.message = message
        self.stage = stage
        self.stage_index = stage_index
        self.hint = hint
        self.span = span
        self.line = span.line if span is not None else None
        self.col = span.col if span is not None else None
        super().__init__(self._fmt())

    def _fmt(self) -> str:
        return format_diagnostic(
            stage=self.stage,
            message=self.message,
            span=self.span,
            stage_index=self.stage_index,
            hint=self.hint,
        )


def lower(textx_model) -> IRProgram:
    """Lower a textX Program model into a stable IRProgram."""
    return IRProgram(
        statements=[_lower_statement(s) for s in textx_model.statements],
        span=_node_span(textx_model),
    )


# ── Statements ────────────────────────────────────────────────────────────────

def _lower_statement(stmt) -> IRStatement:
    t = type(stmt).__name__
    if t == "ImportStatement":
        return IRImport(module_name=stmt.module_name, span=_node_span(stmt))
    if t == "LetStatement":
        return _lower_let(stmt)
    if t == "ExpressionStatement":
        return IRExprStatement(expr=_lower_expr(stmt.expr), span=_node_span(stmt))
    raise FleauxLoweringError(
        f"Cannot lower statement type '{t}'.",
        hint="Use an import, let, or expression statement.",
        span=_node_span(stmt),
    )


def _lower_let(stmt) -> IRLet:
    qualifier, name = _split_id(stmt.id)
    params = _lower_param_list(stmt.params)
    return_type = _lower_type(stmt.rtype, span=_node_span(stmt))
    is_builtin = isinstance(stmt.expr, str) and stmt.expr == "__builtin__"
    body = None if is_builtin else _lower_expr(stmt.expr)
    return IRLet(
        qualifier=qualifier,
        name=name,
        params=params,
        return_type=return_type,
        body=body,
        is_builtin=is_builtin,
        span=_node_span(stmt),
    )


def _lower_param_list(params_node) -> list[IRParam]:
    if params_node is None:
        return []
    return [
        IRParam(name=p.param_name, type=_lower_type(p.type, span=_node_span(p)), span=_node_span(p))
        for p in params_node.params
    ]


# ── Types ─────────────────────────────────────────────────────────────────────

def _lower_type(t, *, span=None) -> IRType:
    node_span = _node_span(t) or span
    if isinstance(t, str):
        variadic = t.endswith("...")
        name = t.removesuffix("...").strip()
        return IRSimpleType(name=name, variadic=variadic, span=node_span)
    t_class = type(t).__name__
    if t_class == "TypeList":
        # This is the inner types list of a Tuple(...) type.
        types = [_lower_type(inner, span=node_span) for inner in t.types]
        return IRTupleType(types=types, variadic=False, span=node_span)
    raise FleauxLoweringError(
        f"Cannot lower type node class '{t_class}'.",
        hint="Use a simple type name, a simple variadic type (e.g. 'Any...'), or 'Tuple(...)'.",
        span=node_span,
    )


# ── Expressions ───────────────────────────────────────────────────────────────

def _lower_expr(expr) -> IRExpr:
    node_type = type(expr).__name__
    expr_span = _node_span(expr)

    # Transparent wrapper node.
    if node_type == "Expression":
        return _lower_expr(expr.expr)

    if node_type == "FlowExpression":
        # FlowExpression is left-associative: lhs ('->' rhs)*.
        # We support two stage forms in the chain:
        #  1) call target stage:   <expr> -> Std.Add
        #  2) tuple-template stage: <expr> -> (_, 2) -> Std.Divide
        #     where '_' is replaced by the current pipeline value.
        result = _lower_primary(expr.lhs)

        rhs_chain = list(expr.rhs or [])
        i = 0
        while i < len(rhs_chain):
            call_target = _try_extract_call_target_from_primary(rhs_chain[i])
            if call_target is not None:
                result = IRFlowExpr(lhs=result, rhs=call_target, span=expr_span)
                i += 1
                continue

            template_expr = _lower_primary(rhs_chain[i])
            if isinstance(template_expr, IRFlowExpr) and isinstance(template_expr.lhs, IRTupleExpr):
                templated_lhs = _replace_placeholder(template_expr.lhs, result)
                result = IRFlowExpr(lhs=templated_lhs, rhs=template_expr.rhs, span=expr_span)
                i += 1
                continue

            if not isinstance(template_expr, IRTupleExpr):
                raise FleauxLoweringError(
                    "Invalid pipeline stage shape: non-call stages must be tuple templates.",
                    stage_index=i + 1,
                    hint="Use a call target like '-> Std.Add' or a tuple template like '-> (_, 2) -> Std.Add'.",
                    span=_node_span(rhs_chain[i]),
                )
            if i + 1 >= len(rhs_chain):
                raise FleauxLoweringError(
                    "Tuple template stage is missing a following call target.",
                    stage_index=i + 1,
                    hint="Append a call target, e.g. '-> (_, 2) -> Std.Divide'.",
                    span=_node_span(rhs_chain[i]),
                )

            next_target = _extract_call_target_from_primary(rhs_chain[i + 1])
            templated_lhs = _replace_placeholder(template_expr, result)
            result = IRFlowExpr(lhs=templated_lhs, rhs=next_target, span=expr_span)
            i += 2

        return result

    if node_type == "DelimitedExpression":
        return IRTupleExpr(items=[_lower_expr(e) for e in expr.items], span=expr_span)

    # Bare Python primitives (textX sometimes produces these for constant matches).
    if isinstance(expr, (int, float, bool)):
        return IRConstant(val=expr, span=expr_span)
    if isinstance(expr, str):
        return IRConstant(val=expr, span=expr_span)

    raise FleauxLoweringError(
        f"Cannot lower expression node type '{node_type}'.",
        hint="Use an expression wrapper, flow expression, delimited tuple expression, or literal constant.",
        span=expr_span,
    )


def _lower_primary(primary) -> IRExpr:
    """Lower a Primary node, which is: base=Atom (extra=TupleConstruction)*"""
    node_type = type(primary).__name__
    primary_span = _node_span(primary)

    if node_type == "Primary":
        # Start with the base atom
        result = _lower_atom(primary.base)

        # Apply any tuple constructions (flow chains)
        if primary.extra:
            for construction in primary.extra:
                call_target = _lower_call_target(construction.target)
                result = IRFlowExpr(lhs=result, rhs=call_target, span=primary_span)

        return result

    # If it's not a Primary, try to lower it as an atom directly
    return _lower_atom(primary)


def _lower_atom(atom) -> IRExpr:
    """Lower an Atom node: '(' inner=DelimitedExpression ')' | constant | qualified_var | var"""
    node_type = type(atom).__name__
    atom_span = _node_span(atom)

    if node_type == "Atom":
        if atom.inner is not None:
            # Parenthesized expression: (DelimitedExpression)
            return _lower_expr(atom.inner)
        if atom.constant is not None:
            return IRConstant(val=atom.constant.val, span=_node_span(atom.constant) or atom_span)
        if atom.qualified_var is not None:
            q, n = _split_id(atom.qualified_var)
            return IRNameRef(qualifier=q, name=n, span=_node_span(atom.qualified_var) or atom_span)
        if atom.var is not None:
            return IRNameRef(qualifier=None, name=atom.var, span=atom_span)
        # Empty tuple case: all attributes are None, means we have ()
        if (atom.inner is None and atom.constant is None and
            atom.qualified_var is None and atom.var is None):
            return IRTupleExpr(items=[], span=atom_span)
        raise FleauxLoweringError(
            "Encountered an atom with no resolvable variant.",
            hint="Use a parenthesized expression, constant, qualified name, or identifier.",
            span=atom_span,
        )

    # Fallback for bare primitives
    if isinstance(atom, (int, float, bool)):
        return IRConstant(val=atom, span=atom_span)
    if isinstance(atom, str):
        return IRConstant(val=atom, span=atom_span)

    raise FleauxLoweringError(
        f"Cannot lower atom node type '{node_type}'.",
        hint="Use an Atom node or a primitive literal value.",
        span=atom_span,
    )


def _extract_call_target_from_primary(primary) -> IRCallTarget:
    """Extract a CallTarget from a Primary that represents a function/operator reference."""
    # In the new grammar, when we have 'a -> b -> c', the 'b' and 'c' are Primaries
    # that should be simple name/qualified references or operators, not complex expressions
    node_type = type(primary).__name__
    primary_span = _node_span(primary)

    if node_type == "Primary":
        atom = primary.base
        # Should be a simple Atom with just a var/qualified_var/op reference
        if atom.qualified_var is not None:
            q, n = _split_id(atom.qualified_var)
            return IRNameRef(qualifier=q, name=n, span=_node_span(atom.qualified_var) or primary_span)
        if atom.var is not None:
            # Check if it's an operator
            if _is_operator(atom.var):
                return IROperatorRef(op=atom.var, span=primary_span)
            return IRNameRef(qualifier=None, name=atom.var, span=primary_span)
        raise FleauxLoweringError(
            "Primary used as flow target must be a simple name, qualified name, or operator.",
            hint="Valid targets: 'Std.Add', 'MyFunc', '+', '/', '&&'.",
            span=primary_span,
        )

    raise FleauxLoweringError(
        f"Cannot extract call target from node type '{node_type}'.",
        hint="Use a callable stage target such as 'Std.Add', 'MyFunc', or '+'.",
        span=primary_span,
    )


def _try_extract_call_target_from_primary(primary) -> IRCallTarget | None:
    try:
        return _extract_call_target_from_primary(primary)
    except FleauxLoweringError:
        return None


def _replace_placeholder(template: IRExpr, current_value: IRExpr) -> IRExpr:
    """Replace '_' name references in a tuple template with the current pipeline value."""
    replaced = _replace_placeholder_impl(template, current_value)
    if not _contains_placeholder(replaced):
        return replaced
    # If placeholders remain, nested forms were not fully traversed.
    raise FleauxLoweringError(
        "Unresolved '_' placeholder remained in tuple template.",
        hint="Use '_' only as an argument position inside tuple templates like '(_, 2)'.",
        span=_node_span(template),
    )


def _replace_placeholder_impl(expr: IRExpr, current_value: IRExpr) -> IRExpr:
    if isinstance(expr, IRNameRef) and expr.qualifier is None and expr.name == "_":
        return current_value
    if isinstance(expr, IRTupleExpr):
        return IRTupleExpr(items=[_replace_placeholder_impl(item, current_value) for item in expr.items], span=expr.span)
    if isinstance(expr, IRFlowExpr):
        return IRFlowExpr(
            lhs=_replace_placeholder_impl(expr.lhs, current_value),
            rhs=expr.rhs,
            span=expr.span,
        )
    return expr


def _contains_placeholder(expr: IRExpr) -> bool:
    if isinstance(expr, IRNameRef):
        return expr.qualifier is None and expr.name == "_"
    if isinstance(expr, IRTupleExpr):
        return any(_contains_placeholder(item) for item in expr.items)
    if isinstance(expr, IRFlowExpr):
        return _contains_placeholder(expr.lhs)
    return False


def _lower_call_target(target) -> IRCallTarget:
    """Lower a CallTarget node: qualified_var | var | op"""
    if target.qualified_var is not None:
        q, n = _split_id(target.qualified_var)
        return IRNameRef(qualifier=q, name=n, span=_node_span(target))
    if target.var is not None:
        return IRNameRef(qualifier=None, name=target.var, span=_node_span(target))
    if target.op is not None:
        return IROperatorRef(op=target.op, span=_node_span(target))
    raise FleauxLoweringError(
        "Cannot lower call target: no variant matched.",
        hint="Use a qualified name, identifier, or operator as the call target.",
        span=_node_span(target),
    )


# ── Helpers ───────────────────────────────────────────────────────────────────

def _is_operator(name: str) -> bool:
    """Check if a name is a recognized operator."""
    operators = {'^', '/', '*', '%', '+', '-', '==', '!=', '<', '>', '>=', '<=', '!', '&&', '||'}
    return name in operators


def _node_span(node):
    return getattr(node, "span", None)


def _split_id(id_node) -> tuple[str | None, str]:
    """Return (qualifier, name) from a QualifiedId object or a plain Identifier string."""
    if isinstance(id_node, str):
        return None, id_node
    # QualifiedId: .qualifier.qualifier (str) and .id (str)
    return id_node.qualifier.qualifier, id_node.id



