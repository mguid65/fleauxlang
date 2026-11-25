"""A/B timing of fleaux binaries: the best median of several `--bench` runs of each program on each binary.

The binaries take turns on every program in every round, so a change in the machine's load lands on all of them.

    python bench/ab.py BINARY,BINARY... PROGRAM,PROGRAM... [--rounds N] [--bench N]
"""
from __future__ import annotations

import argparse
from pathlib import Path

from fleaux_bench import ProgramCopies, bench_environment, bench_median


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('binaries', help='comma-separated fleaux binaries')
    parser.add_argument('programs', help='comma-separated .fleaux programs')
    parser.add_argument('--rounds', type=int, default=11, help='rounds; each program reports its best median')
    parser.add_argument('--bench', type=int, default=2000, help='executions per --bench run')
    args = parser.parse_args()

    binaries = [Path(item).resolve() for item in args.binaries.split(',')]
    programs = [Path(item).resolve() for item in args.programs.split(',')]
    environment = bench_environment()
    best: dict[tuple[Path, Path], float] = {}
    with ProgramCopies(programs) as copies:
        for _ in range(args.rounds):
            for program in programs:
                for binary in binaries:
                    median = bench_median(binary, copies.copy_of(program), args.bench, environment)
                    key = (program, binary)
                    best[key] = min(best.get(key, float('inf')), median)

    for program in programs:
        cells = '  '.join(f'{binary.stem}={best[(program, binary)]:9.2f}' for binary in binaries)
        print(f'{program.stem:40s} {cells}')


if __name__ == '__main__':
    main()
