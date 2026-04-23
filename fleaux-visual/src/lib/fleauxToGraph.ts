import type { Node } from '@xyflow/react';
import type { FleauxEdge, FleauxNodeData, LiteralValueType, StdFuncData, UserFuncData } from './types';
import { formatFunctionDisplayName, matchesArity } from './functionSignatures';
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
type LetDef = { name: string; typeParams?: string[]; params: Param[]; returnType: string; body: string };

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
};

const stdFunctionsByName = new Map<string, StdFunctionEntry[]>();
for (const entry of STD_FUNCTIONS) {
  const group = stdFunctionsByName.get(entry.qualifiedName) ?? [];
  group.push(entry);
  stdFunctionsByName.set(entry.qualifiedName, group);
}
const stdValueByName = new Map(STD_VALUES.map((entry) => [entry.qualifiedName, entry]));

const OPERATOR_TO_STD_TARGET = new Map<string, string>([
  ['+', 'Std.Add'],
  ['-', 'Std.Subtract'],
  ['*', 'Std.Multiply'],
  ['/', 'Std.Divide'],
  ['%', 'Std.Mod'],
  ['^', 'Std.Pow'],
  ['>', 'Std.GreaterThan'],
  ['<', 'Std.LessThan'],
  ['>=', 'Std.GreaterOrEqual'],
  ['<=', 'Std.LessOrEqual'],
  ['==', 'Std.Equal'],
  ['!=', 'Std.NotEqual'],
  ['&&', 'Std.And'],
  ['||', 'Std.Or'],
]);

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
    return null;
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

