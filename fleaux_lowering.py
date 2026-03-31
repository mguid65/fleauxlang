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
from fleaux_diagnostics import format_diagnostic


class FleauxLoweringError(Exception):
    def __init__(
        self,
        message: str,
        *,
        stage: str = "lowering",
        stage_index: int | None = None,
        hint: str | None = None,
    ):
        self.message = message
        self.stage = stage
        self.stage_index = stage_index
        self.hint = hint
        super().__init__(self._fmt())

    def _fmt(self) -> str:
        return format_diagnostic(
            stage=self.stage,
            message=self.message,
            stage_index=self.stage_index,
            hint=self.hint,
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
    raise FleauxLoweringError(
        f"Cannot lower statement type '{t}'.",
        hint="Use an import, let, or expression statement.",
    )


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
    raise FleauxLoweringError(
        f"Cannot lower type node class '{t_class}'.",
        hint="Use a simple type name, a simple variadic type (e.g. 'Any...'), or 'Tuple(...)'.",
    )


# ── Expressions ───────────────────────────────────────────────────────────────

def _lower_expr(expr) -> IRExpr:
    node_type = type(expr).__name__

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
                result = IRFlowExpr(lhs=result, rhs=call_target)
                i += 1
                continue

            template_expr = _lower_primary(rhs_chain[i])
            if isinstance(template_expr, IRFlowExpr) and isinstance(template_expr.lhs, IRTupleExpr):
                templated_lhs = _replace_placeholder(template_expr.lhs, result)
                result = IRFlowExpr(lhs=templated_lhs, rhs=template_expr.rhs)
                i += 1
                continue

            if not isinstance(template_expr, IRTupleExpr):
                raise FleauxLoweringError(
                    "Invalid pipeline stage shape: non-call stages must be tuple templates.",
                    stage_index=i + 1,
                    hint="Use a call target like '-> Std.Add' or a tuple template like '-> (_, 2) -> Std.Add'.",
                )
            if i + 1 >= len(rhs_chain):
                raise FleauxLoweringError(
                    "Tuple template stage is missing a following call target.",
                    stage_index=i + 1,
                    hint="Append a call target, e.g. '-> (_, 2) -> Std.Divide'.",
                )

            next_target = _extract_call_target_from_primary(rhs_chain[i + 1])
            templated_lhs = _replace_placeholder(template_expr, result)
            result = IRFlowExpr(lhs=templated_lhs, rhs=next_target)
            i += 2

        return result

    if node_type == "DelimitedExpression":
        return IRTupleExpr(items=[_lower_expr(e) for e in expr.items])

    # Bare Python primitives (textX sometimes produces these for constant matches).
    if isinstance(expr, (int, float, bool)):
        return IRConstant(val=expr)
    if isinstance(expr, str):
        return IRConstant(val=expr)

    raise FleauxLoweringError(
        f"Cannot lower expression node type '{node_type}'.",
        hint="Use an expression wrapper, flow expression, delimited tuple expression, or literal constant.",
    )


def _lower_primary(primary) -> IRExpr:
    """Lower a Primary node, which is: base=Atom (extra=TupleConstruction)*"""
    node_type = type(primary).__name__
    
    if node_type == "Primary":
        # Start with the base atom
        result = _lower_atom(primary.base)
        
        # Apply any tuple constructions (flow chains)
        if primary.extra:
            for construction in primary.extra:
                call_target = _lower_call_target(construction.target)
                result = IRFlowExpr(lhs=result, rhs=call_target)
        
        return result
    
    # If it's not a Primary, try to lower it as an atom directly
    return _lower_atom(primary)


def _lower_atom(atom) -> IRExpr:
    """Lower an Atom node: '(' inner=DelimitedExpression ')' | constant | qualified_var | var"""
    node_type = type(atom).__name__
    
    if node_type == "Atom":
        if atom.inner is not None:
            # Parenthesized expression: (DelimitedExpression)
            return _lower_expr(atom.inner)
        if atom.constant is not None:
            return IRConstant(val=atom.constant.val)
        if atom.qualified_var is not None:
            q, n = _split_id(atom.qualified_var)
            return IRNameRef(qualifier=q, name=n)
        if atom.var is not None:
            return IRNameRef(qualifier=None, name=atom.var)
        # Empty tuple case: all attributes are None, means we have ()
        if (atom.inner is None and atom.constant is None and 
            atom.qualified_var is None and atom.var is None):
            return IRTupleExpr(items=[])
        raise FleauxLoweringError(
            "Encountered an atom with no resolvable variant.",
            hint="Use a parenthesized expression, constant, qualified name, or identifier.",
        )
    
    # Fallback for bare primitives
    if isinstance(atom, (int, float, bool)):
        return IRConstant(val=atom)
    if isinstance(atom, str):
        return IRConstant(val=atom)
    
    raise FleauxLoweringError(
        f"Cannot lower atom node type '{node_type}'.",
        hint="Use an Atom node or a primitive literal value.",
    )


def _extract_call_target_from_primary(primary) -> IRCallTarget:
    """Extract a CallTarget from a Primary that represents a function/operator reference."""
    # In the new grammar, when we have 'a -> b -> c', the 'b' and 'c' are Primaries
    # that should be simple name/qualified references or operators, not complex expressions
    node_type = type(primary).__name__
    
    if node_type == "Primary":
        atom = primary.base
        # Should be a simple Atom with just a var/qualified_var/op reference
        if atom.qualified_var is not None:
            q, n = _split_id(atom.qualified_var)
            return IRNameRef(qualifier=q, name=n)
        if atom.var is not None:
            # Check if it's an operator
            if _is_operator(atom.var):
                return IROperatorRef(op=atom.var)
            return IRNameRef(qualifier=None, name=atom.var)
        raise FleauxLoweringError(
            "Primary used as flow target must be a simple name, qualified name, or operator.",
            hint="Valid targets: 'Std.Add', 'MyFunc', '+', '/', '&&'.",
        )
    
    raise FleauxLoweringError(
        f"Cannot extract call target from node type '{node_type}'.",
        hint="Use a callable stage target such as 'Std.Add', 'MyFunc', or '+'.",
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
    )


def _replace_placeholder_impl(expr: IRExpr, current_value: IRExpr) -> IRExpr:
    if isinstance(expr, IRNameRef) and expr.qualifier is None and expr.name == "_":
        return current_value
    if isinstance(expr, IRTupleExpr):
        return IRTupleExpr(items=[_replace_placeholder_impl(item, current_value) for item in expr.items])
    if isinstance(expr, IRFlowExpr):
        return IRFlowExpr(
            lhs=_replace_placeholder_impl(expr.lhs, current_value),
            rhs=expr.rhs,
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
        return IRNameRef(qualifier=q, name=n)
    if target.var is not None:
        return IRNameRef(qualifier=None, name=target.var)
    if target.op is not None:
        return IROperatorRef(op=target.op)
    raise FleauxLoweringError(
        "Cannot lower call target: no variant matched.",
        hint="Use a qualified name, identifier, or operator as the call target.",
    )


# ── Helpers ───────────────────────────────────────────────────────────────────

def _is_operator(name: str) -> bool:
    """Check if a name is a recognized operator."""
    operators = {'^', '/', '*', '%', '+', '-', '==', '!=', '<', '>', '>=', '<=', '!', '&&', '||'}
    return name in operators


def _split_id(id_node) -> tuple[str | None, str]:
    """Return (qualifier, name) from a QualifiedId object or a plain Identifier string."""
    if isinstance(id_node, str):
        return None, id_node
    # QualifiedId: .qualifier.qualifier (str) and .id (str)
    return id_node.qualifier.qualifier, id_node.id

