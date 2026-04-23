import { Handle, Position, type Node, type NodeProps } from '@xyflow/react';
import type { StdFuncData } from '../lib/types';
import { formatAritySummary, formatFunctionDisplayName, formatParamSignature, isTerminalReturnType } from '../lib/functionSignatures';
import { NS_COLORS } from '../lib/stdTheme';

export function StdFuncNode({ data }: NodeProps<Node<StdFuncData>>) {
  const isReference = data.isReference === true;
  const isTerminal = data.isTerminal ?? isTerminalReturnType(data.returnType);
  const colors = isTerminal
    ? 'border-red-500 bg-red-950 text-red-200'
    : (NS_COLORS[data.namespace] ?? 'border-slate-500 bg-slate-900 text-slate-200');
  const paramSig = formatParamSignature(data.params);
  const badges: string[] = [];
  const overloadCount = data.overloadCount ?? 1;
  const overloadIndex = data.overloadIndex ?? 1;

  if (overloadCount > 1) {
    badges.push(`overload ${overloadIndex}/${overloadCount}`);
  }

  if (data.hasVariadicTail === true) {
    badges.push(`variadic ${formatAritySummary(data.params)}`);
  }

  if (isTerminal) {
    badges.push('Never');
  }

  const nodeTitle = data.displayName
    ?? formatFunctionDisplayName(data.qualifiedName, data.typeParams);

  return (
    <div 
      className={`rounded-lg border px-3 py-2 shadow-lg min-w-40 max-w-65 ${colors}`}
      style={{ minHeight: `${80 + Math.max(0, data.params.length - 1) * 20}px` }}
    >
      <div className="text-[9px] font-bold opacity-60 mb-0.5 uppercase tracking-wider">{data.namespace}</div>
      <div className="text-sm font-mono font-bold leading-tight">{nodeTitle}</div>
      {isReference && <div className="text-[9px] mt-0.5 opacity-70 font-mono">ref</div>}
      {badges.length > 0 && <div className="text-[9px] mt-0.5 opacity-70 font-mono">{badges.join(' | ')}</div>}
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
      {!isTerminal && <Handle type="source" position={Position.Right} />}
    </div>
  );
}

