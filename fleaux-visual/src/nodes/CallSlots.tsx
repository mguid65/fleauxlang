import { Handle, Position } from '@xyflow/react';
import { callSlotCount, hasVariadicTail, minArity } from '../lib/functionSignatures';
import { useFlowStore } from '../store/flowStore';

type CallSlotsProps = {
  nodeId: string;
  handlePrefix: string;
  params: { name: string; type: string }[];
  variadicSlots: number | undefined;
};

// The argument slots of a call node: one per parameter, and a variadic tail whose slot count is edited with +/-.
export function CallSlots({ nodeId, handlePrefix, params, variadicSlots }: CallSlotsProps) {
  const updateNodeData = useFlowStore((s) => s.updateNodeData);
  const variadic = hasVariadicTail(params);
  const fixedCount = minArity(params);
  const slotCount = callSlotCount(params, variadicSlots);
  const tailSlots = slotCount - fixedCount;

  return (
    <>
      {variadic && (
        <div className="flex gap-1 mt-1 items-center">
          <button
            onClick={() => updateNodeData(nodeId, { variadicSlots: Math.max(0, tailSlots - 1) })}
            disabled={tailSlots === 0}
            className="text-[9px] px-1.5 py-0.5 bg-white/10 hover:bg-white/20 disabled:opacity-50 disabled:cursor-not-allowed rounded transition-colors"
            title="Remove a variadic argument slot"
          >
            -
          </button>
          <button
            onClick={() => updateNodeData(nodeId, { variadicSlots: tailSlots + 1 })}
            className="text-[9px] px-1.5 py-0.5 bg-white/10 hover:bg-white/20 rounded transition-colors"
            title="Add a variadic argument slot"
          >
            +
          </button>
          <span className="text-[9px] opacity-60 font-mono">{tailSlots} variadic</span>
        </div>
      )}
      {Array.from({ length: slotCount }, (_, index) => (
        <Handle
          key={index}
          type="target"
          position={Position.Left}
          id={`${handlePrefix}${index}`}
          style={{ top: `${68 + index * 20}px` }}
          title={(index < fixedCount ? params[index] : params[params.length - 1])?.name}
        />
      ))}
    </>
  );
}
