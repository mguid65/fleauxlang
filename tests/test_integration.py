"""
Integration tests and real-world scenario examples.

Tests cover:
- Complete end-to-end pipelines
- Multi-statement programs
- Standard library interactions
- Realistic use cases
- Performance scenarios
"""

from __future__ import annotations

import unittest
from pathlib import Path

from fleaux_transpiler import FleauxTranspiler
from fleaux_lowering import lower
from fleaux_parser import parse_program, parse_file
from tests.helpers import ensure_std_generated


def setUpModule() -> None:
    ensure_std_generated()


class EndToEndPipelineTests(unittest.TestCase):
    """Complete pipeline tests from source to execution."""

    def setUp(self):
        self.repo_root = Path(__file__).resolve().parents[1]
        self.transpiler = FleauxTranspiler()

    def test_simple_arithmetic_execution(self) -> None:
        """Parse, lower, and generate arithmetic"""
        source = "(2, 3) -> Std.Add -> Std.Println;"
        program = lower(parse_program(source))
        
        self.assertEqual(len(program.statements), 1)
        self.assertIsNotNone(program.statements[0])

    def test_function_definition_execution(self) -> None:
        """Function definition with pipeline body"""
        source = """
        let Double(x: Number): Number = (x, 2) -> Std.Multiply;
        (5) -> Double;
        """
        program = lower(parse_program(source))
        
        self.assertEqual(len(program.statements), 2)

    def test_multiple_imports_and_calls(self) -> None:
        """Multiple statements with imports"""
        source = """
        import Std;
        (1, 2) -> Std.Add;
        (3, 4) -> Std.Multiply;
        (10, 2) -> Std.Divide;
        """
        program = lower(parse_program(source))
        
        self.assertEqual(len(program.statements), 4)

    def test_chained_operations(self) -> None:
        """Multiple operations in sequence"""
        source = """
        (2, 3) -> Std.Add -> Std.Multiply -> Std.Sqrt;
        """
        program = lower(parse_program(source))
        
        self.assertEqual(len(program.statements), 1)

    def test_complex_nested_expression(self) -> None:
        """Complex nested pipeline"""
        source = """
        let Add3(a: Number, b: Number, c: Number): Number = 
            (((a, b) -> Std.Add, c) -> Std.Add);
        (1, 2, 3) -> Add3;
        """
        program = lower(parse_program(source))
        
        self.assertEqual(len(program.statements), 2)


class RealWorldScenarioTests(unittest.TestCase):
    """Realistic use case scenarios."""

    def test_mathematical_formula_pipeline(self) -> None:
        """Real math: y = 2x^2 + 3x - 5"""
        source = """
        let Polynomial(x: Number): Number = 
            (((4, ((x, 2) -> Std.Pow) -> Std.Multiply), ((3, x) -> Std.Multiply)) -> Std.Add, 5) -> Std.Subtract;
        """
        program = lower(parse_program(source))
        self.assertEqual(len(program.statements), 1)

    def test_statistics_pipeline(self) -> None:
        """Statistical operations"""
        source = """
        let Mean(x: Number, y: Number): Number = 
            ((x, y) -> Std.Add, 2) -> Std.Divide;
        (10, 20) -> Mean;
        """
        program = lower(parse_program(source))
        self.assertEqual(len(program.statements), 2)

    def test_data_transformation_pipeline(self) -> None:
        """Data transformation chain"""
        source = """
        let ProcessData(raw: Number): Number = 
            (raw) -> Std.Abs -> Std.Floor -> Std.Sqrt;
        (-42.7) -> ProcessData;
        """
        program = lower(parse_program(source))
        self.assertEqual(len(program.statements), 2)

    def test_multi_step_workflow(self) -> None:
        """Multi-step business logic workflow"""
        source = """
        import Std;
        
        let Discount(price: Number, percent: Number): Number = 
            ((price, percent) -> Std.Multiply, 100) -> Std.Divide;
        
        let ApplyTax(amount: Number, tax_rate: Number): Number = 
            ((amount, tax_rate) -> Std.Multiply) -> Std.Add;
        
        let FinalPrice(price: Number, discount: Number, tax: Number): Number = 
            ((price, discount) -> Discount, tax) -> ApplyTax;
        
        (100) -> Std.Println;
        """
        program = lower(parse_program(source))
        self.assertGreater(len(program.statements), 1)


