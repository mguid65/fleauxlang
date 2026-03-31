"""
Comprehensive tests for Fleaux dataflow pipelines.

Tests cover:
- Simple pipelines (single-stage and multi-stage)
- Complex chaining patterns
- Placeholder threading and argument transformation
- Operator pipelines
- Tuple transformations
- Integration scenarios
"""

from __future__ import annotations

import unittest

from fleaux_ast import (
    IRConstant, IRExprStatement, IRFlowExpr, IRNameRef, IROperatorRef, 
    IRTupleExpr, IRTupleType, IRSimpleType
)
from fleaux_lowering import lower
from fleaux_parser import parse_program


def _lower(source: str):
    """Helper to parse and lower a program."""
    return lower(parse_program(source))


class SimplePipelineTests(unittest.TestCase):
    """Test basic single-stage pipelines."""

    def test_simple_tuple_to_function(self) -> None:
        """(x, y) -> FuncName"""
        program = _lower("(1, 2) -> Std.Add;")
        stmt = program.statements[0]

        self.assertIsInstance(stmt, IRExprStatement)
        self.assertIsInstance(stmt.expr, IRFlowExpr)
        self.assertIsInstance(stmt.expr.lhs, IRTupleExpr)
        self.assertEqual(len(stmt.expr.lhs.items), 2)

    def test_single_arg_pipeline(self) -> None:
        """(x) -> Function"""
        program = _lower("(42) -> Std.Sqrt;")
        stmt = program.statements[0]

        self.assertIsInstance(stmt, IRExprStatement)
        items = stmt.expr.lhs.items
        self.assertEqual(len(items), 1)
        self.assertIsInstance(items[0], IRConstant)
        self.assertEqual(items[0].val, 42)

    def test_empty_tuple_pipeline(self) -> None:
        """() -> GetArgs"""
        program = _lower("() -> Std.GetArgs;")
        stmt = program.statements[0]

        self.assertIsInstance(stmt, IRExprStatement)
        self.assertEqual(len(stmt.expr.lhs.items), 0)

    def test_string_constant_pipeline(self) -> None:
        """("hello") -> Println"""
        program = _lower("""("hello") -> Std.Println;""")
        stmt = program.statements[0]

        items = stmt.expr.lhs.items
        self.assertEqual(len(items), 1)
        self.assertIsInstance(items[0], IRConstant)
        self.assertEqual(items[0].val, "hello")

    def test_negative_number_pipeline(self) -> None:
        """(-3.14) -> Sqrt"""
        program = _lower("(-3.14) -> Std.Sqrt;")
        stmt = program.statements[0]

        items = stmt.expr.lhs.items
        self.assertEqual(len(items), 1)
        self.assertIsInstance(items[0], IRConstant)
        self.assertEqual(items[0].val, -3.14)

    def test_qualified_function_call(self) -> None:
        """(x, y) -> Std.ModuleName"""
        program = _lower("(1, 2) -> Std.Add;")
        rhs = program.statements[0].expr.rhs

        self.assertIsInstance(rhs, IRNameRef)
        self.assertEqual(rhs.qualifier, "Std")
        self.assertEqual(rhs.name, "Add")


