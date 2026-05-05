import { Handle, Position, type Node, type NodeProps } from '@xyflow/react';
import type { TupleData } from '../lib/types';
import { useFlowStore } from '../store/flowStore';

export function TupleNode({ id, data }: NodeProps<Node<TupleData>>) {
  const updateNodeData = useFlowStore((s) => s.updateNodeData);

  const increaseArity = () => {
    updateNodeData(id, { arity: data.arity + 1 });
  };

  const decreaseArity = () => {
    if (data.arity > 1) {
      updateNodeData(id, { arity: data.arity - 1 });
    }
  };

  return (
    <div 
      className="rounded-lg border border-orange-500 bg-orange-950 text-orange-200 shadow-lg min-w-[120px]"
      style={{ minHeight: `${60 + Math.max(0, data.arity - 1) * 20}px`, padding: '4px' }}
    >
      <div className="text-[10px] font-bold text-orange-400 mb-1">TUPLE VALUE</div>
      <div className="text-xs font-mono font-bold mb-1">( {data.arity} )</div>
      <div className="text-[9px] text-orange-300/80 mb-1">Explicit tuple constructor</div>
      <div className="flex gap-1 mb-1">
        <button
          onClick={decreaseArity}
          disabled={data.arity <= 1}
          className="text-[9px] px-1.5 py-0.5 bg-orange-700 hover:bg-orange-600 disabled:opacity-50 disabled:cursor-not-allowed rounded text-orange-100 transition-colors"
        >
          −
        </button>
        <button
          onClick={increaseArity}
          className="text-[9px] px-1.5 py-0.5 bg-orange-700 hover:bg-orange-600 rounded text-orange-100 transition-colors"
        >
          +
        </button>
      </div>
      {Array.from({ length: data.arity }, (_, i) => (
        <Handle
          key={i}
          type="target"
          position={Position.Left}
          id={`tuple-in-${i}`}
          style={{ top: `${52 + i * 20}px` }}
        />
      ))}
      <Handle type="source" position={Position.Right} />
    </div>
  );
}
