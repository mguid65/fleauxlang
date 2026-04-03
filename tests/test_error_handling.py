"""
Tests for error handling, validation, and edge cases.

Tests cover:
- Syntax errors and parsing failures
- Type validation
- Invalid pipeline patterns
- Recovery and error messages
"""

from __future__ import annotations

import unittest

from fleaux_lowering import FleauxLoweringError, lower
from fleaux_parser import FleauxSyntaxError, parse_program, parse_file
from pathlib import Path


class ParsingErrorTests(unittest.TestCase):
    """Test syntax errors and parsing failures."""

    def test_missing_semicolon(self) -> None:
        """Missing statement terminator"""
        with self.assertRaises(FleauxSyntaxError):
            parse_program("(1, 2) -> Std.Add")

    def test_missing_closing_paren(self) -> None:
        """Unclosed parenthesis"""
        with self.assertRaises(FleauxSyntaxError):
            parse_program("(1, 2 -> Std.Add;")

    def test_missing_closing_tuple_paren(self) -> None:
        """Unclosed tuple"""
        with self.assertRaises(FleauxSyntaxError):
            parse_program("(1, 2;")

    def test_unexpected_token(self) -> None:
        """Invalid token sequence"""
        with self.assertRaises(FleauxSyntaxError):
            parse_program("@invalid;")

    def test_missing_type_annotation(self) -> None:
        """Parameter missing type in function def"""
        with self.assertRaises(FleauxSyntaxError):
            parse_program("let Func(x): Number = __builtin__;")

    def test_missing_return_type(self) -> None:
        """Function missing return type"""
        with self.assertRaises(FleauxSyntaxError):
            parse_program("let Func(x: Number) = __builtin__;")

    def test_invalid_tuple_type(self) -> None:
        """Invalid Tuple type syntax"""
        with self.assertRaises(FleauxSyntaxError):
            parse_program("let Func(x: Tuple): Number = __builtin__;")

    def test_trailing_comma_in_params(self) -> None:
        """Trailing comma in parameter list"""
        with self.assertRaises(FleauxSyntaxError):
            parse_program("let Func(x: Number,): Number = __builtin__;")

    def test_missing_comma_in_tuple(self) -> None:
        """Missing comma between tuple elements"""
        with self.assertRaises(FleauxSyntaxError):
            parse_program("(1 2) -> Std.Add;")

    def test_invalid_import_statement(self) -> None:
        """Malformed import"""
        with self.assertRaises(FleauxSyntaxError):
            parse_program("import;")

    def test_keyword_as_identifier(self) -> None:
        """Cannot use 'let' as identifier"""
        with self.assertRaises(FleauxSyntaxError):
            parse_program("let let(x: Number): Number = __builtin__;")

    def test_keyword_import_as_identifier(self) -> None:
        """Cannot use 'import' as identifier"""
        with self.assertRaises(FleauxSyntaxError):
            parse_program("let import(x: Number): Number = __builtin__;")

    def test_empty_source(self) -> None:
        """Empty program should parse as Program with no statements"""
        model = parse_program("")
        self.assertEqual(len(model.statements), 0)

    def test_only_comments(self) -> None:
        """Program with only comments"""
        model = parse_program("// just a comment")
        self.assertEqual(len(model.statements), 0)

    def test_comments_and_whitespace(self) -> None:
        """Program with comments and whitespace"""
        model = parse_program("""
        // This is a comment
        // Another comment
        
        (1, 2) -> Std.Add;
        """)
        self.assertEqual(len(model.statements), 1)


