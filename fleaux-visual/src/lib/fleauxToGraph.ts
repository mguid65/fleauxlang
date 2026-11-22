import type { Node } from '@xyflow/react';
import type { AliasDeclData, ClosureData, FleauxEdge, FleauxNodeData, FunctionParam, LetData, LiteralValueType, RawSourceData, StdFuncData, TypeDeclData, UserFuncData } from './types';
import { formatAritySummary, formatFunctionDisplayName, hasVariadicTail, matchesArity, minArity } from './functionSignatures';
import { isOperatorToken, resolveOperator } from './operators';
import { RawStringError, rewriteRawStrings } from './stringLiterals';
import { STD_FUNCTIONS, STD_VALUES, type StdFunctionEntry } from './stdCatalogue';

export class FleauxImportError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'FleauxImportError';
  }
}

export interface FleauxGraphImportResult {
  nodes: Node<FleauxNodeData>[];
  edges: FleauxEdge[];
}

type Param = { name: string; type: string };
type LetDef = {
  name: string;
  typeParams?: string[];
  params: Param[];
  returnType: string;
  body: string;
  isValueBinding?: boolean;
};
type TypeDecl = { name: string; targetType: string; separator: '=' | '::' };
type AliasDecl = { name: string; targetType: string };

function createFunctionParams(ownerNodeId: string, params: Param[]): FunctionParam[] {
  return params.map((param, index) => ({
    id: `${ownerNodeId}-param-${index}`,
    name: param.name,
    type: param.type,
  }));
}

type SourceRef = {
  nodeId: string;
  sourceHandle?: string;
};

type CurrentValue =
  | { kind: 'source'; source: SourceRef }
  | { kind: 'args'; args: SourceRef[] };

type BuildContext = {
  nodes: Node<FleauxNodeData>[];
  edges: FleauxEdge[];
  userFunctions: Map<string, LetDef>;
  importedModules: Set<string>;
  letSourceRef?: { nodeId: string; paramIndexByName: Map<string, number> };
  closureScopes: Array<{ nodeId: string; paramIndexByName: Map<string, number> }>;
};

const stdFunctionsByName = new Map<string, StdFunctionEntry[]>();
for (const entry of STD_FUNCTIONS) {
  const group = stdFunctionsByName.get(entry.qualifiedName) ?? [];
  group.push(entry);
  stdFunctionsByName.set(entry.qualifiedName, group);
}
const stdValueByName = new Map(STD_VALUES.map((entry) => [entry.qualifiedName, entry]));

function selectStdFunctionOverload(
  qualifiedName: string,
  inferredArity?: number,
): StdFunctionEntry | null {
  const overloads = stdFunctionsByName.get(qualifiedName);
  if (!overloads || overloads.length === 0) {
    return null;
  }

  if (inferredArity === undefined) {
    return overloads.length === 1 ? overloads[0] : null;
  }

  const matches = overloads.filter((entry) => matchesArity(entry.params, inferredArity));
  if (matches.length === 0) {
    return overloads.length === 1 ? overloads[0] : null;
  }

  if (matches.length === 1) {
    return matches[0];
  }

  const exactMatches = matches.filter((entry) => entry.params.length === inferredArity);
  if (exactMatches.length === 1) {
    return exactMatches[0];
  }

  return null;
}

function formatArityErrorSuffix(inferredArity?: number): string {
  if (inferredArity === undefined) {
    return '';
  }
  return ` with ${inferredArity} argument${inferredArity === 1 ? '' : 's'}`;
}

/** The source with each raw string literal written as an ordinary one, which is all the scanners below read. */
function withoutRawStrings(source: string): string {
  try {
    return rewriteRawStrings(source);
  } catch (error) {
    if (error instanceof RawStringError) {
      throw new FleauxImportError(error.message);
    }
    throw error;
  }
}

/** Strip // line comments from source, preserving string literals. */
function stripLineComments(source: string): string {
  const out: string[] = [];
  let inString = false;
  let escape = false;
  let i = 0;
  while (i < source.length) {
    const ch = source[i];
    if (inString) {
      out.push(ch);
      if (escape) { escape = false; }
      else if (ch === '\\') { escape = true; }
      else if (ch === '"') { inString = false; }
      i += 1;
      continue;
    }
    if (ch === '"') { inString = true; out.push(ch); i += 1; continue; }
    if (ch === '/' && source[i + 1] === '/') {
      // Skip to end of line
      while (i < source.length && source[i] !== '\n') i += 1;
      continue;
    }
    out.push(ch);
    i += 1;
  }
  return out.join('');
}

/**
 * Marks the characters of `text` that sit outside strings and outside any nesting: parentheses, brackets, braces, and,
 * when `trackAngles` is set, the angle brackets of type arguments. In expressions `<` is also an operator, so it opens
 * type arguments only directly after a name (`Tuple<`, `Std.Cast<`) or directly before one (a generic closure
 * `<T>(x: T)`); the operator is written apart from its neighbours (`-> <`).
 */
function topLevelMask(text: string, trackAngles: boolean): boolean[] {
  const mask = new Array<boolean>(text.length).fill(false);
  let depth = 0;
  let angleDepth = 0;
  let inString = false;
  let escape = false;

  for (let index = 0; index < text.length; index += 1) {
    const ch = text[index];
    if (inString) {
      if (escape) { escape = false; }
      else if (ch === '\\') { escape = true; }
      else if (ch === '"') { inString = false; }
      continue;
    }
    if (ch === '"') { inString = true; continue; }
    if (ch === '(' || ch === '[' || ch === '{') { depth += 1; continue; }
    if (ch === ')' || ch === ']' || ch === '}') { depth = Math.max(0, depth - 1); continue; }
    if (trackAngles) {
      if (ch === '<' && opensTypeArguments(text, index)) { angleDepth += 1; continue; }
      if (angleDepth > 0 && isClosingAngle(text, index)) { angleDepth -= 1; continue; }
    }
    mask[index] = depth === 0 && angleDepth === 0;
  }
  return mask;
}

function opensTypeArguments(text: string, index: number): boolean {
  const before = text[index - 1] ?? '';
  const after = text[index + 1] ?? '';
  return /[A-Za-z0-9_]/.test(before) || /[A-Za-z_]/.test(after);
}

function splitStatements(source: string): string[] {
  const statements: string[] = [];
  const topLevel = topLevelMask(source, false);
  let start = 0;

  for (let index = 0; index < source.length; index += 1) {
    if (topLevel[index] && source[index] === ';') {
      const stmt = source.slice(start, index).trim();
      if (stmt.length > 0) statements.push(stmt);
      start = index + 1;
    }
  }

  const tail = source.slice(start).trim();
  if (tail.length > 0) {
    throw new FleauxImportError(`Trailing text without terminating ';': ${tail}`);
  }
  return statements;
}

function splitTopLevel(text: string, delimiter: string): string[] {
  const parts: string[] = [];
  const topLevel = topLevelMask(text, true);
  let cursor = 0;

  for (let index = 0; index < text.length; index += 1) {
    if (topLevel[index] && text.startsWith(delimiter, index)) {
      parts.push(text.slice(cursor, index).trim());
      cursor = index + delimiter.length;
      index += delimiter.length - 1;
    }
  }

  parts.push(text.slice(cursor).trim());
  return parts.filter((part) => part.length > 0);
}

