from __future__ import annotations

import os
import sys
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch

import fleaux_std_builtins as fstd


class ModTest(unittest.TestCase):
    def test_mod(self) -> None:
        self.assertEqual((10, 3) | fstd.Mod(), 1)
        self.assertEqual((7, 2) | fstd.Mod(), 1)


class LengthTests(unittest.TestCase):
    def test_length_of_tuple(self) -> None:
        self.assertEqual(((1, 2, 3),) | fstd.Length(), 3)

    def test_length_of_empty_tuple(self) -> None:
        self.assertEqual(((),) | fstd.Length(), 0)

    def test_length_of_single_element(self) -> None:
        self.assertEqual(((42,),) | fstd.Length(), 1)


class GetArgsTests(unittest.TestCase):
    def test_get_args_returns_tuple(self) -> None:
        result = () | fstd.GetArgs()
        self.assertIsInstance(result, tuple)

    def test_get_args_pipeline_compatible(self) -> None:
        # Verify __ror__ is used (not __call__), which is the pipeline protocol.
        self.assertTrue(hasattr(fstd.GetArgs(), "__ror__"))
        self.assertFalse(hasattr(fstd.GetArgs(), "__call__") and
                         callable(getattr(type(fstd.GetArgs()), "__call__", None).__func__
                                  if hasattr(type(fstd.GetArgs()), "__call__") else None))


class InputTests(unittest.TestCase):
    def test_input_no_prompt(self) -> None:
        with patch("builtins.input", return_value="hello") as mock_input:
            result = () | fstd.Input()
            self.assertEqual(result, "hello")
            mock_input.assert_called_once_with()

    def test_input_with_prompt_tuple_arg(self) -> None:
        with patch("builtins.input", return_value="42") as mock_input:
            result = ("Enter number: ",) | fstd.Input()
            self.assertEqual(result, "42")
            mock_input.assert_called_once_with("Enter number: ")

    def test_input_with_prompt_scalar_arg(self) -> None:
        with patch("builtins.input", return_value="ok") as mock_input:
            result = "Prompt> " | fstd.Input()
            self.assertEqual(result, "ok")
            mock_input.assert_called_once_with("Prompt> ")

    def test_input_wrong_arity_raises(self) -> None:
        with self.assertRaises(TypeError):
            ("a", "b") | fstd.Input()


class ExitTests(unittest.TestCase):
    def test_exit_default_code(self) -> None:
        with self.assertRaises(SystemExit) as cm:
            () | fstd.Exit()
        self.assertEqual(cm.exception.code, 0)

    def test_exit_with_tuple_code(self) -> None:
        with self.assertRaises(SystemExit) as cm:
            (7,) | fstd.Exit()
        self.assertEqual(cm.exception.code, 7)

    def test_exit_with_scalar_code(self) -> None:
        with self.assertRaises(SystemExit) as cm:
            3 | fstd.Exit()
        self.assertEqual(cm.exception.code, 3)

    def test_exit_wrong_arity_raises(self) -> None:
        with self.assertRaises(TypeError):
            (1, 2) | fstd.Exit()


class ToNumTests(unittest.TestCase):
    def test_integer_string(self) -> None:
        self.assertEqual(("42",) | fstd.ToNum(), 42)
        self.assertIsInstance(("42",) | fstd.ToNum(), int)

    def test_float_string(self) -> None:
        self.assertAlmostEqual(("3.14",) | fstd.ToNum(), 3.14)

    def test_negative_string(self) -> None:
        self.assertEqual(("-5",) | fstd.ToNum(), -5)


class SliceTests(unittest.TestCase):
    def test_slice_stop_only(self) -> None:
        self.assertEqual(((1, 2, 3, 4), 2) | fstd.Slice(), (1, 2))

    def test_slice_start_stop(self) -> None:
        self.assertEqual(((1, 2, 3, 4), 1, 3) | fstd.Slice(), (2, 3))

    def test_slice_start_stop_step(self) -> None:
        self.assertEqual(((1, 2, 3, 4, 5), 0, 5, 2) | fstd.Slice(), (1, 3, 5))


