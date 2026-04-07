from __future__ import annotations

import argparse
import json
import keyword
import re
from dataclasses import dataclass
from pathlib import Path

# Full set of C++ (up to C++20) reserved keywords.
# `keyword.iskeyword` only covers Python; these cover the C++ side.
_CPP_KEYWORDS: frozenset[str] = frozenset({
    "alignas", "alignof", "and", "and_eq", "asm", "auto",
    "bitand", "bitor", "bool", "break",
    "case", "catch", "char", "char8_t", "char16_t", "char32_t",
    "class", "compl", "concept", "const", "consteval", "constexpr",
    "constinit", "const_cast", "continue",
    "co_await", "co_return", "co_yield",
    "decltype", "default", "delete", "do", "double", "dynamic_cast",
    "else", "enum", "explicit", "export", "extern",
    "false", "float", "for", "friend",
    "goto", "if", "inline", "int",
    "long", "mutable",
    "namespace", "new", "noexcept", "not", "not_eq", "nullptr",
    "operator", "or", "or_eq",
    "private", "protected", "public",
    "register", "reinterpret_cast", "requires", "return",
    "short", "signed", "sizeof", "static", "static_assert",
    "static_cast", "struct", "switch",
    "template", "this", "thread_local", "throw", "true",
    "typedef", "typeid", "typename",
    "union", "unsigned", "using",
    "virtual", "void", "volatile",
    "wchar_t", "while",
    "xor", "xor_eq",
})

from .fleaux_ast import (
    IRProgram, IRImport, IRLet, IRExprStatement,
    IRFlowExpr, IRTupleExpr, IRConstant, IRNameRef, IROperatorRef,
    IRExpr, IRCallTarget,
)
from .fleaux_diagnostics import format_diagnostic
from .fleaux_lowering import FleauxLoweringError, lower
from .fleaux_parser import FleauxSyntaxError, parse_file


class FleauxCppTranspilerError(Exception):
    def __init__(self, message: str, *, hint: str | None = None, span=None):
        self.stage = "transpile-cpp"
        self.message = message
        self.hint = hint
        self.span = span
        self.line = span.line if span is not None else None
        self.col = span.col if span is not None else None
        super().__init__(
            format_diagnostic(
                stage=self.stage,
                message=self.message,
                span=self.span,
                hint=self.hint,
            )
        )


@dataclass
class _ModuleInfo:
    source: Path
    module_name: str
    program: IRProgram
    import_sources: dict[str, Path]
    known_symbols: dict[tuple[str | None, str], str]