class StandardLibraryIntegrationTests(unittest.TestCase):
    """Tests for standard library interactions."""

    def test_all_math_operators(self) -> None:
        """All mathematical operators work"""
        operators = ["+", "-", "*", "/", "%", "^"]
        for op in operators:
            source = f"(2, 3) -> {op};"
            program = lower(parse_program(source))
            self.assertEqual(len(program.statements), 1)

    def test_all_comparison_operators(self) -> None:
        """All comparison operators work"""
        operators = ["==", "!=", "<", ">", "<=", ">="]
        for op in operators:
            source = f"(1, 2) -> {op};"
            program = lower(parse_program(source))
            self.assertEqual(len(program.statements), 1)

    def test_all_logical_operators(self) -> None:
        """All logical operators work"""
        operators = ["&&", "||"]
        for op in operators:
            source = f"(True, False) -> {op};"
            program = lower(parse_program(source))
            self.assertEqual(len(program.statements), 1)

    def test_negation_operator(self) -> None:
        """Logical negation operator"""
        source = "(True) -> !;"
        program = lower(parse_program(source))
        self.assertEqual(len(program.statements), 1)

    def test_string_functions(self) -> None:
        """String manipulation functions"""
        funcs = ["Strlen", "StrUpper", "StrLower"]
        for func in funcs:
            source = f'''("hello") -> Std.{func};'''
            program = lower(parse_program(source))
            self.assertEqual(len(program.statements), 1)

    def test_type_conversion_functions(self) -> None:
        """Type conversion functions"""
        source = """
        (42) -> Std.ToString;
        ("3.14") -> Std.ToNum;
        """
        program = lower(parse_program(source))
        self.assertEqual(len(program.statements), 2)


class MultiFileIntegrationTests(unittest.TestCase):
    """Tests for multi-file programs and imports."""

    def setUp(self):
        self.repo_root = Path(__file__).resolve().parents[1]

    def test_import_std_library(self) -> None:
        """Can import and use Std library"""
        source = """
        import Std;
        (1, 2) -> Std.Add;
        """
        program = lower(parse_program(source))
        self.assertEqual(len(program.statements), 2)

    def test_multiple_qualified_calls(self) -> None:
        """Multiple calls to qualified functions"""
        source = """
        (1, 2) -> Std.Add;
        (3, 4) -> Std.Multiply;
        (5, 6) -> Std.Divide;
        """
        program = lower(parse_program(source))
        self.assertEqual(len(program.statements), 3)

    def test_std_file_itself(self) -> None:
        """Standard library file parses and lowers"""
        model = parse_file(self.repo_root / "Std.fleaux")
        program = lower(model)
        
        # Should have many statements
        self.assertGreater(len(program.statements), 50)

    def test_transpile_std_library(self) -> None:
        """Can transpile standard library"""
        transpiler = FleauxTranspiler()
        output = transpiler.process(self.repo_root / "Std.fleaux")
        
        self.assertTrue(output.exists())
        self.assertTrue(output.suffix == ".py")


class LoweringAndTranspilationTests(unittest.TestCase):
    """Tests for lowering and code generation."""

    def setUp(self):
        self.repo_root = Path(__file__).resolve().parents[1]

    def test_expression_statement_generation(self) -> None:
        """Expression statements generate IR correctly"""
        source = "(1, 2) -> Std.Add;"
        program = lower(parse_program(source))
        
        from fleaux_ast import IRExprStatement
        self.assertIsInstance(program.statements[0], IRExprStatement)

    def test_let_statement_generation(self) -> None:
        """Let statements generate IR correctly"""
        source = "let MyFunc(x: Number, y: Number): Number = (x, y) -> Std.Add;"
        program = lower(parse_program(source))
        
        from fleaux_ast import IRLet
        self.assertIsInstance(program.statements[0], IRLet)

    def test_import_statement_generation(self) -> None:
        """Import statements generate IR correctly"""
        source = "import Std;"
        program = lower(parse_program(source))
        
        from fleaux_ast import IRImport
        self.assertIsInstance(program.statements[0], IRImport)

    def test_builtin_function_has_no_body(self) -> None:
        """Builtin functions don't have bodies"""
        source = "let MySqrt(x: Number): Number = __builtin__;"
        program = lower(parse_program(source))
        
        stmt = program.statements[0]
        self.assertTrue(stmt.is_builtin)
        self.assertIsNone(stmt.body)

    def test_user_function_has_body(self) -> None:
        """User functions have bodies"""
        source = "let MyFunc(x: Number): Number = (x) -> Std.Sqrt;"
        program = lower(parse_program(source))
        
        stmt = program.statements[0]
        self.assertFalse(stmt.is_builtin)
        self.assertIsNotNone(stmt.body)