class ComparisonTests(unittest.TestCase):
    def test_greater_than_true(self) -> None:
        self.assertTrue((5, 3) | fstd.GreaterThan())

    def test_greater_than_false(self) -> None:
        self.assertFalse((3, 5) | fstd.GreaterThan())

    def test_less_than_true(self) -> None:
        self.assertTrue((2, 4) | fstd.LessThan())

    def test_less_than_false(self) -> None:
        self.assertFalse((4, 2) | fstd.LessThan())

    def test_greater_or_equal_equal(self) -> None:
        self.assertTrue((4, 4) | fstd.GreaterOrEqual())

    def test_greater_or_equal_greater(self) -> None:
        self.assertTrue((5, 4) | fstd.GreaterOrEqual())

    def test_greater_or_equal_false(self) -> None:
        self.assertFalse((3, 4) | fstd.GreaterOrEqual())

    def test_less_or_equal_equal(self) -> None:
        self.assertTrue((4, 4) | fstd.LessOrEqual())

    def test_less_or_equal_less(self) -> None:
        self.assertTrue((3, 4) | fstd.LessOrEqual())

    def test_less_or_equal_false(self) -> None:
        self.assertFalse((5, 4) | fstd.LessOrEqual())

    def test_equal_true(self) -> None:
        self.assertTrue((7, 7) | fstd.Equal())

    def test_equal_false(self) -> None:
        self.assertFalse((7, 8) | fstd.Equal())

    def test_not_equal_true(self) -> None:
        self.assertTrue((7, 8) | fstd.NotEqual())

    def test_not_equal_false(self) -> None:
        self.assertFalse((7, 7) | fstd.NotEqual())


class LogicalTests(unittest.TestCase):
    def test_not_true(self) -> None:
        self.assertFalse((True,) | fstd.Not())

    def test_not_false(self) -> None:
        self.assertTrue((False,) | fstd.Not())

    def test_and_both_true(self) -> None:
        self.assertTrue((True, True) | fstd.And())

    def test_and_one_false(self) -> None:
        self.assertFalse((True, False) | fstd.And())

    def test_or_one_true(self) -> None:
        self.assertTrue((False, True) | fstd.Or())

    def test_or_both_false(self) -> None:
        self.assertFalse((False, False) | fstd.Or())


class UnaryTests(unittest.TestCase):
    def test_unary_plus(self) -> None:
        self.assertEqual((5,) | fstd.UnaryPlus(), 5)

    def test_unary_minus(self) -> None:
        self.assertEqual((5,) | fstd.UnaryMinus(), -5)

    def test_unary_minus_negative(self) -> None:
        self.assertEqual((-3,) | fstd.UnaryMinus(), 3)


class ToStringTests(unittest.TestCase):
    def test_int_to_string(self) -> None:
        self.assertEqual((42,) | fstd.ToString(), "42")

    def test_float_to_string(self) -> None:
        self.assertEqual((3.14,) | fstd.ToString(), "3.14")

    def test_bool_to_string(self) -> None:
        self.assertEqual((True,) | fstd.ToString(), "True")


