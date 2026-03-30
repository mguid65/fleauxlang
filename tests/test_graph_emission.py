from __future__ import annotations

import unittest

from fleaux_graphviz import emit_dot
from fleaux_lowering import lower
from fleaux_parser import parse_program


class GraphEmissionTests(unittest.TestCase):
    def test_emit_dot_contains_pipeline_nodes_and_edges(self) -> None:
        source = """
import Std;
let AddThenPrint(x: Number, y: Number): Number = ((x, y) -> Std.Add) -> Std.Println;
(1, 2) -> AddThenPrint;
"""
        program = lower(parse_program(source))

        dot = emit_dot(program)

        self.assertIn("digraph fleaux", dot)
        self.assertIn("import Std", dot)
        self.assertIn("let AddThenPrint", dot)
        self.assertIn("call Std.Add", dot)
        self.assertIn("call Std.Println", dot)
        self.assertIn("call AddThenPrint", dot)
        self.assertIn("[label=\"pipe\"]", dot)


if __name__ == "__main__":
    unittest.main()

