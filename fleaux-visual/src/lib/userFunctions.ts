import type { Node } from '@xyflow/react';
import type { AliasDeclData, FleauxNodeData, FunctionParam, LetData, TypeDeclData } from './types';

/**
 * Extract user-defined functions from let nodes in the graph.
 * Returns metadata about each function that can be used to create new nodes.
 */
export interface UserFunctionEntry {
  nodeId: string;
  qualifiedName: string;
  name: string;
  typeParams?: string[];
  params: FunctionParam[];
  returnType: string;
}

export interface UserTypeEntry {
  nodeId: string;
  name: string;
  targetType: string;
  separator: '=' | '::';
}

export interface UserAliasEntry {
  nodeId: string;
  name: string;
  targetType: string;
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
        typeParams: letData.typeParams,
        params: letData.params,
        returnType: letData.returnType,
      });
    }
  }

  return userFuncs;
}

export function extractUserTypes(nodes: Node<FleauxNodeData>[]): UserTypeEntry[] {
  const userTypes: UserTypeEntry[] = [];

  for (const node of nodes) {
    if (node.data.kind === 'typeDecl') {
      const typeData = node.data as TypeDeclData;
      userTypes.push({
        nodeId: node.id,
        name: typeData.name,
        targetType: typeData.targetType,
        separator: typeData.separator,
      });
    }
  }

  return userTypes;
}

export function extractUserAliases(nodes: Node<FleauxNodeData>[]): UserAliasEntry[] {
  const userAliases: UserAliasEntry[] = [];

  for (const node of nodes) {
    if (node.data.kind === 'aliasDecl') {
      const aliasData = node.data as AliasDeclData;
      userAliases.push({
        nodeId: node.id,
        name: aliasData.name,
        targetType: aliasData.targetType,
      });
    }
  }

  return userAliases;
}

