import type { Node } from '@xyflow/react';
import type {
  ClosureData,
  FleauxEdge,
  FleauxNodeData,
  LetData,
  LiteralData,
  StdFuncData,
  StdValueData,
  TupleData,
  UserFuncData,
} from './types';
import { hasVariadicTail, minArity } from './functionSignatures';

export class GraphSerializationError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'GraphSerializationError';
  }
}

export interface GraphSerializationResult {
  sourceText: string;
  imports: string[];
  letStatements: string[];
  expressionStatements: string[];
}

type GraphContext = {
  nodesById: Map<string, Node<FleauxNodeData>>;
  incomingByTarget: Map<string, FleauxEdge[]>;
  outgoingBySource: Map<string, FleauxEdge[]>;
  nodeOrder: Map<string, number>;
};

type LetContext = {
  letNodeId: string;
  typeParams?: string[];
  params: { name: string; type: string }[];
};

const LET_BODY_ROOT_HANDLE = 'let-body-root';
const CLOSURE_BODY_ROOT_HANDLE = 'closure-body-root';

function buildGraphContext(
  nodes: Node<FleauxNodeData>[],
  edges: FleauxEdge[],
): GraphContext {
  const nodesById = new Map<string, Node<FleauxNodeData>>();
  const incomingByTarget = new Map<string, FleauxEdge[]>();
  const outgoingBySource = new Map<string, FleauxEdge[]>();
  const nodeOrder = new Map<string, number>();

  nodes.forEach((node, index) => {
    nodesById.set(node.id, node);
    nodeOrder.set(node.id, index);
  });

  for (const edge of edges) {
    if (!nodesById.has(edge.source)) {
      throw new GraphSerializationError(`Edge '${edge.id}' references missing source node '${edge.source}'.`);
    }
    if (!nodesById.has(edge.target)) {
      throw new GraphSerializationError(`Edge '${edge.id}' references missing target node '${edge.target}'.`);
    }

    const incoming = incomingByTarget.get(edge.target) ?? [];
    incoming.push(edge);
    incomingByTarget.set(edge.target, incoming);

    const outgoing = outgoingBySource.get(edge.source) ?? [];
    outgoing.push(edge);
    outgoingBySource.set(edge.source, outgoing);
  }

  return { nodesById, incomingByTarget, outgoingBySource, nodeOrder };
}

function quoteString(value: string): string {
  return JSON.stringify(value);
}

function renderLiteral(data: LiteralData): string {
  switch (data.valueType) {
    case 'String':
      return quoteString(data.value);
    case 'Float64':
    case 'Int64':
    case 'UInt64':
    case 'Number':
      return data.value.trim() === '' ? '0' : data.value;
    case 'Bool': {
      const normalized = data.value.trim().toLowerCase();
      return normalized === 'false' ? 'False' : 'True';
    }
    case 'Null':
      return 'null';
    default:
      throw new GraphSerializationError(`Unsupported literal value type '${String(data.valueType)}'.`);
  }
}

function parseIndexedHandle(handle: string | null | undefined, prefix: string): number | null {
  if (!handle) {
    return null;
  }
  const match = new RegExp(`^${prefix}(\\d+)$`).exec(handle);
  return match ? Number.parseInt(match[1], 10) : null;
}

function getIncomingEdges(
  nodeId: string,
  ctx: GraphContext,
): FleauxEdge[] {
  return ctx.incomingByTarget.get(nodeId) ?? [];
}

function getPipelineInput(
  node: Node<FleauxNodeData>,
  ctx: GraphContext,
): FleauxEdge | null {
  const edges = getIncomingEdges(node.id, ctx).filter((edge) => edge.targetHandle == null);
  if (edges.length > 1) {
    throw new GraphSerializationError(
      `Node '${node.data.label}' has multiple pipeline inputs. Only one unhandled incoming edge is supported.`,
    );
  }
  return edges[0] ?? null;
}