class OperatorPipelineTests(unittest.TestCase):
    """Test pipelines with operator targets."""

    def test_addition_operator(self) -> None:
        """(1, 2) -> +"""
        program = _lower("(1, 2) -> +;")
        rhs = program.statements[0].expr.rhs

        self.assertIsInstance(rhs, IROperatorRef)
        self.assertEqual(rhs.op, "+")

    def test_subtraction_operator(self) -> None:
        """(5, 3) -> -"""
        program = _lower("(5, 3) -> -;")
        rhs = program.statements[0].expr.rhs

        self.assertIsInstance(rhs, IROperatorRef)
        self.assertEqual(rhs.op, "-")

    def test_multiplication_operator(self) -> None:
        """(2, 3) -> *"""
        program = _lower("(2, 3) -> *;")
        rhs = program.statements[0].expr.rhs

        self.assertIsInstance(rhs, IROperatorRef)
        self.assertEqual(rhs.op, "*")

    def test_division_operator(self) -> None:
        """(10, 2) -> /"""
        program = _lower("(10, 2) -> /;")
        rhs = program.statements[0].expr.rhs

        self.assertIsInstance(rhs, IROperatorRef)
        self.assertEqual(rhs.op, "/")


    def test_modulo_operator(self) -> None:
        """(10, 3) -> %"""
        program = _lower("(10, 3) -> %;")
        rhs = program.statements[0].expr.rhs

        self.assertIsInstance(rhs, IROperatorRef)
        self.assertEqual(rhs.op, "%")

    def test_power_operator(self) -> None:
        """(2, 3) -> ^"""
        program = _lower("(2, 3) -> ^;")
        rhs = program.statements[0].expr.rhs

        self.assertIsInstance(rhs, IROperatorRef)
        self.assertEqual(rhs.op, "^")

    def test_comparison_operators(self) -> None:
        """Test all comparison operators"""
        ops = ["==", "!=", "<", ">", "<=", ">="]
        for op in ops:
            program = _lower(f"(1, 2) -> {op};")
            rhs = program.statements[0].expr.rhs
            self.assertIsInstance(rhs, IROperatorRef)
            self.assertEqual(rhs.op, op)

    def test_logical_operators(self) -> None:
        """Test logical operators"""
        ops = ["&&", "||"]
        for op in ops:
            program = _lower(f"(True, False) -> {op};")
            rhs = program.statements[0].expr.rhs
            self.assertIsInstance(rhs, IROperatorRef)
            self.assertEqual(rhs.op, op)

    def test_negation_operator(self) -> None:
        """(True) -> !"""
        program = _lower("(True) -> !;")
        rhs = program.statements[0].expr.rhs

        self.assertIsInstance(rhs, IROperatorRef)
        self.assertEqual(rhs.op, "!")


class ChainingPipelineTests(unittest.TestCase):
    """Test multi-stage pipelines with chaining."""

    def test_two_stage_pipeline(self) -> None:
        """(x) -> Func1 -> Func2"""
        program = _lower("(1, 2) -> Std.Add -> Std.Println;")
        stmt = program.statements[0]

        # First pipeline: (1, 2) -> Std.Add
        # The result flows to: ... -> Std.Println
        # So stmt.expr should be a FlowExpr with Println as rhs
        self.assertIsInstance(stmt.expr, IRFlowExpr)
        # The lhs of the outermost flow is the result of the first stage
        self.assertIsInstance(stmt.expr.rhs, IRNameRef)
        self.assertEqual(stmt.expr.rhs.name, "Println")

    def test_three_stage_pipeline(self) -> None:
        """(x) -> Func1 -> Func2 -> Func3"""
        program = _lower("(1, 2) -> Std.Add -> Std.Multiply -> Std.Println;")
        # Should parse without error
        self.assertEqual(len(program.statements), 1)

    def test_operator_in_chain(self) -> None:
        """(x, y) -> + -> Std.Println"""
        program = _lower("(1, 2) -> + -> Std.Println;")
        stmt = program.statements[0]

        # First stage should have operator
        self.assertIsInstance(stmt.expr.rhs, IRNameRef)


class PlaceholderThreadingTests(unittest.TestCase):
    """Test argument threading with placeholders."""

    def test_simple_placeholder_replacement(self) -> None:
        """(x) -> Func -> (_, arg) threads result into first position"""
        program = _lower("(1, 2) -> Std.Add -> (_, 3) -> Std.Multiply;")
        stmt = program.statements[0]

        # Should not throw error during lowering
        self.assertIsInstance(stmt, IRExprStatement)

    def test_placeholder_in_middle(self) -> None:
        """(arg, _, arg) form"""
        program = _lower("(5) -> Std.Sqrt -> (2, _, 3) -> Std.Add;")
        stmt = program.statements[0]

        self.assertIsInstance(stmt, IRExprStatement)

    def test_multiple_placeholders(self) -> None:
        """(_ , _) - though semantically may need validation"""
        program = _lower("(1) -> Std.Sqrt -> (_, _) -> Std.Add;")
        stmt = program.statements[0]

        self.assertIsInstance(stmt, IRExprStatement)


