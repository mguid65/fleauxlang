from __future__ import annotations


def format_diagnostic(
    *,
    stage: str,
    message: str,
    line: int | None = None,
    col: int | None = None,
    source_line: str | None = None,
    stage_index: int | None = None,
    hint: str | None = None,
) -> str:
    prefix = f"[{stage}] {message}"

    if stage_index is not None:
        prefix += f" (stage {stage_index})"

    if line is not None and col is not None:
        prefix += f" at line {line}, column {col}."

    parts = [prefix]

    if source_line is not None and line is not None and col is not None:
        caret_col = max(col - 1, 0)
        parts.append(source_line)
        parts.append(" " * caret_col + "^")

    if hint is not None:
        parts.append(f"Hint: {hint}")

    return "\n".join(parts)

