import type { Node } from '@xyflow/react';
import type { FleauxEdge, FleauxNodeData } from './types';

type PipelineLinkCandidate = {
  source: string | null | undefined;
  target: string | null | undefined;
  sourceHandle?: string | null;
  targetHandle?: string | null;
};

// Pipeline edges are directional; keep graph clean by rejecting self-loops,
// duplicate source->target links, and any edge involving an import node.
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
  if (sourceNode?.data.kind === 'import' || targetNode?.data.kind === 'import') {
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
