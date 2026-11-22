import { Handle, Position, type Node, type NodeProps } from '@xyflow/react';
import type { LetData } from '../lib/types';
import { useFlowStore } from '../store/flowStore';

export function LetNode({ data, id }: NodeProps<Node<LetData>>) {
  const updateLetNodeSignature = useFlowStore((s) => s.updateLetNodeSignature);

  const typeParamsText = (data.typeParams ?? []).join(', ');
  const isValue = data.isValueBinding === true;

  // A value let has no type or value parameters.
  const toggleValueBinding = () => {
    updateLetNodeSignature(id, isValue ? { isValueBinding: false } : { isValueBinding: true, params: [], typeParams: [] });
  };

  const updateName = (name: string) => {
    updateLetNodeSignature(id, { name });
  };

  const updateReturnType = (returnType: string) => {
    updateLetNodeSignature(id, { returnType: returnType || 'Any' });
  };

  const updateTypeParams = (value: string) => {
    const typeParams = value
      .split(',')
      .map((entry) => entry.trim())
      .filter((entry) => entry.length > 0);
    updateLetNodeSignature(id, { typeParams });
  };

  const updateParam = (index: number, patch: Partial<LetData['params'][number]>) => {
    const params = data.params.map((param, paramIndex) =>
      paramIndex === index ? { ...param, ...patch } : param,
    );
    updateLetNodeSignature(id, { params });
  };

  const addParam = () => {
    const nextIndex = data.params.length + 1;
    const params = [...data.params, { id: `${id}-param-${crypto.randomUUID()}`, name: `p${nextIndex}`, type: 'Any' }];
    updateLetNodeSignature(id, { params });
  };

  const removeParam = (index: number) => {
    const params = data.params.filter((_, paramIndex) => paramIndex !== index);
    updateLetNodeSignature(id, { params });
  };

  return (
    <div
      className="rounded-lg border border-fuchsia-500 bg-fuchsia-950 text-fuchsia-200 px-4 py-3 shadow-lg min-w-[240px]"
      style={{ minHeight: `${84 + Math.max(1, data.params.length) * 34}px`, padding: '4px' }}
    >
      <div className="flex items-center justify-between mb-1">
        <div className="text-[10px] font-bold text-fuchsia-400">{isValue ? 'VALUE DEF' : 'FUNCTION DEF'}</div>
        <button
          onClick={toggleValueBinding}
          className="rounded border border-fuchsia-700 px-1.5 py-0.5 text-[9px] font-mono text-fuchsia-200 hover:bg-fuchsia-900/60"
          title={isValue ? 'Make this a function: let Name(params): Type' : 'Make this a value: let Name: Type'}
        >
          {isValue ? 'value' : 'function'}
        </button>
      </div>
      <input
        value={data.name}
        onChange={(evt) => updateName(evt.target.value)}
        className="w-full rounded border border-fuchsia-700 bg-fuchsia-900/50 px-2 py-1 text-sm font-bold font-mono text-fuchsia-100 outline-none focus:border-fuchsia-400"
      />

      {!isValue && <input
        value={typeParamsText}
        onChange={(evt) => updateTypeParams(evt.target.value)}
        placeholder="type params, e.g. T, U"
        className="mt-2 w-full rounded border border-fuchsia-700 bg-fuchsia-900/40 px-2 py-1 text-xs font-mono text-fuchsia-100 outline-none focus:border-fuchsia-400"
      />}

      {!isValue && <div className="mt-2 space-y-1 text-xs text-fuchsia-300">
        {data.params.map((param, index) => (
          <div key={`${id}-param-${index}`} className="grid grid-cols-[1fr_1fr_auto] gap-1 items-center">
            <input
              value={param.name}
              onChange={(evt) => updateParam(index, { name: evt.target.value })}
              className="rounded border border-fuchsia-700 bg-fuchsia-900/40 px-1.5 py-1 font-mono text-fuchsia-100 outline-none focus:border-fuchsia-400"
              placeholder="name"
            />
            <input
              value={param.type}
              onChange={(evt) => updateParam(index, { type: evt.target.value })}
              className="rounded border border-fuchsia-700 bg-fuchsia-900/40 px-1.5 py-1 font-mono text-fuchsia-100 outline-none focus:border-fuchsia-400"
              placeholder="type"
            />
            <button
              onClick={() => removeParam(index)}
              className="rounded border border-fuchsia-700 px-1.5 py-1 text-[10px] text-fuchsia-200 hover:bg-fuchsia-900/60"
              title="Remove parameter"
            >
              x
            </button>
          </div>
        ))}
        <button
          onClick={addParam}
          className="rounded border border-fuchsia-700 px-2 py-1 text-[10px] font-mono text-fuchsia-200 hover:bg-fuchsia-900/60"
        >
          + param
        </button>
      </div>}

      <div className="mt-2 text-xs font-mono flex items-center gap-1">
        <span className="text-fuchsia-500">-&gt;</span>
        <input
          value={data.returnType}
          onChange={(evt) => updateReturnType(evt.target.value)}
          className="min-w-0 flex-1 rounded border border-fuchsia-700 bg-fuchsia-900/40 px-1.5 py-1 text-fuchsia-100 outline-none focus:border-fuchsia-400"
        />
      </div>

      {data.params.map((param, index) => (
        <Handle
          key={`${id}-out-${index}`}
          type="source"
          position={Position.Right}
          id={`let-param-${index}`}
          style={{ top: `${86 + index * 34}px` }}
          title={param.name || `param_${index}`}
        />
      ))}

      {data.params.length === 0 && (
        <Handle
          type="source"
          position={Position.Right}
          id="let-param-0"
          style={{ top: '86px', opacity: 0.35 }}
          title="Add parameters to expose symbolic outputs"
        />
      )}
    </div>
  );
}
