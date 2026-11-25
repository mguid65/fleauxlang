"""Fleaux against Python on the algorithmic samples, timed in-process so that neither pays its startup.

    python bench/python/compare.py BINARY [--seconds S]

Fleaux: `fleaux --bench N` loads and compiles a sample once, then times N runs with the output discarded.
Python: each port is compiled once, then run N times into a fresh globals dict, with stdout sent to a StringIO and the
cyclic GC off, as timeit does. Before timing, each port's output is checked against the sample's output in `expected/`,
with `print` standing in for Std.Println's text form.

`ports/` holds a direct port of each sample, function for function. `ports/idiomatic/` holds, for some samples, the
loop a Python programmer would write instead.
"""
from __future__ import annotations

import argparse
import builtins
import gc
import io
import sys
import time
from pathlib import Path
from types import CodeType

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))

from fleaux_bench import SAMPLES, ProgramCopies, bench_environment, bench_median  # noqa: E402
from fleaux_print import fleaux_println  # noqa: E402

MIN_RUNS, MAX_RUNS = 1000, 200000

SAMPLE_NAMES = [
    '08_loop', '09_loop_n', '19_composition', '23_binary_search', '25_fleaux_parser', '40_recursion',
    '42_dict_dispatch', '43_accumulator_loops', '44_tuple_map_filter_reduce', '46_select_vs_branch_match',
    '47_tag_dispatch_vs_callable_dispatch', '48_repeat_value_tuple', '50_multi_accumulator_loop_state',
]


def runs_for(median_us: float, seconds: float) -> int:
    """Enough runs to take about `seconds`, within MIN_RUNS and MAX_RUNS."""
    return max(MIN_RUNS, min(MAX_RUNS, int(seconds * 1e6 / max(median_us, 0.1))))


def time_fleaux(binary: Path, program: Path, seconds: float, environment: dict[str, str]) -> float:
    median = bench_median(binary, program, 200, environment)
    return bench_median(binary, program, runs_for(median, seconds), environment)


def python_output(code: CodeType) -> str:
    captured = io.StringIO()
    saved = sys.stdout
    sys.stdout = captured
    try:
        exec(code, {'__name__': '__main__', '__builtins__': builtins, 'print': fleaux_println})
    finally:
        sys.stdout = saved
    return captured.getvalue()


def python_median(code: CodeType, runs: int) -> float:
    sink = io.StringIO()
    saved = sys.stdout
    nanoseconds: list[int] = []
    gc.disable()
    sys.stdout = sink
    try:
        for _ in range(runs):
            namespace = {'__name__': '__main__', '__builtins__': builtins}
            started = time.perf_counter_ns()
            exec(code, namespace)
            nanoseconds.append(time.perf_counter_ns() - started)
            sink.seek(0)
            sink.truncate()
    finally:
        sys.stdout = saved
        gc.enable()
    nanoseconds.sort()
    return nanoseconds[len(nanoseconds) // 2] / 1000


def time_python(code: CodeType, seconds: float) -> float:
    return python_median(code, runs_for(python_median(code, 200), seconds))


def load_port(path: Path, expected: str) -> CodeType:
    code = compile(path.read_text(encoding='utf-8'), str(path), 'exec')
    actual = python_output(code)
    if actual != expected:
        raise SystemExit(f'{path} output differs from Fleaux:\n--- fleaux\n{expected}--- python\n{actual}')
    return code


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('binary', help='the fleaux binary to time')
    parser.add_argument('--seconds', type=float, default=1.0, help='about how long to time each program')
    args = parser.parse_args()

    binary = Path(args.binary).resolve()
    environment = bench_environment()
    empty = HERE / 'empty.fleaux'
    programs = [empty] + [SAMPLES / f'{name}.fleaux' for name in SAMPLE_NAMES]
    with ProgramCopies(programs) as copies:
        floor_fleaux = time_fleaux(binary, copies.copy_of(empty), args.seconds, environment)
        floor_python = time_python(compile('', 'empty', 'exec'), args.seconds)
        print(f'Python {sys.version.split()[0]}, {binary}')
        print(f'Empty program, per run: fleaux {floor_fleaux:.2f} us, python {floor_python:.2f} us (median)')
        print()
        header = f'{"sample":40s} {"fleaux us":>10s} {"python us":>10s} {"py/fx":>6s} {"py idiom us":>12s} {"py/fx":>6s}'
        print(header)
        print('=' * len(header))
        for name in SAMPLE_NAMES:
            expected = (HERE / 'expected' / f'{name}.txt').read_text(encoding='utf-8')
            direct = load_port(HERE / 'ports' / f'{name}.py', expected)
            idiomatic_path = HERE / 'ports' / 'idiomatic' / f'{name}.py'
            idiomatic = load_port(idiomatic_path, expected) if idiomatic_path.exists() else None

            fleaux = time_fleaux(binary, copies.copy_of(SAMPLES / f'{name}.fleaux'), args.seconds, environment)
            python = time_python(direct, args.seconds)
            line = f'{name:40s} {fleaux:10.2f} {python:10.2f} {python / fleaux:6.2f}'
            if idiomatic is not None:
                idiom = time_python(idiomatic, args.seconds)
                line += f' {idiom:12.2f} {idiom / fleaux:6.2f}'
            print(line, flush=True)


if __name__ == '__main__':
    main()
