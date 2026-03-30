from __future__ import annotations

import runpy
import unittest
from pathlib import Path

from fleaux_transpiler import FleauxTranspiler
from tests.helpers import ensure_std_generated


def setUpModule() -> None:
    ensure_std_generated()


class RegressionTests(unittest.TestCase):
    """End-to-end regression tests covering the full vertical slice.
    
    These tests verify that the entire pipeline (parse → lower → transpile → run)
    continues to work correctly for moderately complex programs.
    """

    def setUp(self) -> None:
        self.repo_root = Path(__file__).resolve().parents[1]
        self.test_dir = Path(__file__).resolve().parent
        self.transpiler = FleauxTranspiler()

    def test_regression_program_transpiles_without_error(self) -> None:
        """Verify regression_test.fleaux transpiles to Python."""
        output = self.transpiler.process(self.test_dir / "regression_test.fleaux")

        self.assertTrue(output.exists())
        self.assertIn("regression_test", output.name)

    def test_regression_program_executes_without_error(self) -> None:
        """Verify the transpiled regression_test module can be executed."""
        output = self.transpiler.process(self.test_dir / "regression_test.fleaux")

        # Should not raise any exception.
        runpy.run_path(str(output), run_name="__main__")

    def test_regression_generated_python_contains_expected_symbols(self) -> None:
        """Verify key function definitions appear in generated code."""
        output = self.transpiler.process(self.test_dir / "regression_test.fleaux")
        code = output.read_text()

        # Verify that user-defined functions are in the generated output.
        self.assertIn("_fleaux_impl_Add2", code)
        self.assertIn("_fleaux_impl_IsGreaterThan5", code)
        self.assertIn("_fleaux_impl_Multiply", code)

    def test_regression_imports_standard_library(self) -> None:
        """Verify Std module is imported and generated."""
        output = self.transpiler.process(self.test_dir / "regression_test.fleaux")
        std_output = self.repo_root / "fleaux_generated_module_Std.py"

        # Verify both test and Std modules were generated.
        self.assertTrue(output.exists())
        self.assertTrue(std_output.exists())

        # Verify Std is imported in the generated test module.
        code = output.read_text()
        self.assertIn("fleaux_generated_module_Std", code)


if __name__ == "__main__":
    unittest.main()

