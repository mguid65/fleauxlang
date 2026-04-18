import type { Node } from '@xyflow/react';
import type { FleauxEdge, FleauxNodeData } from '../lib/types';

// ─── Demo graph: `let Add(a: Float64, b: Float64): Float64 = (a, b) -> Std.Add; ((100.0, 150.0) -> Add) -> Std.Println;` ─

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
        { name: 'a', type: 'Float64' },
        { name: 'b', type: 'Float64' },
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
      qualifiedName: 'Std.Add',
      namespace: 'Std',
      params: [
        { name: 'lhs', type: 'Float64 | Int64 | UInt64' },
        { name: 'rhs', type: 'Float64 | Int64 | UInt64' },
      ],
      returnType: 'Float64 | Int64 | UInt64',
      label: 'Std.Add',
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
        { name: 'a', type: 'Float64' },
        { name: 'b', type: 'Float64' },
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
      qualifiedName: 'Std.Println',
      namespace: 'Std',
      params: [{ name: 'args', type: 'Any...' }],
      returnType: 'Tuple(Any...)',
      label: 'Std.Println',
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
    animated: true,
    data: { kind: 'pipeline' },
  },
];