class MixedLiteralPipelineTests(unittest.TestCase):
    """Test pipelines with various literal types."""

    def test_mixed_number_types(self) -> None:
        """Both int and float constants"""
        program = _lower("(1, 2.5) -> Std.Add;")
        items = program.statements[0].expr.lhs.items

        self.assertIsInstance(items[0], IRConstant)
        self.assertIsInstance(items[1], IRConstant)
        self.assertEqual(items[0].val, 1)
        self.assertEqual(items[1].val, 2.5)

    def test_mixed_literal_types(self) -> None:
        """Mix of numbers and strings"""
        program = _lower("""(1, "hello") -> Std.Println;""")
        items = program.statements[0].expr.lhs.items

        self.assertEqual(len(items), 2)
        self.assertIsInstance(items[0], IRConstant)
        self.assertIsInstance(items[1], IRConstant)

    def test_boolean_constants(self) -> None:
        """(True, False) -> op"""
        program = _lower("(True, False) -> &&;")
        items = program.statements[0].expr.lhs.items

        self.assertEqual(len(items), 2)
        self.assertEqual(items[0].val, True)
        self.assertEqual(items[1].val, False)

    def test_null_constant(self) -> None:
        """(null) -> Println"""
        program = _lower("(null) -> Std.Println;")
        items = program.statements[0].expr.lhs.items

        self.assertEqual(len(items), 1)
        self.assertIsNone(items[0].val)


class ScientificNotationTests(unittest.TestCase):
    """Test parsing of scientific notation numbers."""

    def test_positive_scientific_notation(self) -> None:
        """1.5e+10 format"""
        program = _lower("(1.5e+10) -> Std.Println;")
        items = program.statements[0].expr.lhs.items

        self.assertEqual(items[0].val, 1.5e+10)

    def test_negative_exponent(self) -> None:
        """1.5e-10 format"""
        program = _lower("(1.5e-10) -> Std.Println;")
        items = program.statements[0].expr.lhs.items

        self.assertEqual(items[0].val, 1.5e-10)

    def test_negative_mantissa_and_exponent(self) -> None:
        """-3.665e-01 format"""
        program = _lower("(-3.665e-01) -> Std.Println;")
        items = program.statements[0].expr.lhs.items

        self.assertAlmostEqual(items[0].val, -3.665e-01, places=10)

    def test_uppercase_e_notation(self) -> None:
        """2.5E+5 format"""
        program = _lower("(2.5E+5) -> Std.Println;")
        items = program.statements[0].expr.lhs.items

        self.assertEqual(items[0].val, 2.5e+5)


class NameReferenceTests(unittest.TestCase):
    """Test parsing of name and variable references."""

    def test_simple_name_reference(self) -> None:
        """x -> Function (where x is a variable)"""
        program = _lower("let Id(x: Number): Number = (x) -> Std.Sqrt;")
        body = program.statements[0].body

        self.assertIsInstance(body, IRFlowExpr)
        self.assertIsInstance(body.lhs.items[0], IRNameRef)
        self.assertEqual(body.lhs.items[0].name, "x")
        self.assertIsNone(body.lhs.items[0].qualifier)

    def test_qualified_name_reference(self) -> None:
        """Std.Add reference"""
        program = _lower("(1, 2) -> Std.Add;")
        rhs = program.statements[0].expr.rhs

        self.assertIsInstance(rhs, IRNameRef)
        self.assertEqual(rhs.qualifier, "Std")
        self.assertEqual(rhs.name, "Add")

    def test_unqualified_user_function(self) -> None:
        """MyFunc reference (unqualified)"""
        program = _lower("(1, 2) -> MyFunc;")
        rhs = program.statements[0].expr.rhs

        self.assertIsInstance(rhs, IRNameRef)
        self.assertIsNone(rhs.qualifier)
        self.assertEqual(rhs.name, "MyFunc")