class FleauxCppTranspiler:
    RUNTIME_ROOT = Path(__file__).resolve().parent.parent
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

    BUILTIN_NAME_MAP = {
        "Std.Printf": "Printf",
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
        "Std.Math.Sqrt": "Sqrt",
        "Std.Math.Sin": "Sin",
        "Std.Math.Cos": "Cos",
        "Std.Math.Tan": "Tan",
        "Std.Math.Floor": "MathFloor",
        "Std.Math.Ceil": "MathCeil",
        "Std.Math.Abs": "MathAbs",
        "Std.Math.Log": "MathLog",
        "Std.Math.Clamp": "MathClamp",
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
        "Std.OS.Home": "OSHome",
        "Std.OS.TempDir": "OSTempDir",
        "Std.OS.MakeTempFile": "OSMakeTempFile",
        "Std.OS.MakeTempDir": "OSMakeTempDir",
        "Std.Path.Extension": "PathExtension",
        "Std.Path.Stem": "PathStem",
        "Std.Path.WithExtension": "PathWithExtension",
        "Std.Path.WithBasename": "PathWithBasename",
        "Std.File.AppendText": "FileAppendText",
        "Std.File.ReadLines": "FileReadLines",
        "Std.File.Delete": "FileDelete",
        "Std.File.Size": "FileSize",
        "Std.Dir.Create": "DirCreate",
        "Std.Dir.Delete": "DirDelete",
        "Std.Dir.List": "DirList",
        "Std.Dir.ListFull": "DirListFull",
        "Std.Tuple.Append": "TupleAppend",
        "Std.Tuple.Prepend": "TuplePrepend",
        "Std.Tuple.Reverse": "TupleReverse",
        "Std.Tuple.Contains": "TupleContains",
        "Std.Tuple.Zip": "TupleZip",
        "Std.Tuple.Map": "TupleMap",
        "Std.Tuple.Filter": "TupleFilter",
        "Std.Tuple.Sort": "TupleSort",
        "Std.Tuple.Unique": "TupleUnique",
        "Std.Tuple.Min": "TupleMin",
        "Std.Tuple.Max": "TupleMax",
        "Std.Tuple.Reduce": "TupleReduce",
        "Std.Tuple.FindIndex": "TupleFindIndex",
        "Std.Tuple.Any": "TupleAny",
        "Std.Tuple.All": "TupleAll",
        "Std.Tuple.Range": "TupleRange",
        "Std.File.Open": "FileOpen",
        "Std.File.ReadLine": "FileReadLine",
        "Std.File.ReadChunk": "FileReadChunk",
        "Std.File.WriteChunk": "FileWriteChunk",
        "Std.File.Flush": "FileFlush",
        "Std.File.Close": "FileClose",
        "Std.File.WithOpen": "FileWithOpen",
        "Std.Dict.Create": "DictCreate",
        "Std.Dict.Set": "DictSet",
        "Std.Dict.Get": "DictGet",
        "Std.Dict.GetDefault": "DictGetDefault",
        "Std.Dict.Contains": "DictContains",
        "Std.Dict.Delete": "DictDelete",
        "Std.Dict.Keys": "DictKeys",
        "Std.Dict.Values": "DictValues",
        "Std.Dict.Entries": "DictEntries",
        "Std.Dict.Clear": "DictClear",
        "Std.Dict.Length": "DictLength",
    }

    # Runtime nodes currently implemented in cpp/fleaux_runtime.hpp.
    CPP_RUNTIME_BUILTINS = {
        "Wrap", "Unwrap", "ElementAt", "Length", "Take", "Drop", "Slice",
        "Add", "Subtract", "Multiply", "Divide", "Mod", "Pow", "Sqrt",
        "UnaryPlus", "UnaryMinus", "Sin", "Cos", "Tan",
        "GreaterThan", "LessThan", "GreaterOrEqual", "LessOrEqual",
        "Equal", "NotEqual", "Not", "And", "Or",
        "ToString", "ToNum", "Println", "Printf", "Input", "GetArgs", "Exit",
        "Select", "Apply", "Branch", "Loop", "LoopN",
        "StringUpper", "StringLower", "StringTrim", "StringTrimStart", "StringTrimEnd",
        "StringSplit", "StringJoin", "StringReplace", "StringContains",
        "StringStartsWith", "StringEndsWith", "StringLength",
        "MathFloor", "MathCeil", "MathAbs", "MathLog", "MathClamp",
        "Cwd", "PathJoin", "PathNormalize", "PathBasename", "PathDirname",
        "PathExists", "PathIsFile", "PathIsDir", "PathAbsolute",
        "PathExtension", "PathStem", "PathWithExtension", "PathWithBasename",
        "FileReadText", "FileWriteText", "FileAppendText", "FileReadLines",
        "FileDelete", "FileSize",
        "DirCreate", "DirDelete", "DirList", "DirListFull",
        "OSEnv", "OSHasEnv", "OSSetEnv", "OSUnsetEnv",
        "OSIsWindows", "OSIsLinux", "OSIsMacOS", "OSHome", "OSTempDir",
        "OSMakeTempFile", "OSMakeTempDir",
        "TupleAppend", "TuplePrepend", "TupleReverse", "TupleContains", "TupleZip",
        "TupleMap", "TupleFilter", "TupleSort", "TupleUnique", "TupleMin", "TupleMax",
        "TupleReduce", "TupleFindIndex", "TupleAny", "TupleAll", "TupleRange",
        "FileOpen", "FileReadLine", "FileReadChunk", "FileWriteChunk",
        "FileFlush", "FileClose", "FileWithOpen",
        "DictCreate", "DictSet", "DictGet", "DictGetDefault", "DictContains",
        "DictDelete", "DictKeys", "DictValues", "DictEntries", "DictClear", "DictLength",
    }

    def __init__(self):
        self._modules: dict[Path, _ModuleInfo] = {}
        self._in_progress: set[Path] = set()

    def process(self, filename: str | Path) -> Path:
        root = Path(filename).resolve()
        self._collect_module(root)
        return self._emit_cpp(root)

    def _collect_module(self, source: Path) -> None:
        source = source.resolve()
        if source in self._modules:
            return
        if source in self._in_progress:
            raise FleauxCppTranspilerError(
                f"Cyclic import detected while transpiling '{source.stem}'.",
                hint="Break the cycle by moving shared definitions into a third module.",
            )

        self._in_progress.add(source)
        try:
            try:
                parsed_model = parse_file(source)
                program: IRProgram = lower(parsed_model)
            except (FleauxSyntaxError, FleauxLoweringError):
                raise
            except Exception as exc:
                raise FleauxCppTranspilerError(
                    f"Failed to parse/lower '{source.name}': {exc}",
                    hint="Fix parse/lowering errors in the source module and retry.",
                ) from exc

            import_sources: dict[str, Path] = {}
            for stmt in program.statements:
                if not isinstance(stmt, IRImport):
                    continue
                if stmt.module_name == "StdBuiltins":
                    continue
                imported_source = self._resolve_import_source(source, stmt.module_name, span=stmt.span)
                self._collect_module(imported_source)
                import_sources[stmt.module_name] = imported_source

            known_symbols: dict[tuple[str | None, str], str] = {}
            for stmt in program.statements:
                if not isinstance(stmt, IRLet):
                    continue
                sym = self._symbol_name(stmt.qualifier, stmt.name)
                known_symbols[(stmt.qualifier, stmt.name)] = sym
                known_symbols[(None, stmt.name)] = sym

            # Re-export imported module symbols so unqualified references like
            # Add4 and transitive qualified refs like Std.Println resolve in
            # modules that only import an intermediate module.
            for imported_source in import_sources.values():
                imported_module = self._modules[imported_source]
                imported_ns = self._module_namespace(imported_source)
                for (qual, name), imported_sym in imported_module.known_symbols.items():
                    resolved = imported_sym if "::" in imported_sym else f"{imported_ns}::{imported_sym}"
                    known_symbols.setdefault((qual, name), resolved)
                    known_symbols.setdefault((None, name), resolved)

            self._modules[source] = _ModuleInfo(
                source=source,
                module_name=source.stem,
                program=program,
                import_sources=import_sources,
                known_symbols=known_symbols,
            )
        finally:
            self._in_progress.discard(source)

    def _emit_cpp(self, root_source: Path) -> Path:
        root_source = root_source.resolve()
        if root_source not in self._modules:
            raise FleauxCppTranspilerError("Internal error: root module not collected.")

        lines: list[str] = [
            '#include "fleaux_runtime.hpp"',
            "",
            "using fleaux::runtime::operator|;",
            "",
            "namespace {",
            "struct _FleauxMissingBuiltin {",
            "    const char* name;",
            "    fleaux::runtime::Value operator()(fleaux::runtime::Value) const {",
            "        throw std::runtime_error(std::string(\"Builtin '\") + name + \"' is not yet implemented in cpp/fleaux_runtime.hpp\");",
            "    }",
            "};",
            "inline _FleauxMissingBuiltin _fleaux_missing_builtin(const char* name) {",
            "    return _FleauxMissingBuiltin{name};",
            "}",
            "}",
            "",
        ]

        ordered_sources = self._module_order(root_source)
        ordered = [self._modules[s] for s in ordered_sources]
        for module in ordered:
            lines.extend(self._emit_module(module))

        init_calls = [
            f"    {self._module_namespace(source)}::_fleaux_init_module();"
            for source in ordered_sources
        ]
        lines.extend([
            "int main(int argc, char** argv) {",
            "    fleaux::runtime::set_process_args(argc, argv);",
            *init_calls,
            "    return 0;",
            "}",
            "",
        ])

        out = root_source.with_name(f"fleaux_generated_module_{root_source.stem}.cpp")
        out.write_text("\n".join(lines).rstrip() + "\n", encoding="utf-8")
        return out

    def _module_order(self, root_source: Path) -> list[Path]:
        visited: set[Path] = set()
        ordered: list[Path] = []

        def visit(source: Path) -> None:
            source = source.resolve()
            if source in visited:
                return
            visited.add(source)
            module = self._modules[source]
            for imported_source in module.import_sources.values():
                visit(imported_source)
            ordered.append(source)

        visit(root_source)
        return ordered

    def _emit_module(self, module: _ModuleInfo) -> list[str]:
        ns = self._module_namespace(module.source)
        import_aliases = {
            name: self._module_namespace(src)
            for name, src in module.import_sources.items()
        }

        lines: list[str] = [
            f"namespace {ns} {{",
            f"// Source: {module.source}",
            "",
            "fleaux::runtime::Value _fleaux_run_module();",
            "void _fleaux_init_module();",
            "extern fleaux::runtime::Value _fleaux_last_value;",
            "",
        ]

        for stmt in module.program.statements:
            if isinstance(stmt, IRLet) and not stmt.is_builtin:
                symbol_name = self._symbol_name(stmt.qualifier, stmt.name)
                lines.append(
                    f"fleaux::runtime::Value {symbol_name}(fleaux::runtime::Value _fleaux_arg);"
                )
        lines.append("")

        for stmt in module.program.statements:
            if isinstance(stmt, IRLet):
                lines.extend(
                    self._emit_let(stmt, import_aliases, module.known_symbols)
                )

        lines.extend([
            "fleaux::runtime::Value _fleaux_run_module() {",
            "    fleaux::runtime::Value _fleaux_last_value = fleaux::runtime::make_null();",
        ])
        for stmt in module.program.statements:
            if not isinstance(stmt, IRExprStatement):
                continue
            compiled = self._compile_expr(
                stmt.expr,
                local_bindings={},
                import_aliases=import_aliases,
                known_symbols=module.known_symbols,
            )
            lines.append(f"    _fleaux_last_value = {compiled};")
        lines.extend([
            "    return _fleaux_last_value;",
            "}",
            "",
            "fleaux::runtime::Value _fleaux_last_value = fleaux::runtime::make_null();",
            "void _fleaux_init_module() {",
            "    _fleaux_last_value = _fleaux_run_module();",
            "}",
            "",
        ])

        lines.append(f"}} // namespace {ns}")
        lines.append("")
        return lines

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
            node_expr = self._builtin_node_expr(builtin_key)
            return [
                f"fleaux::runtime::Value {symbol_name}(fleaux::runtime::Value _fleaux_arg) {{",
                f"    return (_fleaux_arg | {node_expr});",
                "}",
                "",
            ]

        local_bindings = {p.name: self._sanitize(p.name) for p in let.params}
        body = self._compile_expr(let.body, local_bindings, import_aliases, known_symbols)

        setup: list[str] = []
        n = len(let.params)
        if n == 1:
            lone = local_bindings[let.params[0].name]
            setup.append(
                f"    fleaux::runtime::Value {lone} = fleaux::runtime::unwrap_singleton_arg(_fleaux_arg);"
            )
        elif n > 1:
            setup.append("    const fleaux::runtime::Value& _fleaux_args = _fleaux_arg;")
            for idx, p in enumerate(let.params):
                setup.append(
                    f"    fleaux::runtime::Value {local_bindings[p.name]} = fleaux::runtime::array_at(_fleaux_args, {idx});"
                )

        return [
            f"fleaux::runtime::Value {symbol_name}(fleaux::runtime::Value _fleaux_arg) {{",
            *setup,
            f"    return {body};",
            "}",
            "",
        ]

    def _compile_expr(
        self,
        expr: IRExpr,
        local_bindings: dict[str, str],
        import_aliases: dict[str, str],
        known_symbols: dict[tuple[str | None, str], str],
    ) -> str:
        if isinstance(expr, IRFlowExpr):
            lhs = self._compile_expr(expr.lhs, local_bindings, import_aliases, known_symbols)
            rhs = self._compile_call_target(expr.rhs, local_bindings, import_aliases, known_symbols)
            return f"({lhs} | {rhs})"

        if isinstance(expr, IRTupleExpr):
            items: list[str] = []
            for e in expr.items:
                if isinstance(e, IRNameRef):
                    # Function refs are passed as data via callable handles when
                    # they are tuple elements (e.g. (value, Func) -> Std.Apply).
                    if e.qualifier is not None:
                        ref = self._compile_qualified_ref(e, import_aliases, known_symbols)
                        items.append(f"fleaux::runtime::make_callable_ref({ref})")
                        continue
                    if e.name not in local_bindings:
                        ref = self._compile_name_ref(e, local_bindings, known_symbols)
                        items.append(f"fleaux::runtime::make_callable_ref({ref})")
                        continue
                items.append(self._compile_expr(e, local_bindings, import_aliases, known_symbols))
            return f"fleaux::runtime::make_tuple({', '.join(items)})"

        if isinstance(expr, IRConstant):
            return self._compile_constant(expr.val)

        if isinstance(expr, IRNameRef):
            if expr.qualifier is not None:
                return self._compile_qualified_ref(expr, import_aliases, known_symbols)
            if expr.name in local_bindings:
                return local_bindings[expr.name]
            return self._compile_name_ref(expr, local_bindings, known_symbols)

        raise FleauxCppTranspilerError(
            f"Cannot compile IR expression type '{type(expr).__name__}'.",
            hint="Use a flow expression, tuple expression, constant, or name reference.",
            span=getattr(expr, "span", None),
        )


    def _compile_constant(self, val: int | float | bool | str | None) -> str:
        if val is None:
            return "fleaux::runtime::make_null()"
        if isinstance(val, bool):
            return "fleaux::runtime::make_bool(true)" if val else "fleaux::runtime::make_bool(false)"
        if isinstance(val, int):
            return f"fleaux::runtime::make_int({val})"
        if isinstance(val, float):
            return f"fleaux::runtime::make_float({repr(val)})"
        if isinstance(val, str):
            return f"fleaux::runtime::make_string({json.dumps(val)})"
        raise FleauxCppTranspilerError(f"Unsupported constant value: {val!r}")

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
                raise FleauxCppTranspilerError(
                    f"Unsupported operator '{target.op}'.",
                    hint="Use a supported operator mapping.",
                    span=target.span,
                )
            return self._builtin_node_expr(builtin)

        if isinstance(target, IRNameRef):
            if target.qualifier is not None:
                return self._compile_qualified_ref(target, import_aliases, known_symbols)
            return self._compile_name_ref(target, local_bindings, known_symbols)

        raise FleauxCppTranspilerError(
            f"Cannot compile call target type '{type(target).__name__}'.",
            hint="Use an operator reference or name reference as the call target.",
            span=getattr(target, "span", None),
        )

    def _compile_qualified_ref(
        self,
        ref: IRNameRef,
        import_aliases: dict[str, str],
        known_symbols: dict[tuple[str | None, str], str],
    ) -> str:
        symbol_key = (ref.qualifier, ref.name)
        if symbol_key not in known_symbols:
            target = f"{ref.qualifier}.{ref.name}"
            candidates = [
                f"{qual}.{name}"
                for (qual, name) in known_symbols.keys()
                if qual is not None
            ]
            suggestion = self._best_levenshtein_candidate(target, candidates)
            hint = (
                f"Did you mean '{suggestion}'?"
                if suggestion is not None
                else "Check symbol spelling, module qualification, and imports."
            )
            raise FleauxCppTranspilerError(
                f"Unknown symbol '{target}'.",
                hint=hint,
                span=ref.span,
            )

        sym = known_symbols[symbol_key]
        if "::" in sym:
            return sym
        if ref.qualifier in import_aliases:
            return f"{import_aliases[ref.qualifier]}::{sym}"
        root = ref.qualifier.split(".")[0]
        if root in import_aliases:
            return f"{import_aliases[root]}::{sym}"
        return sym

    def _compile_name_ref(
        self,
        ref: IRNameRef,
        local_bindings: dict[str, str],
        known_symbols: dict[tuple[str | None, str], str],
    ) -> str:
        name = ref.name
        if name in local_bindings:
            return local_bindings[name]
        if (None, name) in known_symbols:
            return known_symbols[(None, name)]

        # Suggest from local bindings and module-local unqualified symbols.
        # Imported symbols are commonly stored with '::' namespace prefixes in
        # known_symbols[(None, name)] and are excluded to avoid misleading
        # suggestions like 'Add' for a misspelling of local 'MyAdd'.
        candidates: list[str] = list(local_bindings.keys())
        for (qual, candidate_name), resolved in known_symbols.items():
            if qual is not None:
                continue
            if "::" in resolved:
                continue
            candidates.append(candidate_name)

        suggestion = self._best_levenshtein_candidate(name, candidates)
        if suggestion is None:
            suggestion = self._best_qualified_candidate_for_unqualified(name, known_symbols)

        hint = (
            f"Did you mean '{suggestion}'?"
            if suggestion is not None
            else "Check symbol spelling or define/import the missing symbol."
        )
        raise FleauxCppTranspilerError(
            f"Unknown symbol '{name}'.",
            hint=hint,
            span=ref.span,
        )

    @staticmethod
    def _best_levenshtein_candidate(target: str, candidates: list[str]) -> str | None:
        normalized = target.strip()
        if not normalized:
            return None

        unique_candidates = sorted({c for c in candidates if c and c != normalized})
        if not unique_candidates:
            return None

        best = min(
            unique_candidates,
            key=lambda c: (FleauxCppTranspiler._levenshtein_distance(normalized, c), len(c), c),
        )
        dist = FleauxCppTranspiler._levenshtein_distance(normalized, best)
        max_dist = 1 if len(normalized) <= 4 else 2 if len(normalized) <= 8 else 3
        return best if dist <= max_dist else None

    @staticmethod
    def _best_qualified_candidate_for_unqualified(
        target: str,
        known_symbols: dict[tuple[str | None, str], str],
    ) -> str | None:
        normalized = target.strip()
        if not normalized:
            return None

        qualified = [
            (f"{qual}.{name}", name)
            for (qual, name) in known_symbols.keys()
            if qual is not None
        ]
        if not qualified:
            return None

        best_display: str | None = None
        best_name: str | None = None
        best_dist: int | None = None
        for display, tail_name in qualified:
            dist = FleauxCppTranspiler._levenshtein_distance(normalized, tail_name)
            if (
                best_dist is None
                or dist < best_dist
                or (dist == best_dist and len(tail_name) < len(best_name or tail_name))
                or (dist == best_dist and tail_name == (best_name or tail_name) and display < (best_display or display))
            ):
                best_display = display
                best_name = tail_name
                best_dist = dist

        if best_display is None or best_name is None or best_dist is None:
            return None

        max_dist = 1 if len(best_name) <= 4 else 2 if len(best_name) <= 8 else 3
        return best_display if best_dist <= max_dist else None

    @staticmethod
    def _levenshtein_distance(a: str, b: str) -> int:
        if a == b:
            return 0
        if not a:
            return len(b)
        if not b:
            return len(a)

        prev = list(range(len(b) + 1))
        for i, ca in enumerate(a, start=1):
            curr = [i]
            for j, cb in enumerate(b, start=1):
                cost = 0 if ca == cb else 1
                curr.append(
                    min(
                        prev[j] + 1,
                        curr[j - 1] + 1,
                        prev[j - 1] + cost,
                    )
                )
            prev = curr
        return prev[-1]

    def _builtin_node_expr(self, name: str) -> str:
        mapped = self.BUILTIN_NAME_MAP.get(name, name)
        if mapped in self.CPP_RUNTIME_BUILTINS:
            return f"fleaux::runtime::{mapped}{{}}"
        quoted = json.dumps(name)
        return f"_fleaux_missing_builtin({quoted})"

    def _resolve_import_source(self, source: Path, module_name: str, *, span=None) -> Path:
        local_source = source.with_name(f"{module_name}.fleaux").resolve()
        if local_source.exists():
            return local_source

        bundled_source = self.BUNDLED_MODULE_SOURCES.get(module_name)
        if bundled_source is not None and bundled_source.exists():
            return bundled_source.resolve()

        bundled_hint = ""
        if bundled_source is not None:
            bundled_hint = f" and bundled module '{bundled_source}'"
        raise FleauxCppTranspilerError(
            f"Unable to resolve import '{module_name}' from '{source}'. "
            f"Looked for '{local_source}'{bundled_hint}.",
            hint="Place the module beside the importing source file, or use a bundled module such as 'Std'.",
            span=span,
        )

    @staticmethod
    def _module_namespace(source: Path) -> str:
        return f"fleaux_gen_{FleauxCppTranspiler._sanitize(source.stem)}"

    @staticmethod
    def _symbol_name(qualifier: str | None, name: str) -> str:
        safe = FleauxCppTranspiler._sanitize(name)
        if qualifier is None:
            return safe
        return f"{FleauxCppTranspiler._sanitize(qualifier)}_{safe}"

    @staticmethod
    def _sanitize(raw: str) -> str:
        safe = re.sub(r"[^a-zA-Z0-9_]", "_", raw)
        if not safe:
            safe = "_"
        if safe[0].isdigit():
            safe = f"_{safe}"
        if keyword.iskeyword(safe) or safe in _CPP_KEYWORDS:
            safe = f"{safe}_"
        return safe


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Transpile a Fleaux source file to a C++ module."
    )
    parser.add_argument(
        "source", nargs="?", default="test.fleaux", help="Input .fleaux file path"
    )
    args = parser.parse_args()

    output = FleauxCppTranspiler().process(args.source)
    print(output)


if __name__ == "__main__":
    main()