function hasTopLevelDelimiter(text: string, delimiter: string): boolean {
  const topLevel = topLevelMask(text, true);
  for (let index = 0; index < text.length; index += 1) {
    if (topLevel[index] && text.startsWith(delimiter, index)) {
      return true;
    }
  }
  return false;
}

/** True when the `>` at `index` closes a type argument list rather than ending `=>` or `->`. */
function isClosingAngle(text: string, index: number): boolean {
  return text[index] === '>' && text[index - 1] !== '=' && text[index - 1] !== '-';
}

/**
 * Split type text (parameter lists, type argument lists) on a top-level delimiter.
 * Unlike `splitTopLevel`, this treats `<...>` as nesting, so `a: Tuple<A, B>, f: (A) => B`
 * splits into two entries. Only use it on type text: in expressions `<` is an operator.
 */
function splitTopLevelType(text: string, delimiter: string): string[] {
  const parts: string[] = [];
  let parenDepth = 0;
  let angleDepth = 0;
  let cursor = 0;

  for (let index = 0; index < text.length; index += 1) {
    const ch = text[index];
    if (ch === '(' || ch === '[') { parenDepth += 1; continue; }
    if (ch === ')' || ch === ']') { parenDepth = Math.max(0, parenDepth - 1); continue; }
    if (ch === '<') { angleDepth += 1; continue; }
    if (isClosingAngle(text, index)) { angleDepth = Math.max(0, angleDepth - 1); continue; }
    if (parenDepth === 0 && angleDepth === 0 && text.startsWith(delimiter, index)) {
      parts.push(text.slice(cursor, index).trim());
      cursor = index + delimiter.length;
      index += delimiter.length - 1;
    }
  }

  parts.push(text.slice(cursor).trim());
  return parts.filter((part) => part.length > 0);
}

/** Index of the `>` matching the `<` at `openIndex`, ignoring the `>` of `=>` and `->`. */
function findMatchingAngle(text: string, openIndex: number): number {
  let depth = 0;
  for (let index = openIndex; index < text.length; index += 1) {
    if (text[index] === '<') {
      depth += 1;
    } else if (isClosingAngle(text, index)) {
      depth -= 1;
      if (depth === 0) {
        return index;
      }
    }
  }
  return -1;
}

function parseParams(paramText: string): Param[] {
  const trimmed = paramText.trim();
  if (!trimmed) return [];
  return splitTopLevelType(trimmed, ',').map((entry) => {
    const colonIdx = entry.indexOf(':');
    if (colonIdx < 1) throw new FleauxImportError(`Invalid parameter entry: '${entry}'`);
    return { name: entry.slice(0, colonIdx).trim(), type: entry.slice(colonIdx + 1).trim() };
  });
}

type ClosureSyntax = { typeParams: string[]; params: Param[]; returnType: string; body: string };

type DefinitionSeparator = { index: number; length: 1 | 2 };

function findMatchingParen(text: string, openIndex: number): number {
  let depth = 0;
  let inString = false;
  let escape = false;

  for (let index = openIndex; index < text.length; index += 1) {
    const ch = text[index];
    if (inString) {
      if (escape) { escape = false; }
      else if (ch === '\\') { escape = true; }
      else if (ch === '"') { inString = false; }
      continue;
    }
    if (ch === '"') { inString = true; continue; }
    if (ch === '(') { depth += 1; continue; }
    if (ch === ')') {
      depth -= 1;
      if (depth === 0) {
        return index;
      }
    }
  }

  return -1;
}

function findTopLevelDefinitionSeparator(text: string): DefinitionSeparator | null {
  let parenDepth = 0;
  let angleDepth = 0;
  let inString = false;
  let escape = false;

  for (let index = 0; index < text.length; index += 1) {
    const ch = text[index];
    if (inString) {
      if (escape) { escape = false; }
      else if (ch === '\\') { escape = true; }
      else if (ch === '"') { inString = false; }
      continue;
    }

    if (ch === '"') { inString = true; continue; }
    if (ch === '(') { parenDepth += 1; continue; }
    if (ch === ')') { parenDepth = Math.max(0, parenDepth - 1); continue; }
    if (ch === '<') { angleDepth += 1; continue; }
    if (isClosingAngle(text, index)) { angleDepth = Math.max(0, angleDepth - 1); continue; }

    if (parenDepth !== 0 || angleDepth !== 0) {
      continue;
    }

    if (text.startsWith('::', index)) {
      return { index, length: 2 };
    }

    if (ch === '=' && text[index + 1] !== '=' && text[index + 1] !== '>') {
      return { index, length: 1 };
    }
  }

  return null;
}

/**
 * If `text` is a closure expression `(params): ReturnType = body`, parse and return its parts.
 * Returns null if the text doesn't match the closure pattern.
 */
function tryParseClosureSyntax(text: string): ClosureSyntax | null {
  let trimmed = text.trim();

  // A generic closure starts with its type parameters: `<T, U: Numeric>(x: T, y: U): R = body`.
  let typeParams: string[] = [];
  if (trimmed.startsWith('<')) {
    const close = findMatchingAngle(trimmed, 0);
    if (close === -1) return null;
    typeParams = splitTopLevelType(trimmed.slice(1, close), ',').map((entry) => entry.replace(/\s*:\s*/, ': '));
    trimmed = trimmed.slice(close + 1).trimStart();
  }
  if (!trimmed.startsWith('(')) return null;

  // Find the matching `)` at depth 0
  let depth = 0;
  let inString = false;
  let escape = false;
  let closeIndex = -1;
  for (let i = 0; i < trimmed.length; i += 1) {
    const ch = trimmed[i];
    if (inString) {
      if (escape) { escape = false; }
      else if (ch === '\\') { escape = true; }
      else if (ch === '"') { inString = false; }
      continue;
    }
    if (ch === '"') { inString = true; continue; }
    if (ch === '(') { depth += 1; continue; }
    if (ch === ')') {
      depth -= 1;
      if (depth === 0) { closeIndex = i; break; }
    }
  }
  if (closeIndex === -1) return null;

  // After the closing `)` must come `: returnType = body`
  const afterParen = trimmed.slice(closeIndex + 1).trimStart();
  if (!afterParen.startsWith(':')) return null;
  const afterColon = afterParen.slice(1).trimStart();

  // Find the `=` separator (not `==`) at depth 0 to locate the body
  const separator = findTopLevelDefinitionSeparator(afterColon);
  if (!separator || separator.length !== 1) return null;

  const returnType = afterColon.slice(0, separator.index).trim();
  const body = afterColon.slice(separator.index + separator.length).trim();
  if (!returnType || !body) return null;

  // Validate the paren content looks like closure params (every comma-part has `:`)
  const paramsText = trimmed.slice(1, closeIndex).trim();
  if (paramsText) {
    const parts = splitTopLevelType(paramsText, ',');
    if (!parts.every((part) => part.includes(':'))) return null;
  }

  let params: Param[];
  try {
    params = parseParams(paramsText);
  } catch {
    return null;
  }

  return { typeParams, params, returnType, body };
}

