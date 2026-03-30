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

    def test_builtin_mapping_supports_nested_names_only(self) -> None:
        self.assertEqual(self.transpiler._builtin_expr("Std.Path.Join"), "fstd.PathJoin")
        self.assertEqual(self.transpiler._builtin_expr("Std.OS.Cwd"), "fstd.Cwd")
        self.assertEqual(self.transpiler._builtin_expr("Std.OS.Env"), "fstd.OSEnv")
        self.assertEqual(self.transpiler._builtin_expr("Std.OS.HasEnv"), "fstd.OSHasEnv")
        self.assertEqual(self.transpiler._builtin_expr("Std.OS.SetEnv"), "fstd.OSSetEnv")
        self.assertEqual(self.transpiler._builtin_expr("Std.OS.UnsetEnv"), "fstd.OSUnsetEnv")
        self.assertEqual(self.transpiler._builtin_expr("Std.OS.IsWindows"), "fstd.OSIsWindows")
        self.assertEqual(self.transpiler._builtin_expr("Std.OS.IsLinux"), "fstd.OSIsLinux")
        self.assertEqual(self.transpiler._builtin_expr("Std.OS.IsMacOS"), "fstd.OSIsMacOS")
        self.assertEqual(self.transpiler._builtin_expr("Std.OS.Home"), "fstd.OSHome")
        self.assertEqual(self.transpiler._builtin_expr("Std.OS.TempDir"), "fstd.OSTempDir")
        self.assertEqual(self.transpiler._builtin_expr("Std.File.ReadText"), "fstd.FileReadText")
        self.assertEqual(self.transpiler._builtin_expr("Std.File.WriteText"), "fstd.FileWriteText")
        self.assertEqual(self.transpiler._builtin_expr("Std.File.AppendText"), "fstd.FileAppendText")
        self.assertEqual(self.transpiler._builtin_expr("Std.File.ReadLines"), "fstd.FileReadLines")
        self.assertEqual(self.transpiler._builtin_expr("Std.File.Delete"), "fstd.FileDelete")
        self.assertEqual(self.transpiler._builtin_expr("Std.File.Size"), "fstd.FileSize")
        self.assertEqual(self.transpiler._builtin_expr("Std.Path.Extension"), "fstd.PathExtension")
        self.assertEqual(self.transpiler._builtin_expr("Std.Path.Stem"), "fstd.PathStem")
        self.assertEqual(self.transpiler._builtin_expr("Std.Path.WithExtension"), "fstd.PathWithExtension")
        self.assertEqual(self.transpiler._builtin_expr("Std.Path.WithBasename"), "fstd.PathWithBasename")
        self.assertEqual(self.transpiler._builtin_expr("Std.Dir.Create"), "fstd.DirCreate")
        self.assertEqual(self.transpiler._builtin_expr("Std.Dir.Delete"), "fstd.DirDelete")
        self.assertEqual(self.transpiler._builtin_expr("Std.Dir.List"), "fstd.DirList")
        self.assertEqual(self.transpiler._builtin_expr("Std.Dir.ListFull"), "fstd.DirListFull")
        self.assertEqual(self.transpiler._builtin_expr("Std.Tuple.Append"), "fstd.TupleAppend")
        self.assertEqual(self.transpiler._builtin_expr("Std.Tuple.Prepend"), "fstd.TuplePrepend")
        self.assertEqual(self.transpiler._builtin_expr("Std.Tuple.Reverse"), "fstd.TupleReverse")
        self.assertEqual(self.transpiler._builtin_expr("Std.Tuple.Contains"), "fstd.TupleContains")
        self.assertEqual(self.transpiler._builtin_expr("Std.Tuple.Zip"), "fstd.TupleZip")
        self.assertEqual(self.transpiler._builtin_expr("Std.Tuple.Map"), "fstd.TupleMap")
        self.assertEqual(self.transpiler._builtin_expr("Std.Tuple.Filter"), "fstd.TupleFilter")
        # math – reuses existing classes for trig, new classes for the rest
        self.assertEqual(self.transpiler._builtin_expr("Std.Math.Sqrt"), "fstd.Sqrt")
        self.assertEqual(self.transpiler._builtin_expr("Std.Math.Sin"), "fstd.Sin")
        self.assertEqual(self.transpiler._builtin_expr("Std.Math.Cos"), "fstd.Cos")
        self.assertEqual(self.transpiler._builtin_expr("Std.Math.Tan"), "fstd.Tan")
        self.assertEqual(self.transpiler._builtin_expr("Std.Math.Floor"), "fstd.MathFloor")
        self.assertEqual(self.transpiler._builtin_expr("Std.Math.Ceil"), "fstd.MathCeil")
        self.assertEqual(self.transpiler._builtin_expr("Std.Math.Abs"), "fstd.MathAbs")
        self.assertEqual(self.transpiler._builtin_expr("Std.Math.Log"), "fstd.MathLog")
        self.assertEqual(self.transpiler._builtin_expr("Std.Math.Clamp"), "fstd.MathClamp")
        # string
        self.assertEqual(self.transpiler._builtin_expr("Std.String.Upper"), "fstd.StringUpper")
        self.assertEqual(self.transpiler._builtin_expr("Std.String.Lower"), "fstd.StringLower")
        self.assertEqual(self.transpiler._builtin_expr("Std.String.Trim"), "fstd.StringTrim")
        self.assertEqual(self.transpiler._builtin_expr("Std.String.Split"), "fstd.StringSplit")
        self.assertEqual(self.transpiler._builtin_expr("Std.String.Join"), "fstd.StringJoin")
        self.assertEqual(self.transpiler._builtin_expr("Std.String.Replace"), "fstd.StringReplace")
        self.assertEqual(self.transpiler._builtin_expr("Std.String.Contains"), "fstd.StringContains")
        self.assertEqual(self.transpiler._builtin_expr("Std.String.StartsWith"), "fstd.StringStartsWith")
        self.assertEqual(self.transpiler._builtin_expr("Std.String.EndsWith"), "fstd.StringEndsWith")
        self.assertEqual(self.transpiler._builtin_expr("Std.String.Length"), "fstd.StringLength")

        self.assertIn("_fleaux_missing_builtin", self.transpiler._builtin_expr("Std.PathJoin"))
        self.assertIn("_fleaux_missing_builtin", self.transpiler._builtin_expr("Std.Cwd"))
        self.assertIn("_fleaux_missing_builtin", self.transpiler._builtin_expr("Std.ReadText"))
        self.assertIn("_fleaux_missing_builtin", self.transpiler._builtin_expr("Std.WriteText"))


if __name__ == "__main__":
    unittest.main()

