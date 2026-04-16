import { Handle, Position, type Node, type NodeProps } from '@xyflow/react';
import { useState } from 'react';
import type { LiteralData } from '../lib/types';
import { useFlowStore } from '../store/flowStore';

const VALUE_TYPE_COLORS: Record<LiteralData['valueType'], string> = {
  Number: 'border-sky-500 bg-sky-950 text-sky-300',
  String: 'border-emerald-500 bg-emerald-950 text-emerald-300',
  Bool: 'border-amber-500 bg-amber-950 text-amber-300',
  Null: 'border-slate-500 bg-slate-900 text-slate-400',
};

const BADGE_COLORS: Record<LiteralData['valueType'], string> = {
  Number: 'bg-sky-800 text-sky-200',
  String: 'bg-emerald-800 text-emerald-200',
  Bool: 'bg-amber-800 text-amber-200',
  Null: 'bg-slate-700 text-slate-300',
};

export function LiteralNode({ data, id }: NodeProps<Node<LiteralData>>) {
  const [isEditing, setIsEditing] = useState(false);
  const [editValue, setEditValue] = useState(data.value);
  const updateNodeData = useFlowStore((s) => s.updateNodeData);
  
  const colors = VALUE_TYPE_COLORS[data.valueType];
  const badge = BADGE_COLORS[data.valueType];

  const handleSave = () => {
    if (editValue !== data.value) {
      updateNodeData(id, { value: editValue, label: editValue || '(empty)' });
    }
    setIsEditing(false);
  };

  const handleKeyDown = (e: React.KeyboardEvent) => {
    if (e.key === 'Enter') handleSave();
    if (e.key === 'Escape') {
      setEditValue(data.value);
      setIsEditing(false);
    }
  };

  return (
    <div className={`rounded-lg border px-4 py-2 shadow-lg min-w-[120px] ${colors}`}>
      <span className={`text-[10px] font-bold rounded px-1.5 py-0.5 ${badge} mb-1 inline-block`}>
        {data.valueType}
      </span>
      {isEditing ? (
        <input
          autoFocus
          type={data.valueType === 'Number' ? 'number' : 'text'}
          value={editValue}
          onChange={(e) => setEditValue(e.target.value)}
          onBlur={handleSave}
          onKeyDown={handleKeyDown}
          className="w-full mt-1 px-2 py-1 text-sm font-mono bg-slate-900 border border-slate-600 rounded text-sky-200 focus:border-sky-400 outline-none"
        />
      ) : (
        <div
          onClick={() => setIsEditing(true)}
          className="text-sm font-mono mt-1 truncate max-w-[200px] cursor-text hover:opacity-80 transition-opacity"
          title="Click to edit"
        >
          {data.label}
        </div>
      )}
      <Handle type="source" position={Position.Right} />
    </div>
  );
}
