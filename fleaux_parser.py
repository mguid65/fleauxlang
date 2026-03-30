from pathlib import Path

from textx.metamodel import metamodel_from_file


_GRAMMAR_PATH = Path(__file__).with_name("fleaux_grammar.tx")
_METAMODEL = metamodel_from_file(str(_GRAMMAR_PATH), auto_init_attributes=False)


def parse_program(source: str):
    """Parse Fleaux source text into a textX Program model."""
    return _METAMODEL.model_from_str(source)


def parse_file(file_path: str | Path):
    """Parse a .fleaux file into a textX Program model."""
    return _METAMODEL.model_from_file(str(file_path))


def main() -> None:
    demo_source = """
import Std;
let Add2(x: Number): Number = (x, 2) -> Std.Add;
(3) -> Add2;
"""
    model = parse_program(demo_source)
    print(type(model).__name__)
    print(len(model.statements))


if __name__ == "__main__":
    main()
