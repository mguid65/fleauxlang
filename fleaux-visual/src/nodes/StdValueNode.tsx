import { Handle, Position, type Node, type NodeProps } from '@xyflow/react';
import type { StdValueData } from '../lib/types';
import { NS_COLORS } from '../lib/stdTheme';

export function StdValueNode({ data }: NodeProps<Node<StdValueData>>) {
  const colors = NS_COLORS[data.namespace] ?? 'border-slate-500 bg-slate-900 text-slate-200';

  return (
    <div className={`rounded-lg border px-3 py-2 shadow-lg min-w-[170px] max-w-[280px] ${colors}`}>
      <div className="text-[9px] font-bold opacity-60 mb-0.5 uppercase tracking-wider">{data.namespace}</div>
      <div className="text-sm font-mono font-bold leading-tight">{data.qualifiedName.split('.').pop()}</div>
      <div className="text-[10px] opacity-70 mt-1 font-mono">: {data.valueType}</div>
      <Handle type="target" position={Position.Left} />
      <Handle type="source" position={Position.Right} />
    </div>
  );
}