function splitNameAndTypeParams(rawName: string): { name: string; typeParams: string[] } {
  const trimmed = rawName.trim();
  const open = trimmed.indexOf('<');
  if (open <= 0 || !trimmed.endsWith('>') || findMatchingAngle(trimmed, open) !== trimmed.length - 1) {
    return { name: trimmed, typeParams: [] };
  }

  const name = trimmed.slice(0, open).trim();
  const typeParamsText = trimmed.slice(open + 1, -1).trim();
  const typeParams = typeParamsText.length === 0 ? [] : splitTopLevelType(typeParamsText, ',');
  return { name, typeParams };
}

/**
 * Parse the head of a `type` or `alias` declaration. The returned name keeps any generic
 * parameter list, e.g. `Result<T, E>` or `Id<T>`, so the declaration serializes unchanged.
 */
function parseDeclarationHead(stmt: string, keyword: 'type' | 'alias'): { name: string; rest: string } | null {
  const match = new RegExp('^' + keyword + '\\s+([A-Za-z_][\\w]*)\\s*').exec(stmt);
  if (!match) {
    return null;
  }

  let name = match[1];
  let cursor = match[0].length;
  if (stmt[cursor] === '<') {
    const close = findMatchingAngle(stmt, cursor);
    if (close === -1) {
      return null;
    }
    const typeParams = splitTopLevelType(stmt.slice(cursor + 1, close), ',');
    name += '<' + typeParams.join(', ') + '>';
    cursor = close + 1;
  }

  return { name, rest: stmt.slice(cursor).trimStart() };
}

function parseLet(stmt: string): LetDef {
  const trimmed = stmt.trim();
  if (!trimmed.startsWith('let ')) {
    throw new FleauxImportError(`Unsupported let statement: ${stmt}`);
  }

  const afterLet = trimmed.slice(4).trimStart();

  // A value let, `let Name: Type = expr;`, names its value; it has no parameter list.
  const valueHead = /^([A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*)\s*:/.exec(afterLet);
  if (valueHead) {
    const afterColon = afterLet.slice(valueHead[0].length).trimStart();
    const separator = findTopLevelDefinitionSeparator(afterColon);
    if (!separator) {
      throw new FleauxImportError(`Unsupported let statement: ${stmt}`);
    }
    return {
      name: valueHead[1],
      params: [],
      returnType: afterColon.slice(0, separator.index).trim(),
      body: afterColon.slice(separator.index + separator.length).trim(),
      isValueBinding: true,
    };
  }

  let angleDepth = 0;
  let paramsOpenIndex = -1;
  for (let index = 0; index < afterLet.length; index += 1) {
    const ch = afterLet[index];
    if (ch === '<') {
      angleDepth += 1;
      continue;
    }
    if (isClosingAngle(afterLet, index)) {
      angleDepth = Math.max(0, angleDepth - 1);
      continue;
    }
    if (ch === '(' && angleDepth === 0) {
      paramsOpenIndex = index;
      break;
    }
  }

  if (paramsOpenIndex === -1) {
    throw new FleauxImportError(`Unsupported let statement: ${stmt}`);
  }

  const paramsCloseIndex = findMatchingParen(afterLet, paramsOpenIndex);
  if (paramsCloseIndex === -1) {
    throw new FleauxImportError(`Unsupported let statement: ${stmt}`);
  }

  const rawName = afterLet.slice(0, paramsOpenIndex).trim();
  const afterParams = afterLet.slice(paramsCloseIndex + 1).trimStart();
  if (!afterParams.startsWith(':')) {
    throw new FleauxImportError(`Unsupported let statement: ${stmt}`);
  }

  const afterColon = afterParams.slice(1).trimStart();
  const separator = findTopLevelDefinitionSeparator(afterColon);
  if (!separator) {
    throw new FleauxImportError(`Unsupported let statement: ${stmt}`);
  }

  const { name, typeParams } = splitNameAndTypeParams(rawName);
  return {
    name,
    typeParams,
    params: parseParams(afterLet.slice(paramsOpenIndex + 1, paramsCloseIndex)),
    returnType: afterColon.slice(0, separator.index).trim(),
    body: afterColon.slice(separator.index + separator.length).trim(),
  };
}

function parseTypeDecl(stmt: string): TypeDecl {
  const head = parseDeclarationHead(stmt, 'type');
  const separatorMatch = head ? /^(::|=)\s*(.+)$/s.exec(head.rest) : null;
  if (!head || !separatorMatch) {
    throw new FleauxImportError(`Unsupported type declaration: ${stmt}`);
  }

  return {
    name: head.name,
    separator: separatorMatch[1] as '=' | '::',
    targetType: separatorMatch[2].trim(),
  };
}

function parseAliasDecl(stmt: string): AliasDecl {
  const head = parseDeclarationHead(stmt, 'alias');
  const targetMatch = head ? /^=\s*(.+)$/s.exec(head.rest) : null;
  if (!head || !targetMatch) {
    throw new FleauxImportError(`Unsupported alias declaration: ${stmt}`);
  }

  return {
    name: head.name,
    targetType: targetMatch[1].trim(),
  };
}

function isWrappedByParens(text: string): boolean {
  const trimmed = text.trim();
  if (!trimmed.startsWith('(') || !trimmed.endsWith(')')) return false;
  let depth = 0;
  let inString = false;
  let escape = false;
  for (let index = 0; index < trimmed.length; index += 1) {
    const ch = trimmed[index];
    if (inString) {
      if (escape) { escape = false; }
      else if (ch === '\\') { escape = true; }
      else if (ch === '"') { inString = false; }
      continue;
    }
    if (ch === '"') { inString = true; continue; }
    if (ch === '(') { depth += 1; }
    else if (ch === ')') {
      depth -= 1;
      if (depth === 0 && index !== trimmed.length - 1) return false;
    }
  }
  return depth === 0;
}

function nextIdFactory() {
  let counter = 0;
  return (prefix: string): string => { counter += 1; return `${prefix}-${counter}`; };
}

// True when `name` is a parameter of the enclosing let or of an enclosing closure.
function isParameterInScope(ctx: BuildContext, name: string): boolean {
  return ctx.letSourceRef?.paramIndexByName.has(name) === true
    || ctx.closureScopes.some((scope) => scope.paramIndexByName.has(name));
}

// A name the source does not declare, taken to come from an imported module other than Std (every Std symbol is in
// the catalogue). An import brings in its module's names as written, qualified or not.
function isExternalSymbol(ctx: BuildContext, token: string): boolean {
  return /^[A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*$/.test(token)
    && !token.startsWith('Std.')
    && !isParameterInScope(ctx, token)
    && [...ctx.importedModules].some((moduleName) => moduleName !== 'Std');
}

// The Result tags: lowering turns `Std.Ok` and `Std.Err` into constants, so they are not in the Std catalogue.
const RESULT_TAGS = new Set(['Std.Ok', 'Std.Err']);

function addEdge(ctx: BuildContext, source: SourceRef, target: string, targetHandle?: string): void {
  ctx.edges.push({
    id: `e-${ctx.edges.length + 1}`,
    source: source.nodeId,
    sourceHandle: source.sourceHandle,
    target,
    targetHandle,
    animated: true,
    data: { kind: 'pipeline' },
  });
}

function addClosureBodyRootEdge(
  ctx: BuildContext,
  closureNodeId: string,
  bodySource: SourceRef,
): void {
  ctx.edges.push({
    id: `e-${ctx.edges.length + 1}`,
    source: closureNodeId,
    sourceHandle: bodySource.nodeId === closureNodeId ? bodySource.sourceHandle : 'closure-body-anchor',
    target: bodySource.nodeId,
    targetHandle: 'closure-body-root',
    animated: true,
    data: {
      kind: 'pipeline',
      bodySourceNodeId: bodySource.nodeId,
      bodySourceHandle: bodySource.sourceHandle,
    },
  });
}

function createLiteralNode(
  ctx: BuildContext,
  id: string,
  valueType: LiteralValueType,
  value: string,
): SourceRef {
  const label = valueType === 'String' ? JSON.stringify(value) : value;
  ctx.nodes.push({
    id,
    type: 'literalNode',
    position: { x: 380, y: 130 + ctx.nodes.length * 48 },
    data: { kind: 'literal', valueType, value, label },
  });
  return { nodeId: id };
}

function createWildcardNode(
  ctx: BuildContext,
  id: string,
): SourceRef {
  ctx.nodes.push({
    id,
    type: 'wildcardNode',
    position: { x: 380, y: 130 + ctx.nodes.length * 48 },
    data: { kind: 'wildcard', label: '_' },
  });
  return { nodeId: id };
}

function inferNumericLiteralType(token: string): LiteralValueType {
  if (token.includes('.') || token.includes('e') || token.includes('E')) {
    return 'Float64';
  }

  return token.startsWith('-') ? 'Int64' : 'Int64';
}

function parseLiteralToken(token: string): { valueType: LiteralValueType; value: string } | null {
  if (/^"(?:[^"\\]|\\.)*"$/s.test(token)) {
    try { return { valueType: 'String', value: JSON.parse(token) as string }; }
    catch { return { valueType: 'String', value: token.slice(1, -1) }; }
  }
  const unsigned = /^(\d+)u64$/.exec(token);
  if (unsigned) {
    return { valueType: 'UInt64', value: unsigned[1] };
  }
  if (/^-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?$/.test(token)) {
    return { valueType: inferNumericLiteralType(token), value: token };
  }
  if (token === 'True' || token === 'False') return { valueType: 'Bool', value: token };
  if (token === 'null') return { valueType: 'Null', value: 'null' };
  return null;
}