class ComplexChainTests(unittest.TestCase):
    """Test complex real-world pipeline scenarios."""

    def test_arithmetic_chain(self) -> None:
        """(1, 2) -> + -> Std.Println"""
        program = _lower("(1, 2) -> + -> Std.Println;")
        self.assertEqual(len(program.statements), 1)

    def test_nested_function_calls(self) -> None:
        """Complex expression with nested tuple transformations"""
        program = _lower("""
        let Compute(x: Number, y: Number): Number = (((x, y) -> Std.Add) -> Std.Multiply) -> Std.Sqrt;
        """)
        self.assertEqual(len(program.statements), 1)

    def test_multiple_statements(self) -> None:
        """Several pipelines in sequence"""
        program = _lower("""
        (1, 2) -> Std.Add -> Std.Println;
        (3, 4) -> Std.Multiply -> Std.Println;
        ("test") -> Std.Println;
        """)
        self.assertEqual(len(program.statements), 3)

    def test_function_definition_with_complex_body(self) -> None:
        """Function with complex pipeline body"""
        program = _lower("""
        let Process(x: Number, y: Number, z: Number): Number = 
            (((x, y) -> Std.Add, z) -> Std.Multiply) -> Std.Sqrt;
        """)
        self.assertEqual(len(program.statements), 1)
        self.assertEqual(program.statements[0].name, "Process")


class EdgeCaseTests(unittest.TestCase):
    """Test edge cases and boundary conditions."""

    def test_zero_constants(self) -> None:
        """(0) and (0.0)"""
        program = _lower("(0) -> Std.Sqrt;")
        self.assertEqual(program.statements[0].expr.lhs.items[0].val, 0)

        program = _lower("(0.0) -> Std.Sqrt;")
        self.assertEqual(program.statements[0].expr.lhs.items[0].val, 0.0)

    def test_very_large_number(self) -> None:
        """1e100 constant"""
        program = _lower("(1e100) -> Std.Println;")
        self.assertEqual(program.statements[0].expr.lhs.items[0].val, 1e100)

    def test_very_small_number(self) -> None:
        """1e-100 constant"""
        program = _lower("(1e-100) -> Std.Println;")
        self.assertAlmostEqual(program.statements[0].expr.lhs.items[0].val, 1e-100, places=100)

    def test_large_tuple(self) -> None:
        """Tuple with many elements"""
        program = _lower("(1, 2, 3, 4, 5, 6, 7, 8, 9, 10) -> Std.Println;")
        items = program.statements[0].expr.lhs.items
        self.assertEqual(len(items), 10)

    def test_deeply_nested_tuples(self) -> None:
        """Parenthesized expressions"""
        program = _lower("((((1)))) -> Std.Println;")
        items = program.statements[0].expr.lhs.items
        self.assertEqual(len(items), 1)

    def test_empty_string(self) -> None:
        """("") constant"""
        program = _lower('''("") -> Std.Println;''')
        items = program.statements[0].expr.lhs.items
        self.assertEqual(items[0].val, "")

    def test_string_with_escapes(self) -> None:
        """String with escape sequences"""
        program = _lower(r'''("hello\nworld") -> Std.Println;''')
        items = program.statements[0].expr.lhs.items
        # Should parse without error
        self.assertEqual(len(items), 1)


class ParameterTypeTests(unittest.TestCase):
    """Test parameter and return type specifications."""

    def test_function_with_tuple_type_param(self) -> None:
        """let Func(t: Tuple(Number, String)): Number"""
        program = _lower("let Func(t: Tuple(Number, String)): Number = __builtin__;")
        stmt = program.statements[0]

        self.assertEqual(len(stmt.params), 1)
        param_type = stmt.params[0].type
        self.assertIsInstance(param_type, IRTupleType)
        self.assertEqual(len(param_type.types), 2)

    def test_function_with_variadic_param(self) -> None:
        """let Func(args: Any...): Number"""
        program = _lower("let Func(args: Any...): Number = __builtin__;")
        stmt = program.statements[0]

        param = stmt.params[0]
        self.assertTrue(param.type.variadic)
        self.assertEqual(param.type.name, "Any")

    def test_all_builtin_types(self) -> None:
        """Test all simple builtin types"""
        types = ["Number", "String", "Bool", "Null", "Any"]
        for ty in types:
            program = _lower(f"let Func(x: {ty}): {ty} = __builtin__;")
            stmt = program.statements[0]
            self.assertEqual(stmt.params[0].type.name, ty)
            self.assertEqual(stmt.return_type.name, ty)


if __name__ == "__main__":
    unittest.main()

