from __future__ import annotations

import argparse
import keyword
import re
from pathlib import Path

from textx.metamodel import metamodel_from_file

from fleaux_ast import (
    IRProgram, IRImport, IRLet, IRExprStatement,
    IRFlowExpr, IRTupleExpr, IRConstant, IRNameRef, IROperatorRef,
    IRExpr, IRCallTarget,
)
from fleaux_lowering import lower


class FleauxTranspiler:
    OPERATOR_TO_BUILTIN = {
        "+": "Add",
        "-": "Subtract",
        "*": "Multiply",
        "//": "Divide",
        "%": "Mod",
        "^": "Pow",
        "==": "Equal",
        "!=": "NotEqual",
        "<": "LessThan",
        ">": "GreaterThan",
        ">=": "GreaterOrEqual",
        "<=": "LessOrEqual",
        "!": "Not",
        "&&": "And",
        "||": "Or",
    }

    IMPLEMENTED_BUILTINS = {
        "GetArgs", "Wrap", "Unwrap", "ElementAt", "ToNum", "In",
        "Take", "Drop", "Length", "Slice", "Pow", "Subtract",
        "Multiply", "Divide", "Add", "Sqrt", "Println", "Printf",
        "Tan", "Cos", "Sin",
        # comparison / logic / unary
        "Mod", "GreaterThan", "LessThan", "GreaterOrEqual", "LessOrEqual",
        "Equal", "NotEqual", "Not", "And", "Or",
        "UnaryPlus", "UnaryMinus", "ToString",
        # newly exposed
        "GetArgs", "ToNum", "Length", "Slice",
    }

    def __init__(self):
        grammar_path = Path(__file__).with_name("fleaux_grammar.tx")
        self._mm = metamodel_from_file(str(grammar_path), auto_init_attributes=False)
        self._generated: dict[str, Path] = {}
        self._in_progress: set[str] = set()

    def process(self, filename: str | Path) -> Path:
        return self._process_file(Path(filename).resolve())

    # ── Internal pipeline ─────────────────────────────────────────────────────

    def _process_file(self, source: Path) -> Path:
        module_name = source.stem

        if module_name in self._generated:
            return self._generated[module_name]
        if module_name in self._in_progress:
            raise RuntimeError(
                f"Cyclic import detected while transpiling '{module_name}'."
            )

        self._in_progress.add(module_name)

        # 1. Parse → textX model → IR
        textx_model = self._mm.model_from_file(str(source))
        program: IRProgram = lower(textx_model)

        # 2. Recursively transpile imports first
        import_aliases: dict[str, str] = {}
        import_modules: dict[str, str] = {}

        for stmt in program.statements:
            if not isinstance(stmt, IRImport):
                continue
            if stmt.module_name == "StdBuiltins":
                continue
            imported_source = source.with_name(f"{stmt.module_name}.fleaux")
            imported_output = self._process_file(imported_source)
            import_modules[stmt.module_name] = imported_output.stem
            import_aliases[stmt.module_name] = f"_mod_{self._sanitize(stmt.module_name)}"

        # 3. Build symbol table from let-bindings in this module
        known_symbols: dict[tuple[str | None, str], str] = {}
        for stmt in program.statements:
            if not isinstance(stmt, IRLet):
                continue
            sym = self._symbol_name(stmt.qualifier, stmt.name)
            known_symbols[(stmt.qualifier, stmt.name)] = sym
            known_symbols[(None, stmt.name)] = sym

        # 4. Emit Python source
        lines: list[str] = [
            "import fleaux_std_builtins as fstd",
            "",
            "def _fleaux_missing_builtin(name):",
            "    class _MissingBuiltin:",
            "        def __ror__(self, _tuple_args):",
            "            raise NotImplementedError(",
            "                f\"Builtin '{name}' is not yet implemented in fleaux_std_builtins.py\"",
            "            )",
            "    return _MissingBuiltin",
            "",
        ]

        for imported_name, module_stem in sorted(import_modules.items()):
            lines.append(f"import {module_stem} as {import_aliases[imported_name]}")
        if import_modules:
            lines.append("")

        for stmt in program.statements:
            if isinstance(stmt, IRLet):
                lines.extend(
                    self._emit_let(stmt, import_aliases, known_symbols)
                )
            elif isinstance(stmt, IRExprStatement):
                compiled = self._compile_expr(
                    stmt.expr, {}, import_aliases, known_symbols
                )
                lines.append(f"_fleaux_last_value = {compiled}")

        output_path = source.with_name(f"fleaux_generated_module_{module_name}.py")
        output_path.write_text("\n".join(lines).rstrip() + "\n", encoding="utf-8")

        self._generated[module_name] = output_path
        self._in_progress.remove(module_name)
        return output_path

    # ── Let-statement emission ────────────────────────────────────────────────

    def _emit_let(
        self,
        let: IRLet,
        import_aliases: dict[str, str],
        known_symbols: dict[tuple[str | None, str], str],
    ) -> list[str]:
        symbol_name = self._symbol_name(let.qualifier, let.name)

        if let.is_builtin:
            return [f"{symbol_name} = {self._builtin_expr(let.name)}", ""]

        local_bindings = {p.name: self._sanitize(p.name) for p in let.params}
        fn_name = f"_fleaux_impl_{symbol_name}"
        body = self._compile_expr(let.body, local_bindings, import_aliases, known_symbols)

        setup: list[str] = []
        n = len(let.params)
        if n == 1:
            lone = local_bindings[let.params[0].name]
            setup.append(
                f"    {lone} = _fleaux_args if len(_fleaux_args) != 1 else _fleaux_args[0]"
            )
        elif n > 1:
            setup.append(f"    if len(_fleaux_args) < {n}:")
            setup.append(
                f"        raise TypeError("
                f"f'Expected {n} arguments, got {{len(_fleaux_args)}}')"
            )
            for idx, p in enumerate(let.params):
                setup.append(f"    {local_bindings[p.name]} = _fleaux_args[{idx}]")
        if not setup:
            setup.append("    pass")

        return [
            f"def {fn_name}(*_fleaux_args):",
            *setup,
            f"    return {body}",
            f"{symbol_name} = fstd.make_node({fn_name})",
            "",
        ]

    # ── Expression compilation ────────────────────────────────────────────────

    def _compile_expr(
        self,
        expr: IRExpr,
        local_bindings: dict[str, str],
        import_aliases: dict[str, str],
        known_symbols: dict[tuple[str | None, str], str],
    ) -> str:
        if isinstance(expr, IRFlowExpr):
            lhs = self._compile_tuple(expr.lhs, local_bindings, import_aliases, known_symbols)
            rhs = self._compile_call_target(expr.rhs, local_bindings, import_aliases, known_symbols)
            return f"({lhs} | {rhs}())"

        if isinstance(expr, IRTupleExpr):
            return self._compile_tuple(expr, local_bindings, import_aliases, known_symbols)

        if isinstance(expr, IRConstant):
            return repr(expr.val)

        if isinstance(expr, IRNameRef):
            if expr.qualifier is not None:
                return self._compile_qualified_ref(expr, import_aliases, known_symbols)
            return self._compile_name_ref(expr.name, local_bindings, known_symbols)

        raise NotImplementedError(
            f"Cannot compile IR expression type '{type(expr).__name__}'."
        )

    def _compile_tuple(
        self,
        expr: IRTupleExpr,
        local_bindings: dict[str, str],
        import_aliases: dict[str, str],
        known_symbols: dict[tuple[str | None, str], str],
    ) -> str:
        items = [
            self._compile_expr(e, local_bindings, import_aliases, known_symbols)
            for e in expr.items
        ]
        return self._tuple_literal(items)

    def _compile_call_target(
        self,
        target: IRCallTarget,
        local_bindings: dict[str, str],
        import_aliases: dict[str, str],
        known_symbols: dict[tuple[str | None, str], str],
    ) -> str:
        if isinstance(target, IROperatorRef):
            builtin = self.OPERATOR_TO_BUILTIN.get(target.op)
            if builtin is None:
                raise NotImplementedError(f"Unsupported operator '{target.op}'.")
            return f"fstd.{builtin}"

        if isinstance(target, IRNameRef):
            if target.qualifier is not None:
                return self._compile_qualified_ref(target, import_aliases, known_symbols)
            return self._compile_name_ref(target.name, local_bindings, known_symbols)

        raise NotImplementedError(
            f"Cannot compile call target type '{type(target).__name__}'."
        )

    def _compile_qualified_ref(
        self,
        ref: IRNameRef,
        import_aliases: dict[str, str],
        known_symbols: dict[tuple[str | None, str], str],
    ) -> str:
        sym = known_symbols.get(
            (ref.qualifier, ref.name),
            self._symbol_name(ref.qualifier, ref.name),
        )
        if ref.qualifier in import_aliases:
            return f"{import_aliases[ref.qualifier]}.{sym}"
        return sym

    def _compile_name_ref(
        self,
        name: str,
        local_bindings: dict[str, str],
        known_symbols: dict[tuple[str | None, str], str],
    ) -> str:
        if name in local_bindings:
            return local_bindings[name]
        if (None, name) in known_symbols:
            return known_symbols[(None, name)]
        return self._sanitize(name)

    # ── Helpers ───────────────────────────────────────────────────────────────

    def _builtin_expr(self, name: str) -> str:
        if name in self.IMPLEMENTED_BUILTINS:
            return f"fstd.{name}"
        return f"_fleaux_missing_builtin({name!r})"

    @staticmethod
    def _symbol_name(qualifier: str | None, name: str) -> str:
        safe = FleauxTranspiler._sanitize(name)
        if qualifier is None:
            return safe
        return f"{FleauxTranspiler._sanitize(qualifier)}_{safe}"

    @staticmethod
    def _tuple_literal(items: list[str]) -> str:
        if len(items) == 0:
            return "()"
        if len(items) == 1:
            return f"({items[0]},)"
        return f"({', '.join(items)})"

    @staticmethod
    def _sanitize(raw: str) -> str:
        safe = re.sub(r"[^a-zA-Z0-9_]", "_", raw)
        if not safe:
            safe = "_"
        if safe[0].isdigit():
            safe = f"_{safe}"
        if keyword.iskeyword(safe):
            safe = f"{safe}_"
        return safe


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Transpile a Fleaux source file to a Python module."
    )
    parser.add_argument(
        "source", nargs="?", default="test.fleaux", help="Input .fleaux file path"
    )
    args = parser.parse_args()

    output = FleauxTranspiler().process(args.source)
    print(output)


if __name__ == "__main__":
    main()
