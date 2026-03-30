from __future__ import annotations

import runpy
import sys
import unittest
from pathlib import Path

from fleaux_transpiler import FleauxTranspiler
from tests.helpers import ensure_std_generated, REPO_ROOT

SAMPLES_DIR = REPO_ROOT / "samples"


def setUpModule() -> None:
    ensure_std_generated()


class SamplesCompileAndRunTests(unittest.TestCase):
    """Compile and execute every sample in samples/*.fleaux.

    Each sample gets its own sub-test so a single failure is isolated
    without aborting the rest of the suite.
    """

    def _run_sample(self, source: Path) -> None:
        transpiler = FleauxTranspiler()
        output = transpiler.process(source)

        self.assertTrue(output.exists(), f"Generated module not found for {source.name}")

        runtime_dir = REPO_ROOT
        original = list(sys.path)
        try:
            for entry in (output.parent.resolve(), runtime_dir):
                entry_str = str(entry)
                if entry_str not in sys.path:
                    sys.path.insert(0, entry_str)
            runpy.run_path(str(output), run_name="__main__")
        finally:
            sys.path[:] = original

    def test_samples(self) -> None:
        samples = sorted(SAMPLES_DIR.glob("*.fleaux"))
        self.assertGreater(len(samples), 0, "No sample files found in samples/")

        for source in samples:
            with self.subTest(sample=source.name):
                self._run_sample(source)

