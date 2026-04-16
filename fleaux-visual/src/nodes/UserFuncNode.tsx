import { Handle, Position, type Node, type NodeProps } from '@xyflow/react';
import type { UserFuncData } from '../lib/types';

export function UserFuncNode({ data }: NodeProps<Node<UserFuncData>>) {
  const isReference = data.isReference === true;
  const paramSig = data.params.length === 0
    ? '()'
    : `(${data.params.map((p) => `${p.name}: ${p.type}`).join(', ')})`;

  return (
    <div 
      className="rounded-lg border border-purple-500 bg-purple-950 text-purple-200 px-3 py-2 shadow-lg min-w-[160px] max-w-[260px]"
      style={{ minHeight: `${80 + Math.max(0, data.params.length - 1) * 20}px` }}
    >
      <div className="text-[9px] font-bold opacity-60 mb-0.5 uppercase tracking-wider">USER FUNC</div>
      <div className="text-sm font-mono font-bold leading-tight">{data.functionName}</div>
      {isReference && <div className="text-[9px] mt-0.5 opacity-70 font-mono">ref</div>}
      <div className="text-[10px] opacity-60 mt-1 font-mono truncate" title={paramSig}>{paramSig}</div>
      <div className="text-[10px] opacity-60 font-mono">→ {data.returnType}</div>
      {!isReference && Array.from({ length: data.params.length }, (_, i) => (
        <Handle
          key={i}
          type="target"
          position={Position.Left}
          id={`userfunc-in-${i}`}
          style={{ top: `${68 + i * 20}px` }}
        />
      ))}
      <Handle type="source" position={Position.Right} />
    </div>
  );
}


