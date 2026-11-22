import { Handle, Position, type Node, type NodeProps } from '@xyflow/react';
import type { UserFuncData } from '../lib/types';
import { formatFunctionDisplayName, formatParamSignature } from '../lib/functionSignatures';
import { CallSlots } from './CallSlots';
import { callNodeMinHeight } from '../lib/nodeLayout';

export function UserFuncNode({ id, data }: NodeProps<Node<UserFuncData>>) {
  const isReference = data.isReference === true;
  const paramSig = formatParamSignature(data.params);
  const functionLabel = formatFunctionDisplayName(data.functionName, data.appliedTypeArgs ?? data.typeParams);

  return (
    <div 
      className="rounded-lg border border-purple-500 bg-purple-950 text-purple-200 shadow-lg min-w-[160px] max-w-[260px]"
      style={{ minHeight: `${callNodeMinHeight(data.params, data.variadicSlots)}px`, padding: "4px" }}
    >
      <div className="text-[9px] font-bold opacity-60 mb-0.5 uppercase tracking-wider">USER FUNC</div>
      <div className="text-sm font-mono font-bold leading-tight">{functionLabel}</div>
      {isReference && <div className="text-[9px] mt-0.5 opacity-70 font-mono">ref</div>}
      <div className="text-[10px] opacity-60 mt-1 font-mono truncate" title={paramSig}>{paramSig}</div>
      <div className="text-[10px] opacity-60 font-mono">→ {data.returnType}</div>
      {!isReference && (
        <CallSlots nodeId={id} handlePrefix="userfunc-in-" params={data.params} variadicSlots={data.variadicSlots} />
      )}
      <Handle type="source" position={Position.Right} />
    </div>
  );
}


