import { Handle, Position, type Node, type NodeProps } from '@xyflow/react';
import type { CallValueData } from '../lib/types';
import { useFlowStore } from '../store/flowStore';

// Calls a function value with the arguments in its slots: a parameter or value let of function type, `(a, b) -> f`,
// or a closure, `(a, b) -> (x: A, y: B): R = body`. The first input is the function.
export function CallValueNode({ id, data }: NodeProps<Node<CallValueData>>) {
  const updateNodeData = useFlowStore((s) => s.updateNodeData);

  return (
    <div
      className="rounded-lg border border-teal-500 bg-teal-950 text-teal-200 shadow-lg min-w-[150px]"
      style={{ minHeight: `${86 + Math.max(0, data.slots - 1) * 20}px`, padding: '4px' }}
    >
      <div className="text-[10px] font-bold text-teal-400 mb-0.5">CALL VALUE</div>
      <div className="text-[10px] font-mono opacity-70 truncate" title={data.functionType}>
        f{data.functionType ? `: ${data.functionType}` : ''}
      </div>
      <div className="flex gap-1 mt-1 items-center">
        <button
          onClick={() => updateNodeData(id, { slots: Math.max(0, data.slots - 1) })}
          disabled={data.slots === 0}
          className="text-[9px] px-1.5 py-0.5 bg-white/10 hover:bg-white/20 disabled:opacity-50 disabled:cursor-not-allowed rounded transition-colors"
          title="Remove an argument slot"
        >
          -
        </button>
        <button
          onClick={() => updateNodeData(id, { slots: data.slots + 1 })}
          className="text-[9px] px-1.5 py-0.5 bg-white/10 hover:bg-white/20 rounded transition-colors"
          title="Add an argument slot"
        >
          +
        </button>
        <span className="text-[9px] opacity-60 font-mono">{data.slots} args</span>
      </div>
      <Handle type="target" position={Position.Left} id="callvalue-fn" style={{ top: '24px' }} title="function" />
      {Array.from({ length: data.slots }, (_, index) => (
        <Handle
          key={index}
          type="target"
          position={Position.Left}
          id={`callvalue-in-${index}`}
          style={{ top: `${68 + index * 20}px` }}
          title={`argument ${index + 1}`}
        />
      ))}
      <Handle type="source" position={Position.Right} />
    </div>
  );
}