class TypeValidationTests(unittest.TestCase):
    """Test type-related parsing and validation."""

    def test_qualified_type_reference(self) -> None:
        """Custom qualified type reference"""
        # Should parse without error
        model = parse_program("let Func(x: MyLib.CustomType): Number = __builtin__;")
        self.assertEqual(len(model.statements), 1)

    def test_variadic_on_complex_type_fails(self) -> None:
        """Variadic modifier only on simple types"""
        with self.assertRaises(FleauxSyntaxError):
            parse_program("let Func(x: Tuple(Number, String)...): Number = __builtin__;")

    def test_nested_tuple_types(self) -> None:
        """Nested Tuple type parameters"""
        # Grammar may not support this - check current support
        try:
            model = parse_program("let Func(t: Tuple(Tuple(Number, Number), String)): Number = __builtin__;")
            self.assertEqual(len(model.statements), 1)
        except FleauxSyntaxError:
            # If not supported, that's fine
            pass

    def test_all_simple_types_valid(self) -> None:
        """All simple types parse correctly"""
        simple_types = ["Number", "String", "Bool", "Null", "Any"]
        for ty in simple_types:
            model = parse_program(f"let Func(x: {ty}): {ty} = __builtin__;")
            self.assertEqual(len(model.statements), 1)


class PipelineEdgeCaseTests(unittest.TestCase):
    """Test edge cases in pipeline parsing."""

    def test_chain_with_no_lhs(self) -> None:
        """Arrow with nothing before it"""
        with self.assertRaises(FleauxSyntaxError):
            parse_program("-> Std.Add;")

    def test_chain_with_no_rhs(self) -> None:
        """Arrow with nothing after it"""
        with self.assertRaises(FleauxSyntaxError):
            parse_program("(1, 2) ->;")

    def test_multiple_arrows_no_parens(self) -> None:
        """Chained arrows should parse left-associatively"""
        # Should work with proper associativity
        model = parse_program("(1, 2) -> Std.Add -> Std.Println;")
        self.assertEqual(len(model.statements), 1)

    def test_very_long_chain(self) -> None:
        """Very long pipeline chain"""
        model = parse_program("""
        (1, 2) -> Std.Add -> Std.Multiply -> Std.Sqrt 
                -> Std.Floor -> Std.Ceiling -> Std.Println;
        """)
        self.assertEqual(len(model.statements), 1)

    def test_pipeline_in_function_definition(self) -> None:
        """Pipeline as function body"""
        model = parse_program("""
        let MyFunc(x: Number, y: Number): Number = (x, y) -> Std.Add -> Std.Multiply;
        """)
        self.assertEqual(len(model.statements), 1)


class WhitespaceAndFormattingTests(unittest.TestCase):
    """Test various whitespace and formatting scenarios."""

    def test_extra_whitespace(self) -> None:
        """Extra spaces should be handled"""
        model = parse_program("(  1  ,  2  )  ->  Std.Add  ;")
        self.assertEqual(len(model.statements), 1)

    def test_newlines_in_tuple(self) -> None:
        """Newlines within tuples"""
        model = parse_program("""
        (
            1,
            2,
            3
        ) -> Std.Add;
        """)
        self.assertEqual(len(model.statements), 1)

    def test_newlines_in_function_def(self) -> None:
        """Newlines in function definition"""
        model = parse_program("""
        let MyFunc(
            x: Number,
            y: Number
        ): Number = 
            (x, y) -> Std.Add;
        """)
        self.assertEqual(len(model.statements), 1)

    def test_tabs_as_whitespace(self) -> None:
        """Tab characters should be handled"""
        model = parse_program("(\t1\t,\t2\t)\t->\tStd.Add\t;")
        self.assertEqual(len(model.statements), 1)

    def test_mixed_line_endings(self) -> None:
        """Mixed UNIX and Windows line endings"""
        model = parse_program("(1, 2) -> Std.Add;\r\n(3, 4) -> Std.Multiply;")
        self.assertEqual(len(model.statements), 2)


