from __future__ import annotations

import argparse
import keyword
import re
from pathlib import Path

from fleaux_ast import (
    IRProgram, IRImport, IRLet, IRExprStatement,
    IRFlowExpr, IRTupleExpr, IRConstant, IRNameRef, IROperatorRef,
    IRExpr, IRCallTarget,
)
from fleaux_diagnostics import format_diagnostic
from fleaux_lowering import lower
from fleaux_parser import parse_file


class FleauxTranspilerError(Exception):
    def __init__(self, message: str, *, hint: str | None = None):
        self.stage = "transpile"
        self.message = message
        self.hint = hint
        super().__init__(
            format_diagnostic(
                stage=self.stage,
                message=self.message,
                hint=self.hint,
            )
        )


class FleauxTranspiler:
    RUNTIME_ROOT = Path(__file__).resolve().parent
    BUNDLED_MODULE_SOURCES = {
        "Std": RUNTIME_ROOT / "Std.fleaux",
    }

    OPERATOR_TO_BUILTIN = {
        "+": "Add",
        "-": "Subtract",
        "*": "Multiply",
        "/": "Divide",
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
        "GetArgs", "Wrap", "Unwrap", "ElementAt", "ToNum", "Input", "Exit",
        "Take", "Drop", "Length", "Slice", "Pow", "Subtract",
        "Multiply", "Divide", "Add", "Sqrt", "Println", "Printf",
        "Tan", "Cos", "Sin",
        # comparison / logic / unary
        "Mod", "GreaterThan", "LessThan", "GreaterOrEqual", "LessOrEqual",
        "Equal", "NotEqual", "Not", "And", "Or",
        "UnaryPlus", "UnaryMinus", "ToString",
        # conditional
        "Select", "Branch", "Apply", "Loop", "LoopN",
        # newly exposed
        "GetArgs", "ToNum", "Length", "Slice",
        # path/filesystem (flat aliases)
        "Cwd", "PathJoin", "PathNormalize", "PathBasename", "PathDirname",
        "PathExists", "PathIsFile", "PathIsDir", "PathAbsolute",
        "FileReadText", "FileWriteText",
        "OSEnv", "OSHasEnv", "OSSetEnv", "OSUnsetEnv",
        "OSIsWindows", "OSIsLinux", "OSIsMacOS",
        # math (new)
        "MathFloor", "MathCeil", "MathAbs", "MathLog", "MathClamp",
        # string
        "StringUpper", "StringLower", "StringTrim", "StringTrimStart", "StringTrimEnd",
        "StringSplit", "StringJoin", "StringReplace", "StringContains",
        "StringStartsWith", "StringEndsWith", "StringLength",
        # os extras
        "OSHome", "OSTempDir", "OSMakeTempFile", "OSMakeTempDir",
        # path extras
        "PathExtension", "PathStem", "PathWithExtension", "PathWithBasename",
        # file extras
        "FileAppendText", "FileReadLines", "FileDelete", "FileSize",
        # dir
        "DirCreate", "DirDelete", "DirList", "DirListFull",
        # tuple
        "TupleAppend", "TuplePrepend", "TupleReverse", "TupleContains",
        "TupleZip", "TupleMap", "TupleFilter",
        # streaming file handles
        "FileOpen", "FileReadLine", "FileReadChunk", "FileWriteChunk",
        "FileFlush", "FileClose", "FileWithOpen",
    }

    BUILTIN_NAME_MAP = {
        # nested -> runtime class
        "Std.Path.Join": "PathJoin",
        "Std.Path.Normalize": "PathNormalize",
        "Std.Path.Basename": "PathBasename",
        "Std.Path.Dirname": "PathDirname",
        "Std.Path.Exists": "PathExists",
        "Std.Path.IsFile": "PathIsFile",
        "Std.Path.IsDir": "PathIsDir",
        "Std.Path.Absolute": "PathAbsolute",
        "Std.File.ReadText": "FileReadText",
        "Std.File.WriteText": "FileWriteText",
        "Std.OS.Cwd": "Cwd",
        "Std.OS.Env": "OSEnv",
        "Std.OS.HasEnv": "OSHasEnv",
        "Std.OS.SetEnv": "OSSetEnv",
        "Std.OS.UnsetEnv": "OSUnsetEnv",
        "Std.OS.IsWindows": "OSIsWindows",
        "Std.OS.IsLinux": "OSIsLinux",
        "Std.OS.IsMacOS": "OSIsMacOS",
        # math
        "Std.Math.Sqrt": "Sqrt",
        "Std.Math.Sin": "Sin",
        "Std.Math.Cos": "Cos",
        "Std.Math.Tan": "Tan",
        "Std.Math.Floor": "MathFloor",
        "Std.Math.Ceil": "MathCeil",
        "Std.Math.Abs": "MathAbs",
        "Std.Math.Log": "MathLog",
        "Std.Math.Clamp": "MathClamp",
        # string
        "Std.String.Upper": "StringUpper",
        "Std.String.Lower": "StringLower",
        "Std.String.Trim": "StringTrim",
        "Std.String.TrimStart": "StringTrimStart",
        "Std.String.TrimEnd": "StringTrimEnd",
        "Std.String.Split": "StringSplit",
        "Std.String.Join": "StringJoin",
        "Std.String.Replace": "StringReplace",
        "Std.String.Contains": "StringContains",
        "Std.String.StartsWith": "StringStartsWith",
        "Std.String.EndsWith": "StringEndsWith",
        "Std.String.Length": "StringLength",
        # os extras
        "Std.OS.Home": "OSHome",
        "Std.OS.TempDir": "OSTempDir",
        "Std.OS.MakeTempFile": "OSMakeTempFile",
        "Std.OS.MakeTempDir": "OSMakeTempDir",
        # path extras
        "Std.Path.Extension": "PathExtension",
        "Std.Path.Stem": "PathStem",
        "Std.Path.WithExtension": "PathWithExtension",
        "Std.Path.WithBasename": "PathWithBasename",
        # file extras
        "Std.File.AppendText": "FileAppendText",
        "Std.File.ReadLines": "FileReadLines",
        "Std.File.Delete": "FileDelete",
        "Std.File.Size": "FileSize",
        # dir
        "Std.Dir.Create": "DirCreate",
        "Std.Dir.Delete": "DirDelete",
        "Std.Dir.List": "DirList",
        "Std.Dir.ListFull": "DirListFull",
        # tuple
        "Std.Tuple.Append": "TupleAppend",
        "Std.Tuple.Prepend": "TuplePrepend",
        "Std.Tuple.Reverse": "TupleReverse",
        "Std.Tuple.Contains": "TupleContains",
        "Std.Tuple.Zip": "TupleZip",
        "Std.Tuple.Map": "TupleMap",
        "Std.Tuple.Filter": "TupleFilter",
        # streaming file handles
        "Std.File.Open": "FileOpen",
        "Std.File.ReadLine": "FileReadLine",
        "Std.File.ReadChunk": "FileReadChunk",
        "Std.File.WriteChunk": "FileWriteChunk",
        "Std.File.Flush": "FileFlush",
        "Std.File.Close": "FileClose",
        "Std.File.WithOpen": "FileWithOpen",
    }

    def __init__(self):
        self._generated: dict[Path, Path] = {}
        self._in_progress: set[Path] = set()

    def process(self, filename: str | Path) -> Path:
        return self._process_file(Path(filename).resolve())

    # ── Internal pipeline ─────────────────────────────────────────────────────

    def _process_file(self, source: Path) -> Path:
        source = source.resolve()
        module_name = source.stem

        if source in self._generated:
            return self._generated[source]
        if source in self._in_progress:
            raise FleauxTranspilerError(
                f"Cyclic import detected while transpiling '{module_name}'.",
                hint="Break the import cycle by moving shared definitions into a third module.",
            )

        self._in_progress.add(source)
        try:
            # 1. Parse -> model -> IR
            try:
                parsed_model = parse_file(source)
                program: IRProgram = lower(parsed_model)
            except Exception as exc:
                raise FleauxTranspilerError(
                    f"Failed to parse/lower '{source.name}': {exc}",
                    hint="Fix parse or lowering errors in the source module, then transpile again.",
                ) from exc

            # 2. Recursively transpile imports first
            import_aliases: dict[str, str] = {}
            import_modules: dict[str, str] = {}

            for stmt in program.statements:
                if not isinstance(stmt, IRImport):
                    continue
                if stmt.module_name == "StdBuiltins":
                    continue
                imported_source = self._resolve_import_source(source, stmt.module_name)
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
                alias = import_aliases[imported_name]
                lines.append(f"import {module_stem} as {alias}")
                lines.append(f"from {module_stem} import *")
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

            self._generated[source] = output_path
            return output_path
        finally:
            self._in_progress.discard(source)

    # ── Let-statement emission ────────────────────────────────────────────────

    def _emit_let(
        self,
        let: IRLet,
        import_aliases: dict[str, str],
        known_symbols: dict[tuple[str | None, str], str],
    ) -> list[str]:
        symbol_name = self._symbol_name(let.qualifier, let.name)

        if let.is_builtin:
            builtin_key = let.name
            if let.qualifier:
                qualified = f"{let.qualifier}.{let.name}"
                if "." in let.qualifier or qualified in self.BUILTIN_NAME_MAP:
                    builtin_key = qualified
            return [f"{symbol_name} = {self._builtin_expr(builtin_key)}", ""]

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
            # lhs could be another FlowExpr (from chaining) or a TupleExpr
            # Recursively compile it as an expression
            if isinstance(expr.lhs, IRTupleExpr):
                lhs = self._compile_tuple(expr.lhs, local_bindings, import_aliases, known_symbols)
            else:
                lhs = self._compile_expr(expr.lhs, local_bindings, import_aliases, known_symbols)
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

        raise FleauxTranspilerError(
            f"Cannot compile IR expression type '{type(expr).__name__}'.",
            hint="Use a flow expression, tuple expression, constant, or name reference.",
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
                raise FleauxTranspilerError(
                    f"Unsupported operator '{target.op}'.",
                    hint="Use a supported operator mapping in OPERATOR_TO_BUILTIN.",
                )
            return f"fstd.{builtin}"

        if isinstance(target, IRNameRef):
            if target.qualifier is not None:
                return self._compile_qualified_ref(target, import_aliases, known_symbols)
            return self._compile_name_ref(target.name, local_bindings, known_symbols)

        raise FleauxTranspilerError(
            f"Cannot compile call target type '{type(target).__name__}'.",
            hint="Use an operator reference or name reference as the call target.",
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
        # Nested namespaces like Std.Math, Std.Path etc. have qualifier "Std.Math"
        # but import_aliases only has the root "Std". Fall back to root segment.
        root = ref.qualifier.split(".")[0]
        if root in import_aliases:
            return f"{import_aliases[root]}.{sym}"
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

    def _resolve_import_source(self, source: Path, module_name: str) -> Path:
        local_source = source.with_name(f"{module_name}.fleaux").resolve()
        if local_source.exists():
            return local_source

        bundled_source = self.BUNDLED_MODULE_SOURCES.get(module_name)
        if bundled_source is not None and bundled_source.exists():
            return bundled_source.resolve()

        bundled_hint = ""
        if bundled_source is not None:
            bundled_hint = f" and bundled module '{bundled_source}'"
        raise FleauxTranspilerError(
            f"Unable to resolve import '{module_name}' from '{source}'. "
            f"Looked for '{local_source}'{bundled_hint}.",
            hint="Place the module beside the importing source file, or use a bundled module such as 'Std'.",
        )

    # ── Helpers ───────────────────────────────────────────────────────────────

    def _builtin_expr(self, name: str) -> str:
        mapped = self.BUILTIN_NAME_MAP.get(name, name)
        if mapped in self.IMPLEMENTED_BUILTINS:
            return f"fstd.{mapped}"
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