class PathBuiltinTests(unittest.TestCase):
    def test_cwd(self) -> None:
        self.assertEqual(() | fstd.Cwd(), str(Path.cwd()))

    def test_path_join(self) -> None:
        joined = ("a", "b") | fstd.PathJoin()
        self.assertEqual(joined, str(Path("a") / "b"))

    def test_path_join_three_segments(self) -> None:
        joined = ("a", "b", "c") | fstd.PathJoin()
        self.assertEqual(joined, str(Path("a") / "b" / "c"))

    def test_path_join_four_segments(self) -> None:
        joined = ("root", "sub", "dir", "file.txt") | fstd.PathJoin()
        self.assertEqual(joined, str(Path("root") / "sub" / "dir" / "file.txt"))

    def test_path_join_too_few_segments_raises(self) -> None:
        with self.assertRaises((ValueError, TypeError)):
            ("only_one",) | fstd.PathJoin()

    def test_path_normalize(self) -> None:
        self.assertEqual(("a/./b/..",) | fstd.PathNormalize(), str(Path("a")))

    def test_path_basename_and_dirname(self) -> None:
        p = str(Path("root") / "child" / "file.txt")
        self.assertEqual((p,) | fstd.PathBasename(), "file.txt")
        self.assertEqual((p,) | fstd.PathDirname(), str(Path("root") / "child"))

    def test_path_exists_isfile_isdir_and_absolute(self) -> None:
        with TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            file_path = tmp_path / "x.txt"
            file_path.write_text("ok", encoding="utf-8")

            self.assertTrue((str(file_path),) | fstd.PathExists())
            self.assertTrue((str(file_path),) | fstd.PathIsFile())
            self.assertFalse((str(file_path),) | fstd.PathIsDir())
            self.assertTrue((str(tmp_path),) | fstd.PathIsDir())
            self.assertTrue(Path((str(file_path),) | fstd.PathAbsolute()).is_absolute())


class OSBuiltinTests(unittest.TestCase):
    def test_os_env_and_hasenv(self) -> None:
        with patch.dict(os.environ, {"FLEAUX_TEST_ENV": "abc"}, clear=False):
            self.assertEqual(("FLEAUX_TEST_ENV",) | fstd.OSEnv(), "abc")
            self.assertTrue(("FLEAUX_TEST_ENV",) | fstd.OSHasEnv())
            self.assertIsNone(("FLEAUX_MISSING_ENV",) | fstd.OSEnv())
            self.assertFalse(("FLEAUX_MISSING_ENV",) | fstd.OSHasEnv())

    def test_os_setenv_and_unsetenv(self) -> None:
        with patch.dict(os.environ, {}, clear=False):
            self.assertEqual(("FLEAUX_SET_ENV", "value") | fstd.OSSetEnv(), "value")
            self.assertEqual(os.environ.get("FLEAUX_SET_ENV"), "value")
            self.assertTrue(("FLEAUX_SET_ENV",) | fstd.OSUnsetEnv())
            self.assertFalse(("FLEAUX_SET_ENV",) | fstd.OSUnsetEnv())

    def test_os_platform_predicates(self) -> None:
        self.assertEqual(() | fstd.OSIsWindows(), os.name == "nt")
        self.assertEqual(() | fstd.OSIsLinux(), sys.platform.startswith("linux"))
        self.assertEqual(() | fstd.OSIsMacOS(), sys.platform == "darwin")

    def test_os_home(self) -> None:
        self.assertEqual(() | fstd.OSHome(), str(Path.home()))

    def test_os_home_falls_back_to_dot_when_path_home_fails(self) -> None:
        with patch("fleaux_std_builtins.Path.home", side_effect=RuntimeError("no home")):
            self.assertEqual(() | fstd.OSHome(), ".")

    def test_os_tempdir(self) -> None:
        import tempfile
        self.assertEqual(() | fstd.OSTempDir(), tempfile.gettempdir())


class PathExtrasTests(unittest.TestCase):
    def test_extension(self) -> None:
        self.assertEqual(("file.txt",) | fstd.PathExtension(), ".txt")
        self.assertEqual(("archive.tar.gz",) | fstd.PathExtension(), ".gz")
        self.assertEqual(("noext",) | fstd.PathExtension(), "")

    def test_stem(self) -> None:
        self.assertEqual(("file.txt",) | fstd.PathStem(), "file")
        self.assertEqual(("archive.tar.gz",) | fstd.PathStem(), "archive.tar")

    def test_with_extension(self) -> None:
        self.assertEqual(("file.txt", ".md") | fstd.PathWithExtension(), "file.md")
        self.assertEqual(("file.txt", "") | fstd.PathWithExtension(), "file")

    def test_with_basename(self) -> None:
        p = str(Path("root") / "child" / "file.txt")
        self.assertEqual((p, "other.py") | fstd.PathWithBasename(),
                         str(Path("root") / "child" / "other.py"))


