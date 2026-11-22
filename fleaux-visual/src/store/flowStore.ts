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
import { autoLayout } from '../lib/autoLayout';
import { canCreatePipelineEdge } from '../lib/edgeValidation';
import { functionValueShape } from '../lib/functionValues';
import { formatFunctionDisplayName } from '../lib/functionSignatures';
import type { FleauxEdge, FleauxNodeData, LetData, UserFuncData } from '../lib/types';
import { initialNodes, initialEdges } from './initialGraph';
import { serializeGraphToFleaux } from '../lib/graphToFleaux';
import { wasmFormatSource, wasmRunSource, WasmCoordinatorError, WasmStatusCode } from '../lib/wasmCoordinator';
import { importFleauxSourceToGraph } from '../lib/fleauxToGraph';
import { loadSavedGraph, saveGraph } from '../lib/graphStorage';

export type WasmValidationStatus = 'idle' | 'running' | 'success' | 'error';

export interface GraphSnapshot {
  nodes: Node<FleauxNodeData>[];
  edges: FleauxEdge[];
  documentName: string;
}

//  State shape

export interface FlowState {
  nodes: Node<FleauxNodeData>[];
  edges: FleauxEdge[];
  sourceText: string;
  // The file the graph came from, such as a sample; Save uses it as the file name.
  documentName: string;
  // Undo and redo history: graph states, with their file names, before and after the current one, the latest last.
  past: GraphSnapshot[];
  future: GraphSnapshot[];
  // Set by an import: the new nodes are laid out once React Flow has measured them.
  layoutPending: boolean;
  wasmOutput: string;
  wasmStatus: WasmValidationStatus;
  wasmMessage: string | null;
  setSourceText: (sourceText: string) => void;
  onNodesChange: (changes: NodeChange<Node<FleauxNodeData>>[]) => void;
  onEdgesChange: (changes: EdgeChange<FleauxEdge>[]) => void;
  onConnect: (connection: Connection) => void;
  addNode: (node: Node<FleauxNodeData>) => void;
  updateNodeData: (id: string, data: Partial<FleauxNodeData>) => void;
  updateLetNodeSignature: (
    id: string,
    patch: Partial<Pick<LetData, 'name' | 'typeParams' | 'params' | 'returnType' | 'isValueBinding'>>,
  ) => void;
  clearGraph: () => void;
  loadGraphFromSource: (sourceText: string, documentName?: string) => void;
  // Records an undo step for a change React Flow is about to make itself: a node drag or a deletion.
  markUndoStep: () => void;
  undo: () => void;
  redo: () => void;
  // Lays the whole graph out again, as one undo step.
  tidyLayout: () => void;
  // Lays out a freshly imported graph; part of the import's undo step.
  applyPendingLayout: () => void;
  // The graph as Fleaux source, or null when it cannot be serialized; the error then shows in the output panel.
  graphSource: () => string | null;
  // Replaces the editor's contents with the graph's source, formatted when the WASM formatter is available.
  generateSource: () => Promise<void>;
  // Formats the editor's contents with the core formatter.
  formatEditorSource: () => Promise<void>;
  runGraphWithWasm: () => Promise<void>;
  runEditorSourceWithWasm: () => Promise<void>;
}

// The source formatted by the WASM formatter, or unchanged when the formatter is not available.
async function formatIfAvailable(source: string): Promise<string> {
  try {
    return await wasmFormatSource(source);
  } catch {
    return source;
  }
}

//  History

const HISTORY_LIMIT = 100;
// Edits with the same key within this many milliseconds of each other, such as typing in a field, are one undo step.
const COALESCE_MS = 1000;
let lastEdit: { key: string; time: number } | null = null;

//  Store

const savedGraph = loadSavedGraph();