function createSpreadNode(ctx: BuildContext, id: string, source: SourceRef): SourceRef {
  ctx.nodes.push({
    id,
    type: 'spreadNode',
    position: { x: 500, y: 140 + ctx.nodes.length * 40 },
    data: { kind: 'spread', label: 'x...' },
  });
  addEdge(ctx, source, id, 'spread-in');
  return { nodeId: id };
}

function isSpreadRef(ctx: BuildContext, ref: SourceRef): boolean {
  return ctx.nodes.find((node) => node.id === ref.nodeId)?.data.kind === 'spread';
}

// A spread only belongs in a call's argument list, so a parameter list holding one cannot become a tuple value.
function assertNoSpread(ctx: BuildContext, args: SourceRef[]): void {
  if (args.some((arg) => isSpreadRef(ctx, arg))) {
    throw new FleauxImportError("A spread 'x...' can only be part of a call's argument list.");
  }
}

function createTupleNode(ctx: BuildContext, id: string, args: SourceRef[]): SourceRef {
  ctx.nodes.push({
    id,
    type: 'tupleNode',
    position: { x: 500, y: 140 + ctx.nodes.length * 40 },
    data: { kind: 'tuple', arity: args.length, label: `( ${args.length} )` },
  });
  args.forEach((arg, index) => addEdge(ctx, arg, id, `tuple-in-${index}`));
  return { nodeId: id };
}

/**
 * Resolve a bare identifier as a value source.
 * Handles: _ placeholder, let params, std values, std function refs, user function refs.
 */
function resolveIdentifierAsSource(
  ctx: BuildContext,
  token: string,
  nextId: (prefix: string) => string,
  pipelineSource: SourceRef | null,
): SourceRef {
  const splitToken = splitNameAndTypeParams(token);
  const resolvedToken = splitToken.name;
  const appliedTypeArgs = splitToken.typeParams.length > 0 ? splitToken.typeParams : undefined;

  // _ refers to the current flowing pipeline value
  if (resolvedToken === '_') {
    if (!pipelineSource) {
      return createWildcardNode(ctx, nextId('wildcard'));
    }
    return pipelineSource;
  }

  // Check innermost closure scope first (supports captures from multiple levels)
  for (let i = ctx.closureScopes.length - 1; i >= 0; i -= 1) {
    const scope = ctx.closureScopes[i];
    const closureParamIndex = scope.paramIndexByName.get(resolvedToken);
    if (closureParamIndex !== undefined) {
      return { nodeId: scope.nodeId, sourceHandle: `closure-param-${closureParamIndex}` };
    }
  }

  // Let parameter reference
  const paramIndex = ctx.letSourceRef?.paramIndexByName.get(resolvedToken);
  if (paramIndex !== undefined && ctx.letSourceRef) {
    return { nodeId: ctx.letSourceRef.nodeId, sourceHandle: `let-param-${paramIndex}` };
  }

  if (RESULT_TAGS.has(resolvedToken) && !appliedTypeArgs) {
    const nodeId = nextId('stdval');
    ctx.nodes.push({
      id: nodeId,
      type: 'stdValueNode',
      position: { x: 380, y: 130 + ctx.nodes.length * 40 },
      data: { kind: 'stdValue', qualifiedName: resolvedToken, namespace: 'Std', valueType: 'Bool', label: resolvedToken },
    });
    return { nodeId };
  }

  // Std constant/value
  if (stdValueByName.has(resolvedToken) && !appliedTypeArgs) {
    const stdValue = stdValueByName.get(resolvedToken)!;
    const nodeId = nextId('stdval');
    ctx.nodes.push({
      id: nodeId,
      type: 'stdValueNode',
      position: { x: 380, y: 130 + ctx.nodes.length * 40 },
      data: {
        kind: 'stdValue',
        qualifiedName: stdValue.qualifiedName,
        namespace: stdValue.namespace,
        valueType: stdValue.valueType,
        label: stdValue.qualifiedName,
      },
    });
    return { nodeId };
  }

  // Std function used as a value (callback)
  const stdReference = selectStdFunctionOverload(resolvedToken);
  if (stdReference) {
    const nodeId = nextId('stdfuncref');
    ctx.nodes.push({
      id: nodeId,
      type: 'stdFuncNode',
      position: { x: 380, y: 130 + ctx.nodes.length * 40 },
      data: {
        kind: 'stdFunc',
        qualifiedName: stdReference.qualifiedName,
        namespace: stdReference.namespace,
        typeParams: stdReference.typeParams,
        appliedTypeArgs,
        params: stdReference.params,
        returnType: stdReference.returnType,
        signatureKey: stdReference.signatureKey,
        displayName: formatFunctionDisplayName(stdReference.qualifiedName, appliedTypeArgs ?? stdReference.typeParams),
        displaySignature: stdReference.displaySignature,
        hasVariadicTail: stdReference.hasVariadicTail,
        minimumArity: stdReference.minimumArity,
        overloadIndex: stdReference.overloadIndex,
        overloadCount: stdReference.overloadCount,
        isTerminal: stdReference.isTerminal,
        isReference: true,
        label: stdReference.displayName,
      },
    });
    return { nodeId };
  }

  if (stdFunctionsByName.has(resolvedToken)) {
    throw new FleauxImportError(`Ambiguous overloaded Std function reference '${token}'. Call it with arguments so the visual importer can choose an overload.`);
  }

  // User function used as a value (callback)
  if (ctx.userFunctions.has(resolvedToken)) {
    const user = ctx.userFunctions.get(resolvedToken)!;
    const nodeId = nextId('userfuncref');
    const params = createFunctionParams(nodeId, user.params);
    ctx.nodes.push({
      id: nodeId,
      type: 'userFuncNode',
      position: { x: 380, y: 130 + ctx.nodes.length * 40 },
      data: {
        kind: 'userFunc',
        functionName: user.name,
        functionNodeId: '',
        typeParams: user.typeParams,
        appliedTypeArgs,
        params,
        returnType: user.returnType,
        isReference: true,
        label: formatFunctionDisplayName(user.name, appliedTypeArgs ?? user.typeParams),
      },
    });
    return { nodeId };
  }

  // Imported symbols may not be present in local source; represent them as external user funcs.
  if (isExternalSymbol(ctx, resolvedToken)) {
    const nodeId = nextId('extfuncref');
    ctx.nodes.push({
      id: nodeId,
      type: 'userFuncNode',
      position: { x: 380, y: 130 + ctx.nodes.length * 40 },
      data: {
        kind: 'userFunc',
        functionName: resolvedToken,
        functionNodeId: '',
        appliedTypeArgs,
        params: [{ id: `${nodeId}-param-0`, name: 'arg1', type: 'Any' }],
        returnType: 'Any',
        isReference: true,
        label: formatFunctionDisplayName(resolvedToken, appliedTypeArgs),
      },
    });
    return { nodeId };
  }

  throw new FleauxImportError(`Unsupported value token '${token}'. Add support or simplify source before import.`);
}

