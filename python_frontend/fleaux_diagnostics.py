from __future__ import annotations

from dataclasses import dataclass
import os
import sys


@dataclass(frozen=True, slots=True)
class SourceSpan:
    source_name: str | None
    source_text: str | None
    line: int
    col: int
    end_line: int | None = None
    end_col: int | None = None

    @property
    def source_line(self) -> str | None:
        if self.source_text is None:
            return None
        lines = self.source_text.splitlines() or [""]
        if 1 <= self.line <= len(lines):
            return lines[self.line - 1]
        return None

    @property
    def caret_width(self) -> int:
        if self.end_line == self.line and self.end_col is not None:
            return max(self.end_col - self.col, 1)
        return 1


def merge_source_spans(*spans: SourceSpan | None) -> SourceSpan | None:
    present = [span for span in spans if span is not None]
    if not present:
        return None

    start = present[0]
    end = present[-1]
    return SourceSpan(
        source_name=start.source_name,
        source_text=start.source_text,
        line=start.line,
        col=start.col,
        end_line=end.end_line if end.end_line is not None else end.line,
        end_col=end.end_col if end.end_col is not None else end.col,
    )


def format_diagnostic(
    *,
    stage: str,
    message: str,
    span: SourceSpan | None = None,
    line: int | None = None,
    col: int | None = None,
    source_line: str | None = None,
    stage_index: int | None = None,
    hint: str | None = None,
    use_color: bool | None = None,
) -> str:
    use_color = _should_use_color(use_color)

    if span is not None:
        line = span.line
        col = span.col
        source_line = span.source_line

    prefix = f"[{stage}] {message}"

    if stage_index is not None:
        prefix += f" (stage {stage_index})"

    if span is not None and span.source_name:
        prefix += f" in {span.source_name}"

    if line is not None and col is not None:
        prefix += f" at line {line}, column {col}."

    prefix = _color(prefix, "1;31", use_color)

    parts = [prefix]

    if source_line is not None and line is not None and col is not None:
        caret_col = max(col - 1, 0)
        caret_width = span.caret_width if span is not None else 1
        parts.append(source_line)
        caret = " " * caret_col + "^" + "~" * max(caret_width - 1, 0)
        parts.append(_color(caret, "1;36", use_color))

    if hint is not None:
        parts.append(_color(hint, "1;33", use_color))

    return "\n".join(parts)


def _should_use_color(use_color: bool | None) -> bool:
    if use_color is not None:
        return use_color
    if os.environ.get("NO_COLOR") is not None:
        return False
    if os.environ.get("CLICOLOR_FORCE") == "1":
        return True
    if os.environ.get("TERM") == "dumb":
        return False
    return bool(getattr(sys.stderr, "isatty", lambda: False)())


def _color(text: str, code: str, use_color: bool) -> str:
    if not use_color:
        return text
    return f"\x1b[{code}m{text}\x1b[0m"