function getIndexedInputs(
  node: Node<FleauxNodeData>,
  ctx: GraphContext,
  handlePrefix: string,
): Map<number, FleauxEdge> {
  const indexed = new Map<number, FleauxEdge>();
  const relevant = getIncomingEdges(node.id, ctx);

  for (const edge of relevant) {
    const index = parseIndexedHandle(edge.targetHandle, handlePrefix);
    if (index == null) {
      continue;
    }
    if (indexed.has(index)) {
      throw new GraphSerializationError(
        `Node '${node.data.label}' has multiple inputs connected to slot ${index}.`,
      );
    }
    indexed.set(index, edge);
  }

  return indexed;
}

function wrapTupleItems(items: string[]): string {
  return `(${items.join(', ')})`;
}

function formatTypeParams(typeParams: string[] | undefined): string {
  if (!typeParams || typeParams.length === 0) {
    return '';
  }
  return '<' + typeParams.join(', ') + '>';
}

function resolveEdgeSourceExpression(
  edge: FleauxEdge,
  ctx: GraphContext,
  letCtx: LetContext | null,
  visiting: Set<string>,
): string {
  const sourceNode = ctx.nodesById.get(edge.source);
  if (!sourceNode) {
    throw new GraphSerializationError(`Missing edge source node '${edge.source}'.`);
  }

  if (sourceNode.data.kind === 'let') {
    if (!letCtx || sourceNode.id !== letCtx.letNodeId) {
      throw new GraphSerializationError(
        `Let node '${sourceNode.data.label}' can only provide symbolic parameter outputs within its own body graph.`,
      );
    }

    const paramIndex = parseIndexedHandle(edge.sourceHandle, 'let-param-');
    if (paramIndex == null) {
      throw new GraphSerializationError(
        `Connection from let '${sourceNode.data.name}' must use a parameter output handle (let-param-N).`,
      );
    }

    const param = letCtx.params[paramIndex];
    if (!param) {
      throw new GraphSerializationError(
        `Let '${sourceNode.data.name}' does not have parameter index ${paramIndex}.`,
      );
    }
    if (!param.name.trim()) {
      throw new GraphSerializationError(
        `Let '${sourceNode.data.name}' has an empty parameter name at index ${paramIndex}.`,
      );
    }
    return param.name;
  }

  // Closure node: its param handles provide the parameter names to the body graph.
  if (sourceNode.data.kind === 'closure') {
    const paramIndex = parseIndexedHandle(edge.sourceHandle, 'closure-param-');
    if (paramIndex !== null) {
      const closureData = sourceNode.data as ClosureData;
      const param = closureData.params[paramIndex];
      if (!param) {
        throw new GraphSerializationError(
          `Closure node does not have parameter index ${paramIndex}.`,
        );
      }
      return param.name;
    }
    // Otherwise fall through to serialize the closure expression itself
  }

  return serializeNodeExpression(edge.source, ctx, letCtx, visiting);
}

function serializeNodeExpression(
  nodeId: string,
  ctx: GraphContext,
  letCtx: LetContext | null,
  visiting: Set<string>,
): string {
  if (visiting.has(nodeId)) {
    throw new GraphSerializationError(`Cycle detected while serializing node '${nodeId}'.`);
  }

  const node = ctx.nodesById.get(nodeId);
  if (!node) {
    throw new GraphSerializationError(`Missing node '${nodeId}'.`);
  }

  visiting.add(nodeId);
  try {
    switch (node.data.kind) {
      case 'literal':
        return renderLiteral(node.data);
      case 'stdValue':
        return (node.data as StdValueData).qualifiedName;
      case 'tuple':
        return serializeTupleNode(node as Node<TupleData>, ctx, letCtx, visiting);
      case 'stdFunc':
        return serializeCallNode(node as Node<StdFuncData>, ctx, letCtx, visiting, 'stdfunc-in-');
      case 'userFunc':
        return serializeCallNode(node as Node<UserFuncData>, ctx, letCtx, visiting, 'userfunc-in-');
      case 'closure':
        return serializeClosureNode(node as Node<ClosureData>, ctx, letCtx, visiting);
      case 'import':
        throw new GraphSerializationError(`Import node '${node.data.label}' cannot be used as an expression.`);
      case 'let':
        throw new GraphSerializationError(`Let node '${node.data.label}' cannot be embedded as an expression.`);
      default:
        throw new GraphSerializationError(`Unsupported node kind '${String((node.data as { kind: unknown }).kind)}'.`);
    }
  } finally {
    visiting.delete(nodeId);
  }
}

