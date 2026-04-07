from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
import re

from .fleaux_diagnostics import SourceSpan, format_diagnostic, merge_source_spans


class FleauxSyntaxError(Exception):
    def __init__(
        self,
        msg: str,
        src: str,
        line: int,
        col: int,
        hint: str | None = None,
        *,
        source_name: str | None = None,
        span: SourceSpan | None = None,
    ):
        self.msg, self.src, self.line, self.col = msg, src, line, col
        self.stage = "parse"
        self.hint = hint
        self.source_name = source_name
        self.span = span or SourceSpan(source_name, src, line, col)
        super().__init__(self._fmt())

    def _fmt(self) -> str:
        return format_diagnostic(
            stage=self.stage,
            message=self.msg,
            span=self.span,
            hint=self.hint,
        )


@dataclass
class Tok:
    k: str
    v: str
    line: int
    col: int
    text: str

    @property
    def end_col(self) -> int:
        return self.col + len(self.text)


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


def _lex(src: str, source_name: str | None = None) -> list[Tok]:
    out: list[Tok] = []
    i = 0
    line = 1
    col = 1
    while i < len(src):
        m = _TOKEN_RE.match(src, i)
        if not m:
            raise FleauxSyntaxError(
                f"Unexpected character '{src[i]}'",
                src,
                line,
                col,
                source_name=source_name,
            )
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
            out.append(Tok(k, v, line, col, text))
            col += len(text)
        i = m.end()
    out.append(Tok("EOF", "", line, col, ""))
    return out


@dataclass
class Program:
    statements: list[object]
    span: SourceSpan | None = None


@dataclass
class ImportStatement:
    module_name: str
    span: SourceSpan | None = None


@dataclass
class Qualifier:
    qualifier: str
    span: SourceSpan | None = None


@dataclass
class QualifiedId:
    qualifier: Qualifier
    id: str
    span: SourceSpan | None = None


@dataclass
class Parameter:
    param_name: str
    type: object
    span: SourceSpan | None = None


@dataclass
class ParameterDeclList:
    params: list[Parameter]
    span: SourceSpan | None = None


@dataclass
class TypeList:
    types: list[object]
    span: SourceSpan | None = None


@dataclass
class LetStatement:
    id: object
    params: ParameterDeclList
    rtype: object
    expr: object
    span: SourceSpan | None = None


@dataclass
class ExpressionStatement:
    expr: object
    span: SourceSpan | None = None


@dataclass
class Expression:
    expr: object
    span: SourceSpan | None = None


@dataclass
class FlowExpression:
    lhs: object
    rhs: list[object]
    span: SourceSpan | None = None


@dataclass
class Primary:
    base: object
    extra: list[object] = field(default_factory=list)
    span: SourceSpan | None = None


@dataclass
class Atom:
    inner: object | None = None
    constant: object | None = None
    qualified_var: object | None = None
    var: str | None = None
    span: SourceSpan | None = None


@dataclass
class DelimitedExpression:
    items: list[Expression]
    span: SourceSpan | None = None


@dataclass
class Constant:
    val: object
    span: SourceSpan | None = None


