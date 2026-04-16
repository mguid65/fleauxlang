import { Handle, Position, type Node, type NodeProps } from '@xyflow/react';
import type { StdFuncData } from '../lib/types';
import { NS_COLORS } from '../lib/stdTheme';

export function StdFuncNode({ data }: NodeProps<Node<StdFuncData>>) {
  const isReference = data.isReference === true;
  const colors = NS_COLORS[data.namespace] ?? 'border-slate-500 bg-slate-900 text-slate-200';
  const paramSig = data.params.length === 0
    ? '()'
    : `(${data.params.map((p) => `${p.name}: ${p.type}`).join(', ')})`;

  return (
    <div 
      className={`rounded-lg border px-3 py-2 shadow-lg min-w-[160px] max-w-[260px] ${colors}`}
      style={{ minHeight: `${80 + Math.max(0, data.params.length - 1) * 20}px` }}
    >
      <div className="text-[9px] font-bold opacity-60 mb-0.5 uppercase tracking-wider">{data.namespace}</div>
      <div className="text-sm font-mono font-bold leading-tight">{data.namespace.split('.').pop()}.{data.qualifiedName.split('.').pop()}</div>
      {isReference && <div className="text-[9px] mt-0.5 opacity-70 font-mono">ref</div>}
      <div className="text-[10px] opacity-60 mt-1 font-mono truncate" title={paramSig}>{paramSig}</div>
      <div className="text-[10px] opacity-60 font-mono">→ {data.returnType}</div>
      {!isReference && Array.from({ length: data.params.length }, (_, i) => (
        <Handle
          key={i}
          type="target"
          position={Position.Left}
          id={`stdfunc-in-${i}`}
          style={{ top: `${68 + i * 20}px` }}
        />
      ))}
      <Handle type="source" position={Position.Right} />
    </div>
  );
}

