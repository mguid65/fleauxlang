from __future__ import annotations

from pathlib import Path
import shutil
import subprocess

from fleaux_ast import (
    IRConstant,
    IRExpr,
    IRExprStatement,
    IRFlowExpr,
    IRImport,
    IRLet,
    IRNameRef,
    IROperatorRef,
    IRProgram,
    IRTupleExpr,
)
from fleaux_lowering import lower
from fleaux_parser import parse_file


class GraphEmitError(Exception):
    pass


class GraphvizNotInstalledError(GraphEmitError):
    pass


class GraphvizRenderError(GraphEmitError):
    pass


class _DotBuilder:
    def __init__(self) -> None:
        self._next_id = 0
        self.lines: list[str] = [
            "digraph fleaux {",
            "  rankdir=LR;",
            "  node [shape=box, fontname=\"monospace\"];",
        ]

    def _alloc(self) -> str:
        node_id = f"n{self._next_id}"
        self._next_id += 1
        return node_id

    @staticmethod
    def _escape(label: str) -> str:
        return label.replace("\\", "\\\\").replace('"', '\\"')

    def node(self, label: str) -> str:
        node_id = self._alloc()
        self.lines.append(f'  {node_id} [label="{self._escape(label)}"];')
        return node_id

    def edge(self, src: str, dst: str, label: str | None = None) -> None:
        if label is None:
            self.lines.append(f"  {src} -> {dst};")
            return
        self.lines.append(f'  {src} -> {dst} [label="{self._escape(label)}"];')

    def finish(self) -> str:
        self.lines.append("}")
        return "\n".join(self.lines) + "\n"


def _call_target_label(target: IRNameRef | IROperatorRef) -> str:
    if isinstance(target, IROperatorRef):
        return f"op {target.op}"
    if target.qualifier:
        return f"call {target.qualifier}.{target.name}"
    return f"call {target.name}"


def _emit_expr(builder: _DotBuilder, expr: IRExpr) -> str:
    if isinstance(expr, IRFlowExpr):
        lhs_node = _emit_expr(builder, expr.lhs)
        rhs_node = builder.node(_call_target_label(expr.rhs))
        builder.edge(lhs_node, rhs_node, "pipe")
        return rhs_node

    if isinstance(expr, IRTupleExpr):
        tuple_node = builder.node(f"tuple[{len(expr.items)}]")
        for index, item in enumerate(expr.items):
            item_node = _emit_expr(builder, item)
            builder.edge(item_node, tuple_node, str(index))
        return tuple_node

    if isinstance(expr, IRConstant):
        return builder.node(f"const {expr.val!r}")

    if isinstance(expr, IRNameRef):
        if expr.qualifier:
            return builder.node(f"name {expr.qualifier}.{expr.name}")
        return builder.node(f"name {expr.name}")

    raise TypeError(f"Unsupported IR expression for graph emission: {type(expr).__name__}")


def emit_dot(program: IRProgram) -> str:
    builder = _DotBuilder()

    for index, stmt in enumerate(program.statements):
        if isinstance(stmt, IRImport):
            builder.node(f"stmt[{index}] import {stmt.module_name}")
            continue

        if isinstance(stmt, IRLet):
            name = f"{stmt.qualifier}.{stmt.name}" if stmt.qualifier else stmt.name
            let_node = builder.node(f"stmt[{index}] let {name}")
            if stmt.body is None:
                body_node = builder.node("__builtin__")
            else:
                body_node = _emit_expr(builder, stmt.body)
            builder.edge(let_node, body_node, "body")
            continue

        if isinstance(stmt, IRExprStatement):
            stmt_node = builder.node(f"stmt[{index}] expr")
            expr_node = _emit_expr(builder, stmt.expr)
            builder.edge(stmt_node, expr_node, "value")
            continue

        raise TypeError(f"Unsupported IR statement for graph emission: {type(stmt).__name__}")

    return builder.finish()


def emit_dot_from_source(source: str | Path) -> str:
    model = parse_file(source)
    program = lower(model)
    return emit_dot(program)


def write_dot_for_source(source: str | Path, out_path: str | Path | None = None) -> Path:
    return write_graph_for_source(source, out_path=out_path, graph_format="dot")


def write_graph_for_source(
    source: str | Path,
    out_path: str | Path | None = None,
    *,
    graph_format: str = "dot",
) -> Path:
    source_path = Path(source)
    normalized_format = graph_format.lower()
    if normalized_format not in {"dot", "svg", "png", "pdf"}:
        raise GraphEmitError(
            f"Unsupported graph format '{graph_format}'. Use one of: dot, svg, png, pdf."
        )

    target_path = (
        Path(out_path)
        if out_path is not None
        else source_path.with_suffix(f".{normalized_format}")
    )

    dot = emit_dot_from_source(source_path)
    if normalized_format == "dot":
        target_path.write_text(dot, encoding="utf-8")
        return target_path

    dot_binary = shutil.which("dot")
    if dot_binary is None:
        raise GraphvizNotInstalledError(
            "Graphviz 'dot' executable is required for non-dot graph formats. "
            "Install graphviz or use '--graph-format dot'."
        )

    rendered = subprocess.run(
        [dot_binary, f"-T{normalized_format}", "-o", str(target_path)],
        input=dot,
        text=True,
        capture_output=True,
        check=False,
    )
    if rendered.returncode != 0:
        message = rendered.stderr.strip() or "Unknown Graphviz error"
        raise GraphvizRenderError(f"Graphviz render failed: {message}")

    return target_path