class FileExtrasTests(unittest.TestCase):
    def test_append_text(self) -> None:
        with TemporaryDirectory() as tmp:
            p = Path(tmp) / "f.txt"
            (str(p), "hello") | fstd.FileWriteText()
            (str(p), " world") | fstd.FileAppendText()
            self.assertEqual(p.read_text(encoding="utf-8"), "hello world")

    def test_read_lines(self) -> None:
        with TemporaryDirectory() as tmp:
            p = Path(tmp) / "f.txt"
            p.write_text("a\nb\nc", encoding="utf-8")
            self.assertEqual((str(p),) | fstd.FileReadLines(), ("a", "b", "c"))

    def test_file_delete(self) -> None:
        with TemporaryDirectory() as tmp:
            p = Path(tmp) / "f.txt"
            p.write_text("x", encoding="utf-8")
            self.assertTrue((str(p),) | fstd.FileDelete())
            self.assertFalse(p.exists())

    def test_file_size(self) -> None:
        with TemporaryDirectory() as tmp:
            p = Path(tmp) / "f.txt"
            p.write_text("hello", encoding="utf-8")
            self.assertEqual((str(p),) | fstd.FileSize(), 5)


class DirBuiltinTests(unittest.TestCase):
    def test_dir_create_and_list(self) -> None:
        with TemporaryDirectory() as tmp:
            sub = Path(tmp) / "a" / "b"
            (str(sub),) | fstd.DirCreate()
            self.assertTrue(sub.is_dir())
            (str(Path(tmp) / "x.txt"),) | fstd.FileWriteText() if False else None
            Path(tmp, "x.txt").write_text("", encoding="utf-8")
            names = (str(tmp),) | fstd.DirList()
            self.assertIn("a", names)
            self.assertIn("x.txt", names)

    def test_dir_list_full(self) -> None:
        with TemporaryDirectory() as tmp:
            Path(tmp, "file.txt").write_text("", encoding="utf-8")
            full = (str(tmp),) | fstd.DirListFull()
            self.assertTrue(all(os.path.isabs(p) for p in full))

    def test_dir_delete(self) -> None:
        with TemporaryDirectory() as tmp:
            sub = Path(tmp) / "del_me"
            sub.mkdir()
            self.assertTrue((str(sub),) | fstd.DirDelete())
            self.assertFalse(sub.exists())


