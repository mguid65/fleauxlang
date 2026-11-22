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
  | 'spread'    // x...: the items of a tuple as arguments, inside a call's argument list
  | 'stdValue'  // A concrete Std constant/value symbol
  | 'stdFunc'   // A concrete Std built-in function call node
  | 'userFunc'  // A user-defined function call node
  | 'callValue'; // (args) -> f: a call of a function value (a parameter, value let, or closure)

interface FleauxNodeDataBase extends Record<string, unknown> {
  kind: FleauxNodeKind;
  label: string;
}

export type NumericLiteralValueType = 'Float64' | 'Int64' | 'UInt64';
export type LiteralValueType = NumericLiteralValueType | 'String' | 'Bool' | 'Null';

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
  // A value let, `let Name: Type = expr;`: it names a value and has no parameter list.
  isValueBinding?: boolean;
}

export interface ImportData extends FleauxNodeDataBase {
  kind: 'import';
  moduleName: string;
}

export interface RawSourceData extends FleauxNodeDataBase {
  kind: 'rawSource';
  statementText: string;
  // Why the importer kept the statement as raw source, when it came from an import.
  importError?: string;
}

export interface TupleData extends FleauxNodeDataBase {
  kind: 'tuple';
  arity: number;
}

export interface SpreadData extends FleauxNodeDataBase {
  kind: 'spread';
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
  // The operator the call was written with, such as '+'; serialized instead of the builtin's name.
  operator?: string;
  // The number of argument slots for a variadic tail; 1 when unset.
  variadicSlots?: number;
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
  // The number of argument slots for a variadic tail; 1 when unset.
  variadicSlots?: number;
}

export interface ClosureData extends FleauxNodeDataBase {
  kind: 'closure';
  // A generic closure's type parameters, each with its bound when it has one ('T' or 'T: Numeric').
  typeParams?: string[];
  params: { name: string; type: string }[];
  returnType: string;
}

export interface CallValueData extends FleauxNodeDataBase {
  kind: 'callValue';
  // The number of argument slots.
  slots: number;
  // The called value's function type, when known, for display.
  functionType?: string;
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
  | SpreadData
  | StdValueData
  | StdFuncData
  | UserFuncData
  | ClosureData
  | CallValueData;

//  Fleaux-specific edge types

export type FleauxEdgeKind = 'pipeline';

export interface FleauxEdgeData extends Record<string, unknown> {
  kind: FleauxEdgeKind;
  bodySourceNodeId?: string;
  bodySourceHandle?: string;
}

export type FleauxEdge = Edge<FleauxEdgeData>;