function getClosureBodyRootEdge(closureNodeId: string, ctx: GraphContext): FleauxEdge | null {
  const candidates = (ctx.outgoingBySource.get(closureNodeId) ?? []).filter(
    (edge) => edge.targetHandle === CLOSURE_BODY_ROOT_HANDLE,
  );
  if (candidates.length === 1) return candidates[0];
  if (candidates.length > 1) {
    throw new GraphSerializationError(
      `Closure node '${closureNodeId}' must have exactly one '${CLOSURE_BODY_ROOT_HANDLE}' marker edge; found ${candidates.length}.`,
    );
  }
  return null;
}

function serializeClosureNode(
  node: Node<ClosureData>,
  ctx: GraphContext,
  letCtx: LetContext | null,
  visiting: Set<string>,
): string {
  const paramText = node.data.params.map((p) => `${p.name}: ${p.type}`).join(', ');
  const bodyRootEdge = getClosureBodyRootEdge(node.id, ctx);
  if (!bodyRootEdge) {
    // Empty/stub closure — emit a placeholder
    return `(${paramText}): ${node.data.returnType} = null`;
  }
  const bodyExpr = serializeNodeExpression(bodyRootEdge.target, ctx, letCtx, visiting);
  return `(${paramText}): ${node.data.returnType} = ${bodyExpr}`;
}

function serializeTupleNode(
  node: Node<TupleData>,
  ctx: GraphContext,
  letCtx: LetContext | null,
  visiting: Set<string>,
): string {
  const indexedInputs = getIndexedInputs(node, ctx, 'tuple-in-');
  const pipelineInput = getPipelineInput(node, ctx);
  if (pipelineInput) {
    throw new GraphSerializationError(
      `Tuple node '${node.data.label}' received a pipeline input without a slot handle. Connect tuple inputs to explicit tuple slots.`,
    );
  }

  const items: string[] = [];
  for (let index = 0; index < node.data.arity; index += 1) {
    const edge = indexedInputs.get(index);
    if (edge) {
      items.push(resolveEdgeSourceExpression(edge, ctx, letCtx, visiting));
      continue;
    }

    throw new GraphSerializationError(
      `Tuple node '${node.data.label}' is missing input ${index}.`,
    );
  }

  return wrapTupleItems(items);
}

function formatCallSource(targetName: string, argExprs: string[]): string {
  if (argExprs.length === 0) {
    throw new GraphSerializationError(`Call target '${targetName}' is missing input arguments.`);
  }
  return `${wrapTupleItems(argExprs)} -> ${targetName}`;
}

function isFunctionReferenceNode(node: Node<StdFuncData | UserFuncData>): boolean {
  return node.data.isReference === true;
}

function trySerializeTupleItems(
  nodeId: string,
  ctx: GraphContext,
  letCtx: LetContext | null,
  visiting: Set<string>,
): string[] | null {
  const node = ctx.nodesById.get(nodeId);
  if (!node || node.data.kind !== 'tuple') {
    return null;
  }

  const tupleNode = node as Node<TupleData>;
  const indexedInputs = getIndexedInputs(tupleNode, ctx, 'tuple-in-');
  const items: string[] = [];
  for (let index = 0; index < tupleNode.data.arity; index += 1) {
    const edge = indexedInputs.get(index);
    if (!edge) {
      throw new GraphSerializationError(
        `Tuple node '${tupleNode.data.label}' is missing input ${index}.`,
      );
    }
    items.push(resolveEdgeSourceExpression(edge, ctx, letCtx, visiting));
  }

  return items;
}

