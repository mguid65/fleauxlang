import type { Node } from '@xyflow/react';
import type { FleauxEdge, FleauxNodeData } from '../lib/types';

// ─── Demo graph: `let Add(a: Number, b: Number): Number = (a, b) -> Std.Add` ─

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
        { name: 'a', type: 'Number' },
        { name: 'b', type: 'Number' },
      ],
      returnType: 'Number',
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
        { name: 'lhs', type: 'Number' },
        { name: 'rhs', type: 'Number' },
      ],
      returnType: 'Number',
      label: 'Std.Add',
    },
  },
  {
    id: 'lit-hello',
    type: 'literalNode',
    position: { x: 420, y: 450 },
    data: { kind: 'literal', valueType: 'String', value: 'Hello, Fleaux!', label: '"Hello, Fleaux!"' },
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
];
