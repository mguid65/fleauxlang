from __future__ import annotations

import argparse
import runpy
import subprocess
import sys
from pathlib import Path

from fleaux_cpp_transpiler import FleauxCppTranspiler
from fleaux_graphviz import GraphEmitError, write_graph_for_source
from fleaux_transpiler import FleauxTranspiler


def main() -> int:
    raw_argv = sys.argv[1:]
    runtime_args: list[str] = []
    if "--" in raw_argv:
        split_idx = raw_argv.index("--")
        runtime_args = raw_argv[split_idx + 1 :]
        cli_argv = raw_argv[:split_idx]
    else:
        cli_argv = raw_argv

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
    parser.add_argument(
        "--backend",
        choices=["python", "cpp"],
        default="python",
        help="Execution backend (default: python).",
    )
    parser.add_argument(
        "--no-run",
        action="store_true",
        help="For --backend cpp, transpile and compile but do not run the produced binary.",
    )
    args = parser.parse_args(cli_argv)

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

    runtime_dir = Path(__file__).resolve().parent

    if args.backend == "python":
        if args.no_run:
            print("--no-run is only supported with --backend cpp.")
            return 2
        output = FleauxTranspiler().process(source)
        original_sys_path = list(sys.path)
        original_argv = list(sys.argv)
        try:
            for entry in (output.parent.resolve(), runtime_dir):
                entry_str = str(entry)
                if entry_str not in sys.path:
                    sys.path.insert(0, entry_str)
            sys.argv = [str(output), *runtime_args]
            runpy.run_path(str(output), run_name="__main__")
        finally:
            sys.path[:] = original_sys_path
            sys.argv = original_argv
        return 0

    output = FleauxCppTranspiler().process(source)
    binary = output.with_suffix("")
    compile_cmd = [
        "c++",
        "-std=c++20",
        "-O2",
        str(output),
        "-I",
        str(runtime_dir / "cpp"),
        "-I",
        str(runtime_dir / "third_party" / "datatree" / "include"),
        "-I",
        str(runtime_dir / "third_party" / "tl" / "include"),
        "-o",
        str(binary),
    ]
    compile_proc = subprocess.run(compile_cmd, text=True, capture_output=True, check=False)
    if compile_proc.returncode != 0:
        if compile_proc.stdout:
            print(compile_proc.stdout)
        if compile_proc.stderr:
            print(compile_proc.stderr)
        return compile_proc.returncode

    if args.no_run:
        return 0

    run_proc = subprocess.run([str(binary), *runtime_args], check=False)
    if run_proc.returncode != 0:
        return run_proc.returncode

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

