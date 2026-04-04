from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

from fleaux_cpp_transpiler import FleauxCppTranspiler
from fleaux_graphviz import GraphEmitError, write_graph_for_source


_COMPILER_CANDIDATES = ["clang++-20", "clang++", "g++", "c++"]


def _env_flag(name: str) -> bool:
    raw = os.environ.get(name)
    if raw is None:
        return False
    return raw.strip().lower() in {"1", "true", "yes", "on"}


def _resolve_compiler(requested: str | None) -> str:
    """Return the compiler executable to use.

    *requested* may be:
      - None           → auto-select first available from _COMPILER_CANDIDATES
      - a plain name   → e.g. "clang++-18"; resolved via PATH with shutil.which
      - an absolute or relative path → e.g. "/usr/bin/clang++-18"; checked
                        directly for existence and execute permission

    Exits with code 2 and a diagnostic if the compiler cannot be found.
    """
    if requested:
        # Treat as a filesystem path if it contains a separator character.
        if os.sep in requested or (os.altsep and os.altsep in requested):
            p = Path(requested)
            if p.is_file() and os.access(p, os.X_OK):
                return str(p)
            print(f"Compiler path '{requested}' does not exist or is not executable.")
            raise SystemExit(2)
        # Plain name — search PATH (also works for versioned names like clang++-18).
        found = shutil.which(requested)
        if found is None:
            print(f"Compiler '{requested}' not found on PATH.")
            raise SystemExit(2)
        return found
    for candidate in _COMPILER_CANDIDATES:
        if shutil.which(candidate):
            return candidate
    print("No C++ compiler found on PATH (tried: " + ", ".join(_COMPILER_CANDIDATES) + ").")
    raise SystemExit(2)


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
        "--no-run",
        action="store_true",
        help="Transpile and compile but do not run the produced binary.",
    )
    parser.add_argument(
        "--compiler",
        default=None,
        metavar="EXE",
        help=(
            "C++ compiler to use. "
            "Accepts a plain name searched on PATH (e.g. 'clang++-18', 'g++'), "
            "or an absolute/relative path (e.g. '/usr/bin/clang++-18'). "
            "Defaults to auto-selecting clang++-20, clang++, g++, or c++ in that order."
        ),
    )
    parser.add_argument(
        "--unoptimized",
        action="store_true",
        help=(
            "Use faster compile settings intended for tests/dev loops "
            "(currently -O0 instead of -O2). "
            "Can also be enabled with FLEAUX_UNOPTIMIZED=1 or FLEAUX_FAST_COMPILE=1."
        ),
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


    output = FleauxCppTranspiler().process(source)
    binary = output.with_suffix("")
    compiler = _resolve_compiler(args.compiler or os.environ.get("FLEAUX_COMPILER"))
    unoptimized = args.unoptimized or _env_flag("FLEAUX_UNOPTIMIZED") or _env_flag("FLEAUX_FAST_COMPILE")
    opt_flag = "-O0" if unoptimized else "-O2"
    compile_cmd = [
        compiler,
        "-std=c++20",
        opt_flag,
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

