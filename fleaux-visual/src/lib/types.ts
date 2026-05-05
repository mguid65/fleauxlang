import type { Edge } from '@xyflow/react';

//  Fleaux-specific node data types

export type FleauxNodeKind =
  | 'literal'   // A constant value: number, string, bool, null
  | 'wildcard'  // Match wildcard pattern value `_`
  | 'typeDecl'  // type Name = TargetType
  | 'aliasDecl' // alias Name = TargetType
  | 'let'       // let FunctionName(params): ReturnType = expr
  | 'closure'   // (params): ReturnType = expr — inline anonymous function
  | 'import'    // import ModuleName
  | 'rawSource' // Opaque top-level statement preserved verbatim for round-trip fidelity
  | 'tuple'     // (a, b, c) — argument grouping
  | 'stdValue'  // A concrete Std constant/value symbol
  | 'stdFunc'   // A concrete Std built-in function call node
  | 'userFunc'; // A user-defined function call node

interface FleauxNodeDataBase extends Record<string, unknown> {
  kind: FleauxNodeKind;
  label: string;
}

export type NumericLiteralValueType = 'Float64' | 'Int64' | 'UInt64';
export type LegacyNumericLiteralValueType = 'Number';
export type LiteralValueType = NumericLiteralValueType | LegacyNumericLiteralValueType | 'String' | 'Bool' | 'Null';

export interface FunctionParam {
  id: string;
  name: string;
  type: string;
}

// Data carried on every node (union-narrowed by `kind`)
export interface LiteralData extends FleauxNodeDataBase {
  kind: 'literal';
  valueType: LiteralValueType;
  value: string;
}

export interface WildcardData extends FleauxNodeDataBase {
  kind: 'wildcard';
}

export type TypeDeclarationSeparator = '=' | '::';

export interface TypeDeclData extends FleauxNodeDataBase {
  kind: 'typeDecl';
  name: string;
  targetType: string;
  separator: TypeDeclarationSeparator;
}

export interface AliasDeclData extends FleauxNodeDataBase {
  kind: 'aliasDecl';
  name: string;
  targetType: string;
}

export interface LetData extends FleauxNodeDataBase {
  kind: 'let';
  name: string;
  typeParams?: string[];
  params: FunctionParam[];
  returnType: string;
}

export interface ImportData extends FleauxNodeDataBase {
  kind: 'import';
  moduleName: string;
}

export interface RawSourceData extends FleauxNodeDataBase {
  kind: 'rawSource';
  statementText: string;
}

export interface TupleData extends FleauxNodeDataBase {
  kind: 'tuple';
  arity: number;
}

export interface StdValueData extends FleauxNodeDataBase {
  kind: 'stdValue';
  qualifiedName: string;
  namespace: string;
  valueType: string;
}

export interface StdFuncData extends FleauxNodeDataBase {
  kind: 'stdFunc';
  qualifiedName: string;
  namespace: string;
  typeParams?: string[];
  appliedTypeArgs?: string[];
  params: { name: string; type: string }[];
  returnType: string;
  signatureKey?: string;
  displayName?: string;
  displaySignature?: string;
  hasVariadicTail?: boolean;
  minimumArity?: number;
  overloadIndex?: number;
  overloadCount?: number;
  isTerminal?: boolean;
  // True when node represents a function value/reference, not a call invocation.
  isReference?: boolean;
}

export interface UserFuncData extends FleauxNodeDataBase {
  kind: 'userFunc';
  functionName: string;
  functionNodeId: string;
  typeParams?: string[];
  appliedTypeArgs?: string[];
  params: FunctionParam[];
  returnType: string;
  // True when node represents a function value/reference, not a call invocation.
  isReference?: boolean;
}

export interface ClosureData extends FleauxNodeDataBase {
  kind: 'closure';
  params: { name: string; type: string }[];
  returnType: string;
}

export type FleauxNodeData =
  | LiteralData
  | WildcardData
  | TypeDeclData
  | AliasDeclData
  | LetData
  | ImportData
  | RawSourceData
  | TupleData
  | StdValueData
  | StdFuncData
  | UserFuncData
  | ClosureData;

//  Fleaux-specific edge types

export type FleauxEdgeKind = 'pipeline';

export interface FleauxEdgeData extends Record<string, unknown> {
  kind: FleauxEdgeKind;
  bodySourceNodeId?: string;
  bodySourceHandle?: string;
}

export type FleauxEdge = Edge<FleauxEdgeData>;

