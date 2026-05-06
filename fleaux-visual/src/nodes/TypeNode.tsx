import { type Node, type NodeProps } from '@xyflow/react';
import type { TypeDeclData, TypeDeclarationSeparator } from '../lib/types';
import { useFlowStore } from '../store/flowStore';

function buildLabel(name: string, targetType: string, separator: TypeDeclarationSeparator): string {
  const resolvedName = name.trim() === '' ? 'UnnamedType' : name;
  const resolvedTargetType = targetType.trim() === '' ? 'Any' : targetType;
  return 'type ' + resolvedName + ' ' + separator + ' ' + resolvedTargetType;
}

export function TypeNode({ data, id }: NodeProps<Node<TypeDeclData>>) {
  const updateNodeData = useFlowStore((s) => s.updateNodeData);

  const updateName = (name: string) => {
    updateNodeData(id, { name, label: buildLabel(name, data.targetType, data.separator) });
  };

  const updateTargetType = (targetType: string) => {
    updateNodeData(id, { targetType, label: buildLabel(data.name, targetType, data.separator) });
  };

  const updateSeparator = (separator: TypeDeclarationSeparator) => {
    updateNodeData(id, { separator, label: buildLabel(data.name, data.targetType, separator) });
  };

  return (
    <div className="rounded-lg border border-cyan-500 bg-cyan-950 text-cyan-100 px-4 py-3 shadow-lg min-w-[240px] max-w-[320px]">
      <div className="text-[10px] font-bold text-cyan-300 mb-1">TYPE DECL</div>
      <input
        value={data.name}
        onChange={(evt) => updateName(evt.target.value)}
        placeholder="Type name"
        className="w-full rounded border border-cyan-700 bg-cyan-900/40 px-2 py-1 text-sm font-bold font-mono text-cyan-50 outline-none focus:border-cyan-300"
      />
      <div className="mt-2 grid grid-cols-[88px_1fr] gap-2 items-center">
        <select
          value={data.separator}
          onChange={(evt) => updateSeparator(evt.target.value as TypeDeclarationSeparator)}
          className="rounded border border-cyan-700 bg-cyan-900/40 px-2 py-1 text-xs font-mono text-cyan-50 outline-none focus:border-cyan-300"
        >
          <option value="=">=</option>
          <option value="::">::</option>
        </select>
        <input
          value={data.targetType}
          onChange={(evt) => updateTargetType(evt.target.value)}
          placeholder="Target type"
          className="w-full rounded border border-sky-800 bg-sky-950/40 px-2 py-1 text-xs font-mono text-sky-100 outline-none focus:border-sky-300"
        />
      </div>
      <div className="mt-2 text-[10px] text-cyan-300/70 font-mono">Adds a user strong type declaration to generated source.</div>
    </div>
  );
}

