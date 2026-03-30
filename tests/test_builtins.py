from __future__ import annotations

import unittest

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


class BuiltinParityTests(unittest.TestCase):
    """Verify every builtin declared in Std.fleaux with __builtin__ maps to a real class."""

    BUILTIN_OPS = [
        "Println", "Printf", "In", "Add", "Subtract", "Multiply", "Divide",
        "Mod", "Pow", "Sqrt", "Tan", "Sin", "Cos",
        "GreaterThan", "LessThan", "GreaterOrEqual", "LessOrEqual",
        "Equal", "NotEqual", "Not", "And", "Or",
        "UnaryPlus", "UnaryMinus", "ToString",
        "Take", "Drop", "ElementAt",
        "GetArgs", "ToNum", "Length", "Slice",
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