export const useFlowStore = create<FlowState>()(
  immer((set, get) => {
    // Applies a change to the graph as one undo step.
    const commit = (recipe: (state: FlowState) => void, coalesceKey?: string) => {
      const now = Date.now();
      const continuesEdit =
        coalesceKey !== undefined && lastEdit?.key === coalesceKey && now - lastEdit.time < COALESCE_MS;
      lastEdit = coalesceKey === undefined ? null : { key: coalesceKey, time: now };
      const { nodes, edges, documentName } = get();
      set((state) => {
        if (!continuesEdit) {
          state.past.push({ nodes, edges, documentName });
          if (state.past.length > HISTORY_LIMIT) {
            state.past.shift();
          }
          state.future = [];
        }
        recipe(state);
      });
    };

    const layoutPositions = () => {
      const { nodes, edges } = get();
      return autoLayout(nodes, edges);
    };

    const moveNodes = (state: FlowState, positions: ReturnType<typeof autoLayout>) => {
      for (const node of state.nodes) {
        const position = positions.get(node.id);
        if (position) {
          node.position = position;
        }
      }
    };

    return {
      nodes: savedGraph?.nodes ?? initialNodes,
      edges: savedGraph?.edges ?? initialEdges,
      sourceText: savedGraph?.sourceText ?? '',
      documentName: savedGraph?.documentName ?? '',
      past: [],
      future: [],
      layoutPending: false,
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
        if (!canCreatePipelineEdge(connection, get().edges, get().nodes)) {
          return;
        }
        commit((state) => {
          state.edges = addEdge(
            { ...connection, animated: true, data: { kind: 'pipeline' } },
            state.edges,
          );

          // A call-value node takes its argument slots from the function it calls, until arguments are connected.
          const target = state.nodes.find((node) => node.id === connection.target);
          const source = state.nodes.find((node) => node.id === connection.source);
          if (connection.targetHandle === 'callvalue-fn' && target?.data.kind === 'callValue' && source) {
            const hasArguments = state.edges.some(
              (edge) => edge.target === target.id && edge.targetHandle?.startsWith('callvalue-in-') === true,
            );
            const shape = functionValueShape(source as Node<FleauxNodeData>, connection.sourceHandle);
            if (shape && !hasArguments) {
              target.data = { ...target.data, slots: shape.slots, functionType: shape.functionType };
            }
          }
        });
      },

      addNode(node) {
        commit((state) => {
          state.nodes.push(node);
        });
      },

      updateNodeData(id, data) {
        commit((state) => {
          const node = state.nodes.find((n) => n.id === id);
          if (node) {
            Object.assign(node.data, data);
          }
        }, `edit:${id}`);
      },

      updateLetNodeSignature(id, patch) {
        commit((state) => {
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
        }, `edit:${id}`);
      },

      clearGraph() {
        commit((state) => {
          state.nodes = [];
          state.edges = [];
          state.sourceText = '';
          state.documentName = '';
          state.layoutPending = false;
          state.wasmOutput = '';
          state.wasmStatus = 'idle';
          state.wasmMessage = null;
        });
      },

      loadGraphFromSource(sourceText, documentName) {
        try {
          const imported = importFleauxSourceToGraph(sourceText);
          commit((state) => {
            state.nodes = imported.nodes;
            state.edges = imported.edges;
            state.sourceText = sourceText;
            if (documentName !== undefined) {
              state.documentName = documentName;
            }
            state.layoutPending = true;
            state.wasmOutput = '';
            state.wasmStatus = 'success';
            state.wasmMessage = `Loaded ${imported.nodes.length} nodes from source`;
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

      markUndoStep() {
        commit(() => {});
      },

      undo() {
        const { past, nodes, edges, documentName } = get();
        const previous = past.at(-1);
        if (!previous) {
          return;
        }
        lastEdit = null;
        set((state) => {
          state.past.pop();
          state.future.push({ nodes, edges, documentName });
          state.nodes = previous.nodes;
          state.edges = previous.edges;
          state.documentName = previous.documentName;
        });
      },

      redo() {
        const { future, nodes, edges, documentName } = get();
        const next = future.at(-1);
        if (!next) {
          return;
        }
        lastEdit = null;
        set((state) => {
          state.future.pop();
          state.past.push({ nodes, edges, documentName });
          state.nodes = next.nodes;
          state.edges = next.edges;
          state.documentName = next.documentName;
        });
      },

      tidyLayout() {
        const positions = layoutPositions();
        commit((state) => moveNodes(state, positions));
      },

      applyPendingLayout() {
        const positions = layoutPositions();
        set((state) => {
          moveNodes(state, positions);
          state.layoutPending = false;
        });
      },

      graphSource() {
        try {
          return serializeGraphToFleaux(get().nodes, get().edges).sourceText;
        } catch (error) {
          const message = error instanceof Error ? error.message : String(error);
          set((state) => {
            state.wasmStatus = 'error';
            state.wasmOutput = '';
            state.wasmMessage = `Failed to generate source: ${message}`;
          });
          return null;
        }
      },

      async generateSource() {
        const source = get().graphSource();
        if (source === null) {
          return;
        }
        const formatted = await formatIfAvailable(source);
        set((state) => {
          state.sourceText = formatted;
          state.wasmStatus = 'success';
          state.wasmOutput = '';
          state.wasmMessage = 'Generated source from the graph';
        });
      },

      async formatEditorSource() {
        const source = get().sourceText;
        if (source.trim().length === 0) {
          return;
        }
        try {
          const formatted = await wasmFormatSource(source);
          set((state) => {
            // Text typed while the formatter ran is left alone.
            if (state.sourceText !== source) {
              state.wasmMessage = 'The editor changed while formatting. Format again.';
              return;
            }
            state.sourceText = formatted;
            state.wasmStatus = 'success';
            state.wasmOutput = '';
            state.wasmMessage = 'Formatted the editor source';
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

      async runGraphWithWasm() {
        let generatedSource = '';

        set((state) => {
          state.wasmStatus = 'running';
          state.wasmMessage = 'Generating canonical Fleaux source…';
        });

        try {
          const { sourceText } = serializeGraphToFleaux(get().nodes, get().edges);
          generatedSource = await formatIfAvailable(sourceText);

          set((state) => {
            state.sourceText = generatedSource;
            state.wasmOutput = '';
            state.wasmMessage = 'Running graph with Fleaux WASM…';
          });

          const result = await wasmRunSource(generatedSource);

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
    };
  }),
);

//  Saving across reloads

const SAVE_DELAY_MS = 400;
let saveTimer: ReturnType<typeof setTimeout> | undefined;

function saveNow() {
  saveTimer = undefined;
  const { nodes, edges, sourceText, documentName } = useFlowStore.getState();
  saveGraph({ nodes, edges, sourceText, documentName });
}

useFlowStore.subscribe((state, previous) => {
  if (
    state.nodes === previous.nodes
    && state.edges === previous.edges
    && state.sourceText === previous.sourceText
    && state.documentName === previous.documentName
  ) {
    return;
  }
  clearTimeout(saveTimer);
  saveTimer = setTimeout(saveNow, SAVE_DELAY_MS);
});

window.addEventListener('pagehide', () => {
  if (saveTimer !== undefined) {
    clearTimeout(saveTimer);
    saveNow();
  }
});
