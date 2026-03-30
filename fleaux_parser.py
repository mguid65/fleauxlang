from pathlib import Path

from fleaux_hand_parser import parse_program as _parse_program_hand, parse_file as _parse_file_hand


def parse_program(source: str):
    """Parse Fleaux source text into a Program model using the hand-rolled parser."""
    return _parse_program_hand(source)


def parse_file(file_path: str | Path):
    """Parse a .fleaux file into a Program model using the hand-rolled parser."""
    return _parse_file_hand(file_path)


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
