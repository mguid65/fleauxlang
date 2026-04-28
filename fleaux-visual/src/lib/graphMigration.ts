import type { Node } from '@xyflow/react';
import { STD_FUNCTIONS } from './stdCatalogue';
import type { FleauxNodeData } from './types';

const stdApplyEntry = STD_FUNCTIONS.find((entry) => entry.qualifiedName === 'Std.Apply' && entry.params.length === 2);

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

    // Closure nodes pass through — no migration needed
    if (data.kind === 'closure') {
      return {
        ...node,
        data: data as FleauxNodeData,
      };
    }

    // Check if this is an old-style std namespace node
    if (data.kind === 'std' && typeof data.namespace === 'string') {
      // Convert to a stdFunc node using Std.Apply as a sensible default
      // that can work with any Std namespace
      if (!stdApplyEntry) {
        throw new Error('Expected Std.Apply entry in generated Std catalogue.');
      }

      return {
        ...node,
        type: 'stdFuncNode',
        data: {
          kind: 'stdFunc' as const,
          qualifiedName: stdApplyEntry.qualifiedName,
          namespace: stdApplyEntry.namespace,
          typeParams: stdApplyEntry.typeParams,
          params: stdApplyEntry.params,
          returnType: stdApplyEntry.returnType,
          signatureKey: stdApplyEntry.signatureKey,
          displayName: stdApplyEntry.displayName,
          displaySignature: stdApplyEntry.displaySignature,
          hasVariadicTail: stdApplyEntry.hasVariadicTail,
          minimumArity: stdApplyEntry.minimumArity,
          overloadIndex: stdApplyEntry.overloadIndex,
          overloadCount: stdApplyEntry.overloadCount,
          isTerminal: stdApplyEntry.isTerminal,
          label: stdApplyEntry.displayName,
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


