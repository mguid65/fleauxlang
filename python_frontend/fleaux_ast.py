"""
fleaux_ast.py – Stable IR (Intermediate Representation) for Fleaux programs.

These dataclasses are the single, grammar-independent representation that the
transpiler (and any future passes) operate on.  The textX model is only ever
seen inside fleaux_lowering.py; everything downstream uses these types.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Union

from .fleaux_diagnostics import SourceSpan


# ── Types ─────────────────────────────────────────────────────────────────────

@dataclass
class IRSimpleType:
    """A primitive or user-defined type name: Number, String, Bool, Null, Any, MyType."""
    name: str
    variadic: bool = False
    span: SourceSpan | None = None


@dataclass
class IRTupleType:
    """Tuple(T1, T2, ...) structural type."""
    types: list[IRType]
    variadic: bool = False
    span: SourceSpan | None = None


IRType = Union[IRSimpleType, IRTupleType]


# ── Parameters ────────────────────────────────────────────────────────────────

@dataclass
class IRParam:
    name: str
    type: IRType
    span: SourceSpan | None = None


# ── Expressions ───────────────────────────────────────────────────────────────

@dataclass
class IRConstant:
    """A literal constant: a number, string, bool, or null."""
    val: int | float | bool | str | None
    span: SourceSpan | None = None


@dataclass
class IRNameRef:
    """A reference to an identifier, optionally qualified: Foo or Std.Add."""
    qualifier: str | None
    name: str
    span: SourceSpan | None = None


@dataclass
class IROperatorRef:
    """An operator used as a pipeline call target: +, -, *, /, ^, etc."""
    op: str
    span: SourceSpan | None = None


@dataclass
class IRTupleExpr:
    """A parenthesized, comma-separated expression list: (a, b, c).

    A single-item tuple (x,) is still represented here with len(items) == 1.
    """
    items: list[IRExpr]
    span: SourceSpan | None = None


@dataclass
class IRFlowExpr:
    """A pipeline expression: (lhs) -> rhs."""
    lhs: IRTupleExpr
    rhs: IRCallTarget
    span: SourceSpan | None = None


IRExpr = Union[IRFlowExpr, IRTupleExpr, IRConstant, IRNameRef]
IRCallTarget = Union[IRNameRef, IROperatorRef]


# ── Statements ────────────────────────────────────────────────────────────────

@dataclass
class IRImport:
    module_name: str
    span: SourceSpan | None = None


@dataclass
class IRLet:
    """A let-binding: a named function/constant definition.

    If is_builtin is True, body is None and the implementation is
    provided by the active runtime backend (C++ runtime builtins).
    """
    qualifier: str | None
    name: str
    params: list[IRParam]
    return_type: IRType
    body: IRExpr | None
    is_builtin: bool = False
    span: SourceSpan | None = None


@dataclass
class IRExprStatement:
    expr: IRExpr
    span: SourceSpan | None = None


IRStatement = Union[IRImport, IRLet, IRExprStatement]


# ── Program ───────────────────────────────────────────────────────────────────

@dataclass
class IRProgram:
    statements: list[IRStatement] = field(default_factory=list)
    span: SourceSpan | None = None



