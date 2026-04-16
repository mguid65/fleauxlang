import {
  ReactFlow,
  Background,
  Controls,
  MiniMap,
  BackgroundVariant,
  MarkerType,
  useReactFlow,
  type Node,
} from '@xyflow/react';
import { useEffect, useMemo, useRef } from 'react';
import { canCreatePipelineEdge } from '../lib/edgeValidation';
import { useFlowStore } from '../store/flowStore';
import { nodeTypes } from '../nodes';
import { Toolbar } from './Toolbar';
import type { FleauxEdge, FleauxNodeData } from '../lib/types';

export function Canvas() {
  const nodes = useFlowStore((s) => s.nodes);
  const edges = useFlowStore((s) => s.edges);
  const onNodesChange = useFlowStore((s) => s.onNodesChange);
  const onEdgesChange = useFlowStore((s) => s.onEdgesChange);
  const onConnect = useFlowStore((s) => s.onConnect);
  const renderedEdges = useMemo(
    () => edges.map((edge) => ({
      ...edge,
      animated: false,
      markerEnd: edge.markerEnd ?? { type: MarkerType.ArrowClosed },
    })),
    [edges],
  );
  const reactFlow = useReactFlow<Node<FleauxNodeData>, FleauxEdge>();
  const previousNodeCount = useRef(nodes.length);
  const showMiniMap = nodes.length <= 180;

  useEffect(() => {
    const prev = previousNodeCount.current;
    previousNodeCount.current = nodes.length;

    // Fit once for large topology swaps (e.g. loading a file), not every render.
    if (Math.abs(nodes.length - prev) < 20) {
      return;
    }

    requestAnimationFrame(() => {
      reactFlow.fitView({ padding: 0.2, duration: 250 });
    });
  }, [nodes.length, reactFlow]);

  const isValidConnection = (candidate: FleauxEdge | { source: string | null; target: string | null }) =>
    canCreatePipelineEdge(candidate, edges, nodes);

  return (
    <div className="w-full h-full relative">
      <Toolbar />
      <ReactFlow<Node<FleauxNodeData>, FleauxEdge>
        nodes={nodes}
        edges={renderedEdges}
        onNodesChange={onNodesChange}
        onEdgesChange={onEdgesChange}
        onConnect={onConnect}
        isValidConnection={isValidConnection}
        nodeTypes={nodeTypes}
        defaultEdgeOptions={{
          animated: true,
          style: { stroke: '#6d6aff', strokeWidth: 2 },
        }}
      >
        <Background
          variant={BackgroundVariant.Dots}
          gap={24}
          size={1}
          color="#2d3148"
        />
        <Controls
          style={{ bottom: 24, right: 24, left: 'unset' }}
        />
        {showMiniMap && <MiniMap
          nodeColor={(node) => {
            const kind = (node.data as { kind: string }).kind;
            const map: Record<string, string> = {
              import: '#0d9488',
              let: '#a21caf',
              tuple: '#c2410c',
              std: '#be123c',
              literal: '#0284c7',
            };
            return map[kind] ?? '#6b7280';
          }}
          style={{ bottom: 24, right: 24 }}
          maskColor="rgba(15, 17, 23, 0.8)"
        />}
      </ReactFlow>
    </div>
  );
}
