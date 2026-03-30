from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
import re

from fleaux_diagnostics import format_diagnostic


class FleauxSyntaxError(Exception):
    def __init__(self, msg: str, src: str, line: int, col: int, hint: str | None = None):
        self.msg, self.src, self.line, self.col = msg, src, line, col
        self.stage = "parse"
        self.hint = hint
        super().__init__(self._fmt())

    def _fmt(self) -> str:
        lines = self.src.splitlines() or [""]
        bad = lines[self.line - 1] if 1 <= self.line <= len(lines) else ""
        return format_diagnostic(
            stage=self.stage,
            message=self.msg,
            line=self.line,
            col=self.col,
            source_line=bad,
            hint=self.hint,
        )


@dataclass
class Tok:
    k: str
    v: str
    line: int
    col: int


_TOKEN_RE = re.compile(
    r"""
(?P<WS>[ \t\r]+)
|(?P<NL>\n)
|(?P<COMMENT>//[^\n]*)
|(?P<MULTI>\.\.\.|->|::|==|!=|>=|<=|&&|\|\|)
|(?P<STRING>"([^"\\]|\\.)*")
|(?P<NUMBER>\d+(\.\d+)?([eE][+-]?\d+)?)
|(?P<IDENT>[A-Za-z_][A-Za-z0-9_]*)
|(?P<SINGLE>[()\[\],:;.=+\-*/%^!<>])
    """,
    re.VERBOSE,
)


def _lex(src: str) -> list[Tok]:
    out: list[Tok] = []
    i = 0
    line = 1
    col = 1
    while i < len(src):
        m = _TOKEN_RE.match(src, i)
        if not m:
            raise FleauxSyntaxError(f"Unexpected character '{src[i]}'", src, line, col)
        text = m.group(0)
        kind = m.lastgroup
        if kind == "NL":
            line += 1
            col = 1
        elif kind in ("WS", "COMMENT"):
            col += len(text)
        else:
            k = text if kind in ("MULTI", "SINGLE") else kind
            if kind == "STRING":
                v = bytes(text[1:-1], "utf-8").decode("unicode_escape")
            else:
                v = text
            if kind == "IDENT" and text in ("True", "False"):
                k = "BOOL"
            if kind == "IDENT" and text == "null":
                k = "NULL"
            out.append(Tok(k, v, line, col))
            col += len(text)
        i = m.end()
    out.append(Tok("EOF", "", line, col))
    return out


@dataclass
class Program: statements: list[object]
@dataclass
class ImportStatement: module_name: str
@dataclass
class Qualifier: qualifier: str
@dataclass
class QualifiedId: qualifier: Qualifier; id: str
@dataclass
class Parameter: param_name: str; type: object
@dataclass
class ParameterDeclList: params: list[Parameter]
@dataclass
class TypeList: types: list[object]
@dataclass
class LetStatement: id: object; params: ParameterDeclList; rtype: object; expr: object
@dataclass
class ExpressionStatement: expr: object
@dataclass
class Expression: expr: object
@dataclass
class FlowExpression: lhs: object; rhs: list[object]
@dataclass
class Primary: base: object; extra: list[object] = field(default_factory=list)
@dataclass
class Atom: inner: object | None = None; constant: object | None = None; qualified_var: object | None = None; var: str | None = None
@dataclass
class DelimitedExpression: items: list[Expression]
@dataclass
class Constant: val: object