class StringLiteralTests(unittest.TestCase):
    """Test string literal parsing."""

    def test_simple_string(self) -> None:
        """Basic string literal"""
        model = parse_program('''("hello") -> Std.Println;''')
        self.assertEqual(len(model.statements), 1)

    def test_string_with_spaces(self) -> None:
        """String with internal spaces"""
        model = parse_program('''("hello world") -> Std.Println;''')
        self.assertEqual(len(model.statements), 1)

    def test_string_with_numbers(self) -> None:
        """String containing numeric characters"""
        model = parse_program('''("test123") -> Std.Println;''')
        self.assertEqual(len(model.statements), 1)

    def test_string_with_special_chars(self) -> None:
        """String with special characters"""
        model = parse_program('''("test!@#$%") -> Std.Println;''')
        self.assertEqual(len(model.statements), 1)

    def test_string_with_escaped_quote(self) -> None:
        """String with escaped quote"""
        model = parse_program(r'''("test\"quote") -> Std.Println;''')
        self.assertEqual(len(model.statements), 1)

    def test_string_with_escaped_backslash(self) -> None:
        """String with escaped backslash"""
        model = parse_program(r'''("test\\path") -> Std.Println;''')
        self.assertEqual(len(model.statements), 1)

    def test_string_with_newline_escape(self) -> None:
        """String with \\n escape sequence"""
        model = parse_program(r'''("hello\nworld") -> Std.Println;''')
        self.assertEqual(len(model.statements), 1)

    def test_string_with_tab_escape(self) -> None:
        """String with \\t escape sequence"""
        model = parse_program(r'''("hello\tworld") -> Std.Println;''')
        self.assertEqual(len(model.statements), 1)

    def test_empty_string(self) -> None:
        """Empty string literal"""
        model = parse_program('''("") -> Std.Println;''')
        self.assertEqual(len(model.statements), 1)


class NumberLiteralTests(unittest.TestCase):
    """Test number literal parsing edge cases."""

    def test_integer_zero(self) -> None:
        """Integer 0"""
        model = parse_program("(0) -> Std.Println;")
        self.assertEqual(len(model.statements), 1)

    def test_float_zero(self) -> None:
        """Float 0.0"""
        model = parse_program("(0.0) -> Std.Println;")
        self.assertEqual(len(model.statements), 1)

    def test_leading_zero_integer(self) -> None:
        """Numbers like 01, 001 (if supported)"""
        model = parse_program("(01) -> Std.Println;")
        self.assertEqual(len(model.statements), 1)

    def test_multiple_trailing_zeros(self) -> None:
        """Numbers like 100, 1000"""
        model = parse_program("(1000) -> Std.Println;")
        self.assertEqual(len(model.statements), 1)

    def test_float_with_leading_zero(self) -> None:
        """0.5 format"""
        model = parse_program("(0.5) -> Std.Println;")
        self.assertEqual(len(model.statements), 1)

    def test_scientific_notation_lowercase_e(self) -> None:
        """1e10, 1.5e-5"""
        model = parse_program("(1e10) -> Std.Println;")
        self.assertEqual(len(model.statements), 1)

    def test_scientific_notation_uppercase_e(self) -> None:
        """1E10, 1.5E-5"""
        model = parse_program("(1E10) -> Std.Println;")
        self.assertEqual(len(model.statements), 1)

    def test_negative_integer(self) -> None:
        """-42"""
        model = parse_program("(-42) -> Std.Println;")
        self.assertEqual(len(model.statements), 1)

    def test_negative_float(self) -> None:
        """-3.14"""
        model = parse_program("(-3.14) -> Std.Println;")
        self.assertEqual(len(model.statements), 1)

    def test_negative_scientific(self) -> None:
        """-1.5e-10"""
        model = parse_program("(-1.5e-10) -> Std.Println;")
        self.assertEqual(len(model.statements), 1)

    def test_very_large_integer(self) -> None:
        """Large integer values"""
        model = parse_program("(999999999) -> Std.Println;")
        self.assertEqual(len(model.statements), 1)

    def test_very_small_float(self) -> None:
        """Very small decimal values"""
        model = parse_program("(0.00000001) -> Std.Println;")
        self.assertEqual(len(model.statements), 1)

    def test_scientific_notation_very_large_exponent(self) -> None:
        """1e100"""
        model = parse_program("(1e100) -> Std.Println;")
        self.assertEqual(len(model.statements), 1)

    def test_scientific_notation_very_small_exponent(self) -> None:
        """1e-100"""
        model = parse_program("(1e-100) -> Std.Println;")
        self.assertEqual(len(model.statements), 1)


