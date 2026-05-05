import type { Node } from '@xyflow/react';
import { formatFunctionDisplayName } from './functionSignatures';
import { STD_FUNCTIONS } from './stdCatalogue';
import type { FleauxNodeData, FunctionParam, LetData, UserFuncData } from './types';

const stdApplyEntry = STD_FUNCTIONS.find((entry) => entry.qualifiedName === 'Std.Apply' && entry.params.length === 2);

function createParamId(ownerNodeId: string, index: number): string {
  return `${ownerNodeId}-param-${index}`;
}

function normalizeFunctionParams(ownerNodeId: string, params: unknown): FunctionParam[] {
  if (!Array.isArray(params)) {
    return [];
  }

  return params.map((param, index) => {
    const record = (param ?? {}) as Record<string, unknown>;
    return {
      id: typeof record.id === 'string' && record.id.trim().length > 0 ? record.id : createParamId(ownerNodeId, index),
      name: typeof record.name === 'string' ? record.name : `p${index + 1}`,
      type: typeof record.type === 'string' ? record.type : 'Any',
    };
  });
}

/**
 * Migrate old graph data where `std` namespace nodes existed.
 * Old nodes with kind: 'std' are converted to a default stdFunc placeholder.
 */
export function migrateGraphNodes(
  nodes: Array<Node<Record<string, unknown>>>,
): Node<FleauxNodeData>[] {
  const basicMigrated = nodes.map((node): Node<FleauxNodeData> => {
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

    if (data.kind === 'let') {
      const letData = data as unknown as LetData;
      return {
        ...node,
        data: {
          ...letData,
          params: normalizeFunctionParams(node.id, letData.params),
        } as FleauxNodeData,
      };
    }

    if (data.kind === 'userFunc') {
      const userFuncData = data as unknown as UserFuncData;
      return {
        ...node,
        data: {
          ...userFuncData,
          functionNodeId: typeof userFuncData.functionNodeId === 'string' ? userFuncData.functionNodeId : '',
          params: normalizeFunctionParams(node.id, userFuncData.params),
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

  const letNodesById = new Map<string, Node<FleauxNodeData>>();
  const letNodeIdsByName = new Map<string, string[]>();
  for (const node of basicMigrated) {
    if (node.data.kind !== 'let') {
      continue;
    }
    letNodesById.set(node.id, node);
    const group = letNodeIdsByName.get(node.data.name) ?? [];
    group.push(node.id);
    letNodeIdsByName.set(node.data.name, group);
  }

  return basicMigrated.map((node): Node<FleauxNodeData> => {
    if (node.data.kind !== 'userFunc') {
      return node;
    }

    const userFuncData = node.data as UserFuncData;
    const linkedLetId = (() => {
      if (userFuncData.functionNodeId && letNodesById.has(userFuncData.functionNodeId)) {
        return userFuncData.functionNodeId;
      }
      const matches = letNodeIdsByName.get(userFuncData.functionName) ?? [];
      return matches.length === 1 ? matches[0] : '';
    })();

    const linkedLet = linkedLetId ? letNodesById.get(linkedLetId) : undefined;
    if (!linkedLet || linkedLet.data.kind !== 'let') {
      return {
        ...node,
        data: {
          ...userFuncData,
          functionNodeId: linkedLetId,
          params: normalizeFunctionParams(node.id, userFuncData.params),
        },
      };
    }

    const letData = linkedLet.data as LetData;
    return {
      ...node,
      data: {
        ...userFuncData,
        functionName: letData.name,
        functionNodeId: linkedLet.id,
        typeParams: letData.typeParams,
        params: letData.params.map((param) => ({ ...param })),
        returnType: letData.returnType,
        label: formatFunctionDisplayName(letData.name, letData.typeParams),
      } as FleauxNodeData,
    };
  });
}


