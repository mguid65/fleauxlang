from __future__ import annotations

import unittest
from pathlib import Path

from textx import TextXSyntaxError
from textx.metamodel import metamodel_from_file

from fleaux_parser import parse_file, parse_program


class TextXGrammarParsingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        repo_root = Path(__file__).resolve().parents[1]
        grammar_path = repo_root / "fleaux_grammar.tx"
        cls.textx_mm = metamodel_from_file(str(grammar_path), auto_init_attributes=False)
        cls.repo_root = repo_root

    def test_textx_parses_test_file(self) -> None:
        model = self.textx_mm.model_from_file(str(self.repo_root / "test.fleaux"))

        self.assertEqual(type(model).__name__, "Program")
        self.assertEqual(len(model.statements), 3)
        self.assertEqual(type(model.statements[0]).__name__, "ImportStatement")
        self.assertEqual(type(model.statements[1]).__name__, "LetStatement")
        self.assertEqual(type(model.statements[2]).__name__, "ExpressionStatement")

    def test_textx_parses_std_library_fixture(self) -> None:
        model = self.textx_mm.model_from_file(str(self.repo_root / "Std.fleaux"))

        self.assertEqual(type(model).__name__, "Program")
        self.assertGreaterEqual(len(model.statements), 2)
        self.assertEqual(type(model.statements[0]).__name__, "ImportStatement")

    def test_textx_parses_import_fixture(self) -> None:
        model = self.textx_mm.model_from_file(str(self.repo_root / "test_import.fleaux"))

        self.assertEqual(type(model).__name__, "Program")
        self.assertEqual(len(model.statements), 2)
        self.assertEqual(type(model.statements[0]).__name__, "ImportStatement")
        self.assertEqual(type(model.statements[1]).__name__, "LetStatement")

    def test_textx_rejects_keyword_as_identifier(self) -> None:
        source = "let let(x: Number): Number = x;"

        with self.assertRaises(TextXSyntaxError):
            self.textx_mm.model_from_str(source)

    def test_textx_rejects_chained_flow_without_parenthesized_lhs(self) -> None:
        source = "let AddPrint(x: Number, y: Number): Number = (x, y) -> Add -> Std.Println;"

        with self.assertRaises(TextXSyntaxError):
            self.textx_mm.model_from_str(source)

    def test_textx_accepts_chained_flow_with_parenthesized_lhs(self) -> None:
        source = "let AddPrint(x: Number, y: Number): Number = ((x, y) -> Add) -> Std.Println;"
        model = self.textx_mm.model_from_str(source)

        self.assertEqual(type(model).__name__, "Program")
        self.assertEqual(len(model.statements), 1)


class TextXParserApiTests(unittest.TestCase):
    def test_parse_program_supports_import_and_pipeline_chain(self) -> None:
        source = """
import Std;
let Add(x: Number, y: Number): Number = (x, y) -> Std.Add;
let AddPrint(x: Number, y: Number): Number = ((x, y) -> Add) -> Std.Println;
(1, 2) -> AddPrint;
"""
        model = parse_program(source)

        self.assertEqual(type(model).__name__, "Program")
        self.assertEqual(len(model.statements), 4)
        self.assertEqual(type(model.statements[0]).__name__, "ImportStatement")

    def test_parse_program_accepts_empty_tuple_lhs(self) -> None:
        model = parse_program("() -> Std.GetArgs;")

        self.assertEqual(type(model).__name__, "Program")
        self.assertEqual(len(model.statements), 1)

    def test_parse_file_parses_existing_fixture(self) -> None:
        repo_root = Path(__file__).resolve().parents[1]
        model = parse_file(repo_root / "test.fleaux")

        self.assertEqual(type(model).__name__, "Program")
        self.assertGreaterEqual(len(model.statements), 1)


if __name__ == "__main__":
    unittest.main()