class TupleBuiltinTests(unittest.TestCase):
    def test_append(self) -> None:
        self.assertEqual(((1, 2), 3) | fstd.TupleAppend(), (1, 2, 3))

    def test_prepend(self) -> None:
        self.assertEqual(((2, 3), 1) | fstd.TuplePrepend(), (1, 2, 3))

    def test_reverse(self) -> None:
        self.assertEqual(((1, 2, 3),) | fstd.TupleReverse(), (3, 2, 1))

    def test_contains_true(self) -> None:
        self.assertTrue(((1, 2, 3), 2) | fstd.TupleContains())

    def test_contains_false(self) -> None:
        self.assertFalse(((1, 2, 3), 9) | fstd.TupleContains())

    def test_zip(self) -> None:
        self.assertEqual(((1, 2, 3), ("a", "b", "c")) | fstd.TupleZip(),
                         ((1, "a"), (2, "b"), (3, "c")))

    def test_map(self) -> None:
        Double = fstd.make_node(lambda x: x * 2)
        self.assertEqual(((1, 2, 3), Double) | fstd.TupleMap(), (2, 4, 6))

    def test_filter(self) -> None:
        IsEven = fstd.make_node(lambda x: x % 2 == 0)
        self.assertEqual(((1, 2, 3, 4, 5), IsEven) | fstd.TupleFilter(), (2, 4))

    def test_sort_numbers(self) -> None:
        self.assertEqual(((3, 1, 2, 2),) | fstd.TupleSort(), (1, 2, 2, 3))

    def test_sort_strings(self) -> None:
        self.assertEqual((("c", "a", "b"),) | fstd.TupleSort(), ("a", "b", "c"))

    def test_sort_nested_tuples(self) -> None:
        self.assertEqual((((2, 1), (1, 9), (1, 2)),) | fstd.TupleSort(), ((1, 2), (1, 9), (2, 1)))

    def test_sort_mixed_types_raises(self) -> None:
        with self.assertRaises(TypeError):
            ((1, "a"),) | fstd.TupleSort()

    def test_unique_preserves_first_occurrence_order(self) -> None:
        self.assertEqual(((3, 1, 3, 2, 1, 2),) | fstd.TupleUnique(), (3, 1, 2))

    def test_min_and_max_numbers(self) -> None:
        self.assertEqual(((7, 2, 4, 2),) | fstd.TupleMin(), 2)
        self.assertEqual(((7, 2, 4, 2),) | fstd.TupleMax(), 7)

    def test_min_empty_raises(self) -> None:
        with self.assertRaises(ValueError):
            (((),)) | fstd.TupleMin()

    def test_max_empty_raises(self) -> None:
        with self.assertRaises(ValueError):
            (((),)) | fstd.TupleMax()

    def test_reduce_sum(self) -> None:
        AddPair = fstd.make_node(lambda acc, x: acc + x)
        self.assertEqual(((1, 2, 3, 4), 0, AddPair) | fstd.TupleReduce(), 10)

    def test_find_index(self) -> None:
        IsEven = fstd.make_node(lambda x: x % 2 == 0)
        self.assertEqual(((1, 3, 4, 7), IsEven) | fstd.TupleFindIndex(), 2)
        self.assertEqual(((1, 3, 5), IsEven) | fstd.TupleFindIndex(), -1)

    def test_any_and_all(self) -> None:
        IsEven = fstd.make_node(lambda x: x % 2 == 0)
        self.assertTrue(((1, 3, 4), IsEven) | fstd.TupleAny())
        self.assertFalse(((1, 3, 5), IsEven) | fstd.TupleAny())
        self.assertTrue(((2, 4, 6), IsEven) | fstd.TupleAll())
        self.assertFalse(((2, 3, 6), IsEven) | fstd.TupleAll())

    def test_range_forms(self) -> None:
        self.assertEqual((5,) | fstd.TupleRange(), (0, 1, 2, 3, 4))
        self.assertEqual((2, 6) | fstd.TupleRange(), (2, 3, 4, 5))
        self.assertEqual((6, 2, -2) | fstd.TupleRange(), (6, 4))

    def test_range_zero_step_raises(self) -> None:
        with self.assertRaises(ValueError):
            (0, 3, 0) | fstd.TupleRange()


class MathBuiltinTests(unittest.TestCase):
    def test_floor(self) -> None:
        self.assertEqual((3.9,) | fstd.MathFloor(), 3)
        self.assertEqual((-1.1,) | fstd.MathFloor(), -2)

    def test_ceil(self) -> None:
        self.assertEqual((3.1,) | fstd.MathCeil(), 4)
        self.assertEqual((-1.9,) | fstd.MathCeil(), -1)

    def test_abs(self) -> None:
        self.assertEqual((-5,) | fstd.MathAbs(), 5)
        self.assertEqual((5,) | fstd.MathAbs(), 5)

    def test_log(self) -> None:
        import math
        self.assertAlmostEqual((math.e,) | fstd.MathLog(), 1.0)
        self.assertAlmostEqual((1.0,) | fstd.MathLog(), 0.0)

    def test_clamp_within_range(self) -> None:
        self.assertEqual((5, 0, 10) | fstd.MathClamp(), 5)

    def test_clamp_below_lo(self) -> None:
        self.assertEqual((-3, 0, 10) | fstd.MathClamp(), 0)

    def test_clamp_above_hi(self) -> None:
        self.assertEqual((15, 0, 10) | fstd.MathClamp(), 10)

    def test_math_namespace_reuses_existing_trig_classes(self) -> None:
        # Std.Math.Sqrt/Sin/Cos/Tan map to existing Sqrt/Sin/Cos/Tan runtime classes
        import math
        self.assertAlmostEqual((4.0,) | fstd.Sqrt(), 2.0)
        self.assertAlmostEqual((0.0,) | fstd.Sin(), 0.0)
        self.assertAlmostEqual((0.0,) | fstd.Cos(), 1.0)
        self.assertAlmostEqual((0.0,) | fstd.Tan(), 0.0)


