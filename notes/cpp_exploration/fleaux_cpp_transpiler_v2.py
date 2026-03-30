"""
fleaux_cpp_transpiler_v2.py – Transpile Fleaux IR to strongly-typed C++20 code.

This transpiler converts the IR representation into C++20 with:
- Actual tuple types based on IR type information
- No type erasure (std::any)
- Template-based generic functions
- Strong compile-time type safety
"""
from __future__ import annotations

import argparse
import re
from pathlib import Path
from typing import Any

from fleaux_ast import (
    IRProgram, IRImport, IRLet, IRExprStatement,
    IRFlowExpr, IRTupleExpr, IRConstant, IRNameRef, IROperatorRef,
    IRExpr, IRCallTarget, IRSimpleType, IRTupleType, IRType,
)
from fleaux_diagnostics import format_diagnostic
from fleaux_lowering import lower
from fleaux_parser import parse_file


class FleauxCppV2TranspilerError(Exception):
    """Exception for C++ transpilation errors."""
    def __init__(self, message: str, *, hint: str | None = None):
        self.stage = "cpp_v2_transpile"
        self.message = message
        self.hint = hint
        super().__init__(
            format_diagnostic(
                stage=self.stage,
                message=self.message,
                hint=self.hint,
            )
        )


class FleauxCppV2Transpiler:
    """Transpiles Fleaux IR to strongly-typed C++20 code."""

    OPERATOR_TO_FUNCTION = {
        "+": "fleaux::operator_add",
        "-": "fleaux::operator_subtract",
        "*": "fleaux::operator_multiply",
        "/": "fleaux::operator_divide",
        "%": "fleaux::operator_mod",
        "^": "fleaux::operator_pow",
        "==": "fleaux::operator_equal",
        "!=": "fleaux::operator_not_equal",
        "<": "fleaux::operator_less_than",
        ">": "fleaux::operator_greater_than",
        ">=": "fleaux::operator_greater_or_equal",
        "<=": "fleaux::operator_less_or_equal",
        "!": "fleaux::operator_not",
        "&&": "fleaux::operator_and",
        "||": "fleaux::operator_or",
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
            raise FleauxCppV2TranspilerError(
                f"Cyclic import detected while transpiling '{module_name}'.",
                hint="Break the import cycle by moving shared definitions into a third module.",
            )

        self._in_progress.add(module_name)

        # 1. Parse -> model -> IR
        try:
            parsed_model = parse_file(source)
            program: IRProgram = lower(parsed_model)
        except Exception as exc:
            raise FleauxCppV2TranspilerError(
                f"Failed to parse/lower '{source.name}': {exc}",
                hint="Fix parse or lowering errors in the source module, then transpile again.",
            ) from exc

        # 2. Recursively transpile imports first
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

        # 3. Build symbol table from let-bindings in this module
        known_symbols: dict[tuple[str | None, str], str] = {}
        for stmt in program.statements:
            if not isinstance(stmt, IRLet):
                continue
            sym = self._symbol_name(stmt.qualifier, stmt.name)
            known_symbols[(stmt.qualifier, stmt.name)] = sym
            known_symbols[(None, stmt.name)] = sym

        # 4. Emit C++20 header
        header_lines = self._emit_header(module_name, program, import_headers, known_symbols)

        header_path = source.with_name(f"fleaux_generated_{module_name}_v2.hpp")
        header_path.write_text("\n".join(header_lines).rstrip() + "\n", encoding="utf-8")

        # 5. Emit C++20 implementation
        impl_lines = self._emit_impl(module_name, program, known_symbols)

        impl_path = source.with_name(f"fleaux_generated_{module_name}_v2.cpp")
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
        guard = f"FLEAUX_GENERATED_{module_name.upper()}_V2_HPP"
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
            "#include \"fleaux_runtime_v2.hpp\"",
            "",
        ]

        # Include imported headers
        for imported_module, header_file in sorted(import_headers.items()):
            lines.append(f"#include \"{header_file}\"")
        if import_headers:
            lines.append("")

        lines.append("namespace fleaux {")
        lines.append("")

        # Generate declarations for all let-bindings
        for stmt in program.statements:
            if isinstance(stmt, IRLet):
                lines.extend(self._emit_let_decl(stmt, known_symbols))

        if any(isinstance(stmt, IRLet) for stmt in program.statements):
            lines.append("")

        lines.append("} // namespace fleaux")
        lines.append("")
        lines.append(f"#endif // {guard}")

        return lines

    def _emit_let_decl(
        self,
        let: IRLet,
        known_symbols: dict[tuple[str | None, str], str],
    ) -> list[str]:
        """Generate forward declarations for let-bindings."""
        symbol_name = self._symbol_name(let.qualifier, let.name)
        return_type = self._type_to_cpp(let.return_type)
        param_types = [self._type_to_cpp(p.type) for p in let.params]

        if let.params:
            args = ", ".join(param_types)
            lines = [
                f"// {symbol_name}: ({args}) -> {return_type}",
                f"extern std::function<{return_type}({args})> {symbol_name};",
                "",
            ]
        else:
            lines = [
                f"// {symbol_name}: () -> {return_type}",
                f"extern std::function<{return_type}()> {symbol_name};",
                "",
            ]
        return lines

    # ── Implementation generation ────────────────────────────────────────────

    def _emit_impl(
        self,
        module_name: str,
        program: IRProgram,
        known_symbols: dict[tuple[str | None, str], str],
    ) -> list[str]:
        lines = [
            f"#include \"fleaux_generated_{module_name}_v2.hpp\"",
            "",
            "namespace fleaux {",
            "",
        ]

        for stmt in program.statements:
            if isinstance(stmt, IRLet):
                lines.extend(self._emit_let(stmt, known_symbols))
            elif isinstance(stmt, IRExprStatement):
                # Top-level expression
                compiled = self._compile_expr(stmt.expr, {}, known_symbols)
                lines.append(f"// Top-level expression: {compiled}")
                lines.append("")

        lines.append("} // namespace fleaux")

        return lines

    def _emit_let(
        self,
        let: IRLet,
        known_symbols: dict[tuple[str | None, str], str],
    ) -> list[str]:
        symbol_name = self._symbol_name(let.qualifier, let.name)
        return_type = self._type_to_cpp(let.return_type)
        param_types = [self._type_to_cpp(p.type) for p in let.params]
        param_names = [self._sanitize(p.name) for p in let.params]

        if let.is_builtin:
            # For builtins, generate a simple assignment
            builtin_func = self._builtin_to_function(let.name)
            if let.params:
                args = ", ".join(param_names)
                lines = [
                    f"std::function<{return_type}({', '.join(param_types)})> {symbol_name} =",
                    f"    [](const auto&... args) {{ return {builtin_func}(std::make_tuple(args...)); }};",
                    "",
                ]
            else:
                lines = [
                    f"std::function<{return_type}()> {symbol_name} = []() {{ return {builtin_func}(); }};",
                    "",
                ]
            return lines

        # Non-builtin: generate lambda
        local_bindings = {p.name: pname for p, pname in zip(let.params, param_names)}

        if let.params:
            args_sig = ", ".join(f"const auto& {pname}" for pname in param_names)
            lines = [
                f"std::function<{return_type}({', '.join(param_types)})> {symbol_name} =",
                f"    [](const auto&... args) {{",
            ]

            # Unpack arguments
            for idx, (pname, ptype) in enumerate(zip(param_names, param_types)):
                lines.append(f"        auto {pname} = std::get<{idx}>(std::tuple_cat(args...));")

            # Compile body
            body = self._compile_expr(let.body, local_bindings, known_symbols)
            lines.append(f"        return {body};")
            lines.append(f"    }};")
        else:
            body = self._compile_expr(let.body, {}, known_symbols)
            lines = [
                f"std::function<{return_type}()> {symbol_name} =",
                f"    []() {{ return {body}; }};",
            ]

        lines.append("")
        return lines

    # ── Expression compilation ────────────────────────────────────────────────

    def _compile_expr(
        self,
        expr: IRExpr,
        local_bindings: dict[str, str],
        known_symbols: dict[tuple[str | None, str], str],
    ) -> str:
        if isinstance(expr, IRFlowExpr):
            # (tuple) -> function
            lhs = self._compile_expr(expr.lhs, local_bindings, known_symbols)
            rhs = self._compile_call_target(expr.rhs, local_bindings, known_symbols)
            return f"({lhs} | {rhs})"

        if isinstance(expr, IRTupleExpr):
            return self._compile_tuple(expr, local_bindings, known_symbols)

        if isinstance(expr, IRConstant):
            return self._compile_constant(expr.val)

        if isinstance(expr, IRNameRef):
            return self._compile_name_ref(expr, local_bindings, known_symbols)

        raise FleauxCppV2TranspilerError(
            f"Cannot compile IR expression type '{type(expr).__name__}'.",
            hint="Use a flow expression, tuple expression, constant, or name reference.",
        )

    def _compile_tuple(
        self,
        expr: IRTupleExpr,
        local_bindings: dict[str, str],
        known_symbols: dict[tuple[str | None, str], str],
    ) -> str:
        items = [
            self._compile_expr(e, local_bindings, known_symbols)
            for e in expr.items
        ]
        if len(items) == 1:
            return f"std::make_tuple({items[0]})"
        return f"std::make_tuple({', '.join(items)})"

    def _compile_constant(self, val: Any) -> str:
        if isinstance(val, bool):
            return "true" if val else "false"
        if isinstance(val, str):
            escaped = val.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
            return f'std::string("{escaped}")'
        if isinstance(val, (int, float)):
            return str(float(val))
        if val is None:
            return "0.0"  # Default value
        return str(val)

    def _compile_call_target(
        self,
        target: IRCallTarget,
        local_bindings: dict[str, str],
        known_symbols: dict[tuple[str | None, str], str],
    ) -> str:
        if isinstance(target, IROperatorRef):
            return self.OPERATOR_TO_FUNCTION.get(target.op, f"unknown_op_{target.op}")

        if isinstance(target, IRNameRef):
            return self._compile_name_ref(target, local_bindings, known_symbols)

        raise FleauxCppV2TranspilerError(
            f"Cannot compile call target type '{type(target).__name__}'.",
        )

    def _compile_name_ref(
        self,
        ref: IRNameRef,
        local_bindings: dict[str, str],
        known_symbols: dict[tuple[str | None, str], str],
    ) -> str:
        if ref.qualifier is None and ref.name in local_bindings:
            return local_bindings[ref.name]

        key = (ref.qualifier, ref.name)
        if key in known_symbols:
            return known_symbols[key]

        if (None, ref.name) in known_symbols:
            return known_symbols[(None, ref.name)]

        return self._sanitize(ref.name)

    # ── Type conversion ──────────────────────────────────────────────────────

    def _type_to_cpp(self, ir_type: IRType) -> str:
        """Convert IR type to C++20 type string."""
        if isinstance(ir_type, IRSimpleType):
            cpp_type_map = {
                "Number": "double",
                "String": "std::string",
                "Bool": "bool",
                "Null": "std::nullptr_t",
                "Any": "double",  # For now, map Any to double (most common case)
            }
            base = cpp_type_map.get(ir_type.name, ir_type.name)
            # For variadic, use vector
            if ir_type.variadic:
                return f"std::vector<{base}>"
            return base

        if isinstance(ir_type, IRTupleType):
            inner_types = [self._type_to_cpp(t) for t in ir_type.types]
            # Check if any type is variadic - if so, skip complex tuple generation
            if any(isinstance(t, IRSimpleType) and t.variadic for t in ir_type.types):
                # For variadic tuples, use a vector of doubles
                return f"std::vector<double>"
            return f"std::tuple<{', '.join(inner_types)}>"

        raise FleauxCppV2TranspilerError(
            f"Unknown type: {ir_type}",
            hint="Use IRSimpleType or IRTupleType.",
        )

    def _builtin_to_function(self, name: str) -> str:
        """Map builtin name to C++ function."""
        builtin_map = {
            "Add": "fleaux::operator_add",
            "Subtract": "fleaux::operator_subtract",
            "Multiply": "fleaux::operator_multiply",
            "Divide": "fleaux::operator_divide",
            "Mod": "fleaux::operator_mod",
            "Pow": "fleaux::operator_pow",
            "Println": "fleaux::println",
            "Sqrt": "fleaux::sqrt_op",
            "Sin": "fleaux::sin_op",
            "Cos": "fleaux::cos_op",
            "Tan": "fleaux::tan_op",
            "ElementAt": "fleaux::element_at",
            "Wrap": "fleaux::wrap_value",
            "Unwrap": "fleaux::unwrap_value",
            "Equal": "fleaux::operator_equal",
            "NotEqual": "fleaux::operator_not_equal",
            "LessThan": "fleaux::operator_less_than",
            "GreaterThan": "fleaux::operator_greater_than",
            "LessOrEqual": "fleaux::operator_less_or_equal",
            "GreaterOrEqual": "fleaux::operator_greater_or_equal",
            "And": "fleaux::operator_and",
            "Or": "fleaux::operator_or",
            "Not": "fleaux::operator_not",
        }
        return builtin_map.get(name, f"unknown_builtin_{name}")

    # ── Helpers ───────────────────────────────────────────────────────────────

    @staticmethod
    def _symbol_name(qualifier: str | None, name: str) -> str:
        safe = FleauxCppV2Transpiler._sanitize(name)
        if qualifier is None:
            return safe
        return f"{FleauxCppV2Transpiler._sanitize(qualifier)}_{safe}"

    @staticmethod
    def _sanitize(raw: str) -> str:
        safe = re.sub(r"[^a-zA-Z0-9_]", "_", raw)
        if not safe:
            safe = "_"
        if safe[0].isdigit():
            safe = f"_{safe}"

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
    parser = argparse.ArgumentParser(
        description="Transpile a Fleaux source file to strongly-typed C++20."
    )
    parser.add_argument(
        "source", nargs="?", default="test.fleaux", help="Input .fleaux file path"
    )
    args = parser.parse_args()

    output = FleauxCppV2Transpiler().process(args.source)
    print(output)


if __name__ == "__main__":
    main()