/** The items of a parenthesized list's inner text, and whether it ends with a trailing comma (`(a, b,)`). */
function splitItems(inner: string): { items: string[]; trailingComma: boolean } {
  const topLevel = topLevelMask(inner, true);
  const items: string[] = [];
  let cursor = 0;
  for (let index = 0; index < inner.length; index += 1) {
    if (topLevel[index] && inner[index] === ',') {
      items.push(inner.slice(cursor, index).trim());
      cursor = index + 1;
    }
  }
  const last = inner.slice(cursor).trim();
  const trailingComma = items.length > 0 && last.length === 0;
  if (!trailingComma) {
    items.push(last);
  }
  if (items.some((item) => item.length === 0)) {
    throw new FleauxImportError(`Empty item in '(${inner})'.`);
  }
  return { items, trailingComma };
}

/**
 * Parse a value expression (no top-level ->). Handles literals, empty tuples,
 * tuple groupings, and identifiers. pipelineSource is used for _ placeholders.
 */
function parseValueAsSource(
  ctx: BuildContext,
  text: string,
  nextId: (prefix: string) => string,
  pipelineSource: SourceRef | null,
): SourceRef {
  const token = text.trim();

  const literal = parseLiteralToken(token);
  if (literal) return createLiteralNode(ctx, nextId('lit'), literal.valueType, literal.value);

  // Check for closure expression before tuple: `(params): ReturnType = body`
  const closureSyntax = tryParseClosureSyntax(token);
  if (closureSyntax) {
    return createClosureNodeFromSyntax(ctx, closureSyntax, nextId);
  }

  if (isWrappedByParens(token)) {
    const inner = token.slice(1, -1).trim();
    if (!inner) {
      // Empty tuple ()
      return createTupleNode(ctx, nextId('tuple'), []);
    }
    const { items, trailingComma } = splitItems(inner);
    if (items.length === 1 && !trailingComma) {
      return parseExpressionAsSource(ctx, inner, nextId, pipelineSource);
    }
    // Several items, or a trailing comma, make a tuple value. Each item may itself be a pipeline expression.
    const args = items.map((item) => parseExpressionAsSource(ctx, item, nextId, pipelineSource));
    return createTupleNode(ctx, nextId('tuple'), args);
  }

  return resolveIdentifierAsSource(ctx, token, nextId, pipelineSource);
}

/**
 * Parse any expression: if it contains -> at depth 0, treat as a pipeline;
 * otherwise treat as a plain value. pipelineSource is propagated for _ in values.
 */
function parseExpressionAsSource(
  ctx: BuildContext,
  text: string,
  nextId: (prefix: string) => string,
  pipelineSource: SourceRef | null,
): SourceRef {
  const trimmed = text.trim();
  if (hasTopLevelDelimiter(trimmed, '->')) {
    // Nested pipeline – _ inside it refers to its own pipeline stages, not the outer one
    return parsePipelineExpression(trimmed, ctx, nextId);
  }
  return parseValueAsSource(ctx, trimmed, nextId, pipelineSource);
}

function stdCallData(std: StdFunctionEntry, appliedTypeArgs: string[] | undefined): StdFuncData {
  return {
    kind: 'stdFunc',
    qualifiedName: std.qualifiedName,
    namespace: std.namespace,
    typeParams: std.typeParams,
    appliedTypeArgs,
    params: std.params,
    returnType: std.returnType,
    signatureKey: std.signatureKey,
    displayName: formatFunctionDisplayName(std.qualifiedName, appliedTypeArgs ?? std.typeParams),
    displaySignature: std.displaySignature,
    hasVariadicTail: std.hasVariadicTail,
    minimumArity: std.minimumArity,
    overloadIndex: std.overloadIndex,
    overloadCount: std.overloadCount,
    isTerminal: std.isTerminal,
    label: std.displayName,
  };
}

