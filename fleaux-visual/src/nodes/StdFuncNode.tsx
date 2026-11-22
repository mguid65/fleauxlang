import { Handle, Position, type Node, type NodeProps } from '@xyflow/react';
import { CallSlots } from './CallSlots';
import { callNodeMinHeight } from '../lib/nodeLayout';
import type { StdFuncData } from '../lib/types';
import { formatAritySummary, formatFunctionDisplayName, formatParamSignature, isTerminalStdFunc } from '../lib/functionSignatures';
import { stdFuncGroup, stdNodeStyle } from '../lib/stdTheme';

export function StdFuncNode({ id, data }: NodeProps<Node<StdFuncData>>) {
  const isReference = data.isReference === true;
  const isTerminal = isTerminalStdFunc(data);
  const terminalClasses = isTerminal ? 'border-red-500 bg-red-950 text-red-200' : '';
  const colourStyle = isTerminal ? undefined : stdNodeStyle(stdFuncGroup(data));
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
    ?? formatFunctionDisplayName(data.qualifiedName, data.appliedTypeArgs ?? data.typeParams);

  return (
    <div 
      className={`rounded-lg border shadow-lg min-w-40 max-w-65 ${terminalClasses}`}
      style={{ ...colourStyle, minHeight: `${callNodeMinHeight(data.params, data.variadicSlots)}px`, padding: '4px' }}
    >
      <div className="text-[9px] font-bold opacity-60 mb-0.5 uppercase tracking-wider">
        {data.operator ? `operator ${data.qualifiedName}` : data.namespace}
      </div>
      <div className="text-sm font-mono font-bold leading-tight">{nodeTitle}</div>
      {isReference && <div className="text-[9px] mt-0.5 opacity-70 font-mono">ref</div>}
      {badges.length > 0 && <div className="text-[9px] mt-0.5 opacity-70 font-mono">{badges.join(' | ')}</div>}
      <div className="text-[10px] opacity-60 mt-1 font-mono truncate" title={paramSig}>{paramSig}</div>
      <div className="text-[10px] opacity-60 font-mono">→ {data.returnType}</div>
      {!isReference && (
        <CallSlots nodeId={id} handlePrefix="stdfunc-in-" params={data.params} variadicSlots={data.variadicSlots} />
      )}
      {!isTerminal && <Handle type="source" position={Position.Right} />}
    </div>
  );
}

