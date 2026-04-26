#!/usr/bin/env python3

from __future__ import annotations

import argparse
import subprocess
import sys
from collections.abc import Callable
from pathlib import Path
import glob

def repo_root() -> Path:
    return Path(__file__).resolve().parent


def default_fleaux_binary(root: Path) -> Path | None:
    possible_fleaux_path_pattern = (root / "core" / "*" / "bin" / "fleaux").resolve()
    results = glob.glob(str(possible_fleaux_path_pattern))

    print('Possible Fleaux Binary Paths:')
    for match in results:
        print(f'  {match}')

    for match in results:
        match_path = Path(match)
        if match_path.exists() and match_path.is_file():
            return match_path
    return None


SampleArgProvider = Callable[[Path], list[str]]


SAMPLE_RUNTIME_ARG_PROVIDERS: dict[str, SampleArgProvider] = {
    "25_fleaux_parser.fleaux": lambda sample_path: [str(sample_path)],
}


def collect_samples(samples_dir: Path, requested_samples: list[str]) -> list[Path]:
    samples = sorted(path for path in samples_dir.iterdir() if path.is_file() and path.suffix == ".fleaux")
    if not requested_samples:
        return samples

    sample_by_name = {sample.name: sample for sample in samples}
    missing = [name for name in requested_samples if name not in sample_by_name]
    if missing:
        missing_list = ", ".join(missing)
        raise ValueError(f"requested sample file(s) not found under {samples_dir}: {missing_list}")

    return [sample_by_name[name] for name in requested_samples]


def sample_runtime_args(sample_path: Path) -> list[str]:
    provider = SAMPLE_RUNTIME_ARG_PROVIDERS.get(sample_path.name)
    if provider is None:
        return []
    return provider(sample_path)


def run_one(binary: Path, sample_path: Path, extra_args: list[str]) -> int:
    forwarded_args = [*sample_runtime_args(sample_path), *extra_args]
    command = [str(binary), str(sample_path)]
    if forwarded_args:
        command.extend(["--", *forwarded_args])

    print(f"[vm] {sample_path.name}")
    result = subprocess.run(command, cwd=repo_root())
    if result.returncode != 0:
        print(f"FAILED [vm] {sample_path.name} (exit={result.returncode})")
    return result.returncode


def parse_args() -> argparse.Namespace:
    root = repo_root()

    parser = argparse.ArgumentParser(description="Run all Fleaux samples via the fleaux CLI.")
    parser.add_argument(
        "--fleaux-bin",
        type=Path,
        default=default_fleaux_binary(root),
        help="Path to fleaux executable.",
    )
    parser.add_argument(
        "--samples-dir",
        type=Path,
        default=root / "samples",
        help="Directory that contains .fleaux sample files.",
    )
    parser.add_argument(
        "--sample",
        dest="samples",
        action="append",
        default=[],
        help="Run only the named sample file. May be passed multiple times.",
    )
    parser.add_argument(
        "runtime_args",
        nargs=argparse.REMAINDER,
        help="Optional runtime arguments forwarded to every sample. Prefix with -- to separate.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()

    binary = args.fleaux_bin.resolve()
    samples_dir = args.samples_dir.resolve()
    runtime_args = args.runtime_args
    if runtime_args and runtime_args[0] == "--":
        runtime_args = runtime_args[1:]

    if not binary.exists():
        print(f"fleaux binary not found: {binary}", file=sys.stderr)
        return 2
    if not samples_dir.is_dir():
        print(f"samples directory not found: {samples_dir}", file=sys.stderr)
        return 2

    try:
        samples = collect_samples(samples_dir, args.samples)
    except ValueError as exc:
        print(str(exc), file=sys.stderr)
        return 2

    if not samples:
        print(f"no .fleaux files found under: {samples_dir}", file=sys.stderr)
        return 2

    failures: list[tuple[str, int]] = []
    for sample in samples:
        code = run_one(binary, sample, runtime_args)
        if code != 0:
            failures.append((sample.name, code))

    if failures:
        print("\nSample run failures:")
        for name, code in failures:
            print(f"  [vm] {name}: exit={code}")
        return 1

    print(f"\nAll sample runs passed ({len(samples)} samples).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

