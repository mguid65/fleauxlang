from __future__ import annotations

import argparse
import runpy
import sys
from pathlib import Path

from fleaux_transpiler import FleauxTranspiler


def main() -> int:
    parser = argparse.ArgumentParser(description="Transpile and execute a Fleaux source file.")
    parser.add_argument("source", nargs="?", default="test.fleaux", help="Input .fleaux file path")
    args = parser.parse_args()

    source = Path(args.source)
    output = FleauxTranspiler().process(source)
    runpy.run_path(str(output), run_name="__main__")

    print(f"Executed generated module: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

