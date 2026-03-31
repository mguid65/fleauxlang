"""
Comprehensive tests for loops via recursion in Fleaux.

In Fleaux, loops are expressed as recursive functions using Branch to dispatch
between base case and recursive step. Two patterns emerge:

  Single-arg recursion:
    (condition, value, BaseFunc, StepFunc) -> Std.Branch
    where StepFunc calls back into the same function.

  Multi-arg recursion (accumulator / two-argument):
    Pass a tuple as the value; both BaseFunc and StepFunc receive it
    as a splatted argument pair.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path
import uuid

import fleaux_std_builtins as fstd
from fleaux_transpiler import FleauxTranspiler
from fleaux_parser import parse_program
from fleaux_lowering import lower
from tests.helpers import ensure_std_generated


def setUpModule() -> None:
    ensure_std_generated()


# ---------------------------------------------------------------------------
# Helper
# ---------------------------------------------------------------------------

def _run(source: str):
    """Transpile a Fleaux source string, execute it, return last value."""
    import importlib.util
    repo_root = Path(__file__).resolve().parents[1]
    uid = uuid.uuid4().hex[:8]
    tmp = repo_root / f"_test_loop_{uid}.fleaux"
    try:
        tmp.write_text(source, encoding="utf-8")
        out_path = FleauxTranspiler().process(tmp)
        spec = importlib.util.spec_from_file_location(f"_fl_{uid}", out_path)
        mod = importlib.util.module_from_spec(spec)
        repo_str = str(repo_root)
        if repo_str not in sys.path:
            sys.path.insert(0, repo_str)
        spec.loader.exec_module(mod)
        return getattr(mod, "_fleaux_last_value", None)
    finally:
        tmp.unlink(missing_ok=True)
        for f in repo_root.glob(f"fleaux_generated_module__test_loop_{uid}*.py"):
            f.unlink(missing_ok=True)


# ---------------------------------------------------------------------------
# Shared Fleaux preamble blocks
# ---------------------------------------------------------------------------

_STD = "import Std;\n"

# Identity and ReturnOne are utility base-case functions
_UTILS = (
    "let Identity(x: Number): Number = x;\n"
    "let ReturnOne(x: Number): Number = 1;\n"
)


# ---------------------------------------------------------------------------
# Loop builtin pattern (non-recursive iteration)
# ---------------------------------------------------------------------------

_LOOP_SUM = (
    _STD +
    "let ContinueSum(n: Number, acc: Number): Bool = (n, 0) -> Std.GreaterThan;\n"
    "let StepSum(n: Number, acc: Number): Tuple(Number, Number) =\n"
    "    ((n, 1) -> Std.Subtract, (acc, n) -> Std.Add);\n"
    "let SumToLoop(n: Number): Number =\n"
    "    ((n, 0), ContinueSum, StepSum) -> Std.Loop -> (_, 1) -> Std.ElementAt;\n"
)


_LOOP_FACT = (
    _STD +
    "let ContinueFact(n: Number, acc: Number): Bool = (n, 1) -> Std.GreaterThan;\n"
    "let StepFact(n: Number, acc: Number): Tuple(Number, Number) =\n"
    "    ((n, 1) -> Std.Subtract, (acc, n) -> Std.Multiply);\n"
    "let FactorialLoop(n: Number): Number =\n"
    "    ((n, 1), ContinueFact, StepFact) -> Std.Loop -> (_, 1) -> Std.ElementAt;\n"
)


class LoopBuiltinTests(unittest.TestCase):
    def test_loop_sum_zero(self):
        self.assertEqual(_run(_LOOP_SUM + "(0) -> SumToLoop;"), 0)

    def test_loop_sum_five(self):
        self.assertEqual(_run(_LOOP_SUM + "(5) -> SumToLoop;"), 15)

    def test_loop_sum_ten(self):
        self.assertEqual(_run(_LOOP_SUM + "(10) -> SumToLoop;"), 55)

    def test_loop_factorial_zero(self):
        self.assertEqual(_run(_LOOP_FACT + "(0) -> FactorialLoop;"), 1)

    def test_loop_factorial_five(self):
        self.assertEqual(_run(_LOOP_FACT + "(5) -> FactorialLoop;"), 120)

    def test_loop_factorial_ten(self):
        self.assertEqual(_run(_LOOP_FACT + "(10) -> FactorialLoop;"), 3628800)

    def test_loop_bounded_sum_success(self):
        result = _run(
            _STD +
            "let ContinueSum(n: Number, acc: Number): Bool = (n, 0) -> Std.GreaterThan;\n"
            "let StepSum(n: Number, acc: Number): Tuple(Number, Number) =\n"
            "    ((n, 1) -> Std.Subtract, (acc, n) -> Std.Add);\n"
            "let SumToBounded(n: Number): Number =\n"
            "    ((n, 0), ContinueSum, StepSum, 100) -> Std.LoopN -> (_, 1) -> Std.ElementAt;\n"
            "(10) -> SumToBounded;"
        )
        self.assertEqual(result, 55)

    def test_loop_bounded_factorial_success(self):
        result = _run(
            _STD +
            "let ContinueFact(n: Number, acc: Number): Bool = (n, 1) -> Std.GreaterThan;\n"
            "let StepFact(n: Number, acc: Number): Tuple(Number, Number) =\n"
            "    ((n, 1) -> Std.Subtract, (acc, n) -> Std.Multiply);\n"
            "let FactorialBounded(n: Number): Number =\n"
            "    ((n, 1), ContinueFact, StepFact, 100) -> Std.LoopN -> (_, 1) -> Std.ElementAt;\n"
            "(6) -> FactorialBounded;"
        )
        self.assertEqual(result, 720)

    def test_loop_bounded_raises_when_exceeded(self):
        with self.assertRaises(RuntimeError):
            _run(
                _STD +
                "let Continue(n: Number): Bool = (n, 0) -> Std.GreaterThan;\n"
                "let Step(n: Number): Number = (n, 1) -> Std.Subtract;\n"
                "(5, Continue, Step, 4) -> Std.LoopN;"
            )


# ---------------------------------------------------------------------------
# Factorial  (n! = n * (n-1)!)
# ---------------------------------------------------------------------------

_FACTORIAL = (
    _STD + _UTILS +
    "let FactStep(n: Number): Number =\n"
    "    (n, 1) -> Std.Subtract -> Factorial -> (_, n) -> Std.Multiply;\n"
    "let Factorial(n: Number): Number =\n"
    "    ((n, 1) -> Std.LessOrEqual, n, ReturnOne, FactStep) -> Std.Branch;\n"
)


class FactorialTests(unittest.TestCase):

    def _fact(self, n: int) -> int:
        import math
        return math.factorial(n)

    def test_factorial_zero(self):
        self.assertEqual(_run(_FACTORIAL + "(0) -> Factorial;"), 1)

    def test_factorial_one(self):
        self.assertEqual(_run(_FACTORIAL + "(1) -> Factorial;"), 1)

    def test_factorial_two(self):
        self.assertEqual(_run(_FACTORIAL + "(2) -> Factorial;"), 2)

    def test_factorial_five(self):
        self.assertEqual(_run(_FACTORIAL + "(5) -> Factorial;"), 120)

    def test_factorial_ten(self):
        self.assertEqual(_run(_FACTORIAL + "(10) -> Factorial;"), 3628800)

    def test_factorial_sequential(self):
        """Multiple factorial calls compose correctly in a single expression."""
        # Fleaux let requires parameters; compose directly in a tuple
        result = _run(
            _FACTORIAL +
            "((4) -> Factorial, (3) -> Factorial) -> Std.Add;"
        )
        self.assertEqual(result, 24 + 6)


# ---------------------------------------------------------------------------
# Fibonacci  (fib(n) = fib(n-1) + fib(n-2))
# ---------------------------------------------------------------------------

_FIBONACCI = (
    _STD + _UTILS +
    "let FibStep(n: Number): Number =\n"
    "    ((n, 1) -> Std.Subtract -> Fib,\n"
    "     (n, 2) -> Std.Subtract -> Fib) -> Std.Add;\n"
    "let Fib(n: Number): Number =\n"
    "    ((n, 2) -> Std.LessThan, n, Identity, FibStep) -> Std.Branch;\n"
)


class FibonacciTests(unittest.TestCase):

    def _fib(self, n):
        a, b = 0, 1
        for _ in range(n):
            a, b = b, a + b
        return a

    def test_fib_0(self):
        self.assertEqual(_run(_FIBONACCI + "(0) -> Fib;"), 0)

    def test_fib_1(self):
        self.assertEqual(_run(_FIBONACCI + "(1) -> Fib;"), 1)

    def test_fib_2(self):
        self.assertEqual(_run(_FIBONACCI + "(2) -> Fib;"), 1)

    def test_fib_3(self):
        self.assertEqual(_run(_FIBONACCI + "(3) -> Fib;"), 2)

    def test_fib_5(self):
        self.assertEqual(_run(_FIBONACCI + "(5) -> Fib;"), 5)

    def test_fib_10(self):
        self.assertEqual(_run(_FIBONACCI + "(10) -> Fib;"), 55)

    def test_fib_sequence_matches_reference(self):
        for n in range(8):
            result = _run(_FIBONACCI + f"({n}) -> Fib;")
            self.assertEqual(result, self._fib(n), f"Fib({n}) failed")


# ---------------------------------------------------------------------------
# Accumulator pattern: Sum 1..N
#   SumAccum(n, acc): if n <= 0 then acc else SumAccum(n-1, acc+n)
# ---------------------------------------------------------------------------

_SUM_ACCUM = (
    _STD +
    "let SumDone(n: Number, acc: Number): Number = acc;\n"
    "let SumStep(n: Number, acc: Number): Number =\n"
    "    ((n, 1) -> Std.Subtract,\n"
    "     (n, acc) -> Std.Add) -> SumAccum;\n"
    "let SumAccum(n: Number, acc: Number): Number =\n"
    "    ((n, 0) -> Std.LessOrEqual, (n, acc), SumDone, SumStep) -> Std.Branch;\n"
    "let SumTo(n: Number): Number = (n, 0) -> SumAccum;\n"
)


class AccumulatorTests(unittest.TestCase):

    def test_sum_zero(self):
        self.assertEqual(_run(_SUM_ACCUM + "(0) -> SumTo;"), 0)

    def test_sum_one(self):
        self.assertEqual(_run(_SUM_ACCUM + "(1) -> SumTo;"), 1)

    def test_sum_five(self):
        # 1+2+3+4+5 = 15
        self.assertEqual(_run(_SUM_ACCUM + "(5) -> SumTo;"), 15)

    def test_sum_ten(self):
        # n*(n+1)/2 = 55
        self.assertEqual(_run(_SUM_ACCUM + "(10) -> SumTo;"), 55)

    def test_sum_hundred(self):
        self.assertEqual(_run(_SUM_ACCUM + "(100) -> SumTo;"), 5050)


# ---------------------------------------------------------------------------
# Two-argument recursion: GCD (Euclidean algorithm)
#   Gcd(a, b): if b == 0 then a else Gcd(b, a mod b)
# ---------------------------------------------------------------------------

_GCD = (
    _STD +
    "let GcdDone(a: Number, b: Number): Number = a;\n"
    "let GcdStep(a: Number, b: Number): Number =\n"
    "    (b, (a, b) -> Std.Mod) -> Gcd;\n"
    "let Gcd(a: Number, b: Number): Number =\n"
    "    ((b, 0) -> Std.Equal, (a, b), GcdDone, GcdStep) -> Std.Branch;\n"
)


class GcdTests(unittest.TestCase):

    import math as _math

    def _gcd(self, a, b):
        import math
        return math.gcd(int(a), int(b))

    def test_gcd_same(self):
        self.assertEqual(_run(_GCD + "(6, 6) -> Gcd;"), 6)

    def test_gcd_coprime(self):
        self.assertEqual(_run(_GCD + "(7, 13) -> Gcd;"), 1)

    def test_gcd_12_8(self):
        self.assertEqual(_run(_GCD + "(12, 8) -> Gcd;"), 4)

    def test_gcd_48_18(self):
        self.assertEqual(_run(_GCD + "(48, 18) -> Gcd;"), 6)

    def test_gcd_100_75(self):
        self.assertEqual(_run(_GCD + "(100, 75) -> Gcd;"), 25)

    def test_gcd_with_zero(self):
        self.assertEqual(_run(_GCD + "(5, 0) -> Gcd;"), 5)

    def test_gcd_several(self):
        cases = [(12, 8, 4), (100, 75, 25), (17, 5, 1), (36, 24, 12)]
        for a, b, expected in cases:
            result = _run(_GCD + f"({a}, {b}) -> Gcd;")
            self.assertEqual(result, expected, f"Gcd({a},{b}) failed")


# ---------------------------------------------------------------------------
# Integer power: Pow(base, exp) via repeated multiplication
#   Pow(base, 0) = 1
#   Pow(base, n) = base * Pow(base, n-1)
# ---------------------------------------------------------------------------

_POW = (
    _STD + _UTILS +
    "let PowDone(base: Number, exp: Number): Number = 1;\n"
    "let PowStep(base: Number, exp: Number): Number =\n"
    "    (base, (base, (exp, 1) -> Std.Subtract) -> Pow) -> Std.Multiply;\n"
    "let Pow(base: Number, exp: Number): Number =\n"
    "    ((exp, 0) -> Std.LessOrEqual, (base, exp), PowDone, PowStep) -> Std.Branch;\n"
)


class PowerTests(unittest.TestCase):

    def test_pow_zero_exponent(self):
        self.assertEqual(_run(_POW + "(5, 0) -> Pow;"), 1)

    def test_pow_one_exponent(self):
        self.assertEqual(_run(_POW + "(5, 1) -> Pow;"), 5)

    def test_pow_two_squared(self):
        self.assertEqual(_run(_POW + "(2, 2) -> Pow;"), 4)

    def test_pow_two_to_ten(self):
        self.assertEqual(_run(_POW + "(2, 10) -> Pow;"), 1024)

    def test_pow_three_cubed(self):
        self.assertEqual(_run(_POW + "(3, 3) -> Pow;"), 27)

    def test_pow_base_zero(self):
        self.assertEqual(_run(_POW + "(0, 5) -> Pow;"), 0)

    def test_pow_several(self):
        cases = [(2, 8, 256), (3, 4, 81), (10, 3, 1000)]
        for base, exp, expected in cases:
            result = _run(_POW + f"({base}, {exp}) -> Pow;")
            self.assertEqual(result, expected, f"Pow({base},{exp}) failed")


# ---------------------------------------------------------------------------
# Countdown: verify the loop counter decrements correctly
#   CountSteps(n): returns n (counts how many steps were taken down to 0)
#   Acts as a pure sanity check that the recursion depth is correct.
# ---------------------------------------------------------------------------

_COUNTDOWN = (
    _STD + _UTILS +
    "let CountDone(n: Number): Number = 0;\n"
    "let CountStep(n: Number): Number =\n"
    "    (n, 1) -> Std.Subtract -> CountDown -> (_, 1) -> Std.Add;\n"
    "let CountDown(n: Number): Number =\n"
    "    ((n, 0) -> Std.LessOrEqual, n, CountDone, CountStep) -> Std.Branch;\n"
)


class CountdownTests(unittest.TestCase):
    """CountDown(n) returns exactly n, verifying each recursive step fires once."""

    def test_countdown_zero(self):
        self.assertEqual(_run(_COUNTDOWN + "(0) -> CountDown;"), 0)

    def test_countdown_one(self):
        self.assertEqual(_run(_COUNTDOWN + "(1) -> CountDown;"), 1)

    def test_countdown_five(self):
        self.assertEqual(_run(_COUNTDOWN + "(5) -> CountDown;"), 5)

    def test_countdown_fifty(self):
        self.assertEqual(_run(_COUNTDOWN + "(50) -> CountDown;"), 50)


# ---------------------------------------------------------------------------
# IR / parse-level: recursion is syntactically valid
# ---------------------------------------------------------------------------

class RecursionParseTests(unittest.TestCase):
    """Verify recursive definitions parse and lower without errors."""

    def test_self_reference_in_branch_parses(self):
        src = (
            "import Std;\n"
            "let Identity(x: Number): Number = x;\n"
            "let Step(n: Number): Number = (n, 1) -> Std.Subtract -> Loop;\n"
            "let Loop(n: Number): Number =\n"
            "    ((n, 0) -> Std.LessOrEqual, n, Identity, Step) -> Std.Branch;\n"
        )
        prog = lower(parse_program(src))
        names = [s.name for s in prog.statements if hasattr(s, "name")]
        self.assertIn("Identity", names)
        self.assertIn("Step", names)
        self.assertIn("Loop", names)

    def test_mutual_recursion_parses(self):
        """A function can reference another function defined later in the file."""
        src = (
            "import Std;\n"
            "let Identity(x: Number): Number = x;\n"
            "let BStep(n: Number): Number = (n, 1) -> Std.Subtract -> FuncA;\n"
            "let AStep(n: Number): Number = (n, 1) -> Std.Subtract -> FuncB;\n"
            "let FuncA(n: Number): Number =\n"
            "    ((n, 0) -> Std.LessOrEqual, n, Identity, AStep) -> Std.Branch;\n"
            "let FuncB(n: Number): Number =\n"
            "    ((n, 0) -> Std.LessOrEqual, n, Identity, BStep) -> Std.Branch;\n"
        )
        prog = lower(parse_program(src))
        names = [s.name for s in prog.statements if hasattr(s, "name")]
        self.assertIn("FuncA", names)
        self.assertIn("FuncB", names)


if __name__ == "__main__":
    unittest.main()

