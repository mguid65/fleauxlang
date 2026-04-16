import { create } from 'zustand';
import { immer } from 'zustand/middleware/immer';
import {
  addEdge,
  applyEdgeChanges,
  applyNodeChanges,
  type Connection,
  type EdgeChange,
  type Node,
  type NodeChange,
} from '@xyflow/react';
import { canCreatePipelineEdge } from '../lib/edgeValidation';
import type { FleauxEdge, FleauxNodeData } from '../lib/types';
import { initialNodes, initialEdges } from './initialGraph';
import { migrateGraphNodes } from '../lib/graphMigration';
import { serializeGraphToFleaux } from '../lib/graphToFleaux';
import { wasmRunSource } from '../lib/wasmCoordinator';
import { importFleauxSourceToGraph } from '../lib/fleauxToGraph';

export type WasmValidationStatus = 'idle' | 'running' | 'success' | 'error';

// ─── State shape ─────────────────────────────────────────────────────────────

export interface FlowState {
  nodes: Node<FleauxNodeData>[];
  edges: FleauxEdge[];
  sourceText: string;
  wasmOutput: string;
  wasmStatus: WasmValidationStatus;
  wasmMessage: string | null;
  setSourceText: (sourceText: string) => void;
  onNodesChange: (changes: NodeChange<Node<FleauxNodeData>>[]) => void;
  onEdgesChange: (changes: EdgeChange<FleauxEdge>[]) => void;
  onConnect: (connection: Connection) => void;
  addNode: (node: Node<FleauxNodeData>) => void;
  updateNodeData: (id: string, data: Partial<FleauxNodeData>) => void;
  removeNode: (id: string) => void;
  removeEdge: (id: string) => void;
  clearGraph: () => void;
  loadGraphFromSource: (sourceText: string) => void;
  runGraphWithWasm: () => Promise<void>;
}

// ─── Store ───────────────────────────────────────────────────────────────────

export const useFlowStore = create<FlowState>()(
  immer((set, get) => ({
    nodes: migrateGraphNodes(initialNodes),
    edges: initialEdges,
    sourceText: '',
    wasmOutput: '',
    wasmStatus: 'idle',
    wasmMessage: null,

    setSourceText(sourceText) {
      set((state) => {
        state.sourceText = sourceText;
      });
    },

    onNodesChange(changes) {
      set((state) => {
        state.nodes = applyNodeChanges(changes, state.nodes);
      });
    },

    onEdgesChange(changes) {
      set((state) => {
        state.edges = applyEdgeChanges(changes, state.edges);
      });
    },

    onConnect(connection) {
      set((state) => {
        if (!canCreatePipelineEdge(connection, state.edges, state.nodes)) {
          return;
        }

        state.edges = addEdge(
          { ...connection, animated: true, data: { kind: 'pipeline' } },
          state.edges,
        );
      });
    },

    addNode(node) {
      set((state) => {
        state.nodes.push(node);
      });
    },

    updateNodeData(id, data) {
      set((state) => {
        const node = state.nodes.find((n) => n.id === id);
        if (node) {
          Object.assign(node.data, data);
        }
      });
    },

    removeNode(id) {
      set((state) => {
        state.nodes = state.nodes.filter((n) => n.id !== id);
        state.edges = state.edges.filter(
          (e) => e.source !== id && e.target !== id,
        );
      });
    },

    clearGraph() {
      set((state) => {
        state.nodes = [];
        state.edges = [];
        state.sourceText = '';
        state.wasmOutput = '';
        state.wasmStatus = 'idle';
        state.wasmMessage = null;
      });
    },

    loadGraphFromSource(sourceText) {
      const imported = importFleauxSourceToGraph(sourceText);
      set((state) => {
        state.nodes = imported.nodes;
        state.edges = imported.edges;
        state.sourceText = sourceText;
        state.wasmOutput = '';
        state.wasmStatus = 'idle';
        state.wasmMessage = `Loaded ${imported.nodes.length} nodes from source`;
      });
    },

    removeEdge(id) {
      set((state) => {
        state.edges = state.edges.filter((e) => e.id !== id);
      });
    },

    async runGraphWithWasm() {
      let generatedSource = '';

      set((state) => {
        state.wasmStatus = 'running';
        state.wasmMessage = 'Generating canonical Fleaux source…';
      });

      try {
        const { sourceText } = serializeGraphToFleaux(get().nodes, get().edges);
        generatedSource = sourceText;

        set((state) => {
          state.sourceText = sourceText;
          state.wasmOutput = '';
          state.wasmMessage = 'Running graph with Fleaux WASM…';
        });

        const result = await wasmRunSource(sourceText);

        set((state) => {
          state.wasmStatus = 'success';
          state.wasmOutput = result.output;
          state.wasmMessage = `Ran with ${result.version} (exit ${result.exitCode})`;
        });
      } catch (error) {
        const message = error instanceof Error ? error.message : String(error);
        set((state) => {
          state.wasmStatus = 'error';
          state.wasmMessage = message;
          state.sourceText = generatedSource;
          state.wasmOutput = '';
        });
      }
    },
  })),
);
