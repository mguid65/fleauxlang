from __future__ import annotations

import unittest

from fleaux_ast import (
    IRConstant, IRExprStatement, IRFlowExpr, IRImport, IRLet,
    IRNameRef, IROperatorRef, IRSimpleType, IRTupleExpr, IRTupleType,
)
from fleaux_lowering import lower
from fleaux_parser import parse_program


def _lower(source: str):
    return lower(parse_program(source))


class IRLoweringImportTests(unittest.TestCase):
    def test_lowers_import_to_IRImport(self) -> None:
        program = _lower("import Std;")

        self.assertEqual(len(program.statements), 1)
        stmt = program.statements[0]
        self.assertIsInstance(stmt, IRImport)
        self.assertEqual(stmt.module_name, "Std")

    def test_lowers_digit_prefixed_import_to_IRImport(self) -> None:
        program = _lower("import 20_export;")

        self.assertEqual(len(program.statements), 1)
        stmt = program.statements[0]
        self.assertIsInstance(stmt, IRImport)
        self.assertEqual(stmt.module_name, "20_export")


class IRLoweringLetTests(unittest.TestCase):
    def test_simple_let_name_and_params(self) -> None:
        program = _lower("let Add(x: Number, y: Number): Number = (x, y) -> Std.Add;")

        stmt = program.statements[0]
        self.assertIsInstance(stmt, IRLet)
        self.assertIsNone(stmt.qualifier)
        self.assertEqual(stmt.name, "Add")
        self.assertEqual(len(stmt.params), 2)
        self.assertEqual(stmt.params[0].name, "x")
        self.assertEqual(stmt.params[1].name, "y")
        self.assertFalse(stmt.is_builtin)
        self.assertIsNotNone(stmt.body)

    def test_qualified_let_name(self) -> None:
        program = _lower("let Std.Add(x: Number, y: Number): Number = __builtin__;")

        stmt = program.statements[0]
        self.assertIsInstance(stmt, IRLet)
        self.assertEqual(stmt.qualifier, "Std")
        self.assertEqual(stmt.name, "Add")

    def test_builtin_let_has_no_body(self) -> None:
        program = _lower("let Sqrt(x: Number): Number = __builtin__;")

        stmt = program.statements[0]
        self.assertIsInstance(stmt, IRLet)
        self.assertTrue(stmt.is_builtin)
        self.assertIsNone(stmt.body)

    def test_zero_param_let(self) -> None:
        program = _lower("let Pi(): Number = __builtin__;")

        stmt = program.statements[0]
        self.assertIsInstance(stmt, IRLet)
        self.assertEqual(stmt.params, [])

    def test_return_type_is_simple(self) -> None:
        program = _lower("let Foo(x: Number): Number = __builtin__;")

        stmt = program.statements[0]
        self.assertIsInstance(stmt.return_type, IRSimpleType)
        self.assertEqual(stmt.return_type.name, "Number")


class IRLoweringTypeTests(unittest.TestCase):
    def test_simple_type(self) -> None:
        program = _lower("let Foo(x: Number): Number = __builtin__;")
        t = program.statements[0].params[0].type

        self.assertIsInstance(t, IRSimpleType)
        self.assertEqual(t.name, "Number")
        self.assertFalse(t.variadic)

    def test_variadic_type(self) -> None:
        program = _lower("let Foo(x: Any...): Number = __builtin__;")
        t = program.statements[0].params[0].type

        self.assertIsInstance(t, IRSimpleType)
        self.assertEqual(t.name, "Any")
        self.assertTrue(t.variadic)

    def test_tuple_type(self) -> None:
        program = _lower("let Foo(t: Tuple(Number, String)): Number = __builtin__;")
        t = program.statements[0].params[0].type

        self.assertIsInstance(t, IRTupleType)
        self.assertEqual(len(t.types), 2)
        self.assertEqual(t.types[0].name, "Number")
        self.assertEqual(t.types[1].name, "String")


