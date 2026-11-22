// String literals, read and written the way the core lexer reads them (core/src/frontend/lexer.cpp).
//
// Raw string literals: three or more
// quotes open a raw string and the same number close it, and its content is taken as written, without escapes. When
// only whitespace follows the opening quotes on their line, the raw string is multi-line: its content is the lines
// between that line and the closing quotes, which stand on a line of their own. The whitespace before the closing
// quotes is removed from every content line, and a line indented less than the closing quotes is an error. Lines of
// only whitespace become empty, and lines end with '\n' whatever the source uses.

export class RawStringError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'RawStringError';
  }
}

interface RawStringScan {
  value: string;
  // Offset just past the closing quotes.
  end: number;
}

function quoteRunLength(text: string, offset: number): number {
  let length = 0;
  while (text[offset + length] === '"') {
    length += 1;
  }
  return length;
}

function longestQuoteRun(text: string): number {
  return Math.max(0, ...(text.match(/"+/g) ?? []).map((run) => run.length));
}

function tooManyQuotes(run: number): RawStringError {
  return new RawStringError(`Raw string contains ${run} quotes in a row`);
}

function scanOneLine(source: string, start: number, quotes: number): RawStringScan {
  let value = '';
  let cursor = start + quotes;
  while (cursor < source.length && source[cursor] !== '\n') {
    if (source[cursor] !== '"') {
      value += source[cursor];
      cursor += 1;
      continue;
    }
    const run = quoteRunLength(source, cursor);
    if (run === quotes) {
      return { value, end: cursor + run };
    }
    if (run > quotes) {
      throw tooManyQuotes(run);
    }
    value += '"'.repeat(run);
    cursor += run;
  }
  throw new RawStringError('Unterminated raw string literal');
}

function finishMultiLine(contentLines: string[], closingIndent: string, end: number): RawStringScan {
  if (contentLines.length === 0) {
    throw new RawStringError('Empty multi-line raw string literal');
  }
  const lines = contentLines.map((line) => {
    if (/^[ \t]*$/.test(line)) {
      return '';
    }
    if (!line.startsWith(closingIndent)) {
      throw new RawStringError('Raw string line is indented less than its closing quotes');
    }
    return line.slice(closingIndent.length);
  });
  return { value: lines.join('\n'), end };
}

function scanMultiLine(source: string, quotes: number, firstLine: number): RawStringScan {
  const contentLines: string[] = [];
  let lineStart = firstLine;
  while (lineStart < source.length) {
    const newline = source.indexOf('\n', lineStart);
    const lineEnd = newline === -1 ? source.length : newline;
    let line = source.slice(lineStart, lineEnd);
    if (line.endsWith('\r')) {
      line = line.slice(0, -1);
    }

    const indent = /^[ \t]*/.exec(line)?.[0].length ?? 0;
    const run = quoteRunLength(line, indent);
    if (run >= quotes) {
      if (run > quotes) {
        throw tooManyQuotes(run);
      }
      return finishMultiLine(contentLines, line.slice(0, indent), lineStart + indent + run);
    }
    // Closing quotes stand on a line of their own, so a run as long as them inside a line is an error.
    const longest = longestQuoteRun(line);
    if (longest >= quotes) {
      throw tooManyQuotes(longest);
    }
    contentLines.push(line);
    if (newline === -1) {
      break;
    }
    lineStart = newline + 1;
  }
  throw new RawStringError('Unterminated raw string literal');
}

// The raw string starting at `start`, which holds at least three quotes.
function scanRawString(source: string, start: number): RawStringScan {
  const quotes = quoteRunLength(source, start);
  const afterOpen = start + quotes;
  const newline = source.indexOf('\n', afterOpen);
  const lineEnd = newline === -1 ? source.length : newline;
  if (/[^ \t\r]/.test(source.slice(afterOpen, lineEnd))) {
    return scanOneLine(source, start, quotes);
  }
  if (newline === -1) {
    throw new RawStringError('Unterminated raw string literal');
  }
  return scanMultiLine(source, quotes, newline + 1);
}

/**
 * An ordinary string literal with the value `value`. The core lexer knows only these escapes, so other characters
 * are written as they are.
 */
export function ordinaryStringLiteral(value: string): string {
  const escaped = value.replace(/[\\"\n\r\t]/g, (ch) => {
    switch (ch) {
      case '\n':
        return '\\n';
      case '\r':
        return '\\r';
      case '\t':
        return '\\t';
      default:
        return `\\${ch}`;
    }
  });
  return `"${escaped}"`;
}

/**
 * Rewrites every raw string literal in `source` as an ordinary string literal with the same value, leaving comments
 * and ordinary strings as they are, so that code scanning the result only meets ordinary strings.
 * Throws a RawStringError for a malformed raw string.
 */
export function rewriteRawStrings(source: string): string {
  if (!source.includes('"""')) {
    return source;
  }
  let out = '';
  let index = 0;
  while (index < source.length) {
    const ch = source[index];
    if (ch === '/' && source[index + 1] === '/') {
      const newline = source.indexOf('\n', index);
      const end = newline === -1 ? source.length : newline;
      out += source.slice(index, end);
      index = end;
      continue;
    }
    if (ch === '"' && source.startsWith('"""', index)) {
      const raw = scanRawString(source, index);
      out += ordinaryStringLiteral(raw.value);
      index = raw.end;
      continue;
    }
    if (ch === '"') {
      let end = index + 1;
      while (end < source.length && source[end] !== '"') {
        end += source[end] === '\\' ? 2 : 1;
      }
      out += source.slice(index, end + 1);
      index = end + 1;
      continue;
    }
    out += ch;
    index += 1;
  }
  return out;
}

/**
 * `value` as a multi-line raw string, or null when it has fewer than two non-empty lines, which read better escaped,
 * or when a raw string cannot hold it exactly: a control character other than tab, or a line of only whitespace,
 * which a raw string reads back as empty. The content and the closing quotes start in the first column; the formatter
 * indents them with the code around them.
 */
export function multiLineRawStringLiteral(value: string): string | null {
  const lines = value.split('\n');
  if (lines.filter((line) => line.length > 0).length < 2) {
    return null;
  }
  // Tab is \x09 and line feed is \x0A.
  // eslint-disable-next-line no-control-regex
  if (/[\x00-\x08\x0B-\x1F\x7F]/.test(value)) {
    return null;
  }
  if (lines.some((line) => /^[ \t]+$/.test(line))) {
    return null;
  }
  const delimiter = '"'.repeat(Math.max(3, longestQuoteRun(value) + 1));
  return `${delimiter}\n${value}\n${delimiter}`;
}