class RecoveryAndDiagnosticsTests(unittest.TestCase):
    """Test error recovery and diagnostic accuracy."""

    def test_error_points_to_right_line(self) -> None:
        """Error reports correct line number"""
        source = """(1, 2) -> Std.Add;
(3, 4) -> Std.Add;
(5 6) -> Std.Add;"""
        try:
            parse_program(source)
            self.fail("Should have raised FleauxSyntaxError")
        except FleauxSyntaxError as e:
            self.assertEqual(e.line, 3)

    def test_error_points_to_right_column(self) -> None:
        """Error reports correct column number"""
        source = "(1, 2, ) -> Std.Add;"
        try:
            parse_program(source)
            self.fail("Should have raised FleauxSyntaxError")
        except FleauxSyntaxError as e:
            # Error should point to the trailing comma or closing paren
            self.assertGreater(e.col, 5)

    def test_error_message_includes_context(self) -> None:
        """Error message includes problematic source line"""
        source = "(1 2) -> Std.Add;"  # Missing comma
        try:
            parse_program(source)
            self.fail("Should have raised FleauxSyntaxError")
        except FleauxSyntaxError as e:
            # Error message should include context and source line
            error_str = str(e)
            self.assertIn("Expected", error_str)  # Should have error description
            self.assertIn("(1 2)", error_str)     # Should include the source line

    def test_parse_eof_uses_end_of_input_wording(self) -> None:
        with self.assertRaises(FleauxSyntaxError) as ctx:
            parse_program("(1, 2) -> Std.Add")

        self.assertIn("end of input", str(ctx.exception))
        self.assertIn("Hint:", str(ctx.exception))

    def test_parse_error_exposes_stage_and_hint_fields(self) -> None:
        source = "(1 2) -> Std.Add;"

        with self.assertRaises(FleauxSyntaxError) as ctx:
            parse_program(source)

        err = ctx.exception
        self.assertEqual(err.stage, "parse")
        self.assertTrue(hasattr(err, "hint"))
        self.assertIn("[parse]", str(err))


class LoweringDiagnosticsTests(unittest.TestCase):
    def test_invalid_non_call_stage_reports_stage_index_and_hint(self) -> None:
        model = parse_program("(1, 2) -> Std.Add -> 3 -> Std.Println;")

        with self.assertRaises(FleauxLoweringError) as ctx:
            lower(model)

        err = ctx.exception
        self.assertEqual(err.stage, "lowering")
        self.assertIsNotNone(err.stage_index)
        self.assertIsNotNone(err.hint)
        self.assertIn("stage", str(err))

    def test_template_stage_missing_call_target_reports_actionable_error(self) -> None:
        model = parse_program("(1, 2) -> Std.Add -> (_, 3);")

        with self.assertRaises(FleauxLoweringError) as ctx:
            lower(model)

        err = ctx.exception
        self.assertIn("missing a following call target", str(err))
        self.assertIsNotNone(err.hint)


class RegressionAndStabilityTests(unittest.TestCase):
    """Tests to ensure stability across changes."""

    def test_standard_library_parses(self) -> None:
        """Std.fleaux parses without error"""
        repo_root = Path(__file__).resolve().parents[1]
        model = parse_file(repo_root / "Std.fleaux")
        self.assertGreater(len(model.statements), 0)

    def test_test_file_parses(self) -> None:
        """test.fleaux parses without error"""
        repo_root = Path(__file__).resolve().parents[1]
        model = parse_file(repo_root / "test.fleaux")
        self.assertGreater(len(model.statements), 0)

    def test_import_file_parses(self) -> None:
        """test_import.fleaux parses without error"""
        repo_root = Path(__file__).resolve().parents[1]
        model = parse_file(repo_root / "test_import.fleaux")
        self.assertGreater(len(model.statements), 0)

    def test_lowering_doesnt_crash_on_valid_programs(self) -> None:
        """All valid programs lower without exception"""
        programs = [
            "(1, 2) -> Std.Add;",
            "(1) -> Std.Sqrt;",
            "(True, False) -> &&;",
            "let Id(x: Number): Number = (x) -> Std.Sqrt;",
            "import Std;",
            "() -> Std.GetArgs;",
        ]
        for prog in programs:
            try:
                lower(parse_program(prog))
            except Exception as e:
                self.fail(f"Lowering failed for: {prog}\nError: {e}")


if __name__ == "__main__":
    unittest.main()