function tryParseCallTarget(
  targetToken: string,
  ctx: BuildContext,
  inferredArity?: number,
): { kind: 'stdFunc'; data: StdFuncData } | { kind: 'userFunc'; data: UserFuncData } | null {
  const splitTarget = splitNameAndTypeParams(targetToken);
  const resolvedToken = splitTarget.name;
  const appliedTypeArgs = splitTarget.typeParams.length > 0 ? splitTarget.typeParams : undefined;

  // An operator stands for a Std builtin chosen by its operand count; the node keeps the operator, so a strong type's
  // operator function still applies when it is serialized.
  if (isOperatorToken(resolvedToken)) {
    const operandCount = inferredArity ?? 2;
    const entry = resolveOperator(resolvedToken, operandCount);
    const std = entry ? selectStdFunctionOverload(entry.qualifiedName, operandCount) : null;
    if (!entry || !std) {
      throw new FleauxImportError(`Operator '${resolvedToken}' does not take ${operandCount} operand(s).`);
    }
    return {
      kind: 'stdFunc',
      data: { ...stdCallData(std, appliedTypeArgs), operator: entry.token, displayName: entry.token, label: entry.token },
    };
  }

  const std = selectStdFunctionOverload(resolvedToken, inferredArity);
  if (std) {
    return { kind: 'stdFunc', data: stdCallData(std, appliedTypeArgs) };
  }

  if (stdFunctionsByName.has(resolvedToken)) {
    throw new FleauxImportError(
      `Could not resolve a unique Std overload for '${resolvedToken}'${formatArityErrorSuffix(inferredArity)}.`,
    );
  }

  const user = ctx.userFunctions.get(resolvedToken);
  if (user && user.isValueBinding !== true) {
    const params = createFunctionParams('', user.params);
    return {
      kind: 'userFunc',
      data: {
        kind: 'userFunc',
        functionName: user.name,
        functionNodeId: '',
        typeParams: user.typeParams,
        appliedTypeArgs,
        params,
        returnType: user.returnType,
        label: formatFunctionDisplayName(user.name, appliedTypeArgs ?? user.typeParams),
      },
    };
  }

  if (isExternalSymbol(ctx, resolvedToken)) {
    const arity = inferredArity ?? 0;
    return {
      kind: 'userFunc',
      data: {
        kind: 'userFunc',
        functionName: resolvedToken,
        functionNodeId: '',
        appliedTypeArgs,
        params: Array.from({ length: arity }, (_, index) => ({ id: `external-param-${index}`, name: `arg${index + 1}`, type: 'Any' })),
        returnType: 'Any',
        label: formatFunctionDisplayName(resolvedToken, appliedTypeArgs),
      },
    };
  }

  return null;
}

function parseCallTarget(
  targetToken: string,
  ctx: BuildContext,
  inferredArity?: number,
): { kind: 'stdFunc'; data: StdFuncData } | { kind: 'userFunc'; data: UserFuncData } {
  const resolved = tryParseCallTarget(targetToken, ctx, inferredArity);
  if (resolved) {
    return resolved;
  }

  throw new FleauxImportError(`Unsupported call target '${targetToken}'.`);
}

/**
 * If text is a paren-wrapped tuple, return its elements as SourceRefs.
 * pipelineSource is used to resolve _ placeholders inside the tuple.
 * Returns null if text is not paren-wrapped.
 */
function parseTupleArgsIfPresent(
  text: string,
  ctx: BuildContext,
  nextId: (prefix: string) => string,
  pipelineSource: SourceRef | null,
): SourceRef[] | null {
  const token = text.trim();
  if (!isWrappedByParens(token)) return null;
  const inner = token.slice(1, -1).trim();
  if (!inner) return [];
  const { items, trailingComma } = splitItems(inner);
  const spreads = items.map((item) => item.length > 3 && item.endsWith('...'));
  if (trailingComma && spreads.some(Boolean)) {
    throw new FleauxImportError("A tuple written with a trailing comma cannot spread items with 'x...'.");
  }
  // `(x)` is the value x and `(a, b,)` one tuple value: neither is a list of several arguments. `(x...)` is.
  if (trailingComma || (items.length === 1 && !spreads[0])) return null;
  return items.map((item, index) => (spreads[index]
    ? createSpreadNode(ctx, nextId('spread'), parseExpressionAsSource(ctx, item.slice(0, -3).trim(), nextId, pipelineSource))
    : parseExpressionAsSource(ctx, item, nextId, pipelineSource)));
}

/**
 * Build a closure node from already-parsed closure syntax and wire its body.
 */
function createClosureNodeFromSyntax(
  ctx: BuildContext,
  closureSyntax: ClosureSyntax,
  nextId: (prefix: string) => string,
): SourceRef {
  const { typeParams, params, returnType, body } = closureSyntax;
  const closureNodeId = nextId('closure');
  const displayParams =
    (typeParams.length > 0 ? `<${typeParams.join(', ')}>` : '')
    + (params.length === 0 ? '()' : `(${params.map((p) => `${p.name}: ${p.type}`).join(', ')})`);

  ctx.nodes.push({
    id: closureNodeId,
    type: 'closureNode',
    position: { x: 380, y: 130 + ctx.nodes.length * 48 },
    data: {
      kind: 'closure',
      typeParams: typeParams.length > 0 ? typeParams : undefined,
      params,
      returnType,
      label: `${displayParams}: ${returnType}`,
    } as ClosureData,
  });

  // Push closure scope so body nodes can reference its params
  const paramIndexByName = new Map<string, number>();
  params.forEach((param, index) => paramIndexByName.set(param.name, index));
  ctx.closureScopes.push({ nodeId: closureNodeId, paramIndexByName });

  let bodySource: SourceRef;
  try {
    bodySource = parsePipelineExpression(body, ctx, nextId);
  } finally {
    ctx.closureScopes.pop();
  }

  // Wire body-root edge (from closure-body-anchor → body root node's closure-body-root handle)
  addClosureBodyRootEdge(ctx, closureNodeId, bodySource);

  return { nodeId: closureNodeId };
}

type CallTarget = { kind: 'stdFunc'; data: StdFuncData } | { kind: 'userFunc'; data: UserFuncData };

// True when a call target names a function value: a parameter in scope or a value let.
function isFunctionValueName(ctx: BuildContext, name: string): boolean {
  return isParameterInScope(ctx, name) || ctx.userFunctions.get(name)?.isValueBinding === true;
}

// The declared type of a function value named by a call target, when the source declares it.
function functionValueType(ctx: BuildContext, name: string): string | undefined {
  for (let index = ctx.closureScopes.length - 1; index >= 0; index -= 1) {
    const scope = ctx.closureScopes[index];
    const paramIndex = scope.paramIndexByName.get(name);
    if (paramIndex !== undefined) {
      const closure = ctx.nodes.find((node) => node.id === scope.nodeId)?.data as ClosureData | undefined;
      return closure?.params[paramIndex]?.type;
    }
  }
  const letParamIndex = ctx.letSourceRef?.paramIndexByName.get(name);
  if (letParamIndex !== undefined && ctx.letSourceRef) {
    const letData = ctx.nodes.find((node) => node.id === ctx.letSourceRef!.nodeId)?.data as { params?: Param[] } | undefined;
    return letData?.params?.[letParamIndex]?.type;
  }
  return ctx.userFunctions.get(name)?.returnType;
}

/** Push a call of the function value `fn` with `args` in its slots: `(args) -> f`, or a closure stage. */
function pushCallValueNode(
  ctx: BuildContext,
  nextId: (prefix: string) => string,
  fn: SourceRef,
  args: SourceRef[],
  stageIndex: number,
  functionType: string | undefined,
): SourceRef {
  const nodeId = nextId('callvalue');
  ctx.nodes.push({
    id: nodeId,
    type: 'callValueNode',
    position: { x: 640 + stageIndex * 140, y: 170 + ctx.nodes.length * 8 },
    data: { kind: 'callValue', slots: args.length, functionType, label: 'call value' },
  });
  addEdge(ctx, fn, nodeId, 'callvalue-fn');
  args.forEach((arg, index) => addEdge(ctx, arg, nodeId, `callvalue-in-${index}`));
  return { nodeId };
}