class Parser:
    KW = {"let", "import", "Number", "String", "Bool", "Null", "Any", "Tuple", "__builtin__"}
    # Only these are forbidden even as namespace segments (structural keywords)
    STRUCTURAL_KW = {"let", "import", "__builtin__"}
    OPS = {"^", "/", "*", "%", "+", "-", "==", "!=", "<", ">", ">=", "<=", "!", "&&", "||"}
    SIMPLE_TYPES = {"Number", "String", "Bool", "Null", "Any"}

    def __init__(self, src: str):
        self.src = src
        self.t = _lex(src)
        self.i = 0

    def p(self) -> Program:
        s = []
        while not self._is("EOF"):
            s.append(self._stmt())
            self._eat(";")
        return Program(s)

    def _stmt(self):
        if self._is_ident("import"):
            self._next(); return ImportStatement(self._import_module_name())
        if self._is_ident("let"):
            return self._let()
        return ExpressionStatement(self._expr())

    def _import_module_name(self) -> str:
        """Parse module names for `import`.

        Import names may start with digits (e.g. `20_export`) to match file names.
        This does not change identifier rules for let names, params, or references.
        """
        t = self._peek()

        if t.k == "IDENT":
            if t.v in self.KW:
                self._err(f"Keyword '{t.v}' cannot be used as an import module name", t)
            return self._next().v

        if t.k == "NUMBER":
            parts = [self._next()]
            while self._peek().k in ("NUMBER", "IDENT"):
                nxt = self._peek()
                prev = parts[-1]
                prev_end_col = prev.col + len(prev.v)
                # Only stitch tokens that are lexically adjacent on the same line.
                if nxt.line != prev.line or nxt.col != prev_end_col:
                    break
                parts.append(self._next())
            return "".join(p.v for p in parts)

        self._err(
            "Expected import module name",
            t,
            hint="Use a module name like 'Std' or a digit-leading name like '20_export'.",
        )

    def _let(self):
        self._eat_ident("let")
        lid = self._opt_qid()
        self._eat("(")
        params = []
        if not self._is(")"):
            while True:
                n = self._ident(); self._eat(":"); ty = self._type(); params.append(Parameter(n, ty))
                if not self._m(","): break
        self._eat(")"); self._eat(":")
        rty = self._type()
        if not (self._m("::") or self._m("=")):
            self._err("Expected '::' or '='")
        if self._is_ident("__builtin__"):
            self._next(); ex = "__builtin__"
        else:
            ex = self._expr()
        return LetStatement(lid, ParameterDeclList(params), rty, ex)

    def _type(self):
        if self._is_ident("Tuple"):
            self._next(); self._eat("(")
            ts = []
            if not self._is(")"):
                while True:
                    ts.append(self._type())
                    if not self._m(","): break
            self._eat(")")
            base = TypeList(ts)
        elif self._peek().k == "IDENT" and self._peek().v in self.SIMPLE_TYPES:
            base = self._next().v
        elif self._is("IDENT"):
            base = self._opt_qid()
        else:
            self._err("Expected type")
        if self._m("..."):
            if isinstance(base, str):
                return base + "..."
            self._err("Variadic '...' only supported on simple type names")
        return base

    def _expr(self):
        return Expression(self._flow())

    def _flow(self):
        lhs = self._primary(); rhs = []
        while self._m("->"):
            rhs.append(self._primary())
        return FlowExpression(lhs, rhs)

    def _primary(self):
        return Primary(self._atom(), [])

    def _atom(self):
        if self._m("("):
            if self._m(")"):
                return Atom(None, None, None, None)
            items = []
            while True:
                items.append(self._expr())
                if not self._m(","): break
            self._eat(")")
            return Atom(inner=DelimitedExpression(items))
        # Handle unary minus before a number
        if self._peek().v == "-" and self._peek_ahead(1) and self._peek_ahead(1).k == "NUMBER":
            self._next()  # consume '-'
            raw = self._next().v
            val = float(raw) if any(c in raw for c in ".eE") else int(raw)
            return Atom(constant=Constant(-val))
        if self._is("NUMBER"):
            raw = self._next().v
            val = float(raw) if any(c in raw for c in ".eE") else int(raw)
            return Atom(constant=Constant(val))
        if self._is("STRING"):
            return Atom(constant=Constant(self._next().v))
        if self._is("BOOL"):
            return Atom(constant=Constant(self._next().v == "True"))
        if self._is("NULL"):
            self._next(); return Atom(constant=Constant(None))
        if self._peek().k in self.OPS:
            return Atom(var=self._next().v)
        q = self._opt_qid()
        if isinstance(q, QualifiedId):
            return Atom(qualified_var=q)
        return Atom(var=q)

    def _opt_qid(self):
        head = self._ident()
        parts = [head]
        while self._m("."):
            parts.append(self._ns_ident())
        if len(parts) == 1:
            return head
        return QualifiedId(Qualifier(".".join(parts[:-1])), parts[-1])

    def _ident(self):
        t = self._eat("IDENT")
        if t.v in self.KW:
            self._err(f"Keyword '{t.v}' cannot be used as an identifier", t)
        return t.v

    def _ns_ident(self):
        """Parse an identifier segment inside a dotted namespace path.
        Type keywords (String, Number, Bool, Any, Tuple, Null) are allowed
        as namespace segment names (e.g. Std.String.Upper, Std.Tuple.Append).
        Structural keywords (let, import, __builtin__) remain forbidden.
        """
        t = self._eat("IDENT")
        if t.v in self.STRUCTURAL_KW:
            self._err(f"Keyword '{t.v}' cannot be used as a namespace segment", t)
        return t.v

    def _peek(self): return self.t[self.i]
    def _peek_ahead(self, offset):
        """Look ahead offset tokens. Returns None if out of bounds."""
        idx = self.i + offset
        if idx < len(self.t):
            return self.t[idx]
        return None
    def _next(self):
        x = self.t[self.i]; self.i += 1; return x
    def _is(self, k): return self._peek().k == k
    def _is_ident(self, v):
        t = self._peek(); return t.k == "IDENT" and t.v == v
    def _m(self, k):
        if self._is(k): self.i += 1; return True
        return False
    def _eat(self, k):
        t = self._peek()
        if t.k != k:
            got = "end of input" if t.k == "EOF" else f"'{t.v}'"
            self._err(
                f"Expected '{k}', got {got}",
                t,
                hint="Check for a missing token earlier in the statement.",
            )
        return self._next()
    def _eat_ident(self, v):
        t = self._peek()
        if not (t.k == "IDENT" and t.v == v):
            got = "end of input" if t.k == "EOF" else f"'{t.v}'"
            self._err(
                f"Expected '{v}', got {got}",
                t,
                hint="Check keyword spelling and statement structure.",
            )
        return self._next()
    def _err(self, msg, t=None, hint: str | None = None):
        tok = t or self._peek()
        raise FleauxSyntaxError(msg, self.src, tok.line, tok.col, hint=hint)


def parse_program(source: str) -> Program:
    return Parser(source).p()


def parse_file(file_path: str | Path) -> Program:
    p = Path(file_path)
    return parse_program(p.read_text(encoding="utf-8"))