function splitStatements(source: string): string[] {
  const statements: string[] = [];
  let depth = 0;
  let inString = false;
  let escape = false;
  let start = 0;

  for (let index = 0; index < source.length; index += 1) {
    const ch = source[index];
    if (inString) {
      if (escape) { escape = false; }
      else if (ch === '\\') { escape = true; }
      else if (ch === '"') { inString = false; }
      continue;
    }
    if (ch === '"') { inString = true; continue; }
    if (ch === '(') { depth += 1; continue; }
    if (ch === ')') { depth = Math.max(0, depth - 1); continue; }
    if (ch === ';' && depth === 0) {
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
  let depth = 0;
  let inString = false;
  let escape = false;
  let cursor = 0;

  for (let index = 0; index < text.length; index += 1) {
    const ch = text[index];
    if (inString) {
      if (escape) { escape = false; }
      else if (ch === '\\') { escape = true; }
      else if (ch === '"') { inString = false; }
      continue;
    }
    if (ch === '"') { inString = true; continue; }
    if (ch === '(') { depth += 1; continue; }
    if (ch === ')') { depth = Math.max(0, depth - 1); continue; }
    if (depth === 0 && text.startsWith(delimiter, index)) {
      parts.push(text.slice(cursor, index).trim());
      cursor = index + delimiter.length;
      index += delimiter.length - 1;
    }
  }

  parts.push(text.slice(cursor).trim());
  return parts.filter((part) => part.length > 0);
}

function hasTopLevelDelimiter(text: string, delimiter: string): boolean {
  let depth = 0;
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
    if (ch === '(') { depth += 1; continue; }
    if (ch === ')') { depth = Math.max(0, depth - 1); continue; }
    if (depth === 0 && text.startsWith(delimiter, index)) {
      return true;
    }
  }

  return false;
}

function parseParams(paramText: string): Param[] {
  const trimmed = paramText.trim();
  if (!trimmed) return [];
  return splitTopLevel(trimmed, ',').map((entry) => {
    const colonIdx = entry.indexOf(':');
    if (colonIdx < 1) throw new FleauxImportError(`Invalid parameter entry: '${entry}'`);
    return { name: entry.slice(0, colonIdx).trim(), type: entry.slice(colonIdx + 1).trim() };
  });
}

function splitNameAndTypeParams(rawName: string): { name: string; typeParams: string[] } {
  const trimmed = rawName.trim();
  if (!trimmed.endsWith('>')) {
    return { name: trimmed, typeParams: [] };
  }

  let depth = 0;
  for (let index = trimmed.length - 1; index >= 0; index -= 1) {
    const ch = trimmed[index];
    if (ch === '>') {
      depth += 1;
      continue;
    }
    if (ch === '<') {
      depth -= 1;
      if (depth === 0) {
        const name = trimmed.slice(0, index).trim();
        const typeParamsText = trimmed.slice(index + 1, -1).trim();
        const typeParams = typeParamsText.length === 0 ? [] : splitTopLevel(typeParamsText, ',');
        return { name, typeParams };
      }
    }
  }

  return { name: trimmed, typeParams: [] };
}

function parseLet(stmt: string): LetDef {
  const letMatch = /^let\s+(.+?)\s*\((.*)\)\s*:\s*(.+?)\s*(?:::|=)\s*(.+)$/s.exec(stmt);
  if (!letMatch) throw new FleauxImportError(`Unsupported let statement: ${stmt}`);
  const { name, typeParams } = splitNameAndTypeParams(letMatch[1]);
  return {
    name,
    typeParams,
    params: parseParams(letMatch[2]),
    returnType: letMatch[3].trim(),
    body: letMatch[4].trim(),
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

function isQualifiedIdentifier(token: string): boolean {
  return /^[A-Za-z_][\w]*(?:\.[A-Za-z_][\w]*)*$/.test(token);
}

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
  if (/^-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?$/.test(token)) {
    return { valueType: inferNumericLiteralType(token), value: token };
  }
  if (token === 'True' || token === 'False') return { valueType: 'Bool', value: token };
  if (token === 'null') return { valueType: 'Null', value: 'null' };
  return null;
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
  // _ refers to the current flowing pipeline value
  if (token === '_') {
    if (!pipelineSource) throw new FleauxImportError('_ used with no current pipeline value.');
    return pipelineSource;
  }

  // Let parameter reference
  const paramIndex = ctx.letSourceRef?.paramIndexByName.get(token);
  if (paramIndex !== undefined && ctx.letSourceRef) {
    return { nodeId: ctx.letSourceRef.nodeId, sourceHandle: `let-param-${paramIndex}` };
  }

  // Std constant/value
  if (stdValueByName.has(token)) {
    const stdValue = stdValueByName.get(token)!;
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
  const stdReference = selectStdFunctionOverload(token);
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
        params: stdReference.params,
        returnType: stdReference.returnType,
        signatureKey: stdReference.signatureKey,
        displayName: stdReference.displayName,
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

  if (stdFunctionsByName.has(token)) {
    throw new FleauxImportError(`Ambiguous overloaded Std function reference '${token}'. Call it with arguments so the visual importer can choose an overload.`);
  }

  // User function used as a value (callback)
  if (ctx.userFunctions.has(token)) {
    const user = ctx.userFunctions.get(token)!;
    const nodeId = nextId('userfuncref');
    ctx.nodes.push({
      id: nodeId,
      type: 'userFuncNode',
      position: { x: 380, y: 130 + ctx.nodes.length * 40 },
      data: {
        kind: 'userFunc',
        functionName: user.name,
        functionNodeId: '',
        typeParams: user.typeParams,
        params: user.params,
        returnType: user.returnType,
        isReference: true,
        label: formatFunctionDisplayName(user.name, user.typeParams),
      },
    });
    return { nodeId };
  }

  // Imported symbols may not be present in local source; represent them as external user funcs.
  if (ctx.importedModules.size > 0 && isQualifiedIdentifier(token)) {
    const nodeId = nextId('extfuncref');
    ctx.nodes.push({
      id: nodeId,
      type: 'userFuncNode',
      position: { x: 380, y: 130 + ctx.nodes.length * 40 },
      data: {
        kind: 'userFunc',
        functionName: token,
        functionNodeId: '',
        params: [{ name: 'arg1', type: 'Any' }],
        returnType: 'Any',
        isReference: true,
        label: token,
      },
    });
    return { nodeId };
  }

  throw new FleauxImportError(`Unsupported value token '${token}'. Add support or simplify source before import.`);
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

  if (isWrappedByParens(token)) {
    const inner = token.slice(1, -1).trim();
    if (!inner) {
      // Empty tuple ()
      return createTupleNode(ctx, nextId('tuple'), []);
    }
    const parts = splitTopLevel(inner, ',');
    // Each tuple element may itself be a pipeline expression
    const args = parts.map((part) => parseExpressionAsSource(ctx, part, nextId, pipelineSource));
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

function tryParseCallTarget(
  targetToken: string,
  ctx: BuildContext,
  inferredArity?: number,
): { kind: 'stdFunc'; data: StdFuncData } | { kind: 'userFunc'; data: UserFuncData } | null {
  const resolvedToken = OPERATOR_TO_STD_TARGET.get(targetToken) ?? targetToken;

  const std = selectStdFunctionOverload(resolvedToken, inferredArity);
  if (std) {
    return {
      kind: 'stdFunc',
      data: {
        kind: 'stdFunc',
        qualifiedName: std.qualifiedName,
        namespace: std.namespace,
        typeParams: std.typeParams,
        params: std.params,
        returnType: std.returnType,
        signatureKey: std.signatureKey,
        displayName: std.displayName,
        displaySignature: std.displaySignature,
        hasVariadicTail: std.hasVariadicTail,
        minimumArity: std.minimumArity,
        overloadIndex: std.overloadIndex,
        overloadCount: std.overloadCount,
        isTerminal: std.isTerminal,
        label: std.displayName,
      },
    };
  }

  if (stdFunctionsByName.has(resolvedToken)) {
    throw new FleauxImportError(
      `Could not resolve a unique Std overload for '${resolvedToken}'${formatArityErrorSuffix(inferredArity)}.`,
    );
  }

  const user = ctx.userFunctions.get(resolvedToken);
  if (user) {
    return {
      kind: 'userFunc',
      data: {
        kind: 'userFunc',
        functionName: user.name,
        functionNodeId: '',
        typeParams: user.typeParams,
        params: user.params,
        returnType: user.returnType,
        label: formatFunctionDisplayName(user.name, user.typeParams),
      },
    };
  }

  if (ctx.importedModules.size > 0 && isQualifiedIdentifier(resolvedToken)) {
    const arity = Math.max(1, inferredArity ?? 1);
    return {
      kind: 'userFunc',
      data: {
        kind: 'userFunc',
        functionName: resolvedToken,
        functionNodeId: '',
        params: Array.from({ length: arity }, (_, index) => ({ name: `arg${index + 1}`, type: 'Any' })),
        returnType: 'Any',
        label: resolvedToken,
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
  const parts = splitTopLevel(inner, ',');
  return parts.map((part) => parseExpressionAsSource(ctx, part, nextId, pipelineSource));
}

/** Push a call node (stdFunc or userFunc) and return its id. */
function pushCallNode(
  ctx: BuildContext,
  nodeId: string,
  target: { kind: 'stdFunc'; data: StdFuncData } | { kind: 'userFunc'; data: UserFuncData },
  stageIndex: number,
): void {
  if (target.kind === 'stdFunc') {
    ctx.nodes.push({
      id: nodeId,
      type: 'stdFuncNode',
      position: { x: 640 + stageIndex * 140, y: 170 + ctx.nodes.length * 8 },
      data: target.data,
    });
  } else {
    ctx.nodes.push({
      id: nodeId,
      type: 'userFuncNode',
      position: { x: 640 + stageIndex * 140, y: 170 + ctx.nodes.length * 8 },
      data: target.data,
    });
  }
}

/** Wire args or a single source into a call node. */
function wireInputs(
  ctx: BuildContext,
  nodeId: string,
  target: { kind: 'stdFunc'; data: StdFuncData } | { kind: 'userFunc'; data: UserFuncData },
  nextId: (prefix: string) => string,
  inputArgs: SourceRef[] | null,
  singleSource: SourceRef | null,
): void {
  const paramCount = target.data.params.length;
  const prefix = target.kind === 'stdFunc' ? 'stdfunc-in-' : 'userfunc-in-';

  if (inputArgs !== null) {
    if (inputArgs.length === paramCount) {
      inputArgs.forEach((arg, i) => addEdge(ctx, arg, nodeId, `${prefix}${i}`));
    } else {
      const tupleRef = createTupleNode(ctx, nextId('tuple'), inputArgs);
      addEdge(ctx, tupleRef, nodeId);
    }
  } else if (singleSource !== null) {
    if (paramCount === 1) {
      addEdge(ctx, singleSource, nodeId, `${prefix}0`);
    } else {
      addEdge(ctx, singleSource, nodeId);
    }
  }
}

function parsePipelineExpression(
  expression: string,
  ctx: BuildContext,
  nextId: (prefix: string) => string,
): SourceRef {
  const parts = splitTopLevel(expression, '->');
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
        const tupleRef = createTupleNode(ctx, nextId('tuple'), current.args);
        current = { kind: 'source', source: tupleRef };
        currentSource = tupleRef;
      }

      const newArgs = parseTupleArgsIfPresent(part, ctx, nextId, currentSource)!;
      stageIndex += 1;

      if (stageIndex >= parts.length) {
        // Trailing tuple with no following call target – return it as a value
        return createTupleNode(ctx, nextId('tuple'), newArgs);
      }

      const callTargetToken = parts[stageIndex].trim();
      const target = parseCallTarget(callTargetToken, ctx, newArgs.length);
      const nodeId = nextId('call');
      pushCallNode(ctx, nodeId, target, stageIndex);
      wireInputs(ctx, nodeId, target, nextId, newArgs, null);
      if (target.kind === 'stdFunc' && target.data.isTerminal === true && stageIndex < parts.length - 1) {
        throw new FleauxImportError(`Terminal Std function '${target.data.qualifiedName}' cannot be followed by additional pipeline stages.`);
      }
      current = { kind: 'source', source: { nodeId } };
    } else {
      // Direct call target; if not callable, treat this stage as a value source.
      const target = tryParseCallTarget(part, ctx, current.kind === 'args' ? current.args.length : 1);

      if (target) {
        const nodeId = nextId('call');
        pushCallNode(ctx, nodeId, target, stageIndex);

        if (current.kind === 'args') {
          wireInputs(ctx, nodeId, target, nextId, current.args, null);
        } else {
          wireInputs(ctx, nodeId, target, nextId, null, current.source);
        }
        if (target.kind === 'stdFunc' && target.data.isTerminal === true && stageIndex < parts.length - 1) {
          throw new FleauxImportError(`Terminal Std function '${target.data.qualifiedName}' cannot be followed by additional pipeline stages.`);
        }
        current = { kind: 'source', source: { nodeId } };
      } else {
        // Materialize args to a source so '_' can bind if used in the value expression.
        const currentSource = current.kind === 'source'
          ? current.source
          : createTupleNode(ctx, nextId('tuple'), current.args);
        const valueRef = parseExpressionAsSource(ctx, part, nextId, currentSource);
        current = { kind: 'source', source: valueRef };
      }
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

  const ctx: BuildContext = { nodes, edges, userFunctions, importedModules };

  const statements = splitStatements(stripLineComments(sourceText));

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

  //  Second pass: build nodes and edges
  let importY = 60;
  let letY = 200;

  for (const stmt of statements) {
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
      continue;
    }

    if (stmt.startsWith('let ')) {
      const letDef = parsedLetsByStatement.get(stmt) ?? parseLet(stmt);
      const letNodeId = nextId('let');
      nodes.push({
        id: letNodeId,
        type: 'letNode',
        position: { x: 60, y: letY },
        data: {
          kind: 'let',
          name: letDef.name,
          typeParams: letDef.typeParams,
          params: letDef.params,
          returnType: letDef.returnType,
          label: `let ${formatFunctionDisplayName(letDef.name, letDef.typeParams)}`,
        },
      });

      const paramIndexByName = new Map<string, number>();
      letDef.params.forEach((param, index) => paramIndexByName.set(param.name, index));
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
      letY += 180;
      continue;
    }

    // Top-level expression statement
    parsePipelineExpression(stmt, ctx, nextId);
  }

  return { nodes, edges };
}







