import type { Node } from '@xyflow/react';
import type { FleauxNodeData } from './types';

/**
 * Migrate old graph data where `std` namespace nodes existed.
 * Old nodes with kind: 'std' are converted to a default stdFunc placeholder.
 */
export function migrateGraphNodes(
  nodes: Array<Node<Record<string, unknown>>>,
): Node<FleauxNodeData>[] {
  return nodes.map((node): Node<FleauxNodeData> => {
    const data = node.data as Record<string, unknown>;

    if (data.kind === 'literal' && data.valueType === 'Number') {
      return {
        ...node,
        data: {
          ...data,
          valueType: 'Float64',
        } as FleauxNodeData,
      };
    }

    // Check if this is an old-style std namespace node
    if (data.kind === 'std' && typeof data.namespace === 'string') {
      // Convert to a stdFunc node using Std.Apply as a sensible default
      // that can work with any Std namespace
      return {
        ...node,
        type: 'stdFuncNode',
        data: {
          kind: 'stdFunc' as const,
          qualifiedName: 'Std.Apply',
          namespace: 'Std',
          params: [
            { name: 'value', type: 'Any' },
            { name: 'func', type: 'Any' },
          ],
          returnType: 'Any',
          label: 'Std.Apply',
        } as FleauxNodeData,
      };
    }

    // All other nodes pass through unchanged
    return {
      ...node,
      data: data as FleauxNodeData,
    };
  });
}


