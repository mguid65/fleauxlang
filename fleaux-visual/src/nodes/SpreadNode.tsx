import { Handle, Position } from '@xyflow/react';

// Spreads the items of a tuple into a call's argument list, `(x...) -> F`. Its output connects only to call argument
// slots; the slots after it may stay empty, since the spread can supply their arguments.
export function SpreadNode() {
  return (
    <div className="rounded-lg border border-rose-500 bg-rose-950 text-rose-200 shadow-lg min-w-[110px]" style={{ padding: '4px' }}>
      <div className="text-[10px] font-bold text-rose-400 mb-1">SPREAD</div>
      <div className="text-xs font-mono font-bold">x...</div>
      <div className="text-[9px] text-rose-300/80">tuple items as arguments</div>
      <Handle type="target" position={Position.Left} id="spread-in" style={{ top: '30px' }} />
      <Handle type="source" position={Position.Right} />
    </div>
  );
}
