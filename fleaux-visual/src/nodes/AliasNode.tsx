import { type Node, type NodeProps } from '@xyflow/react';
import type { AliasDeclData } from '../lib/types';
import { useFlowStore } from '../store/flowStore';

function buildLabel(name: string, targetType: string): string {
  const resolvedName = name.trim() === '' ? 'UnnamedAlias' : name;
  const resolvedTargetType = targetType.trim() === '' ? 'Any' : targetType;
  return 'alias ' + resolvedName + ' = ' + resolvedTargetType;
}

export function AliasNode({ data, id }: NodeProps<Node<AliasDeclData>>) {
  const updateNodeData = useFlowStore((s) => s.updateNodeData);

  const updateName = (name: string) => {
    updateNodeData(id, { name, label: buildLabel(name, data.targetType) });
  };

  const updateTargetType = (targetType: string) => {
    updateNodeData(id, { targetType, label: buildLabel(data.name, targetType) });
  };

  return (
    <div className="rounded-lg border border-lime-500 bg-lime-950 text-lime-100 shadow-lg min-w-60 max-w-[320px]"
         style={{padding: '4px'}}
    >
      <div className="text-[10px] font-bold text-lime-300 mb-1">ALIAS DECL</div>
      <input
        value={data.name}
        onChange={(evt) => updateName(evt.target.value)}
        placeholder="Alias name"
        className="w-full rounded border border-lime-700 bg-lime-900/40 px-2 py-1 text-sm font-bold font-mono text-lime-50 outline-none focus:border-lime-300"
      />
      <input
        value={data.targetType}
        onChange={(evt) => updateTargetType(evt.target.value)}
        placeholder="Target type"
        className="mt-2 w-full rounded border border-emerald-800 bg-emerald-950/40 px-2 py-1 text-xs font-mono text-emerald-100 outline-none focus:border-emerald-300"
      />
      <div className="mt-2 text-[10px] text-lime-300/70 font-mono">Adds a user alias declaration to generated source.</div>
    </div>
  );
}

