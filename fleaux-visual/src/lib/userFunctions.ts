import type { Node } from '@xyflow/react';
import type { FleauxNodeData, LetData } from './types';

/**
 * Extract user-defined functions from let nodes in the graph.
 * Returns metadata about each function that can be used to create new nodes.
 */
export interface UserFunctionEntry {
  nodeId: string;
  qualifiedName: string;
  name: string;
  params: { name: string; type: string }[];
  returnType: string;
}

export function extractUserFunctions(nodes: Node<FleauxNodeData>[]): UserFunctionEntry[] {
  const userFuncs: UserFunctionEntry[] = [];

  for (const node of nodes) {
    if (node.data.kind === 'let') {
      const letData = node.data as LetData;
      userFuncs.push({
        nodeId: node.id,
        qualifiedName: letData.name,
        name: letData.name,
        params: letData.params,
        returnType: letData.returnType,
      });
    }
  }

  return userFuncs;
}