class FileBuiltinTests(unittest.TestCase):
    def test_write_and_read_text_round_trip(self) -> None:
        with TemporaryDirectory() as tmp:
            file_path = Path(tmp) / "note.txt"
            out = (str(file_path), "hello") | fstd.FileWriteText()
            self.assertEqual(out, str(file_path))
            self.assertEqual((str(file_path),) | fstd.FileReadText(), "hello")

    def test_write_overwrites_existing_content(self) -> None:
        with TemporaryDirectory() as tmp:
            file_path = Path(tmp) / "note.txt"
            file_path.write_text("first", encoding="utf-8")
            (str(file_path), "second") | fstd.FileWriteText()
            self.assertEqual(file_path.read_text(encoding="utf-8"), "second")

    def test_file_with_open_accepts_make_node_class_ref(self) -> None:
        with TemporaryDirectory() as tmp:
            file_path = Path(tmp) / "with_open.txt"
            file_path.write_text("line1\nline2\n", encoding="utf-8")

            ReadFirst = fstd.make_node(lambda h: ((h,) | fstd.FileReadLine())[1])
            out = (str(file_path), "r", ReadFirst) | fstd.FileWithOpen()
            self.assertEqual(out, "line1")

    def test_file_with_open_accepts_callable_instance(self) -> None:
        with TemporaryDirectory() as tmp:
            file_path = Path(tmp) / "with_open_instance.txt"
            file_path.write_text("value\n", encoding="utf-8")

            ReadFirstNode = fstd.make_node(lambda h: ((h,) | fstd.FileReadLine())[1])
            out = (str(file_path), "r", ReadFirstNode()) | fstd.FileWithOpen()
            self.assertEqual(out, "value")


class LoopTests(unittest.TestCase):
    def test_loop_countdown_scalar(self) -> None:
        Continue = fstd.make_node(lambda n: n > 0)
        Step = fstd.make_node(lambda n: n - 1)
        self.assertEqual((5, Continue, Step) | fstd.Loop(), 0)

    def test_loop_accumulator_tuple_state(self) -> None:
        # state = (n, acc); while n > 0: n = n-1, acc = acc+n
        Continue = fstd.make_node(lambda n, acc: n > 0)
        Step = fstd.make_node(lambda n, acc: (n - 1, acc + n))
        self.assertEqual(((5, 0), Continue, Step) | fstd.Loop(), (0, 15))

    def test_loop_no_iteration_when_condition_false(self) -> None:
        Continue = fstd.make_node(lambda n: n > 0)
        Step = fstd.make_node(lambda n: n - 1)
        self.assertEqual((0, Continue, Step) | fstd.Loop(), 0)

    def test_loop_wrong_arity_raises(self) -> None:
        Continue = fstd.make_node(lambda n: n > 0)
        Step = fstd.make_node(lambda n: n - 1)
        with self.assertRaises(TypeError):
            (5, Continue) | fstd.Loop()
        with self.assertRaises(TypeError):
            (5, Continue, Step, 10) | fstd.Loop()

    def test_loop_supports_builtin_refs(self) -> None:
        # while n > 1: n = sqrt(n*n)
        Continue = fstd.make_node(lambda n: n > 1)
        Step = fstd.make_node(lambda n: (n, n) | fstd.Multiply())
        self.assertEqual((1, Continue, Step) | fstd.Loop(), 1)

    def test_loop_bounded_success(self) -> None:
        Continue = fstd.make_node(lambda n: n > 0)
        Step = fstd.make_node(lambda n: n - 1)
        self.assertEqual((5, Continue, Step, 5) | fstd.LoopN(), 0)

    def test_loop_bounded_exceeds_limit(self) -> None:
        Continue = fstd.make_node(lambda n: n > 0)
        Step = fstd.make_node(lambda n: n - 1)
        with self.assertRaises(RuntimeError):
            (5, Continue, Step, 4) | fstd.LoopN()

    def test_loop_bounded_zero_limit(self) -> None:
        Continue = fstd.make_node(lambda n: n > 0)
        Step = fstd.make_node(lambda n: n - 1)
        with self.assertRaises(RuntimeError):
            (1, Continue, Step, 0) | fstd.LoopN()

    def test_loop_bounded_wrong_arity_raises(self) -> None:
        Continue = fstd.make_node(lambda n: n > 0)
        Step = fstd.make_node(lambda n: n - 1)
        with self.assertRaises(TypeError):
            (5, Continue, Step) | fstd.LoopN()

    def test_loop_bounded_non_int_limit_raises(self) -> None:
        Continue = fstd.make_node(lambda n: n > 0)
        Step = fstd.make_node(lambda n: n - 1)
        with self.assertRaises(TypeError):
            (5, Continue, Step, 5.0) | fstd.LoopN()

    def test_loop_bounded_negative_limit_raises(self) -> None:
        Continue = fstd.make_node(lambda n: n > 0)
        Step = fstd.make_node(lambda n: n - 1)
        with self.assertRaises(ValueError):
            (5, Continue, Step, -1) | fstd.LoopN()


