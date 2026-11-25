"""The standard benchmark set: the timed samples, the programs in bench/programs, and the runtime microbenchmark.

    python bench/suite.py run BINARY [--microbench EXE] [--rounds N] [--out FILE]
    python bench/suite.py compare BASELINE RESULTS [--threshold PERCENT]

`run` writes each program's best median, in microseconds, to a JSON file. `compare` prints two such files side by side
and marks every program that got slower by more than the threshold. The exit check of a stabilization cycle compares a
cycle's last build with bench/baseline.json, which holds the numbers of the cycle's first commit on the machine that
measured them.

On Windows the clock `--bench` reads advances in steps of 0.1 us, so a program that takes a microsecond or two moves
by a step from run to run. `compare` does not mark a difference of one step.
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

from fleaux_bench import PROGRAMS, SAMPLES, ProgramCopies, bench_environment, bench_median


@dataclass(frozen=True)
class Entry:
    path: Path
    bench_runs: int

    @property
    def name(self) -> str:
        return self.path.stem


def _samples(names: list[str], bench_runs: int) -> list[Entry]:
    return [Entry(SAMPLES / f'{name}.fleaux', bench_runs) for name in names]


def _programs(names: list[str], bench_runs: int) -> list[Entry]:
    return [Entry(PROGRAMS / f'{name}.fleaux', bench_runs) for name in names]


ENTRIES: list[Entry] = [
    # The algorithmic samples, which bench/python also compares with Python.
    *_samples(['08_loop', '09_loop_n', '19_composition', '23_binary_search', '25_fleaux_parser', '40_recursion',
               '42_dict_dispatch', '43_accumulator_loops', '44_tuple_map_filter_reduce', '46_select_vs_branch_match',
               '47_tag_dispatch_vs_callable_dispatch', '48_repeat_value_tuple', '50_multi_accumulator_loop_state'],
              2000),
    # Samples that are mostly builtin calls.
    *_samples(['10_strings', '11_tuples', '17_printf_and_tostring', '24_dicts', '61_raw_strings'], 2000),
    # Tasks and parallel maps.
    *_samples(['33_exp_parallel', '35_concurrency_tasks', '36_parallel_options_and_empty_inputs',
               '37_parallel_error_paths', '38_parallel_inline_closures'], 300),
    *_programs(['calls20', 'fact20', 'print10', 'unary_calls'], 1000),
    *_programs(['stress_sequential'], 100),
    *_programs(['parallel_trivial', 'parallel_trivial_4096', 'stress_parallel_50', 'stress_parallel_100',
                'stress_parallel_200', 'stress_parallel_400', 'stress_parallel_800'], 200),
]

_MICROBENCH_LINE = re.compile(r'^(.*\S)\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s*$')

# The step of the clock `fleaux --bench` reads on Windows, in microseconds, with room for rounding.
_CLOCK_STEP_US = 0.1 + 1e-9


def run_microbench(executable: Path, environment: dict[str, str]) -> dict[str, float]:
    """The median ns/op of each benchmark of fleaux_runtime_microbench."""
    result = subprocess.run([str(executable)], capture_output=True, text=True, env=environment, timeout=900)
    if result.returncode != 0:
        raise RuntimeError(f'{executable.name} exited with {result.returncode}: {result.stderr.strip()}')
    medians: dict[str, float] = {}
    for line in result.stdout.splitlines():
        if match := _MICROBENCH_LINE.match(line):
            medians[match.group(1)] = float(match.group(4))
    return medians


def run(args: argparse.Namespace) -> None:
    binary = Path(args.binary).resolve()
    environment = bench_environment()
    best: dict[str, float] = {}
    with ProgramCopies(entry.path for entry in ENTRIES) as copies:
        for round_index in range(args.rounds):
            for entry in ENTRIES:
                median = bench_median(binary, copies.copy_of(entry.path), entry.bench_runs, environment)
                best[entry.name] = min(best.get(entry.name, float('inf')), median)
            print(f'round {round_index + 1} of {args.rounds}', file=sys.stderr, flush=True)

    results = {
        'binary': args.label or binary.name,
        'platform': platform.platform(),
        'cpu_count': os.cpu_count(),
        'rounds': args.rounds,
        'programs_us': best,
    }
    if args.microbench:
        microbench = Path(args.microbench).resolve()
        rounds = [run_microbench(microbench, environment) for _ in range(args.rounds)]
        results['microbench_ns_per_op'] = {name: min(medians[name] for medians in rounds) for name in rounds[0]}

    text = json.dumps(results, indent=2) + '\n'
    if args.out:
        Path(args.out).write_text(text, encoding='utf-8')
    else:
        sys.stdout.write(text)


def compare(args: argparse.Namespace) -> None:
    baseline = json.loads(Path(args.baseline).read_text(encoding='utf-8'))
    results = json.loads(Path(args.results).read_text(encoding='utf-8'))
    slower = 0
    for section, unit, step in (('programs_us', 'us', _CLOCK_STEP_US), ('microbench_ns_per_op', 'ns/op', 0.0)):
        before_section = baseline.get(section, {})
        after_section = results.get(section, {})
        names = [name for name in before_section if name in after_section]
        if not names:
            continue
        print(f'{section}:')
        for name in names:
            before = before_section[name]
            after = after_section[name]
            change = (after - before) / before * 100.0 if before > 0 else 0.0
            mark = '  slower' if change > args.threshold and after - before > step else ''
            slower += 1 if mark else 0
            print(f'  {name:40s} {before:10.2f} {after:10.2f} {unit:5s} {change:+6.1f}%{mark}')
    print(f'{slower} slower by more than {args.threshold}%')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)

    run_parser = commands.add_parser('run', help='time the standard set with one binary')
    run_parser.add_argument('binary')
    run_parser.add_argument('--microbench', help='fleaux_runtime_microbench to run as well')
    run_parser.add_argument('--rounds', type=int, default=7, help='rounds; each program reports its best median')
    run_parser.add_argument('--out', help='the JSON file to write; standard output when absent')
    run_parser.add_argument('--label', help='what to call the binary in the file; its file name when absent')
    run_parser.set_defaults(handler=run)

    compare_parser = commands.add_parser('compare', help='compare two results files')
    compare_parser.add_argument('baseline')
    compare_parser.add_argument('results')
    compare_parser.add_argument('--threshold', type=float, default=5.0, help='percent slower to mark')
    compare_parser.set_defaults(handler=compare)

    args = parser.parse_args()
    args.handler(args)


if __name__ == '__main__':
    main()