// A stage the parser takes as a call target: a name or qualified name (with type arguments or not), or an operator.
function isCallTargetStage(part: string): boolean {
  return /^[A-Za-z_]\w*(?:\.[A-Za-z_]\w*)*(?:\s*<.*>)?$/s.test(part) || isOperatorToken(part);
}

/**
 * A closure written as a stage, starting at `parts[stageIndex]`. As in the parser, its body is the value after `=`
 * followed by stages up to and including the first stage that is a call target; the stages after that are outside
 * the closure. (A closure anywhere else takes the rest of its expression, which `parsePipelineExpression` handles.)
 */
function matchClosureStage(parts: string[], stageIndex: number): { syntax: ClosureSyntax; endIndex: number } | null {
  const first = parts[stageIndex]?.trim() ?? '';
  if ((!first.startsWith('(') && !first.startsWith('<')) || !tryParseClosureSyntax(first)) {
    return null;
  }
  let candidate = first;
  let endIndex = stageIndex;
  for (let index = stageIndex + 1; index < parts.length; index += 1) {
    const stage = parts[index].trim();
    candidate = `${candidate} -> ${stage}`;
    endIndex = index;
    if (isCallTargetStage(stage)) {
      break;
    }
  }
  const syntax = tryParseClosureSyntax(candidate);
  return syntax ? { syntax, endIndex } : null;
}

/**
 * Push a call node whose arguments are `args`, wired to its slots: one slot per parameter, and one per argument of a
 * variadic tail. A call whose argument count the callee does not take is not imported.
 */
function pushCallNode(ctx: BuildContext, nodeId: string, target: CallTarget, stageIndex: number, args: SourceRef[]): void {
  const params = target.data.params;
  const name = target.kind === 'stdFunc' ? target.data.qualifiedName : target.data.functionName;
  // A spread supplies an unknown number of arguments; the slots after it may stay empty. Without one, the argument
  // count must match.
  if (!args.some((arg) => isSpreadRef(ctx, arg))) {
    if (!matchesArity(params, args.length)) {
      throw new FleauxImportError(`'${name}' takes ${formatAritySummary(params)} but is called with ${args.length}.`);
    }
  } else if (!hasVariadicTail(params) && args.length > params.length) {
    throw new FleauxImportError(`'${name}' takes ${formatAritySummary(params)} but is called with ${args.length}.`);
  }
  if (hasVariadicTail(params)) {
    target.data.variadicSlots = Math.max(0, args.length - minArity(params));
  }

  ctx.nodes.push({
    id: nodeId,
    type: target.kind === 'stdFunc' ? 'stdFuncNode' : 'userFuncNode',
    position: { x: 640 + stageIndex * 140, y: 170 + ctx.nodes.length * 8 },
    data: target.data,
  });
  const prefix = target.kind === 'stdFunc' ? 'stdfunc-in-' : 'userfunc-in-';
  args.forEach((arg, index) => addEdge(ctx, arg, nodeId, `${prefix}${index}`));
}

function parsePipelineExpression(
  expression: string,
  ctx: BuildContext,
  nextId: (prefix: string) => string,
): SourceRef {
  const trimmed = expression.trim();

  // Check if the entire expression is a closure BEFORE splitting by `->`
  // This handles: let body = closure, and tuple-element closures with pipeline bodies.
  const wholeExprClosure = tryParseClosureSyntax(trimmed);
  if (wholeExprClosure) {
    return createClosureNodeFromSyntax(ctx, wholeExprClosure, nextId);
  }

  const parts = splitTopLevel(trimmed, '->');
  if (parts.length === 0) throw new FleauxImportError('Empty expression.');

  if (parts.length === 1) {
    return parseValueAsSource(ctx, parts[0], nextId, null);
  }

  // Establish initial current value from parts[0]
  let current: CurrentValue;
  const initialArgs = parseTupleArgsIfPresent(parts[0], ctx, nextId, null);
  if (initialArgs !== null) {
    current = { kind: 'args', args: initialArgs };
  } else {
    current = { kind: 'source', source: parseValueAsSource(ctx, parts[0], nextId, null) };
  }

  let stageIndex = 1;
  while (stageIndex < parts.length) {
    const part = parts[stageIndex].trim();

    if (isWrappedByParens(part)) {
      // Intermediate tuple args: e.g. (_, foo) -> Bar
      // _ refers to the current pipeline value; materialise args if needed.
      let currentSource: SourceRef;
      if (current.kind === 'source') {
        currentSource = current.source;
      } else {
        // Materialise the accumulated args into a concrete tuple node
        assertNoSpread(ctx, current.args);
        const tupleRef = createTupleNode(ctx, nextId('tuple'), current.args);
        current = { kind: 'source', source: tupleRef };
        currentSource = tupleRef;
      }

      stageIndex += 1;
      if (stageIndex >= parts.length) {
        // A tuple template with no call target after it is a value.
        return parseValueAsSource(ctx, part, nextId, currentSource);
      }

      // The template is the parameter list of the target after it; `(x)` and `(a, b,)` are one argument.
      const newArgs = parseTupleArgsIfPresent(part, ctx, nextId, currentSource)
        ?? [parseValueAsSource(ctx, part, nextId, currentSource)];
      const templateClosure = matchClosureStage(parts, stageIndex);
      if (templateClosure) {
        const closureRef = createClosureNodeFromSyntax(ctx, templateClosure.syntax, nextId);
        current = { kind: 'source', source: pushCallValueNode(ctx, nextId, closureRef, newArgs, stageIndex, undefined) };
        stageIndex = templateClosure.endIndex + 1;
        continue;
      }
      const callTargetToken = parts[stageIndex].trim();
      const target = parseCallTarget(callTargetToken, ctx, newArgs.length);
      const nodeId = nextId('call');
      pushCallNode(ctx, nodeId, target, stageIndex, newArgs);
      if (target.kind === 'stdFunc' && target.data.isTerminal === true && stageIndex < parts.length - 1) {
        throw new FleauxImportError(`Terminal Std function '${target.data.qualifiedName}' cannot be followed by additional pipeline stages.`);
      }
      current = { kind: 'source', source: { nodeId } };
    } else {
      // A closure stage calls the closure with the stage's arguments.
      const closureStage = matchClosureStage(parts, stageIndex);
      if (closureStage) {
        const closureRef = createClosureNodeFromSyntax(ctx, closureStage.syntax, nextId);
        const closureArgs = current.kind === 'args' ? current.args : [current.source];
        current = { kind: 'source', source: pushCallValueNode(ctx, nextId, closureRef, closureArgs, stageIndex, undefined) };
        stageIndex = closureStage.endIndex + 1;
        continue;
      }

      // Direct call target: a parameter list's items are the arguments, anything else is one argument.
      const args = current.kind === 'args' ? current.args : [current.source];
      const target = tryParseCallTarget(part, ctx, args.length);

      if (target) {
        const nodeId = nextId('call');
        pushCallNode(ctx, nodeId, target, stageIndex, args);
        if (target.kind === 'stdFunc' && target.data.isTerminal === true && stageIndex < parts.length - 1) {
          throw new FleauxImportError(`Terminal Std function '${target.data.qualifiedName}' cannot be followed by additional pipeline stages.`);
        }
        current = { kind: 'source', source: { nodeId } };
      } else if (isFunctionValueName(ctx, part)) {
        // A parameter or value let of function type: call the value it holds.
        const fn = resolveIdentifierAsSource(ctx, part, nextId, null);
        current = { kind: 'source', source: pushCallValueNode(ctx, nextId, fn, args, stageIndex, functionValueType(ctx, part)) };
      } else if (args.length === 0 && stdValueByName.has(part)) {
        // `() -> Std.Pi`: a Std constant.
        current = { kind: 'source', source: parseValueAsSource(ctx, part, nextId, null) };
      } else {
        throw new FleauxImportError(`'${part}' is not a function the importer knows.`);
      }
    }

    if (stageIndex >= parts.length) {
      break;
    }
    stageIndex += 1;
  }

  if (current.kind !== 'source') throw new FleauxImportError('Pipeline did not resolve to a source node.');
  return current.source;
}