function serializeCallNode(
  node: Node<StdFuncData | UserFuncData>,
  ctx: GraphContext,
  letCtx: LetContext | null,
  visiting: Set<string>,
  handlePrefix: string,
): string {
  const targetName = node.data.kind === 'stdFunc'
    ? node.data.qualifiedName
    : node.data.functionName;

  // Function-reference nodes serialize as bare symbols (e.g. `Double`, `Std.String.Trim`).
  if (isFunctionReferenceNode(node)) {
    return targetName;
  }

  const paramCount = node.data.params.length;
  const variadic = hasVariadicTail(node.data.params);
  const requiredInputs = minArity(node.data.params);
  const indexedInputs = getIndexedInputs(node, ctx, handlePrefix);
  const pipelineInput = getPipelineInput(node, ctx);

  if (indexedInputs.size > 0 && pipelineInput) {
    throw new GraphSerializationError(
      `Call node '${node.data.label}' mixes direct argument inputs with a pipeline input. Use one style per call.`,
    );
  }

  if (indexedInputs.size > 0) {
    const args: string[] = [];
    for (let index = 0; index < paramCount; index += 1) {
      const edge = indexedInputs.get(index);
      if (edge) {
        args.push(resolveEdgeSourceExpression(edge, ctx, letCtx, visiting));
        continue;
      }

      if (variadic && index === paramCount - 1) {
        continue;
      }

      throw new GraphSerializationError(`Call node '${node.data.label}' is missing argument ${index}.`);
    }

    if (args.length < requiredInputs) {
      throw new GraphSerializationError(`Call node '${node.data.label}' is missing required input arguments.`);
    }

    if (args.length === 0 && requiredInputs === 0) {
      return `${wrapTupleItems([])} -> ${targetName}`;
    }

    return formatCallSource(targetName, args);
  }

  if (pipelineInput) {
    if (variadic) {
      const tupleItems = trySerializeTupleItems(pipelineInput.source, ctx, letCtx, visiting);
      if (tupleItems) {
        if (tupleItems.length < requiredInputs) {
          throw new GraphSerializationError(`Call node '${node.data.label}' is missing required input arguments.`);
        }
        if (tupleItems.length === 0) {
          return `${wrapTupleItems([])} -> ${targetName}`;
        }
        return formatCallSource(targetName, tupleItems);
      }
    }

    const lhs = resolveEdgeSourceExpression(pipelineInput, ctx, letCtx, visiting);
    if (paramCount == 1) {
      return `${wrapTupleItems([lhs])} -> ${targetName}`;
    }
    return `${lhs} -> ${targetName}`;
  }

  if (paramCount === 0 || requiredInputs === 0) {
    return `${wrapTupleItems([])} -> ${targetName}`;
  }


  throw new GraphSerializationError(`Call node '${node.data.label}' has no input.`);
}

