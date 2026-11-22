import { Handle, Position, type Node, type NodeProps } from '@xyflow/react';
import type { ClosureData } from '../lib/types';
import { useFlowStore } from '../store/flowStore';

export function ClosureNode({ data, id }: NodeProps<Node<ClosureData>>) {
  const updateNodeData = useFlowStore((s) => s.updateNodeData);

  const updateReturnType = (returnType: string) => {
    updateNodeData(id, { returnType: returnType || 'Any' });
  };

  const typeParamsText = (data.typeParams ?? []).join(', ');
  const updateTypeParams = (value: string) => {
    const typeParams = value
      .split(',')
      .map((entry) => entry.trim())
      .filter((entry) => entry.length > 0);
    updateNodeData(id, { typeParams: typeParams.length > 0 ? typeParams : undefined });
  };

  const updateParam = (index: number, patch: Partial<ClosureData['params'][number]>) => {
    const params = data.params.map((param, paramIndex) =>
      paramIndex === index ? { ...param, ...patch } : param,
    );
    updateNodeData(id, { params, label: buildLabel(params, data.returnType) });
  };

  const addParam = () => {
    const nextIndex = data.params.length + 1;
    const params = [...data.params, { name: `p${nextIndex}`, type: 'Any' }];
    updateNodeData(id, { params, label: buildLabel(params, data.returnType) });
  };

  const removeParam = (index: number) => {
    const params = data.params.filter((_, paramIndex) => paramIndex !== index);
    updateNodeData(id, { params, label: buildLabel(params, data.returnType) });
  };

  return (
    <div
      className="rounded-lg border border-violet-500 bg-violet-950 text-violet-200 px-4 py-3 shadow-lg min-w-[240px]"
      style={{ minHeight: `${84 + Math.max(1, data.params.length) * 34}px`, padding: '4px' }}
    >
      <div className="text-[10px] font-bold text-violet-400 mb-1">λ CLOSURE</div>

      <input
        value={typeParamsText}
        onChange={(evt) => updateTypeParams(evt.target.value)}
        placeholder="type params, e.g. T, U: Numeric"
        className="w-full rounded border border-violet-700 bg-violet-900/40 px-2 py-1 text-xs font-mono text-violet-100 outline-none focus:border-violet-400"
      />

      <div className="mt-1 space-y-1 text-xs text-violet-300">
        {data.params.map((param, index) => (
          <div key={`${id}-param-${index}`} className="grid grid-cols-[1fr_1fr_auto] gap-1 items-center">
            <input
              value={param.name}
              onChange={(evt) => updateParam(index, { name: evt.target.value })}
              className="rounded border border-violet-700 bg-violet-900/40 px-1.5 py-1 font-mono text-violet-100 outline-none focus:border-violet-400"
              placeholder="name"
            />
            <input
              value={param.type}
              onChange={(evt) => updateParam(index, { type: evt.target.value })}
              className="rounded border border-violet-700 bg-violet-900/40 px-1.5 py-1 font-mono text-violet-100 outline-none focus:border-violet-400"
              placeholder="type"
            />
            <button
              onClick={() => removeParam(index)}
              className="rounded border border-violet-700 px-1.5 py-1 text-[10px] text-violet-200 hover:bg-violet-900/60"
              title="Remove parameter"
            >
              x
            </button>
          </div>
        ))}
        <button
          onClick={addParam}
          className="rounded border border-violet-700 px-2 py-1 text-[10px] font-mono text-violet-200 hover:bg-violet-900/60"
        >
          + param
        </button>
      </div>

      <div className="mt-2 text-xs font-mono flex items-center gap-1">
        <span className="text-violet-500">-&gt;</span>
        <input
          value={data.returnType}
          onChange={(evt) => updateReturnType(evt.target.value)}
          className="min-w-0 flex-1 rounded border border-violet-700 bg-violet-900/40 px-1.5 py-1 text-violet-100 outline-none focus:border-violet-400"
        />
      </div>

      {/* Param source handles (expose closure params to body nodes) */}
      {data.params.map((param, index) => (
        <Handle
          key={`${id}-out-${index}`}
          type="source"
          position={Position.Right}
          id={`closure-param-${index}`}
          style={{ top: `${86 + index * 34}px` }}
          title={param.name || `param_${index}`}
        />
      ))}

      {data.params.length === 0 && (
        <Handle
          type="source"
          position={Position.Right}
          id="closure-param-0"
          style={{ top: '86px', opacity: 0.2 }}
          title="Add parameters to expose symbolic outputs"
        />
      )}

      {/* Invisible body-anchor handle (source side of the closure-body-root marker edge) */}
      <Handle
        type="source"
        position={Position.Bottom}
        id="closure-body-anchor"
        style={{ opacity: 0, pointerEvents: 'none' }}
        title="Body root anchor"
      />

      {/* Closure value output — the closure itself as a value passed to callers */}
      <Handle
        type="source"
        position={Position.Left}
        id="closure-out"
        style={{ top: '50%' }}
        title="Closure value"
      />
    </div>
  );
}

function buildLabel(params: { name: string; type: string }[], returnType: string): string {
  const paramText =
    params.length === 0 ? '()' : `(${params.map((p) => `${p.name}: ${p.type}`).join(', ')})`;
  return `${paramText}: ${returnType}`;
}

