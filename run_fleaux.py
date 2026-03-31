from __future__ import annotations

import argparse
import runpy
import sys
from pathlib import Path

from fleaux_graphviz import GraphEmitError, write_graph_for_source
from fleaux_transpiler import FleauxTranspiler


def main() -> int:
    parser = argparse.ArgumentParser(description="Transpile and execute a Fleaux source file.")
    parser.add_argument("source", nargs="?", default="test.fleaux", help="Input .fleaux file path")
    parser.add_argument(
        "--emit-graph",
        action="store_true",
        help="Emit a Graphviz DOT representation of the lowered IR.",
    )
    parser.add_argument(
        "--graph-out",
        default=None,
        help="Output graph path for --emit-graph (default: <source>.<graph-format>)",
    )
    parser.add_argument(
        "--graph-format",
        choices=["dot", "svg", "png", "pdf"],
        default="dot",
        help="Graph output format for --emit-graph (default: dot)",
    )
    parser.add_argument(
        "--graph-only",
        action="store_true",
        help="Emit graph output only and skip transpile/execute.",
    )
    args = parser.parse_args()

    source = Path(args.source)
    if args.graph_only and not args.emit_graph:
        print("Graph-only mode requires --emit-graph.")
        return 2

    if args.emit_graph:
        try:
            graph_path = write_graph_for_source(
                source,
                args.graph_out,
                graph_format=args.graph_format,
            )
        except GraphEmitError as exc:
            print(f"Graph emission failed: {exc}")
            return 2
        print(f"Emitted graph: {graph_path}")
        if args.graph_only:
            print("Skipped execution (--graph-only).")
            return 0

    output = FleauxTranspiler().process(source)
    runtime_dir = Path(__file__).resolve().parent
    original_sys_path = list(sys.path)
    try:
        for entry in (output.parent.resolve(), runtime_dir):
            entry_str = str(entry)
            if entry_str not in sys.path:
                sys.path.insert(0, entry_str)
        runpy.run_path(str(output), run_name="__main__")
    finally:
        sys.path[:] = original_sys_path

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