export function importFleauxSourceToGraph(sourceText: string): FleauxGraphImportResult {
  const nextId = nextIdFactory();
  const nodes: Node<FleauxNodeData>[] = [];
  const edges: FleauxEdge[] = [];
  const userFunctions = new Map<string, LetDef>();
  const parsedLetsByStatement = new Map<string, LetDef>();
  const importedModules = new Set<string>();

  const ctx: BuildContext = { nodes, edges, userFunctions, importedModules, closureScopes: [] };

  const statements = splitStatements(stripLineComments(withoutRawStrings(sourceText)));

  //  First pass: collect ALL let definitions for forward-reference support
  for (const stmt of statements) {
    if (stmt.startsWith('let ')) {
      try {
        const letDef = parseLet(stmt);
        parsedLetsByStatement.set(stmt, letDef);
        userFunctions.set(letDef.name, letDef);
      } catch {
        // Surface errors in the second pass where context is clearer
      }
    }
  }

  //  Second pass: build nodes and edges. A statement the graph cannot represent is kept verbatim as raw source, so
  //  one unsupported construct does not fail the whole import.
  let importY = 60;
  let declarationY = 200;

  function importStatement(stmt: string): void {
    const importMatch = /^import\s+([^\s]+)$/.exec(stmt);
    if (importMatch) {
      importedModules.add(importMatch[1]);
      nodes.push({
        id: nextId('import'),
        type: 'importNode',
        position: { x: 60, y: importY },
        data: { kind: 'import', moduleName: importMatch[1], label: `import ${importMatch[1]}` },
      });
      importY += 90;
      return;
    }

    if (stmt.startsWith('type ')) {
      const typeDecl = parseTypeDecl(stmt);
      nodes.push({
        id: nextId('type'),
        type: 'typeNode',
        position: { x: 60, y: declarationY },
        data: {
          kind: 'typeDecl',
          name: typeDecl.name,
          targetType: typeDecl.targetType,
          separator: typeDecl.separator,
          label: 'type ' + typeDecl.name + ' ' + typeDecl.separator + ' ' + typeDecl.targetType,
        } as TypeDeclData,
      });
      declarationY += 110;
      return;
    }

    if (stmt.startsWith('alias ')) {
      const aliasDecl = parseAliasDecl(stmt);
      nodes.push({
        id: nextId('alias'),
        type: 'aliasNode',
        position: { x: 60, y: declarationY },
        data: {
          kind: 'aliasDecl',
          name: aliasDecl.name,
          targetType: aliasDecl.targetType,
          label: 'alias ' + aliasDecl.name + ' = ' + aliasDecl.targetType,
        } as AliasDeclData,
      });
      declarationY += 110;
      return;
    }

    if (stmt.startsWith('let ')) {
      const letDef = parsedLetsByStatement.get(stmt) ?? parseLet(stmt);
      const letNodeId = nextId('let');
      const letParams = createFunctionParams(letNodeId, letDef.params);
      nodes.push({
        id: letNodeId,
        type: 'letNode',
        position: { x: 60, y: declarationY },
        data: {
          kind: 'let',
          name: letDef.name,
          typeParams: letDef.typeParams,
          params: letParams,
          returnType: letDef.returnType,
          isValueBinding: letDef.isValueBinding === true,
          label: `let ${formatFunctionDisplayName(letDef.name, letDef.typeParams)}`,
        },
      });

      const paramIndexByName = new Map<string, number>();
      letParams.forEach((param, index) => paramIndexByName.set(param.name, index));
      ctx.letSourceRef = { nodeId: letNodeId, paramIndexByName };

      const bodySource = parsePipelineExpression(letDef.body, ctx, nextId);
      // Anchor the let body root explicitly so serialization can recover body graphs
      // even for constants/zero-arg calls that do not consume let parameters.
      addEdge(
        ctx,
        { nodeId: letNodeId, sourceHandle: bodySource.sourceHandle },
        bodySource.nodeId,
        'let-body-root',
      );

      ctx.letSourceRef = undefined;
      declarationY += 180;
      return;
    }

    // Top-level expression statement
    parsePipelineExpression(stmt, ctx, nextId);
  }

  for (const stmt of statements) {
    const nodeCount = nodes.length;
    const edgeCount = edges.length;
    try {
      importStatement(stmt);
    } catch (error) {
      if (!(error instanceof FleauxImportError)) {
        throw error;
      }
      nodes.length = nodeCount;
      edges.length = edgeCount;
      ctx.letSourceRef = undefined;
      ctx.closureScopes = [];
      nodes.push({
        id: nextId('raw'),
        type: 'rawSourceNode',
        position: { x: 60, y: declarationY },
        data: {
          kind: 'rawSource',
          statementText: stmt,
          importError: error.message,
          label: stmt.split('\n')[0],
        } as RawSourceData,
      });
      declarationY += 110;
    }
  }

  linkUserFunctionCalls(nodes);
  return { nodes, edges };
}

// Links each user function node to the let node that declares it (when exactly one does), sharing the let's
// parameters, so that editing the let's signature updates its calls and references.
function linkUserFunctionCalls(nodes: Node<FleauxNodeData>[]): void {
  const letNodesByName = new Map<string, Node<FleauxNodeData>[]>();
  for (const node of nodes) {
    if (node.data.kind === 'let') {
      const group = letNodesByName.get(node.data.name) ?? [];
      group.push(node);
      letNodesByName.set(node.data.name, group);
    }
  }
  for (const node of nodes) {
    if (node.data.kind !== 'userFunc') {
      continue;
    }
    const matches = letNodesByName.get(node.data.functionName) ?? [];
    if (matches.length !== 1) {
      continue;
    }
    const letData = matches[0].data as LetData;
    node.data = {
      ...(node.data as UserFuncData),
      functionNodeId: matches[0].id,
      params: letData.params.map((param) => ({ ...param })),
    };
  }
}







