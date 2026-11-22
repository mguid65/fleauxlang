import type { Node } from '@xyflow/react';
import type { FleauxEdge, FleauxNodeData } from './types';

type PipelineLinkCandidate = {
  source: string | null | undefined;
  target: string | null | undefined;
  sourceHandle?: string | null;
  targetHandle?: string | null;
};

const NON_PIPELINE_NODE_KINDS = new Set(['import', 'typeDecl', 'aliasDecl', 'rawSource']);
const CALL_SLOT_PREFIXES = ['stdfunc-in-', 'userfunc-in-', 'callvalue-in-'];

// A spread only belongs in a call's argument list: its output goes to a call argument slot, and its one input is the
// tuple it spreads.
function isValidSpreadEdge(
  candidate: PipelineLinkCandidate,
  sourceNode: Node<FleauxNodeData> | undefined,
  targetNode: Node<FleauxNodeData> | undefined,
): boolean {
  if (sourceNode?.data.kind === 'spread') {
    const isCall = targetNode?.data.kind === 'stdFunc' || targetNode?.data.kind === 'userFunc' || targetNode?.data.kind === 'callValue';
    if (!isCall || !CALL_SLOT_PREFIXES.some((prefix) => candidate.targetHandle?.startsWith(prefix) === true)) {
      return false;
    }
  }
  if (targetNode?.data.kind === 'spread' && candidate.targetHandle !== 'spread-in') {
    return false;
  }
  return true;
}

// Pipeline edges are directional; keep graph clean by rejecting self-loops,
// duplicate source->target links, and any edge involving a declaration-only node.
export function canCreatePipelineEdge(
  candidate: PipelineLinkCandidate,
  edges: FleauxEdge[],
  nodes: Node<FleauxNodeData>[],
): boolean {
  if (!candidate.source || !candidate.target) {
    return false;
  }

  if (candidate.source === candidate.target) {
    return false;
  }

  const sourceNode = nodes.find((n) => n.id === candidate.source);
  const targetNode = nodes.find((n) => n.id === candidate.target);
  if (
    (sourceNode && NON_PIPELINE_NODE_KINDS.has(sourceNode.data.kind))
    || (targetNode && NON_PIPELINE_NODE_KINDS.has(targetNode.data.kind))
  ) {
    return false;
  }

  if (!isValidSpreadEdge(candidate, sourceNode, targetNode)) {
    return false;
  }

  return !edges.some((edge) => {
    return (
      edge.source === candidate.source
      && edge.target === candidate.target
      && (edge.sourceHandle ?? null) === (candidate.sourceHandle ?? null)
      && (edge.targetHandle ?? null) === (candidate.targetHandle ?? null)
    );
  });
}