function getLetBodyRootEdge(letNodeId: string, ctx: GraphContext): FleauxEdge | null {
  const candidates = (ctx.outgoingBySource.get(letNodeId) ?? []).filter(
    (edge) => edge.targetHandle === LET_BODY_ROOT_HANDLE,
  );

  if (candidates.length === 1) {
    return candidates[0];
  }

  if (candidates.length > 1) {
    throw new GraphSerializationError(
      `Let node '${letNodeId}' must have exactly one '${LET_BODY_ROOT_HANDLE}' marker edge; found ${candidates.length}.`,
    );
  }

  const inferredRootNodeId = inferLegacyLetBodyRootNodeId(letNodeId, ctx);
  if (!inferredRootNodeId) {
    const hasAnyNonMarkerOutgoing = (ctx.outgoingBySource.get(letNodeId) ?? []).some(
      (edge) => edge.targetHandle !== LET_BODY_ROOT_HANDLE,
    );
    if (!hasAnyNonMarkerOutgoing) {
      // Fresh let node with no authored body yet; keep serialization usable while editing.
      return null;
    }
    throw new GraphSerializationError(
      `Let node '${letNodeId}' must have exactly one '${LET_BODY_ROOT_HANDLE}' marker edge; found 0.`,
    );
  }

  return {
    id: `inferred-let-body-root-${letNodeId}`,
    source: letNodeId,
    target: inferredRootNodeId,
    targetHandle: LET_BODY_ROOT_HANDLE,
    sourceHandle: null,
    animated: false,
    data: { kind: 'pipeline' },
  };
}

function inferLegacyLetBodyRootNodeId(letNodeId: string, ctx: GraphContext): string | null {
  const outgoing = (ctx.outgoingBySource.get(letNodeId) ?? []).filter(
    (edge) => edge.targetHandle !== LET_BODY_ROOT_HANDLE,
  );

  const rootCandidates = new Set<string>();
  const pending = outgoing.map((edge) => edge.target);
  while (pending.length > 0) {
    const nodeId = pending.pop();
    if (!nodeId || nodeId === letNodeId || rootCandidates.has(nodeId)) {
      continue;
    }
    rootCandidates.add(nodeId);

    for (const edge of ctx.outgoingBySource.get(nodeId) ?? []) {
      if (edge.targetHandle === LET_BODY_ROOT_HANDLE || edge.target === letNodeId) {
        continue;
      }
      pending.push(edge.target);
    }
  }

  if (rootCandidates.size === 0) {
    return null;
  }

  const sinks = Array.from(rootCandidates).filter((nodeId) => {
    const outgoingEdges = (ctx.outgoingBySource.get(nodeId) ?? []).filter(
      (edge) => edge.targetHandle !== LET_BODY_ROOT_HANDLE,
    );
    return !outgoingEdges.some((edge) => rootCandidates.has(edge.target));
  });

  if (sinks.length !== 1) {
    return null;
  }

  return sinks[0];
}

function resolveLetBodyFromMarker(
  letNode: Node<LetData>,
  markerEdge: FleauxEdge,
  letCtx: LetContext,
  ctx: GraphContext,
): string {
  if (markerEdge.target === letNode.id) {
    const paramIndex = parseIndexedHandle(markerEdge.sourceHandle, 'let-param-');
    if (paramIndex == null) {
      throw new GraphSerializationError(
        `Let '${letNode.data.name}' body marker points to self but does not reference a parameter handle.`,
      );
    }
    const param = letCtx.params[paramIndex];
    if (!param) {
      throw new GraphSerializationError(
        `Let '${letNode.data.name}' body marker references missing parameter index ${paramIndex}.`,
      );
    }
    return param.name;
  }

  return serializeNodeExpression(markerEdge.target, ctx, letCtx, new Set<string>());
}

function collectDependencyNodeIds(rootNodeId: string, letNodeId: string, ctx: GraphContext): Set<string> {
  const visited = new Set<string>();
  const pending = [rootNodeId];

  while (pending.length > 0) {
    const nodeId = pending.pop();
    if (!nodeId || visited.has(nodeId) || nodeId === letNodeId) {
      continue;
    }
    visited.add(nodeId);

    for (const edge of ctx.incomingByTarget.get(nodeId) ?? []) {
      if (edge.targetHandle === LET_BODY_ROOT_HANDLE) {
        continue;
      }
      if (edge.targetHandle === CLOSURE_BODY_ROOT_HANDLE) {
        // Don't follow body-root edges backwards (they would lead back to the closure node)
        continue;
      }
      pending.push(edge.source);
    }

    // If this node is a closure, also include its body sub-graph
    const node = ctx.nodesById.get(nodeId);
    if (node?.data.kind === 'closure') {
      for (const edge of ctx.outgoingBySource.get(nodeId) ?? []) {
        if (edge.targetHandle === CLOSURE_BODY_ROOT_HANDLE) {
          pending.push(edge.target);
        }
      }
    }
  }

  return visited;
}


