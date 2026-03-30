from __future__ import annotations

import argparse
import keyword
import re
from pathlib import Path

from fleaux_ast import (
    IRCallTarget,
    IRConstant,
    IRExpr,
    IRExprStatement,
    IRFlowExpr,
    IRImport,
    IRLet,
    IRNameRef,
    IROperatorRef,
    IRProgram,
    IRSimpleType,
    IRTupleExpr,
    IRTupleType,
    IRType,
)
from fleaux_diagnostics import format_diagnostic
from fleaux_lowering import lower
from fleaux_parser import parse_file


class FleauxCppTranspilerError(Exception):
    """Exception for C++ transpilation errors."""
    def __init__(self, message: str, *, hint: str | None = None):
        self.stage = "cpp_transpile"
        self.message = message
        self.hint = hint
        super().__init__(
            format_diagnostic(
                stage=self.stage,
                message=self.message,
                hint=self.hint,
            )
        )


class FleauxCppTranspiler:
    """Transpiles Fleaux IR to C++20 using the fleaux_runtime.hpp library."""

    OPERATOR_TO_BUILTIN = {
        "+": "fleaux::Add",
        "-": "fleaux::Subtract",
        "*": "fleaux::Multiply",
        "/": "fleaux::Divide",
        "%": "fleaux::Mod",
        "^": "fleaux::Pow",
        "==": "fleaux::Equal",
        "!=": "fleaux::NotEqual",
        "<": "fleaux::LessThan",
        ">": "fleaux::GreaterThan",
        ">=": "fleaux::GreaterOrEqual",
        "<=": "fleaux::LessOrEqual",
        "!": "fleaux::Not",
        "&&": "fleaux::And",
        "||": "fleaux::Or",
    }

    IMPLEMENTED_BUILTINS = {
        "Add", "Subtract", "Multiply", "Divide", "Mod", "Pow",
        "Equal", "NotEqual", "LessThan", "GreaterThan", "GreaterOrEqual", "LessOrEqual",
        "And", "Or", "Not",
        "Sqrt", "Sin", "Cos", "Tan",
        "Println", "Printf",
    }
    def __init__(self):
        self._generated: dict[str, Path] = {}
        self._in_progress: set[str] = set()

    def process(self, filename: str | Path) -> Path:
        return self._process_file(Path(filename).resolve())

    def _process_file(self, source: Path) -> Path:
        module_name = source.stem

        if module_name in self._generated:
            return self._generated[module_name]
        if module_name in self._in_progress:
            raise FleauxCppTranspilerError(
                f"Cyclic import detected while transpiling '{module_name}'.",
                hint="Break the import cycle by moving shared definitions into a third module.",
            )

        self._in_progress.add(module_name)

        # 1. Parse -> model -> IR
        try:
            parsed_model = parse_file(source)
            program: IRProgram = lower(parsed_model)
        except Exception as exc:
            raise FleauxCppTranspilerError(
                f"Failed to parse/lower '{source.name}': {exc}",
                hint="Fix parse or lowering errors in the source module, then transpile again.",
            ) from exc

        # 2. Recursively transpile imports first
        import_aliases: dict[str, str] = {}
        import_headers: dict[str, str] = {}

        for stmt in program.statements:
            if not isinstance(stmt, IRImport):
                continue
            if stmt.module_name == "StdBuiltins":
                continue
            imported_source = source.with_name(f"{stmt.module_name}.fleaux")
            imported_output = self._process_file(imported_source)
            header_name = imported_output.with_suffix(".hpp").name
            import_headers[stmt.module_name] = header_name
            import_aliases[stmt.module_name] = self._sanitize(stmt.module_name)

        # 3. Build symbol table from let-bindings in this module
        known_symbols: dict[tuple[str | None, str], str] = {}
        for stmt in program.statements:
            if not isinstance(stmt, IRLet):
                continue
            sym = self._symbol_name(stmt.qualifier, stmt.name)
            known_symbols[(stmt.qualifier, stmt.name)] = sym
            known_symbols[(None, stmt.name)] = sym

        # 4. Emit C++20 header
        header_lines = self._emit_header(
            module_name, program, import_headers, known_symbols
        )

        header_path = source.with_name(f"fleaux_generated_{module_name}.hpp")
        header_path.write_text("\n".join(header_lines).rstrip() + "\n", encoding="utf-8")

        # 5. Emit C++20 implementation
        impl_lines = self._emit_impl(
            module_name, program, import_aliases, known_symbols
        )

        impl_path = source.with_name(f"fleaux_generated_{module_name}.cpp")
        impl_path.write_text("\n".join(impl_lines).rstrip() + "\n", encoding="utf-8")

        self._generated[module_name] = header_path
        self._in_progress.remove(module_name)
        return header_path

    # ── Header generation ────────────────────────────────────────────────────

    def _emit_header(
        self,
        module_name: str,
        program: IRProgram,
        import_headers: dict[str, str],
        known_symbols: dict[tuple[str | None, str], str],
    ) -> list[str]:
        guard = f"FLEAUX_GENERATED_{module_name.upper()}_HPP"
        lines = [
            f"#ifndef {guard}",
            f"#define {guard}",
            "",
            "#include <tuple>",
            "#include <functional>",
            "#include <iostream>",
            "#include <string>",
            "#include <vector>",
            "#include <cmath>",
            "#include \"fleaux_runtime.hpp\"",
            "",
        ]

        # Include imported headers
        for imported_module, header_file in sorted(import_headers.items()):
            lines.append(f"#include \"{header_file}\"")
        if import_headers:
            lines.append("")

        lines.append("namespace fleaux {")
        lines.append("")

        # Forward declare all let-bindings
        for stmt in program.statements:
            if isinstance(stmt, IRLet):
                lines.append(f"    extern fleaux::FlowNode {self._symbol_name(stmt.qualifier, stmt.name)};")

        if any(isinstance(stmt, IRLet) for stmt in program.statements):
            lines.append("")

        lines.append("} // namespace fleaux")
        lines.append("")
        lines.append(f"#endif // {guard}")

        return lines

    # ── Implementation generation ────────────────────────────────────────────

    def _emit_impl(
        self,
        module_name: str,
        program: IRProgram,
        import_aliases: dict[str, str],
        known_symbols: dict[tuple[str | None, str], str],
    ) -> list[str]:
        lines = [
            f"#include \"fleaux_generated_{module_name}.hpp\"",
            "",
            "namespace fleaux {",
            "",
        ]

        for stmt in program.statements:
            if isinstance(stmt, IRLet):
                lines.extend(
                    self._emit_let(stmt, import_aliases, known_symbols)
                )
            elif isinstance(stmt, IRExprStatement):
                # Top-level expression - compile but don't emit directly
                compiled = self._compile_expr(
                    stmt.expr, {}, import_aliases, known_symbols
                )
                lines.append(f"// Top-level expression: {compiled}")
                lines.append("")

        lines.append("} // namespace fleaux")

        return lines

    def _emit_let(
        self,
        let: IRLet,
        import_aliases: dict[str, str],
        known_symbols: dict[tuple[str | None, str], str],
    ) -> list[str]:
        symbol_name = self._symbol_name(let.qualifier, let.name)

        if let.is_builtin:
            return [
                f"fleaux::FlowNode {symbol_name} = make_builtin_node(\"{let.name}\");",
                "",
            ]

        # Non-builtin: generate a lambda-based flow node
        lines = [f"fleaux::FlowNode {symbol_name} = fleaux::FlowNode([](const fleaux::FlexValue& input) -> fleaux::FlexValue {{"]

        # Build parameter extraction code
        param_names = [self._sanitize(p.name) for p in let.params]
        local_bindings = {p.name: pname for p, pname in zip(let.params, param_names)}

        if let.params:
            if len(let.params) == 1:
                lines.append(f"    auto {param_names[0]} = input;")
            else:
                # For multiple params, assume input is a tuple
                lines.append(f"    try {{")
                for idx, pname in enumerate(param_names):
                    lines.append(f"        auto {pname} = std::get<{idx}>(std::any_cast<std::tuple<fleaux::FlexValue" + (", fleaux::FlexValue" * (len(param_names) - 1)) + f">>(input));")
                lines.append(f"    }} catch (...) {{")
                lines.append(f"        throw std::runtime_error(\"{let.name}: Invalid argument tuple\");")
                lines.append(f"    }}")
        else:
            lines.append(f"    (void)input; // unused")

        # Compile the body
        body = self._compile_expr(let.body, local_bindings, import_aliases, known_symbols)
        lines.append(f"    return {body};")
        lines.append(f"}});")
        lines.append("")

        return lines

    # ── Expression compilation ────────────────────────────────────────────────

    def _compile_expr(
        self,
        expr: IRExpr,
        local_bindings: dict[str, str],
        import_aliases: dict[str, str],
        known_symbols: dict[tuple[str | None, str], str],
    ) -> str:
        if isinstance(expr, IRFlowExpr):
            # Compile as (lhs) | (rhs)
            # If lhs is a tuple, it needs to be wrapped in FlexValue
            lhs_compiled = self._compile_expr(expr.lhs, local_bindings, import_aliases, known_symbols)
            if isinstance(expr.lhs, IRTupleExpr):
                lhs_compiled = f"fleaux::FlexValue({lhs_compiled})"
            rhs = self._compile_call_target(expr.rhs, local_bindings, import_aliases, known_symbols)
            return f"({lhs_compiled} | {rhs})"

        if isinstance(expr, IRTupleExpr):
            return self._compile_tuple(expr, local_bindings, import_aliases, known_symbols)

        if isinstance(expr, IRConstant):
            return self._compile_constant(expr.val)

        if isinstance(expr, IRNameRef):
            if expr.qualifier is not None:
                return self._compile_qualified_ref(expr, import_aliases, known_symbols)
            return self._compile_name_ref(expr.name, local_bindings, known_symbols)

        raise FleauxCppTranspilerError(
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

    def _compile_constant(self, val) -> str:
        if isinstance(val, bool):
            return "true" if val else "false"
        if isinstance(val, str):
            # Escape string
            escaped = val.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
            return f"fleaux::FlexValue(std::string(\"{escaped}\"))"
        if isinstance(val, (int, float)):
            # Convert to double to match Fleaux Number type
            return f"fleaux::FlexValue({float(val)})"
        if val is None:
            return "fleaux::FlexValue()"
        return f"fleaux::FlexValue({val})"

    def _compile_call_target(
        self,
        target: IRCallTarget,
        local_bindings: dict[str, str],
        import_aliases: dict[str, str],
        known_symbols: dict[tuple[str | None, str], str],
    ) -> str:
        if isinstance(target, IROperatorRef):
            cpp_func = self.OPERATOR_TO_BUILTIN.get(target.op)
            if cpp_func is None:
                raise FleauxCppTranspilerError(
                    f"Unsupported operator '{target.op}'.",
                    hint="Use a supported operator mapping in OPERATOR_TO_BUILTIN.",
                )
            return cpp_func

        if isinstance(target, IRNameRef):
            if target.qualifier is not None:
                return self._compile_qualified_ref(target, import_aliases, known_symbols)
            return self._compile_name_ref(target.name, local_bindings, known_symbols)

        raise FleauxCppTranspilerError(
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
        # All transpiled symbols are in the fleaux namespace
        return f"fleaux::{sym}"

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

    # ── Type conversion ──────────────────────────────────────────────────────

    def _type_to_cpp(self, ir_type: IRType) -> str:
        """Convert IR type to C++20 type string."""
        if isinstance(ir_type, IRSimpleType):
            cpp_type_map = {
                "Number": "double",
                "String": "std::string",
                "Bool": "bool",
                "Null": "std::nullptr_t",
                "Any": "fleaux::FlexValue",
            }
            base = cpp_type_map.get(ir_type.name, ir_type.name)
            if ir_type.variadic:
                return f"std::vector<{base}>"
            return base

        if isinstance(ir_type, IRTupleType):
            inner_types = [self._type_to_cpp(t) for t in ir_type.types]
            return f"std::tuple<{', '.join(inner_types)}>"

        raise FleauxCppTranspilerError(
            f"Unknown type: {ir_type}",
            hint="Use IRSimpleType or IRTupleType.",
        )

    # ── Helpers ───────────────────────────────────────────────────────────────

    @staticmethod
    def _symbol_name(qualifier: str | None, name: str) -> str:
        safe = FleauxCppTranspiler._sanitize(name)
        if qualifier is None:
            return safe
        return f"{FleauxCppTranspiler._sanitize(qualifier)}_{safe}"

    @staticmethod
    def _tuple_literal(items: list[str]) -> str:
        if len(items) == 0:
            return "std::make_tuple()"
        return f"std::make_tuple({', '.join(items)})"

    @staticmethod
    def _sanitize(raw: str) -> str:
        safe = re.sub(r"[^a-zA-Z0-9_]", "_", raw)
        if not safe:
            safe = "_"
        if safe[0].isdigit():
            safe = f"_{safe}"
        # C++ keywords to avoid
        cpp_keywords = {
            "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor",
            "bool", "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t",
            "class", "compl", "concept", "const", "consteval", "constexpr", "constinit",
            "const_cast", "continue", "co_await", "co_return", "co_yield", "decltype",
            "default", "delete", "do", "double", "dynamic_cast", "else", "enum",
            "explicit", "export", "extern", "false", "float", "for", "friend", "goto",
            "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept",
            "not", "not_eq", "nullptr", "operator", "or", "or_eq", "private", "protected",
            "public", "register", "reinterpret_cast", "requires", "return", "short",
            "signed", "sizeof", "static", "static_assert", "static_cast", "struct",
            "switch", "template", "this", "thread_local", "throw", "true", "try",
            "typedef", "typeid", "typename", "union", "unsigned", "using", "virtual",
            "void", "volatile", "wchar_t", "while", "xor", "xor_eq",
        }
        if safe in cpp_keywords:
            safe = f"{safe}_"
        return safe


def main() -> None:
    parser = argparse.ArgumentParser(description="Transpile Fleaux source to C++.")
    parser.add_argument("source", nargs="?", default="test.fleaux", help="Input .fleaux file path")
    args = parser.parse_args()

    out = FleauxCppTranspiler().process(args.source)
    print(out)


if __name__ == "__main__":
    main()

