import type { Node } from '@xyflow/react';
import { STD_FUNCTIONS } from '../lib/stdCatalogue';
import type { FleauxEdge, FleauxNodeData } from '../lib/types';

function getStdFunctionOrThrow(qualifiedName: string, paramCount: number) {
  const entry = STD_FUNCTIONS.find((fn) => fn.qualifiedName === qualifiedName && fn.params.length === paramCount);
  if (!entry) {
    throw new Error(`Missing Std catalogue entry for ${qualifiedName}/${paramCount}.`);
  }
  return entry;
}

const stdAddEntry = getStdFunctionOrThrow('Std.Add', 2);
const stdPrintlnEntry = getStdFunctionOrThrow('Std.Println', 1);

//  Demo graph: `let Add(a: Float64, b: Float64): Float64 = (a, b) -> Std.Add; ((100.0, 150.0) -> Add) -> Std.Println;`

export const initialNodes: Node<FleauxNodeData>[] = [
  {
    id: 'import-std',
    type: 'importNode',
    position: { x: 60, y: 60 },
    data: { kind: 'import', moduleName: 'Std', label: 'import Std' },
  },
  {
    id: 'let-add',
    type: 'letNode',
    position: { x: 60, y: 200 },
    data: {
      kind: 'let',
      name: 'Add',
      params: [
        { id: 'let-add-param-0', name: 'a', type: 'Float64' },
        { id: 'let-add-param-1', name: 'b', type: 'Float64' },
      ],
      returnType: 'Float64',
      label: 'let Add',
    },
  },
  {
    id: 'std-add',
    type: 'stdFuncNode',
    position: { x: 560, y: 300 },
    data: {
      kind: 'stdFunc',
      qualifiedName: stdAddEntry.qualifiedName,
      namespace: stdAddEntry.namespace,
      typeParams: stdAddEntry.typeParams,
      params: stdAddEntry.params,
      returnType: stdAddEntry.returnType,
      signatureKey: stdAddEntry.signatureKey,
      displayName: stdAddEntry.displayName,
      displaySignature: stdAddEntry.displaySignature,
      hasVariadicTail: stdAddEntry.hasVariadicTail,
      minimumArity: stdAddEntry.minimumArity,
      overloadIndex: stdAddEntry.overloadIndex,
      overloadCount: stdAddEntry.overloadCount,
      isTerminal: stdAddEntry.isTerminal,
      label: stdAddEntry.displayName,
    },
  },
  {
    id: 'lit-left',
    type: 'literalNode',
    position: { x: 340, y: 470 },
    data: { kind: 'literal', valueType: 'Float64', value: '100.0', label: '100.0' },
  },
  {
    id: 'lit-right',
    type: 'literalNode',
    position: { x: 340, y: 560 },
    data: { kind: 'literal', valueType: 'Float64', value: '150.0', label: '150.0' },
  },
  {
    id: 'user-add-call',
    type: 'userFuncNode',
    position: { x: 620, y: 515 },
    data: {
      kind: 'userFunc',
      functionName: 'Add',
      functionNodeId: 'let-add',
      params: [
        { id: 'let-add-param-0', name: 'a', type: 'Float64' },
        { id: 'let-add-param-1', name: 'b', type: 'Float64' },
      ],
      returnType: 'Float64',
      label: 'Add',
    },
  },
  {
    id: 'std-println',
    type: 'stdFuncNode',
    position: { x: 900, y: 515 },
    data: {
      kind: 'stdFunc',
      qualifiedName: stdPrintlnEntry.qualifiedName,
      namespace: stdPrintlnEntry.namespace,
      typeParams: stdPrintlnEntry.typeParams,
      params: stdPrintlnEntry.params,
      returnType: stdPrintlnEntry.returnType,
      signatureKey: stdPrintlnEntry.signatureKey,
      displayName: stdPrintlnEntry.displayName,
      displaySignature: stdPrintlnEntry.displaySignature,
      hasVariadicTail: stdPrintlnEntry.hasVariadicTail,
      minimumArity: stdPrintlnEntry.minimumArity,
      overloadIndex: stdPrintlnEntry.overloadIndex,
      overloadCount: stdPrintlnEntry.overloadCount,
      isTerminal: stdPrintlnEntry.isTerminal,
      label: stdPrintlnEntry.displayName,
    },
  },
];

export const initialEdges: FleauxEdge[] = [
  {
    id: 'e-let-body-root',
    source: 'let-add',
    target: 'std-add',
    targetHandle: 'let-body-root',
    animated: true,
    data: { kind: 'pipeline' },
  },
  {
    id: 'e-let-a-std-0',
    source: 'let-add',
    sourceHandle: 'let-param-0',
    target: 'std-add',
    targetHandle: 'stdfunc-in-0',
    animated: true,
    data: { kind: 'pipeline' },
  },
  {
    id: 'e-let-b-std-1',
    source: 'let-add',
    sourceHandle: 'let-param-1',
    target: 'std-add',
    targetHandle: 'stdfunc-in-1',
    animated: true,
    data: { kind: 'pipeline' },
  },
  {
    id: 'e-lit-left-add-0',
    source: 'lit-left',
    target: 'user-add-call',
    targetHandle: 'userfunc-in-0',
    animated: true,
    data: { kind: 'pipeline' },
  },
  {
    id: 'e-lit-right-add-1',
    source: 'lit-right',
    target: 'user-add-call',
    targetHandle: 'userfunc-in-1',
    animated: true,
    data: { kind: 'pipeline' },
  },
  {
    id: 'e-add-println',
    source: 'user-add-call',
    target: 'std-println',
    targetHandle: 'stdfunc-in-0',
    animated: true,
    data: { kind: 'pipeline' },
  },
];