class Parser:
    KW = {"let", "import", "Number", "String", "Bool", "Null", "Any", "Tuple", "__builtin__"}
    # Only these are forbidden even as namespace segments (structural keywords)
    STRUCTURAL_KW = {"let", "import", "__builtin__"}
    OPS = {"^", "/", "*", "%", "+", "-", "==", "!=", "<", ">", ">=", "<=", "!", "&&", "||"}
    SIMPLE_TYPES = {"Number", "String", "Bool", "Null", "Any"}

    def __init__(self, src: str, source_name: str | None = None):
        self.src = src
        self.source_name = source_name
        self.t = _lex(src, source_name=source_name)
        self.i = 0

    def p(self) -> Program:
        s = []
        while not self._is("EOF"):
            s.append(self._stmt())
            self._eat(";")
        return Program(s, span=merge_source_spans(*(getattr(stmt, "span", None) for stmt in s)))

    def _stmt(self):
        if self._is_ident("import"):
            start = self.i
            self._next()
            return ImportStatement(self._import_module_name(), span=self._span_from_mark(start))
        if self._is_ident("let"):
            return self._let()
        start = self.i
        return ExpressionStatement(self._expr(), span=self._span_from_mark(start))

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
                prev_end_col = prev.end_col
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
        start = self.i
        self._eat_ident("let")
        lid = self._opt_qid()
        self._eat("(")
        params = []
        params_paren_start = self.i - 1
        if not self._is(")"):
            while True:
                param_start = self.i
                n = self._ident(); self._eat(":"); ty = self._type(); params.append(Parameter(n, ty, span=self._span_from_mark(param_start)))
                if not self._m(","): break
        self._eat(")")
        params_span = self._span_from_mark(params_paren_start)
        self._eat(":")
        rty = self._type()
        if not (self._m("::") or self._m("=")):
            self._err(
                "Expected '::' or '='",
                hint="After the return type, use '=' for a normal body or ':: __builtin__' for runtime-provided functions.",
            )
        if self._is_ident("__builtin__"):
            self._next(); ex = "__builtin__"
        else:
            ex = self._expr()
        return LetStatement(
            lid,
            ParameterDeclList(params, span=params_span),
            rty,
            ex,
            span=self._span_from_mark(start),
        )

    def _type(self):
        start = self.i
        if self._is_ident("Tuple"):
            self._next(); self._eat("(")
            ts = []
            if not self._is(")"):
                while True:
                    ts.append(self._type())
                    if not self._m(","): break
            self._eat(")")
            base = TypeList(ts, span=self._span_from_mark(start))
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
        start = self.i
        return Expression(self._flow(), span=self._span_from_mark(start))

    def _flow(self):
        start = self.i
        lhs = self._primary(); rhs = []
        while self._m("->"):
            rhs.append(self._primary())
        return FlowExpression(lhs, rhs, span=self._span_from_mark(start))

    def _primary(self):
        start = self.i
        return Primary(self._atom(), [], span=self._span_from_mark(start))

    def _atom(self):
        start = self.i
        if self._m("("):
            if self._m(")"):
                return Atom(None, None, None, None, span=self._span_from_mark(start))
            items = []
            while True:
                items.append(self._expr())
                if not self._m(","): break
            self._eat(")")
            return Atom(inner=DelimitedExpression(items, span=self._span_from_mark(start)), span=self._span_from_mark(start))
        # Handle unary minus before a number
        if self._peek().v == "-" and self._peek_ahead(1) and self._peek_ahead(1).k == "NUMBER":
            self._next()  # consume '-'
            raw = self._next().v
            val = float(raw) if any(c in raw for c in ".eE") else int(raw)
            constant_span = self._span_from_mark(start)
            return Atom(constant=Constant(-val, span=constant_span), span=constant_span)
        if self._is("NUMBER"):
            raw = self._next().v
            val = float(raw) if any(c in raw for c in ".eE") else int(raw)
            constant_span = self._span_from_mark(start)
            return Atom(constant=Constant(val, span=constant_span), span=constant_span)
        if self._is("STRING"):
            self._next()
            constant_span = self._span_from_mark(start)
            return Atom(constant=Constant(self.t[self.i - 1].v, span=constant_span), span=constant_span)
        if self._is("BOOL"):
            self._next()
            constant_span = self._span_from_mark(start)
            return Atom(constant=Constant(self.t[self.i - 1].v == "True", span=constant_span), span=constant_span)
        if self._is("NULL"):
            self._next(); constant_span = self._span_from_mark(start); return Atom(constant=Constant(None, span=constant_span), span=constant_span)
        if self._peek().k in self.OPS:
            return Atom(var=self._next().v, span=self._span_from_mark(start))
        q = self._opt_qid()
        if isinstance(q, QualifiedId):
            return Atom(qualified_var=q, span=self._span_from_mark(start))
        return Atom(var=q, span=self._span_from_mark(start))

    def _opt_qid(self):
        start = self.i
        head = self._ident()
        parts = [head]
        while self._m("."):
            parts.append(self._ns_ident())
        if len(parts) == 1:
            return head
        qualifier_span = self._span_from_mark(start)
        return QualifiedId(
            Qualifier(".".join(parts[:-1]), span=qualifier_span),
            parts[-1],
            span=self._span_from_mark(start),
        )

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
    def _previous_token(self):
        if self.i == 0:
            return self.t[0]
        return self.t[self.i - 1]
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
                hint=self._hint_for_expected_token(k, t),
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
        raise FleauxSyntaxError(
            msg,
            self.src,
            tok.line,
            tok.col,
            hint=hint,
            source_name=self.source_name,
            span=self._span_from_token(tok),
        )

    def _span_from_token(self, tok: Tok) -> SourceSpan:
        end_col = tok.end_col if tok.k != "EOF" else tok.col
        return SourceSpan(
            self.source_name,
            self.src,
            tok.line,
            tok.col,
            end_line=tok.line,
            end_col=end_col,
        )

    def _span_from_mark(self, start: int) -> SourceSpan:
        start_tok = self.t[start]
        end_tok = self._previous_token() if self.i > start else start_tok
        end_col = end_tok.end_col if end_tok.k != "EOF" else end_tok.col
        return SourceSpan(
            self.source_name,
            self.src,
            start_tok.line,
            start_tok.col,
            end_line=end_tok.line,
            end_col=end_col,
        )

    def _hint_for_expected_token(self, expected: str, got_tok: Tok) -> str:
        prev_tok = self._previous_token()

        if expected == ";":
            if got_tok.k == "EOF":
                return "Add ';' to terminate the final statement. Every statement in Fleaux must end with a semicolon."
            if self._is_statement_start(got_tok):
                return "Add ';' before this next statement. Fleaux requires semicolons between statements."
            return "Terminate the current statement with ';'."

        if expected == ",":
            if self._is_expression_start(got_tok):
                return "Add ',' between tuple elements or argument entries."
            return "Separate entries with ','."

        if expected == ")":
            if got_tok.k == "->":
                return "Close the current tuple/parameter list with ')' before continuing the pipeline."
            if self._is_expression_start(got_tok):
                if prev_tok.k in {"NUMBER", "STRING", "BOOL", "NULL", "IDENT", ")"}:
                    return "Did you forget a comma? Add ',' between tuple elements or function arguments."
            return "Close the current tuple/parameter list with ')'."

        if expected == ":":
            return "Add ':' before a type annotation (for example: x: Number)."

        if expected == "IDENT":
            if prev_tok.k == "->":
                return "A pipeline stage is missing after '->'. Use a call target like Std.Add, MyFunc, or +."
            return "Use a valid identifier (letters/underscore, then letters/digits/underscore)."

        return "Check for a missing or misplaced token earlier in the statement."

    def _is_expression_start(self, tok: Tok) -> bool:
        if tok.k in {"NUMBER", "STRING", "BOOL", "NULL", "IDENT", "("}:
            return True
        if tok.k in self.OPS:
            return True
        return tok.k == "-"

    def _is_statement_start(self, tok: Tok) -> bool:
        return tok.k in {"IDENT", "(", "NUMBER", "STRING", "BOOL", "NULL"} or tok.k in self.OPS

def parse_program(source: str, source_name: str | Path | None = None) -> Program:
    normalized_name = str(source_name) if source_name is not None else None
    return Parser(source, source_name=normalized_name).p()


def parse_file(file_path: str | Path) -> Program:
    p = Path(file_path)
    return parse_program(p.read_text(encoding="utf-8"), source_name=p)



