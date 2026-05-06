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
import { formatFunctionDisplayName } from '../lib/functionSignatures';
import type { FleauxEdge, FleauxNodeData, LetData, UserFuncData } from '../lib/types';
import { initialNodes, initialEdges } from './initialGraph';
import { migrateGraphNodes } from '../lib/graphMigration';
import { serializeGraphToFleaux } from '../lib/graphToFleaux';
import { wasmRunSource, WasmCoordinatorError, WasmStatusCode } from '../lib/wasmCoordinator';
import { importFleauxSourceToGraph } from '../lib/fleauxToGraph';

export type WasmValidationStatus = 'idle' | 'running' | 'success' | 'error';

//  State shape

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
  updateLetNodeSignature: (id: string, patch: Partial<Pick<LetData, 'name' | 'typeParams' | 'params' | 'returnType'>>) => void;
  removeNode: (id: string) => void;
  removeEdge: (id: string) => void;
  clearGraph: () => void;
  loadGraphFromSource: (sourceText: string) => void;
  runGraphWithWasm: () => Promise<void>;
  runEditorSourceWithWasm: () => Promise<void>;
}

//  Store

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

    updateLetNodeSignature(id, patch) {
      set((state) => {
        const letNode = state.nodes.find((node): node is Node<LetData> => node.id === id && node.data.kind === 'let');
        if (!letNode) {
          return;
        }

        const oldLetData = letNode.data;
        const nextLetData: LetData = {
          ...oldLetData,
          ...patch,
          name: patch.name ?? oldLetData.name,
          typeParams: patch.typeParams ?? oldLetData.typeParams,
          params: patch.params ?? oldLetData.params,
          returnType: patch.returnType ?? oldLetData.returnType,
          label: `let ${(patch.name ?? oldLetData.name) || 'Unnamed'}`,
        };

        const oldParamIdsByIndex = oldLetData.params.map((param) => param.id);
        const nextParamIndexById = new Map(nextLetData.params.map((param, index) => [param.id, index]));

        const linkedUserFuncNodes = state.nodes.filter(
          (node): node is Node<UserFuncData> => node.data.kind === 'userFunc' && node.data.functionNodeId === id,
        );
        const oldUserFuncParamIdsByNodeId = new Map(
          linkedUserFuncNodes.map((node) => [node.id, node.data.params.map((param) => param.id)]),
        );

        letNode.data = nextLetData;

        for (const userFuncNode of linkedUserFuncNodes) {
          userFuncNode.data = {
            ...userFuncNode.data,
            functionName: nextLetData.name,
            functionNodeId: id,
            typeParams: nextLetData.typeParams,
            params: nextLetData.params.map((param) => ({ ...param })),
            returnType: nextLetData.returnType,
            label: formatFunctionDisplayName(nextLetData.name, nextLetData.typeParams),
          };
        }

        state.edges = state.edges.filter((edge) => {
          if (edge.source === id && edge.sourceHandle?.startsWith('let-param-')) {
            const oldIndex = Number.parseInt(edge.sourceHandle.slice('let-param-'.length), 10);
            if (Number.isNaN(oldIndex)) {
              return false;
            }
            const paramId = oldParamIdsByIndex[oldIndex];
            const nextIndex = paramId ? nextParamIndexById.get(paramId) : undefined;
            if (nextIndex === undefined) {
              return false;
            }
            edge.sourceHandle = `let-param-${nextIndex}`;
            return true;
          }

          if (edge.targetHandle?.startsWith('userfunc-in-')) {
            const oldParamIds = oldUserFuncParamIdsByNodeId.get(edge.target);
            if (!oldParamIds) {
              return true;
            }
            const oldIndex = Number.parseInt(edge.targetHandle.slice('userfunc-in-'.length), 10);
            if (Number.isNaN(oldIndex)) {
              return false;
            }
            const paramId = oldParamIds[oldIndex];
            const nextIndex = paramId ? nextParamIndexById.get(paramId) : undefined;
            if (nextIndex === undefined) {
              return false;
            }
            edge.targetHandle = `userfunc-in-${nextIndex}`;
            return true;
          }

          return true;
        });
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
      try {
        const imported = importFleauxSourceToGraph(sourceText);
        const migratedNodes = migrateGraphNodes(imported.nodes as Array<Node<Record<string, unknown>>>);
        set((state) => {
          state.nodes = migratedNodes;
          state.edges = imported.edges;
          state.sourceText = sourceText;
          state.wasmOutput = '';
          state.wasmStatus = 'success';
          state.wasmMessage = `Loaded ${migratedNodes.length} nodes from source`;
        });
      } catch (error) {
        const message = error instanceof Error ? error.message : String(error);
        set((state) => {
          state.sourceText = sourceText;
          state.wasmOutput = '';
          state.wasmStatus = 'error';
          state.wasmMessage = `Failed to load source: ${message}`;
        });
        throw error;
      }
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
          state.wasmMessage = `Ran generated graph with ${result.version} (exit ${result.exitCode})`;
        });
      } catch (error) {
        const isUnavailable =
          error instanceof WasmCoordinatorError && error.statusCode === WasmStatusCode.RuntimeUnavailable;
        const message = error instanceof Error ? error.message : String(error);
        set((state) => {
          state.wasmStatus = 'error';
          state.wasmMessage = isUnavailable ? `WASM runtime not available: ${message}` : message;
          state.sourceText = generatedSource;
          state.wasmOutput = '';
        });
      }
    },

    async runEditorSourceWithWasm() {
      const currentSource = get().sourceText;

      if (currentSource.trim().length === 0) {
        set((state) => {
          state.wasmStatus = 'error';
          state.wasmOutput = '';
          state.wasmMessage = 'Editor source is empty. Generate graph source or type Fleaux code before running.';
        });
        return;
      }

      set((state) => {
        state.wasmStatus = 'running';
        state.wasmOutput = '';
        state.wasmMessage = 'Running editor source with Fleaux WASM…';
      });

      try {
        const result = await wasmRunSource(currentSource);
        set((state) => {
          state.wasmStatus = 'success';
          state.wasmOutput = result.output;
          state.wasmMessage = `Ran editor source with ${result.version} (exit ${result.exitCode})`;
        });
      } catch (error) {
        const isUnavailable =
          error instanceof WasmCoordinatorError && error.statusCode === WasmStatusCode.RuntimeUnavailable;
        const message = error instanceof Error ? error.message : String(error);
        set((state) => {
          state.wasmStatus = 'error';
          state.wasmOutput = '';
          state.wasmMessage = isUnavailable ? `WASM runtime not available: ${message}` : message;
        });
      }
    },
  })),
);