class IRLoweringExpressionTests(unittest.TestCase):
    def test_ir_nodes_preserve_source_spans(self) -> None:
        program = _lower("let Id(x: Number): Number = (x) -> Std.Sqrt;")
        stmt = program.statements[0]

        self.assertIsNotNone(program.span)
        self.assertIsNotNone(stmt.span)
        self.assertIsNotNone(stmt.params[0].span)
        self.assertIsNotNone(stmt.body.span)
        self.assertIsNotNone(stmt.body.lhs.span)
        self.assertIsNotNone(stmt.body.rhs.span)

    def test_flow_expr_structure(self) -> None:
        program = _lower("let Id(x: Number): Number = (x) -> Std.Sqrt;")
        body = program.statements[0].body

        self.assertIsInstance(body, IRFlowExpr)
        self.assertIsInstance(body.lhs, IRTupleExpr)
        self.assertEqual(len(body.lhs.items), 1)
        self.assertIsInstance(body.lhs.items[0], IRNameRef)
        self.assertEqual(body.lhs.items[0].name, "x")
        self.assertIsNone(body.lhs.items[0].qualifier)

    def test_flow_expr_qualified_rhs(self) -> None:
        program = _lower("let Id(x: Number): Number = (x) -> Std.Sqrt;")
        rhs = program.statements[0].body.rhs

        self.assertIsInstance(rhs, IRNameRef)
        self.assertEqual(rhs.qualifier, "Std")
        self.assertEqual(rhs.name, "Sqrt")

    def test_flow_expr_nested_qualified_rhs(self) -> None:
        program = _lower("let Id(path: String): String = (path, \"x\") -> Std.Path.Join;")
        rhs = program.statements[0].body.rhs

        self.assertIsInstance(rhs, IRNameRef)
        self.assertEqual(rhs.qualifier, "Std.Path")
        self.assertEqual(rhs.name, "Join")

    def test_operator_call_target(self) -> None:
        program = _lower("(1, 2) -> +;")
        stmt = program.statements[0]

        self.assertIsInstance(stmt, IRExprStatement)
        self.assertIsInstance(stmt.expr, IRFlowExpr)
        self.assertIsInstance(stmt.expr.rhs, IROperatorRef)
        self.assertEqual(stmt.expr.rhs.op, "+")

    def test_constant_int_in_tuple(self) -> None:
        program = _lower("(42) -> Std.Println;")
        items = program.statements[0].expr.lhs.items

        self.assertEqual(len(items), 1)
        self.assertIsInstance(items[0], IRConstant)
        self.assertEqual(items[0].val, 42)

    def test_multi_item_tuple_lhs(self) -> None:
        program = _lower("(1, 2, 3) -> Std.Println;")
        items = program.statements[0].expr.lhs.items

        self.assertEqual(len(items), 3)
        self.assertIsInstance(items[0], IRConstant)
        self.assertIsInstance(items[1], IRConstant)
        self.assertIsInstance(items[2], IRConstant)

    def test_expr_statement_is_IRExprStatement(self) -> None:
        program = _lower("(1, 2) -> Std.Add;")

        self.assertIsInstance(program.statements[0], IRExprStatement)

    def test_empty_tuple_lhs_lowers_to_empty_IRTupleExpr(self) -> None:
        program = _lower("() -> Std.GetArgs;")
        flow = program.statements[0].expr

        self.assertIsInstance(flow, IRFlowExpr)
        self.assertIsInstance(flow.lhs, IRTupleExpr)
        self.assertEqual(flow.lhs.items, [])

    def test_tuple_template_stage_threads_previous_value(self) -> None:
        program = _lower("(10, 20) -> Std.Add -> (_, 2) -> Std.Divide;")
        stmt = program.statements[0]

        self.assertIsInstance(stmt, IRExprStatement)
        self.assertIsInstance(stmt.expr, IRFlowExpr)
        self.assertIsInstance(stmt.expr.rhs, IRNameRef)
        self.assertEqual(stmt.expr.rhs.qualifier, "Std")
        self.assertEqual(stmt.expr.rhs.name, "Divide")

        lhs = stmt.expr.lhs
        self.assertIsInstance(lhs, IRTupleExpr)
        self.assertEqual(len(lhs.items), 2)
        self.assertIsInstance(lhs.items[0], IRFlowExpr)
        self.assertIsInstance(lhs.items[1], IRConstant)
        self.assertEqual(lhs.items[1].val, 2)


if __name__ == "__main__":
    unittest.main()

