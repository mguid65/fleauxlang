"""
Comprehensive tests for conditional primitives: Select, Branch, Apply.

Covers:
  - Unit tests against the Python runtime directly (fstd)
  - End-to-end tests via the transpiler pipeline
  - Edge cases and error conditions
  - Composition and chaining
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

import fleaux_std_builtins as fstd
from fleaux_transpiler import FleauxTranspiler
from fleaux_lowering import lower
from fleaux_parser import parse_program
from tests.helpers import ensure_std_generated


def setUpModule() -> None:
    ensure_std_generated()


# ---------------------------------------------------------------------------
# Helpers shared across tests
# ---------------------------------------------------------------------------

def _transpile_and_run(source: str):
    """Transpile a Fleaux source string and return the last produced value."""
    import importlib.util, uuid

    repo_root = Path(__file__).resolve().parents[1]
    uid = uuid.uuid4().hex[:8]
    tmp = repo_root / f"_test_cond_{uid}.fleaux"
    try:
        tmp.write_text(source, encoding="utf-8")
        transpiler = FleauxTranspiler()
        out_path = transpiler.process(tmp)

        spec = importlib.util.spec_from_file_location(f"_fleaux_cond_{uid}", out_path)
        mod = importlib.util.module_from_spec(spec)
        repo_str = str(repo_root)
        if repo_str not in sys.path:
            sys.path.insert(0, repo_str)
        spec.loader.exec_module(mod)
        return getattr(mod, "_fleaux_last_value", None)
    finally:
        tmp.unlink(missing_ok=True)
        for f in repo_root.glob(f"fleaux_generated_module__test_cond_{uid}*.py"):
            f.unlink(missing_ok=True)


def _make_node(fn):
    """Wrap a plain Python callable as a Fleaux make_node class."""
    return fstd.make_node(fn)


# ---------------------------------------------------------------------------
# Std.Select — unit tests (runtime level)
# ---------------------------------------------------------------------------

class SelectUnitTests(unittest.TestCase):
    """Direct runtime tests for fstd.Select."""

    def test_true_condition_returns_true_val(self):
        result = (True, 10, 20) | fstd.Select()
        self.assertEqual(result, 10)

    def test_false_condition_returns_false_val(self):
        result = (False, 10, 20) | fstd.Select()
        self.assertEqual(result, 20)

    def test_truthy_number_picks_true_val(self):
        result = (1, "yes", "no") | fstd.Select()
        self.assertEqual(result, "yes")

    def test_falsy_zero_picks_false_val(self):
        result = (0, "yes", "no") | fstd.Select()
        self.assertEqual(result, "no")

    def test_string_values(self):
        result = (True, "hello", "world") | fstd.Select()
        self.assertEqual(result, "hello")

    def test_numeric_values(self):
        result = (False, 3.14, 2.71) | fstd.Select()
        self.assertAlmostEqual(result, 2.71)

    def test_nested_tuple_values(self):
        result = (True, (1, 2), (3, 4)) | fstd.Select()
        self.assertEqual(result, (1, 2))

    def test_both_branches_are_evaluated_eagerly(self):
        """Select always evaluates both branches before picking."""
        side_effects = []
        def branch_a():
            side_effects.append("a")
            return "a"
        def branch_b():
            side_effects.append("b")
            return "b"
        # Both values are computed before Select runs
        result = (True, branch_a(), branch_b()) | fstd.Select()
        self.assertEqual(result, "a")
        self.assertEqual(side_effects, ["a", "b"])  # both were called

    def test_wrong_arity_raises(self):
        with self.assertRaises(TypeError):
            (True, 1) | fstd.Select()

    def test_wrong_arity_five_raises(self):
        with self.assertRaises(TypeError):
            (True, 1, 2, 3, 4) | fstd.Select()

    def test_condition_from_comparison(self):
        cond = (5, 3) | fstd.GreaterThan()
        result = (cond, "bigger", "smaller") | fstd.Select()
        self.assertEqual(result, "bigger")

    def test_condition_from_equality(self):
        cond = (4, 4) | fstd.Equal()
        result = (cond, "equal", "not equal") | fstd.Select()
        self.assertEqual(result, "equal")

    def test_none_as_false_val(self):
        result = (False, 42, None) | fstd.Select()
        self.assertIsNone(result)

    def test_select_as_abs(self):
        """Implement abs via Select."""
        x = -7
        cond = (x, 0) | fstd.GreaterOrEqual()
        result = (cond, x, (0, x) | fstd.Subtract()) | fstd.Select()
        self.assertEqual(result, 7)

    def test_select_as_max(self):
        a, b = 4, 9
        cond = (a, b) | fstd.GreaterOrEqual()
        result = (cond, a, b) | fstd.Select()
        self.assertEqual(result, 9)

    def test_select_as_min(self):
        a, b = 4, 9
        cond = (a, b) | fstd.LessOrEqual()
        result = (cond, a, b) | fstd.Select()
        self.assertEqual(result, 4)


# ---------------------------------------------------------------------------
# Std.Apply — unit tests (runtime level)
# ---------------------------------------------------------------------------

class ApplyUnitTests(unittest.TestCase):
    """Direct runtime tests for fstd.Apply."""

    def test_apply_user_defined_node(self):
        Double = fstd.make_node(lambda x: x * 2)
        result = (5, Double) | fstd.Apply()
        self.assertEqual(result, 10)

    def test_apply_builtin_class(self):
        """Apply works with builtin node classes (e.g. fstd.Sqrt)."""
        result = (9, fstd.Sqrt) | fstd.Apply()
        self.assertAlmostEqual(result, 3.0)

    def test_apply_already_instantiated_builtin(self):
        """Apply works whether the node is a class or an instance."""
        result = (9, fstd.Sqrt()) | fstd.Apply()
        self.assertAlmostEqual(result, 3.0)

    def test_apply_string_value(self):
        ToString = fstd.make_node(lambda x: str(x))
        result = (42, ToString) | fstd.Apply()
        self.assertEqual(result, "42")

    def test_apply_tuple_value(self):
        Sum = fstd.make_node(lambda a, b: a + b)
        result = ((3, 4), Sum) | fstd.Apply()
        self.assertEqual(result, 7)

    def test_apply_wrong_arity_raises(self):
        Double = fstd.make_node(lambda x: x * 2)
        with self.assertRaises(TypeError):
            (5, Double, "extra") | fstd.Apply()

    def test_apply_wrong_arity_one_raises(self):
        with self.assertRaises(TypeError):
            (5,) | fstd.Apply()

    def test_apply_chains_with_pipe(self):
        Double = fstd.make_node(lambda x: x * 2)
        AddOne = fstd.make_node(lambda x: x + 1)
        val = (5, Double) | fstd.Apply()
        result = (val, AddOne) | fstd.Apply()
        self.assertEqual(result, 11)

    def test_apply_different_functions_same_value(self):
        Double = fstd.make_node(lambda x: x * 2)
        Negate = fstd.make_node(lambda x: -x)
        self.assertEqual((6, Double) | fstd.Apply(), 12)
        self.assertEqual((6, Negate) | fstd.Apply(), -6)


# ---------------------------------------------------------------------------
# Std.Branch — unit tests (runtime level)
# ---------------------------------------------------------------------------

class BranchUnitTests(unittest.TestCase):
    """Direct runtime tests for fstd.Branch."""

    def setUp(self):
        self.Double = fstd.make_node(lambda x: x * 2)
        self.Negate = fstd.make_node(lambda x: -x)
        self.AddTen = fstd.make_node(lambda x: x + 10)
        self.Identity = fstd.make_node(lambda x: x)

    def test_true_branch_executes(self):
        result = (True, 5, self.Double, self.Negate) | fstd.Branch()
        self.assertEqual(result, 10)

    def test_false_branch_executes(self):
        result = (False, 5, self.Double, self.Negate) | fstd.Branch()
        self.assertEqual(result, -5)

    def test_only_chosen_branch_executes(self):
        """Unlike Select, Branch must NOT call the unchosen function."""
        calls = []

        TrueFunc = fstd.make_node(lambda x: (calls.append("true"), x * 2)[1])
        FalseFunc = fstd.make_node(lambda x: (calls.append("false"), x + 10)[1])

        (True, 5, TrueFunc, FalseFunc) | fstd.Branch()
        self.assertEqual(calls, ["true"])

        calls.clear()
        (False, 5, TrueFunc, FalseFunc) | fstd.Branch()
        self.assertEqual(calls, ["false"])

    def test_truthy_value_as_condition(self):
        result = (1, 7, self.Double, self.AddTen) | fstd.Branch()
        self.assertEqual(result, 14)

    def test_falsy_zero_as_condition(self):
        result = (0, 7, self.Double, self.AddTen) | fstd.Branch()
        self.assertEqual(result, 17)

    def test_condition_from_comparison(self):
        cond = (10, 5) | fstd.GreaterThan()
        result = (cond, 10, self.Double, self.AddTen) | fstd.Branch()
        self.assertEqual(result, 20)

    def test_condition_false_from_comparison(self):
        cond = (3, 5) | fstd.GreaterThan()
        result = (cond, 3, self.Double, self.AddTen) | fstd.Branch()
        self.assertEqual(result, 13)

    def test_branch_with_builtin_true_func(self):
        result = (True, 16, fstd.Sqrt, self.AddTen) | fstd.Branch()
        self.assertAlmostEqual(result, 4.0)

    def test_branch_with_builtin_false_func(self):
        result = (False, 16, self.AddTen, fstd.Sqrt) | fstd.Branch()
        self.assertAlmostEqual(result, 4.0)

    def test_branch_wrong_arity_raises(self):
        with self.assertRaises(TypeError):
            (True, 5, self.Double) | fstd.Branch()

    def test_branch_wrong_arity_five_raises(self):
        with self.assertRaises(TypeError):
            (True, 5, self.Double, self.Negate, self.Identity) | fstd.Branch()

    def test_branch_with_tuple_value(self):
        Sum = fstd.make_node(lambda a, b: a + b)
        Product = fstd.make_node(lambda a, b: a * b)
        result = (True, (3, 4), Sum, Product) | fstd.Branch()
        self.assertEqual(result, 7)
        result = (False, (3, 4), Sum, Product) | fstd.Branch()
        self.assertEqual(result, 12)

    def test_branch_result_can_be_piped(self):
        result = (True, 5, self.Double, self.Negate) | fstd.Branch()
        result = (result, self.AddTen) | fstd.Apply()
        self.assertEqual(result, 20)

    def test_nested_branch(self):
        """Branch whose branches are themselves Branch results."""
        IsPositive = fstd.make_node(lambda x: x > 0)
        IsEven = fstd.make_node(lambda x: x % 2 == 0)

        # Double if positive, negate if negative
        cond1 = (6, 0) | fstd.GreaterThan()
        r1 = (cond1, 6, self.Double, self.Negate) | fstd.Branch()
        self.assertEqual(r1, 12)

        cond2 = (-4, 0) | fstd.GreaterThan()
        r2 = (cond2, -4, self.Double, self.Negate) | fstd.Branch()
        self.assertEqual(r2, 4)

    def test_branch_implements_abs(self):
        for x in (-10, -1, 0, 1, 10):
            cond = (x, 0) | fstd.GreaterOrEqual()
            Neg = fstd.make_node(lambda v: -v)
            Id = fstd.make_node(lambda v: v)
            result = (cond, x, Id, Neg) | fstd.Branch()
            self.assertEqual(result, abs(x))

    def test_branch_implements_max(self):
        pairs = [(3, 7), (7, 3), (5, 5), (-1, 2)]
        for a, b in pairs:
            cond = (a, b) | fstd.GreaterOrEqual()
            # decompose_call splats the tuple so the lambda receives two args
            TakeA = fstd.make_node(lambda x, y: x)
            TakeB = fstd.make_node(lambda x, y: y)
            result = (cond, (a, b), TakeA, TakeB) | fstd.Branch()
            self.assertEqual(result, max(a, b))


# ---------------------------------------------------------------------------
# Select vs Branch — correctness comparison
# ---------------------------------------------------------------------------

class SelectVsBranchTests(unittest.TestCase):
    """Verify Select and Branch agree on results for pure functions."""

    def setUp(self):
        self.Double = fstd.make_node(lambda x: x * 2)
        self.Negate = fstd.make_node(lambda x: -x)

    def _abs_select(self, x):
        cond = (x, 0) | fstd.GreaterOrEqual()
        return (cond, x, (0, x) | fstd.Subtract()) | fstd.Select()

    def _abs_branch(self, x):
        Id = fstd.make_node(lambda v: v)
        Neg = fstd.make_node(lambda v: -v)
        cond = (x, 0) | fstd.GreaterOrEqual()
        return (cond, x, Id, Neg) | fstd.Branch()

    def test_abs_agree_positive(self):
        for v in (1, 3, 100, 0.5):
            self.assertAlmostEqual(self._abs_select(v), self._abs_branch(v))

    def test_abs_agree_negative(self):
        for v in (-1, -3, -100, -0.5):
            self.assertAlmostEqual(self._abs_select(v), self._abs_branch(v))

    def test_abs_zero(self):
        self.assertEqual(self._abs_select(0), self._abs_branch(0))


# ---------------------------------------------------------------------------
# End-to-end transpiler tests
# ---------------------------------------------------------------------------

class SelectTranspilerTests(unittest.TestCase):
    """End-to-end: Fleaux source → transpile → execute for Select."""

    def test_select_true(self):
        result = _transpile_and_run(
            "import Std;\n"
            "((1, 0) -> Std.GreaterThan, 99, 0) -> Std.Select;"
        )
        self.assertEqual(result, 99)

    def test_select_false(self):
        result = _transpile_and_run(
            "import Std;\n"
            "((0, 1) -> Std.GreaterThan, 99, 0) -> Std.Select;"
        )
        self.assertEqual(result, 0)

    def test_select_abs_positive(self):
        result = _transpile_and_run(
            "import Std;\n"
            "let Abs(x: Number): Number =\n"
            "    ((x, 0) -> Std.GreaterOrEqual, x, (0, x) -> Std.Subtract) -> Std.Select;\n"
            "(7) -> Abs;"
        )
        self.assertEqual(result, 7)

    def test_select_abs_negative(self):
        result = _transpile_and_run(
            "import Std;\n"
            "let Abs(x: Number): Number =\n"
            "    ((x, 0) -> Std.GreaterOrEqual, x, (0, x) -> Std.Subtract) -> Std.Select;\n"
            "(-7) -> Abs;"
        )
        self.assertEqual(result, 7)

    def test_select_max(self):
        result = _transpile_and_run(
            "import Std;\n"
            "let Max(a: Number, b: Number): Number =\n"
            "    ((a, b) -> Std.GreaterOrEqual, a, b) -> Std.Select;\n"
            "(3, 9) -> Max;"
        )
        self.assertEqual(result, 9)

    def test_select_min(self):
        result = _transpile_and_run(
            "import Std;\n"
            "let Min(a: Number, b: Number): Number =\n"
            "    ((a, b) -> Std.LessOrEqual, a, b) -> Std.Select;\n"
            "(3, 9) -> Min;"
        )
        self.assertEqual(result, 3)

    def test_select_chained_clamp(self):
        result = _transpile_and_run(
            "import Std;\n"
            "let Max(a: Number, b: Number): Number =\n"
            "    ((a, b) -> Std.GreaterOrEqual, a, b) -> Std.Select;\n"
            "let Min(a: Number, b: Number): Number =\n"
            "    ((a, b) -> Std.LessOrEqual, a, b) -> Std.Select;\n"
            "let Clamp(x: Number, lo: Number, hi: Number): Number =\n"
            "    (x, lo) -> Max -> (_, hi) -> Min;\n"
            "(15, 0, 10) -> Clamp;"
        )
        self.assertEqual(result, 10)

    def test_select_clamp_below(self):
        result = _transpile_and_run(
            "import Std;\n"
            "let Max(a: Number, b: Number): Number =\n"
            "    ((a, b) -> Std.GreaterOrEqual, a, b) -> Std.Select;\n"
            "let Min(a: Number, b: Number): Number =\n"
            "    ((a, b) -> Std.LessOrEqual, a, b) -> Std.Select;\n"
            "let Clamp(x: Number, lo: Number, hi: Number): Number =\n"
            "    (x, lo) -> Max -> (_, hi) -> Min;\n"
            "(-5, 0, 10) -> Clamp;"
        )
        self.assertEqual(result, 0)

    def test_select_clamp_in_range(self):
        result = _transpile_and_run(
            "import Std;\n"
            "let Max(a: Number, b: Number): Number =\n"
            "    ((a, b) -> Std.GreaterOrEqual, a, b) -> Std.Select;\n"
            "let Min(a: Number, b: Number): Number =\n"
            "    ((a, b) -> Std.LessOrEqual, a, b) -> Std.Select;\n"
            "let Clamp(x: Number, lo: Number, hi: Number): Number =\n"
            "    (x, lo) -> Max -> (_, hi) -> Min;\n"
            "(7, 0, 10) -> Clamp;"
        )
        self.assertEqual(result, 7)

    def test_select_equality_check(self):
        result = _transpile_and_run(
            "import Std;\n"
            "((5, 5) -> Std.Equal, 1, 0) -> Std.Select;"
        )
        self.assertEqual(result, 1)


class ApplyTranspilerTests(unittest.TestCase):
    """End-to-end: Fleaux source → transpile → execute for Apply."""

    def test_apply_user_function(self):
        result = _transpile_and_run(
            "import Std;\n"
            "let Double(x: Number): Number = (x, 2) -> Std.Multiply;\n"
            "(5, Double) -> Std.Apply;"
        )
        self.assertEqual(result, 10)

    def test_apply_builtin(self):
        result = _transpile_and_run(
            "import Std;\n"
            "(9, Std.Sqrt) -> Std.Apply;"
        )
        self.assertAlmostEqual(result, 3.0)

    def test_apply_chained(self):
        result = _transpile_and_run(
            "import Std;\n"
            "let Double(x: Number): Number = (x, 2) -> Std.Multiply;\n"
            "let AddOne(x: Number): Number = (x, 1) -> Std.Add;\n"
            "(5, Double) -> Std.Apply -> (_, AddOne) -> Std.Apply;"
        )
        self.assertEqual(result, 11)

    def test_apply_function_as_parameter(self):
        """A function can accept another function as a value and apply it."""
        result = _transpile_and_run(
            "import Std;\n"
            "let Double(x: Number): Number = (x, 2) -> Std.Multiply;\n"
            "let AddTen(x: Number): Number = (x, 10) -> Std.Add;\n"
            "let RunWith5(f: Any): Number = (5, f) -> Std.Apply;\n"
            "(Double) -> RunWith5;"
        )
        self.assertEqual(result, 10)

    def test_apply_different_functions_same_value(self):
        result_double = _transpile_and_run(
            "import Std;\n"
            "let Double(x: Number): Number = (x, 2) -> Std.Multiply;\n"
            "(6, Double) -> Std.Apply;"
        )
        result_add = _transpile_and_run(
            "import Std;\n"
            "let AddTen(x: Number): Number = (x, 10) -> Std.Add;\n"
            "(6, AddTen) -> Std.Apply;"
        )
        self.assertEqual(result_double, 12)
        self.assertEqual(result_add, 16)


class BranchTranspilerTests(unittest.TestCase):
    """End-to-end: Fleaux source → transpile → execute for Branch."""

    def test_branch_true_path(self):
        result = _transpile_and_run(
            "import Std;\n"
            "let Double(x: Number): Number = (x, 2) -> Std.Multiply;\n"
            "let AddTen(x: Number): Number = (x, 10) -> Std.Add;\n"
            "((7, 0) -> Std.GreaterThan, 7, Double, AddTen) -> Std.Branch;"
        )
        self.assertEqual(result, 14)

    def test_branch_false_path(self):
        result = _transpile_and_run(
            "import Std;\n"
            "let Double(x: Number): Number = (x, 2) -> Std.Multiply;\n"
            "let AddTen(x: Number): Number = (x, 10) -> Std.Add;\n"
            "((-2, 0) -> Std.GreaterThan, -2, Double, AddTen) -> Std.Branch;"
        )
        self.assertEqual(result, 8)

    def test_branch_abs_positive(self):
        result = _transpile_and_run(
            "import Std;\n"
            "let Negate(x: Number): Number = (0, x) -> Std.Subtract;\n"
            "let Identity(x: Number): Number = x;\n"
            "let Abs(x: Number): Number =\n"
            "    ((x, 0) -> Std.GreaterOrEqual, x, Identity, Negate) -> Std.Branch;\n"
            "(8) -> Abs;"
        )
        self.assertEqual(result, 8)

    def test_branch_abs_negative(self):
        result = _transpile_and_run(
            "import Std;\n"
            "let Negate(x: Number): Number = (0, x) -> Std.Subtract;\n"
            "let Identity(x: Number): Number = x;\n"
            "let Abs(x: Number): Number =\n"
            "    ((x, 0) -> Std.GreaterOrEqual, x, Identity, Negate) -> Std.Branch;\n"
            "(-8) -> Abs;"
        )
        self.assertEqual(result, 8)

    def test_branch_result_piped_further(self):
        result = _transpile_and_run(
            "import Std;\n"
            "let Double(x: Number): Number = (x, 2) -> Std.Multiply;\n"
            "let Negate(x: Number): Number = (0, x) -> Std.Subtract;\n"
            "let AbsViaBranch(x: Number): Number =\n"
            "    ((x, 0) -> Std.GreaterOrEqual, x, Double, Negate) -> Std.Branch\n"
            "    -> (_, 2) -> Std.Divide;\n"
            "(-6) -> AbsViaBranch;"
        )
        self.assertAlmostEqual(result, 3.0)

    def test_branch_with_builtin_function_ref(self):
        result = _transpile_and_run(
            "import Std;\n"
            "let AddTen(x: Number): Number = (x, 10) -> Std.Add;\n"
            "(1, 25, Std.Sqrt, AddTen) -> Std.Branch;"
        )
        self.assertAlmostEqual(result, 5.0)

    def test_branch_condition_via_equality(self):
        result = _transpile_and_run(
            "import Std;\n"
            "let Double(x: Number): Number = (x, 2) -> Std.Multiply;\n"
            "let AddOne(x: Number): Number = (x, 1) -> Std.Add;\n"
            "((3, 3) -> Std.Equal, 10, Double, AddOne) -> Std.Branch;"
        )
        self.assertEqual(result, 20)

    def test_branch_chained_with_select(self):
        """Branch and Select can be composed in the same pipeline."""
        result = _transpile_and_run(
            "import Std;\n"
            "let Double(x: Number): Number = (x, 2) -> Std.Multiply;\n"
            "let Negate(x: Number): Number = (0, x) -> Std.Subtract;\n"
            "let AbsViaBranch(x: Number): Number =\n"
            "    ((x, 0) -> Std.GreaterOrEqual, x, Double, Negate) -> Std.Branch\n"
            "    -> (_, 2) -> Std.Divide;\n"
            "let AtMostTen(x: Number): Number =\n"
            "    ((x, 10) -> Std.LessOrEqual, x, 10) -> Std.Select;\n"
            "(-16) -> AbsViaBranch -> AtMostTen;"
        )
        self.assertAlmostEqual(result, 8.0)


# ---------------------------------------------------------------------------
# Composition tests — Select + Branch + Apply together
# ---------------------------------------------------------------------------

class CompositionTests(unittest.TestCase):
    """Tests combining Select, Branch, and Apply in the same program."""

    def test_higher_order_conditional(self):
        """Branch picks which function Apply will use."""
        result = _transpile_and_run(
            "import Std;\n"
            "let Double(x: Number): Number = (x, 2) -> Std.Multiply;\n"
            "let AddTen(x: Number): Number = (x, 10) -> Std.Add;\n"
            "let PickAndApply(flag: Bool, x: Number): Number =\n"
            "    (flag, x, Double, AddTen) -> Std.Branch;\n"
            "(1, 5) -> PickAndApply;"
        )
        self.assertEqual(result, 10)

    def test_select_chooses_function_for_apply(self):
        """Select picks a function reference, then Apply uses it."""
        # Fleaux has no variable bindings; wrap in a function that returns
        # the result of applying the selected function.
        result = _transpile_and_run(
            "import Std;\n"
            "let Double(x: Number): Number = (x, 2) -> Std.Multiply;\n"
            "let AddTen(x: Number): Number = (x, 10) -> Std.Add;\n"
            "let ApplyChosen(flag: Bool, x: Number): Number =\n"
            "    (flag, Double, AddTen) -> Std.Select -> (x, _) -> Std.Apply;\n"
            "(1, 5) -> ApplyChosen;"
        )
        self.assertEqual(result, 10)

    def test_apply_inside_branch(self):
        """A branch target uses Apply internally."""
        result = _transpile_and_run(
            "import Std;\n"
            "let Double(x: Number): Number = (x, 2) -> Std.Multiply;\n"
            "let ApplyDouble(x: Number): Number = (x, Double) -> Std.Apply;\n"
            "let Identity(x: Number): Number = x;\n"
            "((1, 0) -> Std.GreaterThan, 6, ApplyDouble, Identity) -> Std.Branch;"
        )
        self.assertEqual(result, 12)


if __name__ == "__main__":
    unittest.main()