class BuiltinParityTests(unittest.TestCase):
    """Verify every builtin declared in Std.fleaux with __builtin__ maps to a real class."""

    BUILTIN_OPS = [
        "Println", "Printf", "Input", "Exit", "Add", "Subtract", "Multiply", "Divide",
        "Mod", "Pow", "Sqrt", "Tan", "Sin", "Cos",
        "GreaterThan", "LessThan", "GreaterOrEqual", "LessOrEqual",
        "Equal", "NotEqual", "Not", "And", "Or",
        "Select", "Apply", "Branch", "Loop", "LoopN",
        "UnaryPlus", "UnaryMinus", "ToString",
        "Take", "Drop", "ElementAt",
        "GetArgs", "ToNum", "Length", "Slice",
        "OSEnv", "OSHasEnv", "OSSetEnv", "OSUnsetEnv",
        "OSIsWindows", "OSIsLinux", "OSIsMacOS", "OSHome", "OSTempDir",
        "MathFloor", "MathCeil", "MathAbs", "MathLog", "MathClamp",
        "Cwd", "PathJoin", "PathNormalize", "PathBasename", "PathDirname",
        "PathExists", "PathIsFile", "PathIsDir", "PathAbsolute",
        "PathExtension", "PathStem", "PathWithExtension", "PathWithBasename",
        "FileReadText", "FileWriteText", "FileAppendText", "FileReadLines",
        "FileDelete", "FileSize",
        "DirCreate", "DirDelete", "DirList", "DirListFull",
        "StringUpper", "StringLower", "StringTrim", "StringTrimStart", "StringTrimEnd",
        "StringSplit", "StringJoin", "StringReplace", "StringContains", "StringStartsWith",
        "StringEndsWith", "StringLength",
        "TupleAppend", "TuplePrepend", "TupleReverse", "TupleContains",
        "TupleZip", "TupleMap", "TupleFilter", "TupleSort", "TupleUnique", "TupleMin", "TupleMax",
        "TupleReduce", "TupleFindIndex", "TupleAny", "TupleAll", "TupleRange",
    ]

    def test_all_declared_builtins_are_importable(self) -> None:
        for name in self.BUILTIN_OPS:
            with self.subTest(builtin=name):
                cls = getattr(fstd, name, None)
                self.assertIsNotNone(
                    cls,
                    msg=f"fstd.{name} is declared in Std.fleaux but missing in fleaux_std_builtins.py",
                )
                self.assertTrue(
                    callable(cls),
                    msg=f"fstd.{name} exists but is not callable",
                )


if __name__ == "__main__":
    unittest.main()

