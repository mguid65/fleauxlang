import type { Node } from '@xyflow/react';
import type { FleauxEdge, FleauxNodeData } from './types';

// The editor keeps the open graph in the browser's local storage, so a reload brings it back.

const STORAGE_KEY = 'fleaux-visual.graph';
// Raise this when the saved shape changes; a graph saved under another version is dropped.
const STORAGE_VERSION = 1;

export interface SavedGraph {
  nodes: Node<FleauxNodeData>[];
  edges: FleauxEdge[];
  sourceText: string;
  documentName: string;
}

export function loadSavedGraph(): SavedGraph | null {
  try {
    const raw = localStorage.getItem(STORAGE_KEY);
    if (raw === null) {
      return null;
    }
    const saved = JSON.parse(raw) as Partial<SavedGraph> & { version?: number };
    if (saved.version !== STORAGE_VERSION || !Array.isArray(saved.nodes) || !Array.isArray(saved.edges)) {
      return null;
    }
    return {
      nodes: saved.nodes,
      edges: saved.edges,
      sourceText: typeof saved.sourceText === 'string' ? saved.sourceText : '',
      documentName: typeof saved.documentName === 'string' ? saved.documentName : '',
    };
  } catch {
    return null;
  }
}

export function saveGraph(graph: SavedGraph): void {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify({ version: STORAGE_VERSION, ...graph }));
  } catch {
    // Storage can be full or disabled; the graph then lives only until the page closes.
  }
}
