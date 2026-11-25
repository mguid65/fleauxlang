"""Shared helpers for the benchmark scripts: running `fleaux --bench` on programs copied away from their caches.

`fleaux --no-emit-bytecode` does not write a `.fleaux.bc` cache, but it still reads one. A cache that another binary
wrote with the same bytecode version would then be run instead of the source, so every program is timed from a copy
of its directory that holds only `.fleaux` files.

Set FLEAUX_BENCH_PATH to directories to put in front of PATH, such as the MinGW `bin` directory that holds the
runtime libraries a MinGW build of fleaux needs.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
from collections.abc import Iterable
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SAMPLES = REPO_ROOT / 'samples'
PROGRAMS = Path(__file__).resolve().parent / 'programs'

_BENCH_LINE = re.compile(r'bench: (\d+) runs, fastest ([\d.]+) us, median ([\d.]+) us')


def bench_environment() -> dict[str, str]:
    """The environment to run fleaux in: PATH with FLEAUX_BENCH_PATH in front of it."""
    environment = dict(os.environ)
    extra = environment.get('FLEAUX_BENCH_PATH')
    if extra:
        environment['PATH'] = extra + os.pathsep + environment.get('PATH', '')
    return environment


class ProgramCopies:
    """Copies the directories of programs, with only their `.fleaux` files, into one temporary directory.

    Each source directory gets a directory of its own, so a program still finds the modules it imports beside it.
    """

    def __init__(self, programs: Iterable[Path]) -> None:
        self._root = Path(tempfile.mkdtemp(prefix='fleaux-bench-'))
        self._copies: dict[Path, Path] = {}
        directories: dict[Path, Path] = {}
        for program in programs:
            source_directory = program.resolve().parent
            if source_directory not in directories:
                target = self._root / str(len(directories))
                target.mkdir()
                for source in source_directory.glob('*.fleaux'):
                    shutil.copyfile(source, target / source.name)
                directories[source_directory] = target
            self._copies[program] = directories[source_directory] / program.name

    def __enter__(self) -> ProgramCopies:
        return self

    def __exit__(self, *exc_info: object) -> None:
        shutil.rmtree(self._root, ignore_errors=True)

    def copy_of(self, program: Path) -> Path:
        return self._copies[program]


def bench_median(binary: Path, program: Path, runs: int, environment: dict[str, str]) -> float:
    """The median time in microseconds of `runs` executions of `program`, as `fleaux --bench` reports it."""
    result = subprocess.run([str(binary), '--no-emit-bytecode', '--bench', str(runs), program.name],
                            capture_output=True, text=True, cwd=program.parent, env=environment,
                            stdin=subprocess.DEVNULL, timeout=900)
    match = _BENCH_LINE.search(result.stderr)
    if result.returncode != 0 or match is None:
        raise RuntimeError(f'{binary.name} {program.name} exited with {result.returncode}: {result.stderr.strip()}')
    return float(match.group(3))
