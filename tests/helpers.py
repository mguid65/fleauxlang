"""
Shared test helpers for the Fleaux test suite.
"""
from __future__ import annotations

import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]


def ensure_runtime_on_path() -> None:
    """Ensure the repo root is on sys.path so generated modules are importable."""
    repo_str = str(REPO_ROOT)
    if repo_str not in sys.path:
        sys.path.insert(0, repo_str)


def ensure_std_generated() -> None:
    """Regenerate fleaux_generated_module_Std.py from the bundled Std.fleaux.

    Idempotent - safe to call from setUpModule in any test file that runs
    generated modules or exercises the transpiler pipeline.
    """
    ensure_runtime_on_path()
    from fleaux_transpiler import FleauxTranspiler
    FleauxTranspiler().process(REPO_ROOT / "Std.fleaux")

