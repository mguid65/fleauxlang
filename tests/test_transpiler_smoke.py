from __future__ import annotations

import runpy
import unittest
from pathlib import Path

from fleaux_transpiler import FleauxTranspiler


class TranspilerSmokeTests(unittest.TestCase):
    def setUp(self) -> None:
        self.repo_root = Path(__file__).resolve().parents[1]
        self.transpiler = FleauxTranspiler()

    def test_transpile_generates_test_and_std_modules(self) -> None:
        output = self.transpiler.process(self.repo_root / "test.fleaux")

        self.assertTrue(output.exists())
        self.assertEqual(output.name, "fleaux_generated_module_test.py")

        std_output = self.repo_root / "fleaux_generated_module_Std.py"
        self.assertTrue(std_output.exists())

    def test_generated_test_module_executes(self) -> None:
        output = self.transpiler.process(self.repo_root / "test.fleaux")

        # The generated module runs top-level expression statements.
        runpy.run_path(str(output), run_name="__main__")


if __name__ == "__main__":
    unittest.main()