function serializeLetStatement(
  node: Node<LetData>,
  ctx: GraphContext,
): { statement: string; bodyNodeIds: Set<string> } {
  const markerEdge = getLetBodyRootEdge(node.id, ctx);
  if (!markerEdge) {
    const paramText = node.data.params.map((param) => `${param.name}: ${param.type}`).join(', ');
    const statement = `let ${node.data.name}${formatTypeParams(node.data.typeParams)}(${paramText}): ${node.data.returnType} = null;`;
    return { statement, bodyNodeIds: new Set<string>() };
  }

  const letCtx: LetContext = {
    letNodeId: node.id,
    typeParams: node.data.typeParams,
    params: node.data.params,
  };
  const bodyExpr = resolveLetBodyFromMarker(node, markerEdge, letCtx, ctx);
  const bodyNodeIds = markerEdge.target === node.id
    ? new Set<string>()
    : collectDependencyNodeIds(markerEdge.target, node.id, ctx);
  const paramText = node.data.params.map((param) => `${param.name}: ${param.type}`).join(', ');
  const statement = `let ${node.data.name}${formatTypeParams(node.data.typeParams)}(${paramText}): ${node.data.returnType} = ${bodyExpr};`;

  return { statement, bodyNodeIds };
}

function serializeTopLevelExpression(
  node: Node<FleauxNodeData>,
  ctx: GraphContext,
): string {
  return `${serializeNodeExpression(node.id, ctx, null, new Set<string>())};`;
}

export function serializeGraphToFleaux(
  nodes: Node<FleauxNodeData>[],
  edges: FleauxEdge[],
): GraphSerializationResult {
  const ctx = buildGraphContext(nodes, edges);
  const imports: string[] = [];
  const letStatements: string[] = [];
  const expressionStatements: string[] = [];
  const letBodyNodes = new Set<string>();

  for (const node of nodes) {
    if (node.data.kind === 'import') {
      imports.push(`import ${node.data.moduleName};`);
    }
  }

  for (const node of nodes) {
    if (node.data.kind !== 'let') {
      continue;
    }
    const { statement, bodyNodeIds } = serializeLetStatement(node as Node<LetData>, ctx);
    letStatements.push(statement);
    for (const bodyNodeId of bodyNodeIds) {
      letBodyNodes.add(bodyNodeId);
    }
  }

  for (const node of nodes) {
    if (node.data.kind === 'import' || node.data.kind === 'let') {
      continue;
    }
    if (letBodyNodes.has(node.id)) {
      continue;
    }

    const outgoing = ctx.outgoingBySource.get(node.id) ?? [];
    // For closure nodes, ignore the body-anchor edge when checking if the node is "consumed"
    const dataOutgoing = node.data.kind === 'closure'
      ? outgoing.filter((e) => e.sourceHandle !== 'closure-body-anchor' && e.targetHandle !== CLOSURE_BODY_ROOT_HANDLE)
      : outgoing;
    if (dataOutgoing.length > 0) {
      continue;
    }

    expressionStatements.push(serializeTopLevelExpression(node, ctx));
  }

  const statements = [...imports, ...letStatements, ...expressionStatements];
  if (statements.length === 0) {
    throw new GraphSerializationError('The graph does not contain any serializable Fleaux statements.');
  }

  return {
    sourceText: `${statements.join('\n')}\n`,
    imports,
    letStatements,
    expressionStatements,
  };
}


