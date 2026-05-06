import { Handle, Position, type Node, type NodeProps } from '@xyflow/react';
import type { WildcardData } from '../lib/types';
import { BodyRootMarkerHandles } from './BodyRootMarkerHandles';

export function WildcardNode({ data }: NodeProps<Node<WildcardData>>) {
  return (
    <div className="rounded-lg border border-slate-500 bg-slate-900 text-slate-100 shadow-lg min-w-20 text-center"
         style={{padding: '4px'}}
    >
      <BodyRootMarkerHandles />
      <div className="text-[10px] font-bold text-slate-400 mb-1">WILDCARD</div>
      <div className="text-lg font-mono font-bold">{data.label}</div>
      <Handle type="source" position={Position.Right} />
    </div>
  );
}



