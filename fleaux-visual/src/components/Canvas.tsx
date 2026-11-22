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
import { useEffect, useMemo } from 'react';
import { canCreatePipelineEdge } from '../lib/edgeValidation';
import { minimapColour } from '../lib/minimapColours';
import { FIT_VIEW_OPTIONS } from '../lib/viewport';
import { useFlowStore } from '../store/flowStore';
import { nodeTypes } from '../nodes';
import { ActionBar } from './ActionBar';
import { EditorPanel } from './EditorPanel';
import { Toolbar } from './Toolbar';
import type { FleauxEdge, FleauxNodeData } from '../lib/types';

const INTERNAL_BODY_ROOT_TARGET_HANDLES = new Set(['let-body-root', 'closure-body-root']);

function isInternalBodyRootEdge(edge: FleauxEdge): boolean {
  return edge.targetHandle != null && INTERNAL_BODY_ROOT_TARGET_HANDLES.has(edge.targetHandle);
}

// Keys typed into a field or the code editor belong to it, not to the graph's shortcuts.
function isTextEntryTarget(target: EventTarget | null): boolean {
  return target instanceof Element && target.closest('input, textarea, select, [contenteditable="true"], .nokey') !== null;
}

export function Canvas() {
  const nodes = useFlowStore((s) => s.nodes);
  const edges = useFlowStore((s) => s.edges);
  const onNodesChange = useFlowStore((s) => s.onNodesChange);
  const onEdgesChange = useFlowStore((s) => s.onEdgesChange);
  const onConnect = useFlowStore((s) => s.onConnect);
  const markUndoStep = useFlowStore((s) => s.markUndoStep);
  const undo = useFlowStore((s) => s.undo);
  const redo = useFlowStore((s) => s.redo);
  const layoutPending = useFlowStore((s) => s.layoutPending);
  const applyPendingLayout = useFlowStore((s) => s.applyPendingLayout);
  const renderedEdges = useMemo(
    () => edges.map((edge) => ({
      ...edge,
      animated: false,
      hidden: isInternalBodyRootEdge(edge),
      markerEnd: edge.markerEnd ?? { type: MarkerType.ArrowClosed },
    })),
    [edges],
  );
  const reactFlow = useReactFlow<Node<FleauxNodeData>, FleauxEdge>();
  const showMiniMap = nodes.length <= 180;
  const allMeasured = nodes.every((node) => node.measured?.width !== undefined && node.measured.height !== undefined);

  // An imported graph is laid out once React Flow has measured its nodes, then brought into view.
  useEffect(() => {
    if (!layoutPending || !allMeasured) {
      return;
    }
    applyPendingLayout();
    requestAnimationFrame(() => void reactFlow.fitView(FIT_VIEW_OPTIONS));
  }, [layoutPending, allMeasured, applyPendingLayout, reactFlow]);

  useEffect(() => {
    const onKeyDown = (event: KeyboardEvent) => {
      if (!(event.ctrlKey || event.metaKey) || event.altKey || isTextEntryTarget(event.target)) {
        return;
      }
      const key = event.key.toLowerCase();
      if (key === 'z' && !event.shiftKey) {
        event.preventDefault();
        undo();
      } else if (key === 'y' || (key === 'z' && event.shiftKey)) {
        event.preventDefault();
        redo();
      }
    };
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, [undo, redo]);

  const isValidConnection = (candidate: FleauxEdge | { source: string | null; target: string | null }) =>
    canCreatePipelineEdge(candidate, edges, nodes);

  return (
    <div className="w-full h-full flex min-w-0">
      <div className="flex-1 min-w-0 h-full relative">
        <Toolbar />
        <ActionBar />
        <ReactFlow<Node<FleauxNodeData>, FleauxEdge>
          nodes={nodes}
          edges={renderedEdges}
          fitView
          fitViewOptions={FIT_VIEW_OPTIONS}
          // Low enough for Fit View to show a whole sample.
          minZoom={0.1}
          onNodesChange={onNodesChange}
          onEdgesChange={onEdgesChange}
          onConnect={onConnect}
          onNodeDragStart={markUndoStep}
          onBeforeDelete={async () => {
            markUndoStep();
            return true;
          }}
          isValidConnection={isValidConnection}
          nodeTypes={nodeTypes}
          defaultEdgeOptions={{
            animated: true,
            style: { stroke: '#8f8f8f', strokeWidth: 2 },
          }}
        >
          <Background
            variant={BackgroundVariant.Dots}
            gap={24}
            size={1}
            color="#2e2e33"
          />
          <Controls
            fitViewOptions={FIT_VIEW_OPTIONS}
            style={{ bottom: 24, right: 24, left: 'unset' }}
          />
          {showMiniMap && <MiniMap
            nodeColor={(node) => minimapColour(node.data as FleauxNodeData)}
            style={{ bottom: 24, right: 24 }}
            maskColor="rgba(15, 15, 16, 0.8)"
          />}
        </ReactFlow>
      </div>
      <EditorPanel />
    </div>
  );
}