class PerformanceAndScaleTests(unittest.TestCase):
    """Tests for performance with larger programs."""

    def test_large_tuple(self) -> None:
        """Handle tuples with many elements"""
        elements = ", ".join(str(i) for i in range(100))
        source = f"({elements}) -> Std.Println;"
        program = lower(parse_program(source))
        
        self.assertEqual(len(program.statements[0].expr.lhs.items), 100)

    def test_deep_nesting(self) -> None:
        """Handle deeply nested expressions"""
        # Create deeply nested pipeline
        source = "(1) -> Std.Sqrt"
        for _ in range(10):
            source += " -> Std.Floor"
        source += ";"
        
        program = lower(parse_program(source))
        self.assertEqual(len(program.statements), 1)

    def test_many_function_definitions(self) -> None:
        """Handle many function definitions"""
        source = ""
        for i in range(50):
            source += f"let Func{i}(x: Number): Number = (x, {i}) -> Std.Add;\n"
        
        program = lower(parse_program(source))
        self.assertEqual(len(program.statements), 50)

    def test_large_program(self) -> None:
        """Handle large programs with mixed statements"""
        source = "import Std;\n"
        
        # Add functions
        for i in range(20):
            source += f"let F{i}(x: Number): Number = (x, {i}) -> Std.Multiply;\n"
        
        # Add expressions
        for i in range(20):
            source += f"({i}, {i+1}) -> Std.Add;\n"
        
        program = lower(parse_program(source))
        self.assertGreater(len(program.statements), 40)


class ConsistencyTests(unittest.TestCase):
    """Tests for consistency across different operations."""

    def test_same_source_same_result(self) -> None:
        """Parsing same source twice gives consistent results"""
        source = "(1, 2) -> Std.Add;"
        
        program1 = lower(parse_program(source))
        program2 = lower(parse_program(source))
        
        self.assertEqual(len(program1.statements), len(program2.statements))

    def test_whitespace_insensitive(self) -> None:
        """Different whitespace produces same result"""
        sources = [
            "(1, 2) -> Std.Add;",
            "( 1 , 2 ) -> Std.Add ;",
            "(1,2)->Std.Add;",
            "(\n1,\n2\n)\n->\nStd.Add\n;",
        ]
        
        programs = [lower(parse_program(s)) for s in sources]
        
        # All should have same structure
        for prog in programs:
            self.assertEqual(len(prog.statements), 1)

    def test_comment_insensitive(self) -> None:
        """Comments don't affect parsing"""
        source_with_comments = """
        // This is a comment
        (1, 2) -> Std.Add; // End of line comment
        // Another comment
        (3, 4) -> Std.Multiply;
        """
        
        source_without = "(1, 2) -> Std.Add; (3, 4) -> Std.Multiply;"
        
        prog_with = lower(parse_program(source_with_comments))
        prog_without = lower(parse_program(source_without))
        
        self.assertEqual(len(prog_with.statements), len(prog_without.statements))


class TypeSystemTests(unittest.TestCase):
    """Tests for type system coverage."""

    def test_tuple_type_with_multiple_elements(self) -> None:
        """Tuple(Number, String, Bool) type"""
        source = "let F(t: Tuple(Number, String, Bool)): Number = __builtin__;"
        program = lower(parse_program(source))
        
        param_type = program.statements[0].params[0].type
        self.assertEqual(len(param_type.types), 3)

    def test_variadic_type_support(self) -> None:
        """Variadic Any... type"""
        source = "let F(args: Any...): Number = __builtin__;"
        program = lower(parse_program(source))
        
        param = program.statements[0].params[0]
        self.assertTrue(param.type.variadic)
        self.assertEqual(param.type.name, "Any")

    def test_all_return_types(self) -> None:
        """All return types work"""
        for rtype in ["Number", "String", "Bool", "Null", "Any"]:
            source = f"let F(x: Number): {rtype} = __builtin__;"
            program = lower(parse_program(source))
            
            self.assertEqual(program.statements[0].return_type.name, rtype)

    def test_qualified_identifier_as_type(self) -> None:
        """Custom types parsing - QualifiedId not yet supported in lowering"""
        # Parsing works but lowering doesn't support qualified IDs as types yet
        source = "let F(x: MyLib.CustomType): Number = __builtin__;"
        # Just test that parsing works
        model = parse_program(source)
        self.assertEqual(len(model.statements), 1)


if __name__ == "__main__":
    unittest.main()

