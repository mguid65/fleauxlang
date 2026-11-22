import type { Node } from '@xyflow/react';
import type {
  AliasDeclData,
  CallValueData,
  ClosureData,
  FleauxEdge,
  FleauxNodeData,
  LetData,
  LiteralData,
  RawSourceData,
  StdFuncData,
  StdValueData,
  TypeDeclData,
  TupleData,
  UserFuncData,
} from './types';
import { callSlotCount, hasVariadicTail, minArity } from './functionSignatures';
import { multiLineRawStringLiteral, ordinaryStringLiteral } from './stringLiterals';

export class GraphSerializationError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'GraphSerializationError';
  }
}

export interface GraphSerializationResult {
  sourceText: string;
  // The top-level statements in source order: the order of their nodes.
  statements: string[];
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

function renderLiteral(data: LiteralData): string {
  switch (data.valueType) {
    case 'String':
      // Text of several lines reads best as a raw string.
      return multiLineRawStringLiteral(data.value) ?? ordinaryStringLiteral(data.value);
    case 'Float64': {
      const text = data.value.trim() === '' ? '0' : data.value.trim();
      return /^-?\d+$/.test(text) ? `${text}.0` : text;
    }
    case 'UInt64': {
      const text = data.value.trim().replace(/u64$/, '');
      return `${text === '' ? '0' : text}u64`;
    }
    case 'Int64':
      return data.value.trim() === '' ? '0' : data.value.trim();
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

function formatCallableTarget(baseName: string, appliedTypeArgs: string[] | undefined): string {
  return baseName + formatTypeParams(appliedTypeArgs);
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
      case 'wildcard':
        return '_';
      case 'stdValue':
        return (node.data as StdValueData).qualifiedName;
      case 'tuple':
        return serializeTupleNode(node as Node<TupleData>, ctx, letCtx, visiting);
      case 'stdFunc':
        return serializeCallNode(node as Node<StdFuncData>, ctx, letCtx, visiting, 'stdfunc-in-');
      case 'userFunc':
        return serializeCallNode(node as Node<UserFuncData>, ctx, letCtx, visiting, 'userfunc-in-');
      case 'closure':
        return serializeClosureNode(node as Node<ClosureData>, ctx, letCtx);
      case 'callValue':
        return serializeCallValueNode(node as Node<CallValueData>, ctx, letCtx, visiting);
      case 'import':
        throw new GraphSerializationError(`Import node '${node.data.label}' cannot be used as an expression.`);
      case 'typeDecl':
        throw new GraphSerializationError(`Type declaration node '${node.data.label}' cannot be used as an expression.`);
      case 'aliasDecl':
        throw new GraphSerializationError(`Alias declaration node '${node.data.label}' cannot be used as an expression.`);
      case 'let':
        throw new GraphSerializationError(`Let node '${node.data.label}' cannot be embedded as an expression.`);
      case 'rawSource':
        throw new GraphSerializationError(`Raw source node '${node.data.label}' cannot be used as an expression.`);
      case 'spread':
        throw new GraphSerializationError("A spread node can only feed a call's argument slot.");
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

function resolveClosureBodyRootExpression(
  markerEdge: FleauxEdge,
  ctx: GraphContext,
  letCtx: LetContext | null,
): string {
  const bodySourceNodeId = typeof markerEdge.data?.bodySourceNodeId === 'string'
    ? markerEdge.data.bodySourceNodeId
    : markerEdge.target;
  const bodySourceHandle = typeof markerEdge.data?.bodySourceHandle === 'string'
    ? markerEdge.data.bodySourceHandle
    : markerEdge.sourceHandle ?? undefined;

  return resolveEdgeSourceExpression(
    {
      ...markerEdge,
      source: bodySourceNodeId,
      sourceHandle: bodySourceHandle,
    },
    ctx,
    letCtx,
    new Set<string>(),
  );
}

function serializeClosureNode(
  node: Node<ClosureData>,
  ctx: GraphContext,
  letCtx: LetContext | null,
): string {
  const typeParamText = formatTypeParams(node.data.typeParams);
  const paramText = node.data.params.map((p) => `${p.name}: ${p.type}`).join(', ');
  const bodyRootEdge = getClosureBodyRootEdge(node.id, ctx);
  if (!bodyRootEdge) {
    // Empty/stub closure — emit a placeholder
    return `${typeParamText}(${paramText}): ${node.data.returnType} = null`;
  }

  const bodySourceNodeId = typeof bodyRootEdge.data?.bodySourceNodeId === 'string'
    ? bodyRootEdge.data.bodySourceNodeId
    : bodyRootEdge.target;
  const bodySourceHandle = typeof bodyRootEdge.data?.bodySourceHandle === 'string'
    ? bodyRootEdge.data.bodySourceHandle
    : bodyRootEdge.sourceHandle ?? undefined;

  if (bodySourceNodeId === node.id) {
    const paramIndex = parseIndexedHandle(bodySourceHandle, 'closure-param-');
    if (paramIndex == null) {
      throw new GraphSerializationError(
        `Closure node '${node.id}' body marker points to self but does not reference a parameter handle.`,
      );
    }
    const param = node.data.params[paramIndex];
    if (!param) {
      throw new GraphSerializationError(
        `Closure node '${node.id}' body marker references missing parameter index ${paramIndex}.`,
      );
    }
    return `${typeParamText}(${paramText}): ${node.data.returnType} = ${param.name}`;
  }

  const bodyExpr = resolveClosureBodyRootExpression(bodyRootEdge, ctx, letCtx);
  return `${typeParamText}(${paramText}): ${node.data.returnType} = ${bodyExpr}`;
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

  return items.length === 1 ? `(${items[0]},)` : wrapTupleItems(items);
}

function isFunctionReferenceNode(node: Node<StdFuncData | UserFuncData>): boolean {
  return node.data.isReference === true;
}

// The arguments in a call node's slots, in order. A spread may supply several arguments, so the empty slots after one
// are skipped; any other empty slot is an error.
function serializeSlotArguments(
  node: Node<FleauxNodeData>,
  ctx: GraphContext,
  letCtx: LetContext | null,
  visiting: Set<string>,
  handlePrefix: string,
  slotCount: number,
  slotName: (index: number) => string | undefined,
): string[] {
  const indexedInputs = getIndexedInputs(node, ctx, handlePrefix);
  for (const index of indexedInputs.keys()) {
    if (index >= slotCount) {
      throw new GraphSerializationError(`Call node '${node.data.label}' has an input connected to missing slot ${index}.`);
    }
  }

  const args: string[] = [];
  let afterSpread = false;
  for (let index = 0; index < slotCount; index += 1) {
    const edge = indexedInputs.get(index);
    if (!edge && afterSpread) {
      continue;
    }
    if (!edge) {
      const name = slotName(index);
      throw new GraphSerializationError(
        `Call node '${node.data.label}' is missing argument ${index + 1}${name ? ` (${name})` : ''}.`,
      );
    }
    const sourceNode = ctx.nodesById.get(edge.source);
    if (sourceNode?.data.kind === 'spread') {
      args.push(serializeSpread(sourceNode, ctx, letCtx, visiting));
      afterSpread = true;
      continue;
    }
    args.push(resolveEdgeSourceExpression(edge, ctx, letCtx, visiting));
  }
  return args;
}

// A call of a function value: `(args) -> f` for a parameter, value let, or function reference, and a closure stage
// `(args) -> (params): R = body` for a closure. A call target must be a name or a closure.
function serializeCallValueNode(
  node: Node<CallValueData>,
  ctx: GraphContext,
  letCtx: LetContext | null,
  visiting: Set<string>,
): string {
  const functionEdges = getIncomingEdges(node.id, ctx).filter((edge) => edge.targetHandle === 'callvalue-fn');
  if (functionEdges.length !== 1) {
    throw new GraphSerializationError(`Call value node '${node.data.label}' needs exactly one function input.`);
  }
  const functionEdge = functionEdges[0];
  const source = ctx.nodesById.get(functionEdge.source);
  const handle = functionEdge.sourceHandle ?? '';
  const callable = (source?.data.kind === 'let' && handle.startsWith('let-param-'))
    || source?.data.kind === 'closure'
    || ((source?.data.kind === 'userFunc' || source?.data.kind === 'stdFunc') && source.data.isReference === true);
  if (!callable) {
    throw new GraphSerializationError(
      `Call value node '${node.data.label}' calls a value that is not a parameter, a let, or a closure; bind it to a let first.`,
    );
  }

  const target = resolveEdgeSourceExpression(functionEdge, ctx, letCtx, visiting);
  const args = serializeSlotArguments(node, ctx, letCtx, visiting, 'callvalue-in-', node.data.slots, () => undefined);
  return `${wrapTupleItems(args)} -> ${target}`;
}

// The tuple a spread node spreads, written `x...`.
function serializeSpread(
  spreadNode: Node<FleauxNodeData>,
  ctx: GraphContext,
  letCtx: LetContext | null,
  visiting: Set<string>,
): string {
  const inputs = getIncomingEdges(spreadNode.id, ctx).filter((edge) => edge.targetHandle === 'spread-in');
  if (inputs.length !== 1) {
    throw new GraphSerializationError('A spread node needs exactly one input: the tuple whose items it spreads.');
  }
  return `${resolveEdgeSourceExpression(inputs[0], ctx, letCtx, visiting)}...`;
}

// A call is the parameter list of its argument slots: one per parameter, and `variadicSlots` for a variadic tail. A
// spread may supply several arguments, so the empty slots after one are skipped.
function serializeCallNode(
  node: Node<StdFuncData | UserFuncData>,
  ctx: GraphContext,
  letCtx: LetContext | null,
  visiting: Set<string>,
  handlePrefix: string,
): string {
  const targetName = node.data.kind === 'stdFunc'
    ? node.data.operator ?? formatCallableTarget(node.data.qualifiedName, node.data.appliedTypeArgs)
    : formatCallableTarget(node.data.functionName, node.data.appliedTypeArgs);

  // Function-reference nodes serialize as bare symbols (e.g. `Double`, `Std.String.Trim`).
  if (isFunctionReferenceNode(node)) {
    return targetName;
  }

  if (getPipelineInput(node, ctx)) {
    throw new GraphSerializationError(
      `Call node '${node.data.label}' has an input that is not connected to an argument slot.`,
    );
  }

  const params = node.data.params;
  const fixedCount = minArity(params);
  const variadic = hasVariadicTail(params);
  const args = serializeSlotArguments(
    node,
    ctx,
    letCtx,
    visiting,
    handlePrefix,
    callSlotCount(params, node.data.variadicSlots),
    (index) => (index < fixedCount || !variadic ? params[index]?.name : params[params.length - 1]?.name),
  );

  return `${wrapTupleItems(args)} -> ${targetName}`;
}

function serializeTypeStatement(node: Node<TypeDeclData>): string {
  const separator = node.data.separator === '::' ? '::' : '=';
  return 'type ' + node.data.name + ' ' + separator + ' ' + node.data.targetType + ';';
}

function serializeAliasStatement(node: Node<AliasDeclData>): string {
  return 'alias ' + node.data.name + ' = ' + node.data.targetType + ';';
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

  const inferredRootNodeId = inferLetBodyRootNodeId(letNodeId, ctx);
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

// The body root of a let without a body marker edge: the one node, reachable from the let's parameters, that feeds
// nothing else they reach. The editor does not let users draw marker edges (the marker handles are hidden), so this
// is how a let built in the editor finds its body; imported lets carry a marker.
function inferLetBodyRootNodeId(letNodeId: string, ctx: GraphContext): string | null {
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

function collectClosureBodyNodeIds(ctx: GraphContext): Set<string> {
  const closureBodyNodes = new Set<string>();

  for (const node of ctx.nodesById.values()) {
    if (node.data.kind !== 'closure') {
      continue;
    }

    const bodyRootEdge = getClosureBodyRootEdge(node.id, ctx);
    if (!bodyRootEdge) {
      continue;
    }

    for (const bodyNodeId of collectDependencyNodeIds(bodyRootEdge.target, node.id, ctx)) {
      closureBodyNodes.add(bodyNodeId);
    }
  }

  return closureBodyNodes;
}

function collectSymbolicInputTargetNodeIds(ctx: GraphContext): Set<string> {
  const symbolicTargets = new Set<string>();
  const pending: string[] = [];

  for (const edges of ctx.incomingByTarget.values()) {
    for (const edge of edges) {
      if (edge.sourceHandle?.startsWith('let-param-')) {
        pending.push(edge.target);
      }
    }
  }

  while (pending.length > 0) {
    const nodeId = pending.pop();
    if (!nodeId || symbolicTargets.has(nodeId)) {
      continue;
    }

    symbolicTargets.add(nodeId);

    for (const edge of ctx.outgoingBySource.get(nodeId) ?? []) {
      if (edge.targetHandle === LET_BODY_ROOT_HANDLE || edge.targetHandle === CLOSURE_BODY_ROOT_HANDLE) {
        continue;
      }
      pending.push(edge.target);
    }
  }

  let changed = true;
  while (changed) {
    changed = false;
    for (const node of ctx.nodesById.values()) {
      if (node.data.kind !== 'closure' || symbolicTargets.has(node.id)) {
        continue;
      }

      const bodyRootEdge = getClosureBodyRootEdge(node.id, ctx);
      if (!bodyRootEdge) {
        continue;
      }

      const bodySourceNodeId = typeof bodyRootEdge.data?.bodySourceNodeId === 'string'
        ? bodyRootEdge.data.bodySourceNodeId
        : bodyRootEdge.target;
      const bodySourceHandle = typeof bodyRootEdge.data?.bodySourceHandle === 'string'
        ? bodyRootEdge.data.bodySourceHandle
        : bodyRootEdge.sourceHandle ?? undefined;

      const capturesLetScopedSymbol = bodySourceHandle?.startsWith('let-param-')
        || symbolicTargets.has(bodySourceNodeId);
      if (!capturesLetScopedSymbol) {
        continue;
      }

      symbolicTargets.add(node.id);
      changed = true;
    }
  }

  return symbolicTargets;
}


function serializeLetStatement(
  node: Node<LetData>,
  ctx: GraphContext,
): { statement: string; bodyNodeIds: Set<string> } {
  const markerEdge = getLetBodyRootEdge(node.id, ctx);
  if (node.data.isValueBinding === true && node.data.params.length > 0) {
    throw new GraphSerializationError(`Value let '${node.data.name}' cannot have parameters.`);
  }
  const header = node.data.isValueBinding === true
    ? `let ${node.data.name}: ${node.data.returnType}`
    : `let ${node.data.name}${formatTypeParams(node.data.typeParams)}(${node.data.params.map((param) => `${param.name}: ${param.type}`).join(', ')}): ${node.data.returnType}`;
  if (!markerEdge) {
    return { statement: `${header} = null;`, bodyNodeIds: new Set<string>() };
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
  return { statement: `${header} = ${bodyExpr};`, bodyNodeIds };
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
  const closureBodyNodes = collectClosureBodyNodeIds(ctx);
  const symbolicInputTargets = collectSymbolicInputTargetNodeIds(ctx);

  // Let bodies are serialized first, so the nodes they use are known not to be statements of their own.
  const letStatementsById = new Map<string, string>();
  const letBodyNodes = new Set<string>();
  for (const node of nodes) {
    if (node.data.kind !== 'let') {
      continue;
    }
    const { statement, bodyNodeIds } = serializeLetStatement(node as Node<LetData>, ctx);
    letStatementsById.set(node.id, statement);
    for (const bodyNodeId of bodyNodeIds) {
      letBodyNodes.add(bodyNodeId);
    }
  }

  // Statements keep the order of their nodes, which is source order for an imported graph: value lets are only
  // visible after their declaration, and raw source statements sit between the others.
  const statements: string[] = [];
  for (const node of nodes) {
    switch (node.data.kind) {
      case 'import':
        statements.push(`import ${node.data.moduleName};`);
        continue;
      case 'typeDecl':
        statements.push(serializeTypeStatement(node as Node<TypeDeclData>));
        continue;
      case 'aliasDecl':
        statements.push(serializeAliasStatement(node as Node<AliasDeclData>));
        continue;
      case 'let':
        statements.push(letStatementsById.get(node.id)!);
        continue;
      case 'rawSource':
        statements.push(`${(node.data as RawSourceData).statementText};`);
        continue;
      default:
        break;
    }

    if (letBodyNodes.has(node.id) || closureBodyNodes.has(node.id) || symbolicInputTargets.has(node.id)) {
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

    statements.push(serializeTopLevelExpression(node, ctx));
  }

  if (statements.length === 0) {
    throw new GraphSerializationError('The graph does not contain any serializable Fleaux statements.');
  }

  return {
    sourceText: `${statements.join('\n')}\n`,
    statements,
  };
}
