import { Handle, Position, type Node, type NodeProps } from '@xyflow/react';
import type { ImportData } from '../lib/types';

export function ImportNode({ data }: NodeProps<Node<ImportData>>) {
  return (
    <div className="rounded-lg border border-teal-500 bg-teal-950 text-teal-200 shadow-lg min-w-[120px]"
         style={{padding: '4px'}}
    >
      <div className="text-[10px] font-bold text-teal-400 mb-1">IMPORT</div>
      <div className="text-sm font-mono font-bold">{data.moduleName}</div>
      <Handle type="source" position={Position.Right} />
    </div>
  );
}
